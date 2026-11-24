#define private public
#define protected public
#include "../src/input/input_buffer.h"
#undef protected
#undef private
#include "../src/input/processing_rate.h"
#include "../src/processing_lifecycle.h"

#include <mist/http_parser.h>
#include <mist/proc_stats.h>
#include <mist/stream.h>
#include <mist/timing.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <pthread.h>
#include <string>
#include <thread>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {
  int failures = 0;
  volatile sig_atomic_t retentionWaitSignals = 0;

  void interruptRetentionWait(int) {
    ++retentionWaitSignals;
  }

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
      void tick(const JSON::Value & configured) { checkProcesses(configured); }

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

  /// The selector a transcode reads its source with: the highest-bitrate original continuous-video
  /// track, as MistProcLivepeer and MistProcAV declare it for a maxbps source selection.
  const char *transcodeSource = "source,!jpeg,!png,|maxbps";

  /// One rung of the FrameWorks ladder: its short side, target bitrate and the track_retain
  /// selector pkg/mist RetainSelector composes for it.
  struct LadderRung {
      const char *name;
      uint32_t shortSide;
      uint64_t bitrate;
      const char *retain;
  };
  const LadderRung ladderRungs[] = {
    {"360p", 360, 900000, "video=h264,hevc,|source,|>0x327,|<0x393,|>439kbps,|<1758kbps"},
    {"480p", 480, 1600000, "video=h264,hevc,|source,|>0x447,|<0x513,|>781kbps,|<3126kbps"},
    {"720p", 720, 4500000, "video=h264,hevc,|source,|>0x683,|<0x757,|>2197kbps,|<8790kbps"},
    {"1080p", 1080, 7800000, "video=h264,hevc,|source,|>0x1025,|<0x1135,|>3808kbps,|<15235kbps"},
  };

  /// Mist track bps (bytes per second) of a rate in megabits per second.
  uint64_t mbit(double rate) {
    return (uint64_t)(rate * 1000000 / 8);
  }

  /// The Livepeer process FrameWorks composes: every profile carries its short-side inhibitor and
  /// its retain selector, and the process masks the originals its rungs replace.
  JSON::Value retainLadder(const std::string & process) {
    JSON::Value livepeer;
    livepeer["process"] = process;
    livepeer["track_inhibit"] = "video=<0x360";
    livepeer["mask_replaced_originals"] = true;
    for (const LadderRung & rung : ladderRungs) {
      JSON::Value profile;
      profile["name"] = rung.name;
      profile["height"] = rung.shortSide;
      profile["bitrate"] = rung.bitrate;
      profile["track_inhibit"] = "video=<0x" + std::to_string(rung.shortSide);
      profile["track_retain"] = rung.retain;
      livepeer["target_profiles"].append(profile);
    }
    return livepeer;
  }

  /// MistProcLivepeer's declaration for retainLadder: it reads the transcode source, and every
  /// rendition carries its profile's inhibitor, retain selector and target bitrate.
  JSON::Value retainLadderDeclaration(const JSON::Value & livepeer) {
    JSON::Value declaration =
      JSON::fromString(R"({"select":{"codecs":[[["+H264","+HEVC","+MPEG2"],["+AAC"]]],"target":{"audio":"none"}}})");
    declaration["select"]["target"]["video"] = transcodeSource;
    jsonForEachConst (livepeer["target_profiles"], prof) {
      JSON::Value output;
      output["output"] = (*prof)["name"];
      output["type"] = "video";
      output["codec"] = "H264";
      output["mask"] = 3;
      output["track_inhibit"] = (*prof)["track_inhibit"];
      output["track_retain"] = (*prof)["track_retain"];
      output["track_retain_bitrate"] = (*prof)["bitrate"];
      declaration["outputs"].append(output);
    }
    return declaration;
  }

  /// One local AV rung of the ladder as pkg/mist ReplaceLivepeerWithLocal writes it.
  JSON::Value retainAVRung(const std::string & process, const LadderRung & rung) {
    JSON::Value av;
    av["process"] = process;
    av["codec"] = "H264";
    av["resolution"] = "x" + std::to_string(rung.shortSide);
    av["bitrate"] = rung.bitrate;
    av["track_select"] = "video=maxbps&audio=none&subtitle=none&meta=none";
    av["track_inhibit"] = "video=<0x" + std::to_string(rung.shortSide);
    av["track_retain"] = rung.retain;
    av["mask_replaced_originals"] = true;
    av["x-LSP-name"] = std::string("Local ") + rung.name;
    return av;
  }

  /// MistProcAV's declaration for retainAVRung.
  JSON::Value retainAVDeclaration() {
    JSON::Value declaration = avDeclaration("H264", transcodeSource, TRACK_VALID_ALL);
    return declaration;
  }

  /// A video track of a retention case: codec, geometry, rate in Mbit/s, and whether a process
  /// produced it (it then has the first track of the case as its source).
  struct CaseTrack {
      const char *codec;
      uint32_t width, height;
      double rate;
      bool derived;
      uint16_t rotation;
      bool mirror;
  };

  /// A stream with original (and derived) video tracks the rendition retention rule decides on,
  /// the Livepeer renditions it must produce and the originals masked once one is produced.
  struct RetainCase {
      const char *name;
      std::vector<CaseTrack> tracks;
      std::set<std::string> produced;
      std::set<size_t> masked; ///< positions in tracks
      std::string skip; ///< why the ladder does not run, when it produces nothing
  };

  std::vector<RetainCase> retainCases() {
    return {
      {"sane 1080/720/360 originals", {{"H264", 1920, 1080, 7.5}, {"H264", 1280, 720, 4}, {"H264", 640, 360, 1}}, {"480p"}, {}},
      {"720p at 15 Mbps is the source and is replaced", {{"H264", 1920, 1080, 7.5}, {"H264", 1280, 720, 15}}, {"360p", "480p", "720p"}, {1}},
      {"HEVC 720p in band is retained", {{"H264", 1920, 1080, 7.5}, {"HEVC", 1280, 720, 4}}, {"360p", "480p"}, {}},
      {"AV1 720p is not retained", {{"H264", 1920, 1080, 7.5}, {"AV1", 1280, 720, 4}}, {"360p", "480p", "720p"}, {1}},
      {"portrait originals", {{"H264", 1080, 1920, 7.5}, {"H264", 720, 1280, 4}}, {"360p", "480p"}, {}},
      {"of two 720p candidates the one closest to the rung bitrate retains it",
       {{"H264", 1920, 1080, 7.5}, {"H264", 1280, 720, 3}, {"H264", 1280, 720, 5}},
       {"360p", "480p"},
       {1}},
      {"a single in-band source fills its own rung and stays visible", {{"H264", 1920, 1080, 6}}, {"360p", "480p", "720p"}, {}},
      {"a single out-of-band source is transcoded and masked", {{"H264", 1920, 1080, 20}}, {"360p", "480p", "720p", "1080p"}, {0}},
      {"a source below every rung keeps playing", {{"H264", 426, 240, 5}}, {}, {}, "track_inhibit 'video=<0x360' matches source tracks"},
      {"an upload carrying every rung starts no ladder",
       {{"H264", 1920, 1080, 7.5}, {"H264", 1280, 720, 4}, {"H264", 854, 480, 1.6}, {"H264", 640, 360, 0.9}},
       {},
       {},
       std::string("every declared rendition is retained or inhibited (360p: track_retain '") + ladderRungs[0].retain + "' satisfied by track 3)"},
      {"too small is judged against the source only", {{"H264", 1920, 1080, 1}, {"H264", 1280, 720, 4}}, {"360p", "480p"}, {0}},
      {"image and derived tracks neither retain nor become the source",
       {{"H264", 1920, 1080, 7.5}, {"JPEG", 640, 360, 50}, {"H264", 1280, 720, 40, true}},
       {"360p", "480p", "720p"},
       {}},
      // A passthrough rendition cannot carry a display orientation (HLS-TS has none).
      {"a rotated in-band original retains nothing",
       {{"H264", 1920, 1080, 7.5}, {"H264", 1280, 720, 4, false, 90}},
       {"360p", "480p", "720p"},
       {1}},
      {"a mirrored in-band original retains nothing",
       {{"H264", 1920, 1080, 7.5}, {"H264", 1280, 720, 4, false, 0, true}},
       {"360p", "480p", "720p"},
       {1}},
    };
  }

  /// A probe stream holding the tracks of a retention case plus an AAC source, with bitrates.
  std::vector<size_t> addCaseTracks(InputBufferProbe & input, const RetainCase & rc) {
    std::vector<size_t> tracks;
    for (const CaseTrack & t : rc.tracks) {
      const size_t idx = input.addTrack("video", t.codec, t.width, t.height, t.derived ? tracks.at(0) : INVALID_TRACK_ID);
      input.meta.setBps(idx, mbit(t.rate));
      input.meta.setRotation(idx, t.rotation);
      input.meta.setMirror(idx, t.mirror);
      tracks.push_back(idx);
    }
    input.addTrack("audio", "AAC", 0, 0);
    return tracks;
  }

  std::set<std::string> graphOutputNames(const Mist::ProcessGraph & graph) {
    std::set<std::string> names;
    jsonForEachConst (graph.outputs, it) { names.insert((*it)["output"].asString()); }
    return names;
  }

  std::string joinedTracks(const std::set<size_t> & tracks) {
    std::string all;
    for (const size_t track : tracks) { all += std::to_string(track) + " "; }
    return all;
  }

  /// Rendition retention: a rung that an original track already satisfies starts no process, the
  /// size inhibitors judge only the transcode source, and once a rung is transcoded the originals
  /// that retain no rung (the source included) are masked from viewers and pushes. The supervisor
  /// asks MistProc<name> --describe-outputs for the declarations MistProcLivepeer and MistProcAV
  /// give for these configurations.
  void testRetention(Util::Config & config) {
    const std::string suffix = std::to_string(getpid());
    const std::string lpName = "RetainLP" + suffix;
    const std::string avName = "RetainAV" + suffix;
    const std::string lpPath = Util::getMyPath() + "MistProc" + lpName;
    const std::string avPath = Util::getMyPath() + "MistProc" + avName;
    auto fixture = [](const JSON::Value & declaration) {
      return "[ \"$1\" = --describe-outputs ] && { echo '" + declaration.toString() + "'; exit 0; }\nexec sleep 30";
    };
    if (!writeFixture(lpPath, fixture(retainLadderDeclaration(retainLadder(lpName)))) ||
        !writeFixture(avPath, fixture(retainAVDeclaration()))) {
      check(false, "could not write retention fixtures next to the test binary");
      return;
    }
    config.is_active = true;

    size_t caseNo = 0;
    for (const RetainCase & rc : retainCases()) {
      const std::string stream = "retain" + std::to_string(++caseNo) + "x" + suffix;
      InputBufferProbe input(&config);
      input.initMetadata(stream);
      const std::vector<size_t> tracks = addCaseTracks(input, rc);
      char statePage[NAME_BUFFER_SIZE];
      snprintf(statePage, sizeof(statePage), SHM_STREAM_STATE, stream.c_str());
      IPC::sharedPage state(statePage, STRMSTATE_PAGE_LEN, true, false);
      if (state) {
        memset(state.mapped, 0, state.len);
        state.mapped[0] = STRMSTAT_READY;
      }

      const JSON::Value livepeer = retainLadder(lpName);
      JSON::Value configured;
      configured.append(livepeer);
      input.tick(configured);
      const std::string name = std::string(rc.name) + ": ";
      check(graphOutputNames(input.processGraph) == rc.produced,
            name + "Livepeer renditions, got " + joined(graphOutputNames(input.processGraph)));
      if (rc.produced.empty()) {
        check(input.processGraph.skipReasons[input.key(livepeer)] == rc.skip,
              name + "the ladder must not run, got '" + input.processGraph.skipReasons[input.key(livepeer)] + "'");
      }
      for (const size_t track : tracks) {
        check(input.meta.trackValid(track) == TRACK_VALID_ALL, name + "no original is masked before a rendition exists");
      }

      if (rc.produced.size()) {
        // The first rendition carries data: the ladder now replaces the originals it does not retain.
        const size_t rendition = input.addTrack("video", "H264", 854, 480, tracks.at(0));
        input.meta.setOutputKey(rendition, DTSC::outputKey(DTSC::processIdentity(input.key(livepeer)), *rc.produced.begin()));
        input.tick(configured);
        std::set<size_t> masked;
        for (size_t i = 0; i < tracks.size(); ++i) {
          const uint8_t valid = input.meta.trackValid(tracks[i]);
          if (valid != TRACK_VALID_ALL) {
            masked.insert(i);
            check(valid == TRACK_VALID_INT_PROCESS, name + "a replaced original stays readable by processes only");
          }
        }
        check(masked == rc.masked, name + "masked originals, got " + joinedTracks(masked));
        check(input.meta.trackValid(rendition) == TRACK_VALID_ALL, name + "the rendition must stay visible");
      }
      input.stopAll();
      if (state) {
        state.master = true;
        state.close();
      }
    }

    {
      // The local AV ladder: one process per rung, each retained by its own original.
      const RetainCase rc = retainCases().at(5); // two 720p candidates
      const std::string stream = "retainAV" + suffix;
      InputBufferProbe input(&config);
      input.initMetadata(stream);
      const std::vector<size_t> tracks = addCaseTracks(input, rc);
      char statePage[NAME_BUFFER_SIZE];
      snprintf(statePage, sizeof(statePage), SHM_STREAM_STATE, stream.c_str());
      IPC::sharedPage state(statePage, STRMSTATE_PAGE_LEN, true, false);
      if (state) {
        memset(state.mapped, 0, state.len);
        state.mapped[0] = STRMSTAT_READY;
      }
      JSON::Value configured;
      for (const LadderRung & rung : ladderRungs) { configured.append(retainAVRung(avName, rung)); }
      input.tick(configured);
      const Mist::ProcessGraph & graph = input.processGraph;
      check(graph.runs.size() == 2 && graph.runs.count(input.key(configured[0u])) && graph.runs.count(input.key(configured[1u])),
            "local AV ladder: only the 360p and 480p rungs run, got " + std::to_string(graph.runs.size()));
      check(graph.skipReasons.count(input.key(configured[2u])) &&
              graph.skipReasons.at(input.key(configured[2u])) ==
                std::string("track_retain '") + ladderRungs[2].retain + "' satisfied by track " + std::to_string(tracks[2]),
            "local AV ladder: the 720p rung is satisfied by the 5 Mbps original, got '" +
              (graph.skipReasons.count(input.key(configured[2u])) ? graph.skipReasons.at(input.key(configured[2u])) : "") + "'");
      check(graph.skipReasons.count(input.key(configured[3u])) &&
              graph.skipReasons.at(input.key(configured[3u])) ==
                std::string("track_retain '") + ladderRungs[3].retain + "' satisfied by track " + std::to_string(tracks[0]),
            "local AV ladder: the 1080p rung is satisfied by the source");
      check(graph.outputs.size() == 2, "local AV ladder: retained rungs add no expected outputs");

      // The 480p rung produces: the 3 Mbps 720p original is replaced, the retaining originals are not.
      const size_t rendition = input.addTrack("video", "H264", 854, 480, tracks.at(0));
      input.meta.setOutputKey(rendition, DTSC::outputKey(DTSC::processIdentity(input.key(configured[1u])), "video"));
      input.tick(configured);
      check(input.meta.trackValid(tracks[0]) == TRACK_VALID_ALL && input.meta.trackValid(tracks[2]) == TRACK_VALID_ALL,
            "local AV ladder: originals that retain another process's rung stay visible");
      check(input.meta.trackValid(tracks[1]) == TRACK_VALID_INT_PROCESS, "local AV ladder: the unretained original is masked");
      input.stopAll();
      if (state) {
        state.master = true;
        state.close();
      }
    }

    {
      // A processing stream reserves only the Livepeer renditions that are produced.
      const RetainCase rc = retainCases().at(0);
      InputBufferProbe input(&config);
      input.initMetadata("retainReserve" + suffix);
      addCaseTracks(input, rc);
      JSON::Value livepeer = retainLadder("Livepeer");
      input.processGraphNodes[input.key(livepeer)] = graphNode(input.streamName, livepeer, retainLadderDeclaration(livepeer));
      input.reserveProcessOutputs(input.key(livepeer), livepeer);
      std::set<std::string> reserved;
      for (const size_t track : input.M.getReservedTracks()) { reserved.insert(input.M.getOutputKey(track)); }
      const std::set<std::string> want = {DTSC::outputKey(DTSC::processIdentity(input.key(livepeer)), "480p")};
      check(reserved == want, "reservations of the sane multi-rendition upload, got " + joined(reserved));
    }

    {
      // The ladder is decided once every original video track reports its geometry and bitrate,
      // or once the decision window passed: an original whose bitrate is still unknown then
      // retains nothing, and its rung is transcoded rather than left without a rendition.
      InputBufferProbe input(&config);
      const std::string stream = "retainPending" + suffix;
      input.initMetadata(stream);
      const size_t source = input.addTrack("video", "H264", 1920, 1080);
      input.addTrack("video", "H264", 1280, 720);
      input.meta.setBps(source, mbit(7.5));
      input.addTrack("audio", "AAC", 0, 0);
      const JSON::Value livepeer = retainLadder(lpName);
      JSON::Value configured;
      configured.append(livepeer);
      const Mist::ProcessGraph *graph = &input.resolveProcessGraph(configured);
      check(!graph->runs.count(input.key(livepeer)), "the ladder waits briefly for an original's bitrate");
      // Process-exit signals must not shorten the real rendition-decision window.
      retentionWaitSignals = 0;
      const auto previous = std::signal(SIGUSR1, interruptRetentionWait);
      check(previous != SIG_ERR, "retention wait must install its signal fixture");
      if (previous != SIG_ERR) {
        const pthread_t waitingThread = pthread_self();
        std::atomic<bool> done(false);
        std::thread interruptions([&]() {
          while (!done.load()) {
            Util::wait(20);
            if (!done.load()) { pthread_kill(waitingThread, SIGUSR1); }
          }
        });
        const uint64_t started = Util::bootMS();
        Util::wait(5200);
        const uint64_t elapsed = Util::bootMS() - started;
        done.store(true);
        interruptions.join();
        std::signal(SIGUSR1, previous);
        check(retentionWaitSignals > 0 && elapsed >= Mist::RENDITION_DECISION_WAIT_MS,
              "retention wait must span the unchanged decision window despite signals");
      } else {
        Util::wait(5200);
      }
      graph = &input.resolveProcessGraph(configured);
      check(graph->runs.count(input.key(livepeer)) && graphOutputNames(*graph) == std::set<std::string>({"360p", "480p", "720p"}),
            "an original without a bitrate must not retain its rung once the window passed, got " +
              joined(graphOutputNames(*graph)));
    }

    {
      // Retention, inhibitors and masks are decided once for the stream's original tracks. Bitrate
      // and geometry drifting afterwards change nothing; a new original track decides again.
      InputBufferProbe input(&config);
      const std::string stream = "retainLatch" + suffix;
      input.initMetadata(stream);
      const size_t source = input.addTrack("video", "H264", 1920, 1080);
      const size_t rung = input.addTrack("video", "H264", 1280, 720);
      input.meta.setBps(source, mbit(7.5));
      input.meta.setBps(rung, mbit(4));
      input.addTrack("audio", "AAC", 0, 0);
      char statePage[NAME_BUFFER_SIZE];
      snprintf(statePage, sizeof(statePage), SHM_STREAM_STATE, stream.c_str());
      IPC::sharedPage state(statePage, STRMSTATE_PAGE_LEN, true, false);
      if (state) {
        memset(state.mapped, 0, state.len);
        state.mapped[0] = STRMSTAT_READY;
      }
      const JSON::Value livepeer = retainLadder(lpName);
      JSON::Value configured;
      configured.append(livepeer);
      input.tick(configured);
      check(graphOutputNames(input.processGraph) == std::set<std::string>({"360p", "480p"}),
            "latch: decided ladder, got " + joined(graphOutputNames(input.processGraph)));
      const size_t rendition = input.addTrack("video", "H264", 854, 480, source);
      input.meta.setOutputKey(rendition, DTSC::outputKey(DTSC::processIdentity(input.key(livepeer)), "480p"));
      input.tick(configured);
      check(input.meta.trackValid(rung) == TRACK_VALID_ALL && input.meta.trackValid(source) == TRACK_VALID_ALL,
            "latch: retaining originals stay visible");

      // The 720p original drifts out of its band, then the source's geometry changes.
      input.meta.setBps(rung, mbit(15));
      input.tick(configured);
      check(input.processGraph.runs.count(input.key(livepeer)) &&
              graphOutputNames(input.processGraph) == std::set<std::string>({"360p", "480p"}),
            "latch: a drifting bitrate must not change the decided ladder, got " + joined(graphOutputNames(input.processGraph)));
      check(input.meta.trackValid(rung) == TRACK_VALID_ALL, "latch: a drifting bitrate must not mask a retaining original");
      input.meta.setWidth(source, 1280);
      input.meta.setHeight(source, 720);
      input.tick(configured);
      check(graphOutputNames(input.processGraph) == std::set<std::string>({"360p", "480p"}),
            "latch: a changing geometry must not change the decided ladder, got " + joined(graphOutputNames(input.processGraph)));

      // A new original track decides again, on what the tracks report now.
      const size_t small = input.addTrack("video", "H264", 640, 360);
      input.meta.setBps(small, mbit(0.9));
      input.tick(configured);
      check(graphOutputNames(input.processGraph) == std::set<std::string>({"480p"}),
            "latch: a new original track decides again, got " + joined(graphOutputNames(input.processGraph)));
      check(input.meta.trackValid(rung) == TRACK_VALID_INT_PROCESS,
            "latch: the 15 Mbps original that now retains nothing is masked by the new decision");
      input.stopAll();
      if (state) {
        state.master = true;
        state.close();
      }
    }

    {
      // A recording writes its header only from a graph that made every rendition decision and was
      // resolved for every original track it records (a Matroska source registers its tracks as
      // their first frames arrive).
      InputBufferProbe input(&config);
      const std::string stream = "retainPublish" + suffix;
      input.initMetadata(stream);
      const size_t audio = input.addTrack("audio", "AAC", 0, 0);
      const JSON::Value livepeer = retainLadder(lpName);
      JSON::Value configured;
      configured.append(livepeer);
      input.publishProcessGraph(input.resolveProcessGraph(configured), Mist::tracksWithDataAnyMask(input.meta));
      JSON::Value published;
      check(Mist::readProcessGraphPage(input.processGraphPage, published), "publish: the graph page must be readable");
      check(Mist::processGraphCoversOriginal(published, audio), "publish: the graph lists the original it was resolved for");
      const size_t video = input.addTrack("video", "H264", 960, 540);
      check(!Mist::processGraphCoversOriginal(published, video), "publish: a later original is not covered by an earlier graph");
      input.publishProcessGraph(input.resolveProcessGraph(configured), Mist::tracksWithDataAnyMask(input.meta));
      check(Mist::readProcessGraphPage(input.processGraphPage, published) && !published["resolved"].asBool(),
            "publish: a graph whose ladder still waits for the new original's bitrate is not resolved");
      input.meta.setBps(video, mbit(2));
      input.publishProcessGraph(input.resolveProcessGraph(configured), Mist::tracksWithDataAnyMask(input.meta));
      check(Mist::readProcessGraphPage(input.processGraphPage, published) && published["resolved"].asBool() &&
              Mist::processGraphCoversOriginal(published, video) && published["outputs"].size() == 2,
            "publish: once decided the graph is resolved for the new original with its renditions, got " + published.toString());
      input.processGraphPage.master = true;
    }

    {
      // A header-only original can gain its first packet before the graph's next publication.
      InputBufferProbe input(&config);
      input.initMetadata("retainFirstPacket" + suffix);
      input.addTrack("audio", "AAC", 0, 0);
      const size_t video = input.meta.addTrack();
      input.meta.setID(video, video + 1);
      input.meta.setType(video, "video");
      input.meta.setCodec(video, "H264");
      input.meta.setWidth(video, 960);
      input.meta.setHeight(video, 540);
      input.meta.setBps(video, mbit(2));
      input.meta.validateTrack(video, TRACK_VALID_ALL);
      input.meta.breakClaim(video);
      JSON::Value configured;
      configured.append(retainLadder(lpName));
      input.publishProcessGraph(input.resolveProcessGraph(configured), Mist::tracksWithDataAnyMask(input.meta));
      JSON::Value published;
      check(Mist::readProcessGraphPage(input.processGraphPage, published), "first-packet graph must be readable");
      input.meta.update(0, 0, video, 1, 0, true, 1);
      input.meta.setBps(video, mbit(2));
      check(input.meta.getValidTracks(true).count(video), "first-packet fixture video must carry data");
      std::map<std::string, std::string> everything;
      everything["video"] = "all";
      everything["audio"] = "all";
      JSON::Value capa;
      capa["codecs"][0u][0u].append("*");
      size_t selected = 0, ready = 0;
      Mist::recordingExpectedOutputs(input.M, Mist::tracksWithDataAnyMask(input.M), published["outputs"], everything,
                                     capa, "", selected, ready);
      const bool admitted = published["resolved"].asBool() && Mist::processGraphCoversOriginal(published, video) &&
        Mist::processingRecordingTrackCountsReady(true, selected, ready, 2, 2, 0, 0);
      check(!admitted, "a graph sampled before the video first packet must not admit a header without its ladder");
      input.publishProcessGraph(input.resolveProcessGraph(configured), Mist::tracksWithDataAnyMask(input.meta));
      check(Mist::readProcessGraphPage(input.processGraphPage, published) && published["resolved"].asBool() &&
              Mist::processGraphCoversOriginal(published, video) && published["outputs"].size() == 2,
            "a refreshed first-packet graph must expect both video renditions");
      Mist::recordingExpectedOutputs(input.M, Mist::tracksWithDataAnyMask(input.M), published["outputs"], everything,
                                     capa, "", selected, ready);
      check(selected == 2 && !ready && !Mist::processingRecordingTrackCountsReady(true, selected, ready, 2, 2, 0, 0),
            "a refreshed graph must hold the header until its renditions have data");
      for (size_t index = 0; index < published["outputs"].size(); ++index) {
        const size_t output = input.addTrack("video", "H264", index ? 854 : 640, index ? 480 : 360, video);
        input.meta.setOutputKey(output, published["outputs"][(unsigned int)index]["key"].asString());
      }
      Mist::recordingExpectedOutputs(input.M, Mist::tracksWithDataAnyMask(input.M), published["outputs"], everything,
                                     capa, "", selected, ready);
      check(selected == 2 && ready == 2 && Mist::processingRecordingTrackCountsReady(true, selected, ready, 2, 2, 2, 2),
            "the header must unblock once both expected renditions carry data");
      input.processGraphPage.master = true;
    }

    unlink(lpPath.c_str());
    unlink(avPath.c_str());
  }

  void testInhibitSeesNewSourceAtGraphSelection(Util::Config & config) {
    InputBufferProbe input(&config);
    input.initMetadata("graph-inhibit-arrival-test");
    input.addTrack("video", "H264", 1920, 1080);
    const JSON::Value process = JSON::fromString(
      R"({"process":"AV","codec":"AAC","track_select":"audio=all&video=none&subtitle=none&meta=none","track_inhibit":"audio=aac"})");
    const JSON::Value declaration = JSON::fromString(
      R"({"select":{"codecs":[[["AAC","opus","AC3"]]],"target":{"audio":"all","video":"none","subtitle":"none","meta":"none"}},"outputs":[{"output":"audio","type":"audio","codec":"AAC","rate":48000,"channels":2,"mask":3}]})");
    Mist::ProcessGraphNode node = graphNode("graph-inhibit-arrival-test", process, declaration);
    node.inhibited = Mist::processInhibitReason(node.proc, input.M, {}, declaration);
    check(node.inhibited.empty(), "no audio source has arrived when the supervisor samples the inhibitor");
    check(!Mist::buildProcessGraph(input.M, {node}).runs.count(node.config), "an audio encoder must wait while no audio input exists");
    // Source metadata is shared: the first AAC packet can land before candidate selection.
    input.addTrack("audio", "AAC", 0, 0);
    const Mist::ProcessGraph graph = Mist::buildProcessGraph(input.M, {node});
    check(!graph.runs.count(node.config), "AAC arriving during graph construction must inhibit its redundant encoder");
    check(graph.skipReasons.count(node.config) && graph.skipReasons.at(node.config).find("track_inhibit") != std::string::npos,
          "the graph must report the newly matched source inhibitor");
    check(!graph.outputs.size(), "a newly inhibited encoder must declare no recording output");
    InputBufferProbe ac3(&config);
    ac3.initMetadata("graph-inhibit-ac3-arrival-test");
    ac3.addTrack("audio", "AC3", 0, 0);
    const Mist::ProcessGraph required = Mist::buildProcessGraph(ac3.M, {node});
    check(required.runs.count(node.config) && required.outputs.size() == 1, "new AC3 audio must still start its required AAC encoder");
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

      const std::set<size_t> all = M.getValidTracks(true);
      check(!Util::inhibitorMatchesSource(M, inhibit360, video), "720p source must keep the 360p rendition");
      check(!Util::inhibitorMatchesSource(M, inhibit480, all), "720p source must keep the 480p rendition");
      check(!Util::inhibitorMatchesSource(M, inhibit720, video), "720p source must keep the 720p rendition");
      check(Util::inhibitorMatchesSource(M, inhibit1080, video), "720p source must drop the 1080p rendition");
      check(!Util::inhibitorMatchesSource(M, "audio=opus", all), "opus produced by another process must not inhibit");
      check(Util::inhibitorMatchesSource(M, "audio=aac", audio), "an AAC source must inhibit the AAC transcode");
      check(!Util::inhibitorMatchesSource(M, "audio=aac", video), "an inhibitor judges only the tracks the process reads");
      check(!Util::inhibitorMatchesSource(M, "subtitle=all", all), "no subtitle source means no subtitle inhibit");
      // The plain selector still sees the previous-run 640x360 rendition; only the source filter ignores it.
      check(Util::wouldSelect(M, std::string("audio=none&video=none&subtitle=none&meta=none&") + inhibit480).size() != 0,
            "fixture sanity: the previous-run rendition must be selectable by the 480p inhibitor");

      JSON::Value opusTranscode;
      opusTranscode["process"] = "AV";
      opusTranscode["codec"] = "opus";
      opusTranscode["track_inhibit"] = "audio=opus";
      check(Mist::processInhibitReason(opusTranscode, M, std::set<std::string>()).empty(),
            "another producer's opus track must not inhibit the opus transcode");

      std::vector<Mist::ProcessGraphNode> nodes;
      nodes.push_back(graphNode("inhibit-720-test", livepeerWithLadder(), livepeerDeclaration(livepeerWithLadder())));
      check(Mist::buildProcessGraph(M, nodes).outputs.size() == 3 && derived,
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
      check(!Util::inhibitorMatchesSource(M, inhibit360, video) && !Util::inhibitorMatchesSource(M, inhibit480, video) &&
              !Util::inhibitorMatchesSource(M, inhibit720, video) && !Util::inhibitorMatchesSource(M, inhibit1080, video),
            "1080p source must keep all four renditions");
      std::vector<Mist::ProcessGraphNode> nodes;
      nodes.push_back(graphNode("inhibit-1080-test", livepeerWithLadder(), livepeerDeclaration(livepeerWithLadder())));
      check(Mist::buildProcessGraph(M, nodes).outputs.size() == 4 && derived, "1080p source must expect all four renditions");
    }

    // An opus source does inhibit the opus transcode, in readiness and in the supervisor.
    {
      InputBufferProbe input(&config);
      input.initMetadata("inhibit-opus-source-test");
      input.addTrack("video", "H264", 1280, 720);
      input.addTrack("audio", "opus", 0, 0);
      check(Util::inhibitorMatchesSource(input.meta, "audio=opus", input.meta.getValidTracks(true)),
            "an opus source must inhibit the opus transcode");
      JSON::Value opusTranscode;
      opusTranscode["process"] = "AV";
      opusTranscode["codec"] = "opus";
      opusTranscode["track_inhibit"] = "audio=opus";
      check(Mist::processInhibitReason(opusTranscode, input.meta, std::set<std::string>()).size(),
            "an opus source must inhibit the opus transcode");
    }
  }

  /// Runs rate-controller ticks with the given procs registered as running
  /// and returns the effective speed after each tick. The node pressure the
  /// controller sees comes from a page this test owns, republished fresh with
  /// cpuSome before every tick, never from the host's Controller-written page.
  std::vector<uint64_t> rampSpeeds(Util::Config & config, const std::string & name, const JSON::Value & proc, double cpuSome = 0) {
    std::vector<uint64_t> speeds;
    const std::string pressureName = "/MstNodePressureTest_" + std::to_string(getpid());
    IPC::sharedPage pressurePage(pressureName, sizeof(NodePressureState), true, false);
    if (!pressurePage.mapped) {
      check(false, "could not create the test's node pressure page");
      return speeds;
    }
    NodePressureState::initPage(pressurePage);
    NodePressureState *pressure = (NodePressureState *)pressurePage.mapped;
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
      input.nodePressurePage = pressureName;
      input.runningProcs[proc.toString()] = child;
      for (int i = 0; i < 12; ++i) {
        pressure->beginPublish();
        pressure->lastUpdateMs = Util::bootMS();
        pressure->flags = NODE_PRESSURE_HAS_PSI;
        pressure->cpuSomeQ0_16 = (uint16_t)(cpuSome * 65535.0);
        pressure->endPublish();
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

    // The same unconstrained feed stays at 1x while the node reports CPU pressure.
    speeds = rampSpeeds(config, "unconstrained-pressure-test", thumbs, 0.15);
    check(speeds.size() == 12 && speeds.back() == 1,
          "an unconstrained feed under node CPU pressure must hold 1x, got " +
            (speeds.size() ? std::to_string(speeds.back()) : std::string("no ticks")));

    // A consequential proc that has not published its contract still holds 1x.
    JSON::Value transcode;
    transcode["process"] = "AV";
    speeds = rampSpeeds(config, "constrained-hold-test", transcode);
    check(speeds.size() == 12 && speeds.back() == 1, "a consequential proc without a contract must hold 1x");
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

    // A busy admission response must be retried without starting the failed process again.
    {
      config.is_active = true;
      InputBufferProbe waiting(&config);
      waiting.initMetadata(stream);
      waiting.addTrack("video", "H264", 1280, 720);
      waiting.replaceResponse = R"({"wait_reason":"memory","retry_after_ms":100})";
      const uint64_t waitDeadline = Util::bootMS() + 5000;
      while (Util::bootMS() < waitDeadline && waiting.replacePayloads.empty()) {
        waiting.tick(configured);
        Util::sleep(20);
      }
      check(waiting.replacePayloads.size() == 1, "initial failure must ask for a replacement");
      JSON::Value ready;
      ready.append(survivor);
      waiting.replaceResponse = ready.toString();
      // A node-capacity event wakes admission before a long configured retry delay.
      waiting.processingCapacityPageName = "/MstCapacityUnit" + suffix;
      IPC::sharedPage capacity(waiting.processingCapacityPageName, 8, true, false);
      check(capacity && capacity.len >= 8, "capacity event page must exist");
      if (capacity && capacity.len >= 8) {
        uint64_t generation = 1;
        memcpy(capacity.mapped, &generation, sizeof(generation));
        waiting.deferredProcessReplacements[failedKey].retryAt = Util::bootMS() + 60000;
        check(waiting.wakeDeferredProcessReplacements(), "released capacity must wake a queued replacement");
        check(!waiting.wakeDeferredProcessReplacements(), "a consumed capacity event must not retrigger admission");
      }
      const uint64_t releaseDeadline = Util::bootMS() + 3000;
      while (Util::bootMS() < releaseDeadline && !waiting.runningProcs.count(survivorKey)) {
        waiting.tick(configured);
        Util::sleep(20);
      }
      check(waiting.runningProcs.count(survivorKey) && Util::Procs::isActive(waiting.runningProcs[survivorKey]),
            "a deferred live ladder must start when admission becomes available");
      check(waiting.replacePayloads.size() == 2, "deferred admission must retry exactly once after release");
      check(!waiting.runningProcs.count(failedKey), "admission retry must not restart the failed process");
      waiting.stopAll();
      waiting.deferredProcessReplacements[failedKey] = {"obsolete", "memory", 0};
      JSON::Value removed;
      removed.append(JSON::Value());
      removed.shrink(0);
      const size_t attempts = waiting.replacePayloads.size();
      waiting.tick(removed);
      check(waiting.deferredProcessReplacements.empty(), "removing a config must cancel its queued replacement");
      check(waiting.replacePayloads.size() == attempts, "a removed config must not request admission");
    }

    input.stopAll();
    state.master = true;
    state.close();
    unlink(failPath.c_str());
    unlink(sleepPath.c_str());
  }

  // A killed process's disconnect can be handled after its restarted process claimed the track
  // again: only the disconnected process's own claim is broken.
  void testDisconnectKeepsReplacementClaim(Util::Config & config) {
    InputBufferProbe input(&config);
    const std::string stream = "claimUnit" + std::to_string(getpid());
    input.initMetadata(stream);
    const size_t track = input.addTrack("video", "H264", 320, 180);
    input.users.reload(stream, true);
    if (!input.users) {
      check(false, "could not create the stream's user page");
      return;
    }
    const uint32_t killedPid = 0x7ffffff0;
    const uint32_t restartedPid = 0x7ffffff1;
    input.users.setPid(killedPid, 0);

    input.processUsers[0] = track;
    input.meta.trackList.setInt(input.meta.trackPidField, restartedPid, track);
    input.userOnDisconnect(0);
    check(input.M.isClaimedBy(track) == restartedPid, "the late disconnect of a killed process broke its restarted process's claim");

    input.processUsers[0] = track;
    input.meta.trackList.setInt(input.meta.trackPidField, killedPid, track);
    input.userOnDisconnect(0);
    check(!input.M.isClaimed(track), "the disconnect of the process that claimed the track kept its claim");
  }
} // namespace

int main() {
  Util::Config config("input-buffer-process-unit");
  {
    InputBufferProbe input(&config);
    const std::string page = "/mist_probe_admission_" + std::to_string(getpid());
    input.streamStatus.init(page, STRMSTATE_PAGE_LEN, true, false);
    check(bool(input.streamStatus), "admission fixture owns its status page");
    if (input.streamStatus) {
      memset(input.streamStatus.mapped, 0, input.streamStatus.len);
      input.processControlledRealtime = true;
      input.processOverrideResolved = false;
      input.publishFeedPaused();
      check(input.streamStatus.mapped[STRMSTATE_PROCESS_FEED_PAUSED_OFFSET], "source feed must pause before the first admission response");
      input.processOverrideResolved = true;
      input.publishFeedPaused();
      check(!input.streamStatus.mapped[STRMSTATE_PROCESS_FEED_PAUSED_OFFSET], "resolved admission without a probe releases the feed");
      input.processOverrideResolved = false;
      input.processControlledRealtime = false;
      input.publishFeedPaused();
      check(!input.streamStatus.mapped[STRMSTATE_PROCESS_FEED_PAUSED_OFFSET], "ordinary streams do not wait for processing admission");
    }
  }
  testInhibitSeesNewSourceAtGraphSelection(config);
  testInhibitors(config);
  testGraph(config);
  testGraphSupervisor(config);
  testRetention(config);
  testUnconstrainedRamp(config);
  testSupervisor(config);
  testDisconnectKeepsReplacementClaim(config);
  if (failures) {
    std::cerr << failures << " failure(s)" << std::endl;
    return 1;
  }
  return 0;
}
