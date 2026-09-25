#define private public
#define protected public
#include "../src/input/input_buffer.h"
#undef protected
#undef private
#include "../src/input/processing_rate.h"

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

  void testSupervisor(Util::Config & config) {
    const std::string suffix = std::to_string(getpid());
    const std::string failName = "UnitReplaceFail" + suffix;
    const std::string sleepName = "UnitReplaceSleep" + suffix;
    const std::string failPath = Util::getMyPath() + "MistProc" + failName;
    const std::string sleepPath = Util::getMyPath() + "MistProc" + sleepName;
    if (!writeFixture(failPath, "exit 2") || !writeFixture(sleepPath, "exec sleep 30")) {
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
  testUnconstrainedRamp(config);
  testSupervisor(config);
  if (failures) {
    std::cerr << failures << " failure(s)" << std::endl;
    return 1;
  }
  return 0;
}
