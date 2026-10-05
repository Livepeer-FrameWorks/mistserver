#define private public
#define protected public
#include "../src/input/input_buffer.h"
#undef protected
#undef private
#include "../src/input/processing_rate.h"

#include <mist/http_parser.h>
#include <mist/stream.h>
#include <mist/timing.h>

#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {
  int failures = 0;

  void check(bool ok, const std::string & message) {
    if (ok) { return; }
    std::cerr << "FAIL: " << message << std::endl;
    ++failures;
  }

  const char *inhibit360 = "video=<640x360";
  const char *inhibit480 = "video=<850x480";
  const char *inhibit720 = "video=<1280x720";
  const char *inhibit1080 = "video=<1920x1080";

  class InputBufferProbe : public Mist::InputBuffer {
    public:
      explicit InputBufferProbe(Util::Config *config) : Mist::InputBuffer(config) {}

      std::vector<std::string> replacePayloads;
      std::string replaceResponse;

      bool requestProcessReplacement(const std::string & payload, std::string & response) {
        replacePayloads.push_back(payload);
        if (!replaceResponse.size()) { return false; }
        response = replaceResponse;
        return true;
      }

      void initMetadata(const std::string & name) {
        streamName = name;
        meta.reInit("", true);
      }

      size_t addTrack(const std::string & type, const std::string & codec, uint32_t width, uint32_t height,
                      size_t sourceTrack = INVALID_TRACK_ID) {
        const size_t track = meta.addTrack();
        meta.setID(track, track + 1);
        meta.setType(track, type);
        meta.setCodec(track, codec);
        if (type == "video") {
          meta.setWidth(track, width);
          meta.setHeight(track, height);
        }
        if (type == "audio") {
          meta.setRate(track, 48000);
          meta.setChannels(track, 2);
        }
        if (sourceTrack != INVALID_TRACK_ID) { meta.setSourceTrack(track, sourceTrack); }
        meta.validateTrack(track, TRACK_VALID_ALL);
        meta.update(0, 0, track, 1, 0, true, 1);
        meta.breakClaim(track);
        return track;
      }

      std::string key(const JSON::Value & proc) const {
        JSON::Value keyed = proc;
        keyed["source"] = streamName;
        return keyed.toString();
      }

      // Mirrors how userLeadIn feeds checkProcesses: the replacement layer over the configured list.
      void tick(const JSON::Value & configured) {
        checkProcesses(processReplacements.size() ? applyProcessReplacements(configured) : configured);
      }

      void stopAll() {
        for (std::map<std::string, pid_t>::iterator it = runningProcs.begin(); it != runningProcs.end(); ++it) {
          if (it->second && Util::Procs::isActive(it->second)) { Util::Procs::Murder(it->second); }
        }
        runningProcs.clear();
      }
  };

  JSON::Value livepeerWithLadder() {
    JSON::Value livepeer;
    livepeer["process"] = "Livepeer";
    const char *names[] = {"360p", "480p", "720p", "1080p"};
    const char *inhibits[] = {inhibit360, inhibit480, inhibit720, inhibit1080};
    for (size_t i = 0; i < 4; ++i) {
      JSON::Value profile;
      profile["name"] = names[i];
      profile["track_inhibit"] = inhibits[i];
      livepeer["target_profiles"].append(profile);
    }
    return livepeer;
  }

  /// Adds the derived tracks a restarted or long-running stream carries besides its source.
  size_t addDerivedClutter(InputBufferProbe & input, size_t sourceVideo, size_t sourceAudio) {
    input.addTrack("video", "JPEG", 160, 90, sourceVideo); // Thumbs preview
    input.addTrack("meta", "thumbvtt", 0, 0, sourceVideo); // Thumbs sprite VTT
    input.addTrack("video", "H264", 640, 360, sourceVideo); // previous-run rendition
    input.addTrack("video", "H264", 854, 480, sourceVideo); // previous-run rendition
    input.addTrack("audio", "opus", 0, 0, sourceAudio); // another AV process' output
    return 5;
  }

  bool writeFixture(const std::string & path, const std::string & body) {
    std::ofstream out(path.c_str());
    if (!out) { return false; }
    out << "#!/bin/sh\n" << body << "\n";
    out.close();
    return chmod(path.c_str(), 0755) == 0;
  }

  std::vector<std::string> lines(const std::string & payload) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
      size_t nl = payload.find('\n', start);
      out.push_back(payload.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
      if (nl == std::string::npos) { break; }
      start = nl + 1;
    }
    return out;
  }

  void testInhibitors(Util::Config & config) {
    // 720p source with a source JSON meta track and the derived tracks of an earlier run.
    {
      InputBufferProbe input(&config);
      input.initMetadata("inhibit-720-test");
      const size_t video = input.addTrack("video", "H264", 1280, 720);
      const size_t audio = input.addTrack("audio", "AAC", 0, 0);
      input.addTrack("meta", "JSON", 0, 0);
      const size_t derived = addDerivedClutter(input, video, audio);
      const DTSC::Meta & M = input.meta;

      check(!Util::inhibitorMatchesSource(M, inhibit360), "720p source must keep the 360p rendition");
      check(!Util::inhibitorMatchesSource(M, inhibit480), "720p source must keep the 480p rendition");
      check(!Util::inhibitorMatchesSource(M, inhibit720), "720p source must keep the 720p rendition");
      check(Util::inhibitorMatchesSource(M, inhibit1080), "720p source must drop the 1080p rendition");
      check(!Util::inhibitorMatchesSource(M, "audio=opus"), "opus produced by another process must not inhibit");
      check(Util::inhibitorMatchesSource(M, "audio=aac"), "an AAC source must inhibit the AAC transcode");
      check(!Util::inhibitorMatchesSource(M, "subtitle=all"), "no subtitle source means no subtitle inhibit");
      // The plain selector still sees the previous-run 640x360 rendition; only the source filter ignores it.
      check(Util::wouldSelect(M, std::string("audio=none&video=none&subtitle=none&meta=none&") + inhibit480).size() != 0,
            "fixture sanity: the previous-run rendition must be selectable by the 480p inhibitor");

      JSON::Value opusTranscode;
      opusTranscode["process"] = "AV";
      opusTranscode["codec"] = "opus";
      opusTranscode["track_inhibit"] = "audio=opus";
      check(input.processingProcessMatchesSource(opusTranscode), "readiness must not treat another producer's opus track as an inhibitor");

      JSON::Value procs;
      procs.append(livepeerWithLadder());
      bool resolved = false;
      check(input.expectedProcessingOutputTracks(procs, resolved) == derived + 3 && resolved,
            "720p source must expect exactly the 360/480/720 renditions");
    }

    // 1080p source keeps all four renditions, even with a 160x90 JPEG preview present.
    {
      InputBufferProbe input(&config);
      input.initMetadata("inhibit-1080-test");
      const size_t video = input.addTrack("video", "H264", 1920, 1080);
      const size_t audio = input.addTrack("audio", "AAC", 0, 0);
      const size_t derived = addDerivedClutter(input, video, audio);
      const DTSC::Meta & M = input.meta;
      check(!Util::inhibitorMatchesSource(M, inhibit360) && !Util::inhibitorMatchesSource(M, inhibit480) &&
              !Util::inhibitorMatchesSource(M, inhibit720) && !Util::inhibitorMatchesSource(M, inhibit1080),
            "1080p source must keep all four renditions");
      JSON::Value procs;
      procs.append(livepeerWithLadder());
      bool resolved = false;
      check(input.expectedProcessingOutputTracks(procs, resolved) == derived + 4 && resolved,
            "1080p source must expect all four renditions");
    }

    // An opus source does inhibit the opus transcode, in readiness and in the supervisor.
    {
      InputBufferProbe input(&config);
      input.initMetadata("inhibit-opus-source-test");
      input.addTrack("video", "H264", 1280, 720);
      input.addTrack("audio", "opus", 0, 0);
      check(Util::inhibitorMatchesSource(input.meta, "audio=opus"), "an opus source must inhibit the opus transcode");
      JSON::Value opusTranscode;
      opusTranscode["process"] = "AV";
      opusTranscode["codec"] = "opus";
      opusTranscode["track_inhibit"] = "audio=opus";
      check(!input.processingProcessMatchesSource(opusTranscode), "readiness must honour an opus source inhibitor");
    }
  }

  /// Runs rate-controller ticks with the given procs registered as running
  /// and returns the effective speed after each tick.
  std::vector<uint64_t> rampSpeeds(Util::Config & config, const std::string & name, const JSON::Value & proc) {
    std::vector<uint64_t> speeds;
    pid_t child = fork();
    if (child == 0) {
      sleep(30);
      _exit(0);
    }
    if (child < 0) {
      check(false, "could not fork a stand-in process");
      return speeds;
    }
    {
      InputBufferProbe input(&config);
      input.initMetadata(name);
      input.runningProcs[proc.toString()] = child;
      for (int i = 0; i < 12; ++i) {
        input.lastRateUpdateMs = 0;
        input.updateProcessingRate();
        speeds.push_back(input.effectiveSpeed);
      }
      input.runningProcs.clear();
    }
    kill(child, SIGKILL);
    waitpid(child, 0, 0);
    return speeds;
  }

  void testUnconstrainedRamp(Util::Config & config) {
    // Only an inconsequential proc (Thumbs) runs, as for chapter finalization:
    // nothing constrains the feed, so the speed ramps up instead of staying 1x.
    JSON::Value thumbs;
    thumbs["process"] = "Thumbs";
    thumbs["inconsequential"] = true;
    std::vector<uint64_t> speeds = rampSpeeds(config, "unconstrained-ramp-test", thumbs);
    check(speeds.size() == 12, "rate controller must run every tick");
    if (speeds.size() == 12) {
      check(speeds.front() <= 2, "unconstrained feed must ramp from 1x, got " + std::to_string(speeds.front()));
      check(speeds.back() > 8, "unconstrained feed must ramp well past 1x, got " + std::to_string(speeds.back()));
      check(speeds.back() <= Mist::PROCESSING_UNCONSTRAINED_SPEED, "unconstrained feed must respect its ceiling");
    }

    // A consequential proc that has not published its contract still holds 1x.
    JSON::Value transcode;
    transcode["process"] = "AV";
    speeds = rampSpeeds(config, "constrained-hold-test", transcode);
    check(speeds.size() == 12 && speeds.back() == 1, "a consequential proc without a contract must hold 1x");
  }

  // Declarations as MistProcAV, MistProcONNX, MistProcThumbs and MistProcLivepeer print them with
  // --describe-outputs for these configurations.
  JSON::Value avDeclaration(const std::string & codec, const std::string & trackSelect, int mask) {
    JSON::Value declaration = JSON::fromString(R"({"select":{"codecs":[[["YUYV"]],[["UYVY"]],[["NV12"]],[["I420"]],[["H264"]],[["AV1"]],[["JPEG"]]],)"
                                               R"("target":{"audio":"none","meta":"none","subtitle":"none"}}})");
    declaration["select"]["target"]["video"] = trackSelect + ",|first";
    JSON::Value output;
    output["output"] = "video";
    output["type"] = "video";
    output["codec"] = codec;
    output["mask"] = mask;
    declaration["outputs"].append(output);
    return declaration;
  }

  JSON::Value onnxDeclaration(const std::string & trackSelect, bool annotated, int mask) {
    JSON::Value declaration =
      JSON::fromString(R"({"select":{"codecs":[[["YUYV","UYVY","NV12","I420","JPEG"]],[["PCM"]],[["ONNXTENSOR"]]],"target":{}}})");
    declaration["select"]["target"] = JSON::Value();
    std::map<std::string, std::string> params;
    HTTP::parseVars(trackSelect, params);
    for (const auto & param : params) { declaration["select"]["target"][param.first] = param.second; }
    JSON::Value output;
    output["output"] = "default/results";
    output["type"] = "meta";
    output["codec"] = "JSON";
    output["mask"] = mask;
    declaration["outputs"].append(output);
    if (annotated) {
      output["output"] = "default/annotations";
      output["type"] = "video";
      output["codec"] = "JPEG";
      declaration["outputs"].append(output);
    }
    return declaration;
  }

  JSON::Value thumbsDeclaration(const std::string & trackSelect) {
    JSON::Value declaration = JSON::fromString(R"({"select":{"codecs":[[["H264","AV1","JPEG"]]]},"outputs":[)"
                                               R"({"codec":"JPEG","lang":"thu","mask":3,"output":"sprite","type":"video"},)"
                                               R"({"codec":"thumbvtt","mask":3,"output":"vtt","type":"meta"},)"
                                               R"({"codec":"JPEG","lang":"pre","mask":3,"output":"preview","type":"video"}]})");
    std::map<std::string, std::string> params;
    HTTP::parseVars(trackSelect, params);
    for (const auto & param : params) { declaration["select"]["target"][param.first] = param.second; }
    return declaration;
  }

  JSON::Value livepeerDeclaration(const JSON::Value & livepeer) {
    JSON::Value declaration = JSON::fromString(
      R"({"select":{"codecs":[[["+H264","+HEVC","+MPEG2"],["+AAC"]]],"target":{"audio":"none","video":"maxbps"}}})");
    jsonForEachConst (livepeer["target_profiles"], prof) {
      JSON::Value output;
      output["output"] = (*prof)["name"];
      output["type"] = "video";
      output["codec"] = "H264";
      output["mask"] = 3;
      if (prof->isMember("track_inhibit")) { output["track_inhibit"] = (*prof)["track_inhibit"]; }
      declaration["outputs"].append(output);
    }
    return declaration;
  }

  Mist::ProcessGraphNode graphNode(const std::string & stream, JSON::Value proc, const JSON::Value & declaration) {
    Mist::ProcessGraphNode node;
    proc["source"] = stream;
    node.proc = proc;
    node.config = proc.toString();
    node.declaration = declaration;
    return node;
  }

  /// The output names the graph expects, as "<process>:<output>" sorted.
  std::set<std::string> expectedOutputs(const Mist::ProcessGraph & graph, const std::vector<Mist::ProcessGraphNode> & nodes) {
    std::set<std::string> names;
    jsonForEachConst (graph.outputs, it) {
      for (const Mist::ProcessGraphNode & node : nodes) {
        if ((*it)["producer"].asStringRef() != node.config) { continue; }
        names.insert(node.proc["process"].asString() + ":" + (*it)["output"].asString() + ":" +
                     (*it)["codec"].asString() + ":" + std::to_string((*it)["mask"].asInt()));
      }
    }
    return names;
  }

  std::string joined(const std::set<std::string> & names) {
    std::string all;
    for (const std::string & name : names) { all += name + " "; }
    return all;
  }

  void testGraph(Util::Config & config) {
    // The graph is decided from the source tracks alone, before any process runs.
    InputBufferProbe input(&config);
    input.initMetadata("graph-test");
    input.addTrack("video", "H264", 1280, 720);
    input.addTrack("audio", "AAC", 0, 0);
    const DTSC::Meta & M = input.meta;

    JSON::Value av;
    av["process"] = "AV";
    av["codec"] = "NV12";
    av["track_select"] = "video=H264&audio=none";
    av["target_mask"] = TRACK_VALID_INT_PROCESS;
    JSON::Value onnx;
    onnx["process"] = "ONNX";
    onnx["track_select"] = "video=NV12&audio=none";
    onnx["target_mask"] = TRACK_VALID_EXT_PUSH;

    {
      std::vector<Mist::ProcessGraphNode> nodes;
      nodes.push_back(graphNode("graph-test", av, avDeclaration("NV12", "H264", TRACK_VALID_INT_PROCESS)));
      nodes.push_back(graphNode("graph-test", onnx, onnxDeclaration("video=NV12&audio=none", false, TRACK_VALID_EXT_PUSH)));
      Mist::ProcessGraph graph = Mist::buildProcessGraph(M, nodes);
      check(graph.runs.size() == 2, "AV->ONNX: both processes must run before either produced anything");
      std::set<std::string> want = {"AV:video:NV12:4", "ONNX:default/results:JSON:2"};
      check(expectedOutputs(graph, nodes) == want, "AV->ONNX outputs, got " + joined(expectedOutputs(graph, nodes)));
    }

    {
      // ONNX drawing its results adds the annotated MJPEG track.
      JSON::Value annotated = onnx;
      annotated["annotated_video"] = true;
      std::vector<Mist::ProcessGraphNode> nodes;
      nodes.push_back(graphNode("graph-test", av, avDeclaration("NV12", "H264", TRACK_VALID_INT_PROCESS)));
      nodes.push_back(graphNode("graph-test", annotated, onnxDeclaration("video=NV12&audio=none", true, TRACK_VALID_EXT_PUSH)));
      Mist::ProcessGraph graph = Mist::buildProcessGraph(M, nodes);
      std::set<std::string> want = {"AV:video:NV12:4", "ONNX:default/annotations:JPEG:2", "ONNX:default/results:JSON:2"};
      check(expectedOutputs(graph, nodes) == want, "ONNX annotated_video outputs, got " + joined(expectedOutputs(graph, nodes)));
    }

    {
      // AV (H264 -> JPEG) -> AV (JPEG -> NV12) -> ONNX: three rounds deep.
      JSON::Value toJpeg = av;
      toJpeg["codec"] = "JPEG";
      JSON::Value toRaw = av;
      toRaw["track_select"] = "video=JPEG&audio=none";
      std::vector<Mist::ProcessGraphNode> nodes;
      nodes.push_back(graphNode("graph-test", onnx, onnxDeclaration("video=NV12&audio=none", false, TRACK_VALID_EXT_PUSH)));
      nodes.push_back(graphNode("graph-test", toRaw, avDeclaration("NV12", "JPEG", TRACK_VALID_INT_PROCESS)));
      nodes.push_back(graphNode("graph-test", toJpeg, avDeclaration("JPEG", "H264", TRACK_VALID_INT_PROCESS)));
      Mist::ProcessGraph graph = Mist::buildProcessGraph(M, nodes);
      check(graph.runs.size() == 3, "AV->AV->ONNX: every stage must run, got " + std::to_string(graph.runs.size()));
      std::set<std::string> want = {"AV:video:JPEG:4", "AV:video:NV12:4", "ONNX:default/results:JSON:2"};
      check(expectedOutputs(graph, nodes) == want, "AV->AV->ONNX outputs, got " + joined(expectedOutputs(graph, nodes)));
    }

    {
      // Thumbs reading the AV1 an AV process makes from the H264 source.
      JSON::Value toAv1 = av;
      toAv1["codec"] = "AV1";
      toAv1["target_mask"] = TRACK_VALID_ALL;
      JSON::Value thumbs;
      thumbs["process"] = "Thumbs";
      thumbs["track_select"] = "video=AV1";
      std::vector<Mist::ProcessGraphNode> nodes;
      nodes.push_back(graphNode("graph-test", thumbs, thumbsDeclaration("video=AV1")));
      nodes.push_back(graphNode("graph-test", toAv1, avDeclaration("AV1", "H264", TRACK_VALID_ALL)));
      Mist::ProcessGraph graph = Mist::buildProcessGraph(M, nodes);
      std::set<std::string> want = {"AV:video:AV1:255", "Thumbs:preview:JPEG:3", "Thumbs:sprite:JPEG:3", "Thumbs:vtt:thumbvtt:3"};
      check(graph.runs.size() == 2, "Thumbs on AV: both must run");
      check(expectedOutputs(graph, nodes) == want, "Thumbs on AV outputs, got " + joined(expectedOutputs(graph, nodes)));
    }

    {
      // Livepeer on a 720p source drops the 1080p profile; ONNX cannot read its H264 renditions.
      JSON::Value livepeer = livepeerWithLadder();
      JSON::Value onLivepeer = onnx;
      onLivepeer["track_select"] = "video=H264&audio=none";
      std::vector<Mist::ProcessGraphNode> nodes;
      nodes.push_back(graphNode("graph-test", livepeer, livepeerDeclaration(livepeer)));
      nodes.push_back(graphNode("graph-test", onLivepeer, onnxDeclaration("video=H264&audio=none", false, TRACK_VALID_EXT_PUSH)));
      Mist::ProcessGraph graph = Mist::buildProcessGraph(M, nodes);
      std::set<std::string> want = {"Livepeer:360p:H264:3", "Livepeer:480p:H264:3", "Livepeer:720p:H264:3"};
      check(expectedOutputs(graph, nodes) == want,
            "inhibited Livepeer profile outputs, got " + joined(expectedOutputs(graph, nodes)));
      check(graph.runs.size() == 1 && graph.skipReasons.count(nodes[1].config),
            "Livepeer->ONNX: ONNX reads no codec Livepeer or the source makes and must not run");

      // Livepeer -> AV (H264 rendition -> NV12) -> ONNX: the AV reads a rendition by codec and size.
      JSON::Value fromRendition = av;
      fromRendition["track_select"] = "video=H264&audio=none";
      nodes.clear();
      nodes.push_back(graphNode("graph-test", livepeer, livepeerDeclaration(livepeer)));
      nodes.push_back(graphNode("graph-test", fromRendition, avDeclaration("NV12", "H264", TRACK_VALID_INT_PROCESS)));
      nodes.push_back(graphNode("graph-test", onnx, onnxDeclaration("video=NV12&audio=none", false, TRACK_VALID_EXT_PUSH)));
      graph = Mist::buildProcessGraph(M, nodes);
      check(graph.runs.size() == 3, "Livepeer->AV->ONNX: every stage must run");
    }

    {
      // A process never runs on its own output alone, and an inhibited or retired one adds nothing.
      JSON::Value jpegOnly = av;
      jpegOnly["codec"] = "JPEG";
      jpegOnly["track_select"] = "video=JPEG&audio=none";
      JSON::Value tagged = onnx;
      tagged["tags_inhibit"] = "noai";
      std::vector<Mist::ProcessGraphNode> nodes;
      nodes.push_back(graphNode("graph-test", jpegOnly, avDeclaration("JPEG", "JPEG", TRACK_VALID_ALL)));
      nodes.push_back(graphNode("graph-test", av, avDeclaration("NV12", "H264", TRACK_VALID_INT_PROCESS)));
      nodes.push_back(graphNode("graph-test", tagged, onnxDeclaration("video=NV12&audio=none", false, TRACK_VALID_EXT_PUSH)));
      std::set<std::string> tags = {"noai"};
      nodes[2].inhibited = Mist::processInhibitReason(nodes[2].proc, M, tags);
      nodes[1].producing = false;
      Mist::ProcessGraph graph = Mist::buildProcessGraph(M, nodes);
      check(!graph.runs.count(nodes[0].config), "a process must not run on its own declared output alone");
      check(graph.skipReasons[nodes[2].config] == "inhibited by stream tag 'noai'", "a tag-inhibited process must not run");
      check(graph.runs.count(nodes[1].config) && graph.outputs.size() == 0, "a retired process must add no expected outputs");
    }

    {
      // A process without a declaration runs on its configured track selection and adds nothing.
      JSON::Value legacy;
      legacy["process"] = "Legacy";
      legacy["track_select"] = "video=H264";
      std::vector<Mist::ProcessGraphNode> nodes;
      nodes.push_back(graphNode("graph-test", legacy, JSON::Value()));
      Mist::ProcessGraph graph = Mist::buildProcessGraph(M, nodes);
      check(graph.runs.size() == 1 && graph.outputs.size() == 0, "an undeclared process must run on its track selection");
    }
  }

  /// The supervisor keeps every process the graph runs configured, and starts each once a track
  /// it selects carries data.
  void testGraphSupervisor(Util::Config & config) {
    const std::string suffix = std::to_string(getpid());
    const std::string avName = "GraphAV" + suffix;
    const std::string onnxName = "GraphONNX" + suffix;
    const std::string avPath = Util::getMyPath() + "MistProc" + avName;
    const std::string onnxPath = Util::getMyPath() + "MistProc" + onnxName;
    auto fixture = [](const JSON::Value & declaration) {
      return "[ \"$1\" = --describe-outputs ] && { echo '" + declaration.toString() + "'; exit 0; }\nexec sleep 30";
    };
    if (!writeFixture(avPath, fixture(avDeclaration("NV12", "H264", TRACK_VALID_INT_PROCESS))) ||
        !writeFixture(onnxPath, fixture(onnxDeclaration("video=NV12&audio=none", false, TRACK_VALID_EXT_PUSH)))) {
      check(false, "could not write graph supervisor fixtures next to the test binary");
      return;
    }

    InputBufferProbe input(&config);
    const std::string stream = "graphSupervisor" + suffix;
    input.initMetadata(stream);
    const size_t video = input.addTrack("video", "H264", 1280, 720);
    input.addTrack("audio", "AAC", 0, 0);
    char statePage[NAME_BUFFER_SIZE];
    snprintf(statePage, sizeof(statePage), SHM_STREAM_STATE, stream.c_str());
    IPC::sharedPage state(statePage, STRMSTATE_PAGE_LEN, true, false);
    if (!state) {
      check(false, "could not create the stream state page");
      unlink(avPath.c_str());
      unlink(onnxPath.c_str());
      return;
    }
    memset(state.mapped, 0, state.len);
    state.mapped[0] = STRMSTAT_READY;
    config.is_active = true;

    JSON::Value av;
    av["process"] = avName;
    av["codec"] = "NV12";
    av["track_select"] = "video=H264&audio=none";
    JSON::Value onnx;
    onnx["process"] = onnxName;
    onnx["track_select"] = "video=NV12&audio=none";
    JSON::Value configured;
    configured.append(av);
    configured.append(onnx);

    input.tick(configured);
    check(input.processGraph.runs.count(input.key(av)) && input.processGraph.runs.count(input.key(onnx)),
          "the graph must run the chained ONNX process before its input exists");
    check(input.runningProcs.count(input.key(av)) && input.runningProcs[input.key(av)], "the AV process must start on the source track");
    check(!input.runningProcs.count(input.key(onnx)), "ONNX must not start before a track it selects carries data");
    const size_t declaredBefore = input.processDeclarations.size();
    input.tick(configured);
    check(input.processDeclarations.size() == declaredBefore && declaredBefore == 2, "each configuration must be described once");

    const size_t raw = input.addTrack("video", "NV12", 1280, 720, video);
    input.meta.setOutputKey(raw, DTSC::outputKey(DTSC::processIdentity(input.key(av)), "video"));
    input.meta.validateTrack(raw, TRACK_VALID_INT_PROCESS);
    input.tick(configured);
    check(input.runningProcs.count(input.key(onnx)) && input.runningProcs[input.key(onnx)],
          "ONNX must start once the AV output it selects carries data");

    input.stopAll();
    state.master = true;
    state.close();
    unlink(avPath.c_str());
    unlink(onnxPath.c_str());
  }

  void testSupervisor(Util::Config & config) {
    const std::string suffix = std::to_string(getpid());
    const std::string failName = "UnitReplaceFail" + suffix;
    const std::string sleepName = "UnitReplaceSleep" + suffix;
    const std::string failPath = Util::getMyPath() + "MistProc" + failName;
    const std::string sleepPath = Util::getMyPath() + "MistProc" + sleepName;
    // Neither fixture declares its outputs, so the buffer supervises them on their configuration.
    const std::string undeclared = "[ \"$1\" = --describe-outputs ] && exit 0\n";
    if (!writeFixture(failPath, undeclared + "exit 2") || !writeFixture(sleepPath, undeclared + "exec sleep 30")) {
      check(false, "could not write supervisor fixture executables next to the test binary");
      return;
    }

    InputBufferProbe input(&config);
    const std::string stream = "replaceUnit" + suffix;
    input.initMetadata(stream);
    const size_t video = input.addTrack("video", "H264", 1280, 720);
    const size_t audio = input.addTrack("audio", "AAC", 0, 0);
    input.addTrack("audio", "opus", 0, 0, audio); // earlier AV output, must not inhibit
    (void)video;

    char statePage[NAME_BUFFER_SIZE];
    snprintf(statePage, sizeof(statePage), SHM_STREAM_STATE, stream.c_str());
    IPC::sharedPage state(statePage, STRMSTATE_PAGE_LEN, true, false);
    if (!state) {
      check(false, "could not create the stream state page");
      unlink(failPath.c_str());
      unlink(sleepPath.c_str());
      return;
    }
    memset(state.mapped, 0, state.len);
    state.mapped[0] = STRMSTAT_READY;
    config.is_active = true;

    // M2 in the supervisor: another producer's opus track does not keep the opus transcode down.
    JSON::Value opusTranscode;
    opusTranscode["process"] = sleepName;
    opusTranscode["codec"] = "opus";
    opusTranscode["track_inhibit"] = "audio=opus";
    JSON::Value inhibited;
    inhibited.append(opusTranscode);
    input.tick(inhibited);
    check(input.runningProcs.count(input.key(opusTranscode)) && input.runningProcs[input.key(opusTranscode)],
          "an opus transcode must start although another producer already made an opus track");
    input.stopAll();

    // M3: a hard failure fires PROCESS_REPLACE once and the replacements start.
    JSON::Value failing;
    failing["process"] = failName;
    failing["x-LSP-name"] = "gateway transcode";
    JSON::Value survivor;
    survivor["process"] = sleepName;
    survivor["x-LSP-name"] = "local rendition";
    JSON::Value failingReplacement;
    failingReplacement["process"] = failName;
    failingReplacement["x-LSP-name"] = "local rendition that also fails";
    JSON::Value response;
    response.append(survivor);
    response.append(failingReplacement);
    input.replaceResponse = response.toString();

    JSON::Value configured;
    configured.append(failing);
    const std::string failedKey = input.key(failing);
    const std::string survivorKey = input.key(survivor);
    const std::string failingReplacementKey = input.key(failingReplacement);

    const uint64_t deadline = Util::bootMS() + 10000;
    while (Util::bootMS() < deadline &&
           !(input.procHardFailed.count(failingReplacementKey) && input.runningProcs.count(survivorKey))) {
      input.tick(configured);
      Util::sleep(50);
    }

    check(input.procHardFailed.count(failedKey), "the failed config must stay hard-failed");
    check(input.replacePayloads.size() == 1,
          "PROCESS_REPLACE must fire exactly once (replacement failure included), got " +
            std::to_string(input.replacePayloads.size()));
    if (input.replacePayloads.size()) {
      std::vector<std::string> payload = lines(input.replacePayloads[0]);
      check(payload.size() == 6, "PROCESS_REPLACE payload must have six lines");
      if (payload.size() == 6) {
        check(payload[0] == stream, "payload line 1 must be the stream name");
        check(payload[1] == failName, "payload line 2 must be the process type");
        check(payload[2] == failedKey, "payload line 3 must be the failed process config");
        check(payload[3] == "2", "payload line 4 must be the exit code");
      }
    }
    check(input.runningProcs.count(survivorKey) && input.runningProcs[survivorKey] &&
            Util::Procs::isActive(input.runningProcs[survivorKey]),
          "the replacement process must be started and running");
    check(input.procHardFailed.count(failingReplacementKey), "the failing replacement must be hard-failed");
    check(!input.runningProcs.count(failedKey), "the failed config must not be restarted");

    // Further ticks keep the layer and never re-trigger.
    for (size_t i = 0; i < 5; ++i) {
      input.tick(configured);
      Util::sleep(20);
    }
    check(input.replacePayloads.size() == 1, "later supervisor passes must not fire PROCESS_REPLACE again");
    check(input.procHardFailed.count(failedKey), "the replaced config must stay hard-failed across passes");
    check(input.runningProcs.count(survivorKey) && Util::Procs::isActive(input.runningProcs[survivorKey]),
          "the replacement must stay supervised across passes");

    // An empty response leaves the failure disabled and still counts as the one attempt.
    {
      InputBufferProbe other(&config);
      other.initMetadata(stream);
      other.addTrack("video", "H264", 1280, 720);
      other.replaceResponse = "[]";
      const uint64_t otherDeadline = Util::bootMS() + 10000;
      while (Util::bootMS() < otherDeadline && !other.procHardFailed.count(failedKey)) {
        other.tick(configured);
        Util::sleep(50);
      }
      for (size_t i = 0; i < 3; ++i) {
        other.tick(configured);
        Util::sleep(20);
      }
      check(other.procHardFailed.count(failedKey), "a failure with an empty replacement must stay hard-failed");
      check(other.replacePayloads.size() == 1, "an empty replacement must not be retried");
      check(other.processReplacements.empty(), "an empty response must not install a replacement layer");
      other.stopAll();
    }

    input.stopAll();
    state.master = true;
    state.close();
    unlink(failPath.c_str());
    unlink(sleepPath.c_str());
  }
} // namespace

int main() {
  Util::Config config("input-buffer-process-unit");
  testInhibitors(config);
  testGraph(config);
  testGraphSupervisor(config);
  testUnconstrainedRamp(config);
  testSupervisor(config);
  if (failures) {
    std::cerr << failures << " failure(s)" << std::endl;
    return 1;
  }
  return 0;
}
