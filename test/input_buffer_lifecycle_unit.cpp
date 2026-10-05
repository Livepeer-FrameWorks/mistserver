#define private public
#define protected public
#include "../src/input/input_buffer.h"
#undef protected
#undef private

#include <cassert>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

namespace {
  int fail(const char *message) {
    fprintf(stderr, "%s\n", message);
    return 1;
  }

  class InputBufferProbe : public Mist::InputBuffer {
    public:
      explicit InputBufferProbe(Util::Config *config) : Mist::InputBuffer(config) {}

      bool openStatePage(const char *name) {
        streamStatus.init(name, STRMSTATE_PAGE_LEN, true, false);
        if (!streamStatus) { return false; }
        memset(streamStatus.mapped, 0, streamStatus.len);
        return true;
      }

      void reset(bool processControlled, bool resume, bool hadPush, bool pushing, size_t consumers) {
        config->is_active = true;
        processControlledRealtime = processControlled;
        resumeMode = resume;
        everHadPush = hadPush;
        hasPush = pushing;
        allProcsRunning = true;
        drainConsumerUsers = consumers;
        processUsers.clear();
        runningProcs.clear();
        streamStatus.mapped[0] = STRMSTAT_WAIT;
      }

      uint8_t tick() {
        userLeadOut();
        return streamStatus.mapped[0];
      }

      bool active() const { return config->is_active; }

      void initMetadata(const std::string & name) {
        streamName = name;
        meta.reInit("", true);
        const size_t source = meta.addTrack();
        meta.setID(source, 1);
        meta.setType(source, "video");
        meta.setCodec(source, "H264");
      }

      void initPageFixture(const std::string & name) {
        streamName = name;
        meta.reInit(name, true);
        const size_t source = meta.addTrack();
        meta.setID(source, 1);
        meta.setType(source, "video");
        meta.setCodec(source, "H264");
        bufferTime = 50000;
        idleTime = 60000;
        cutTime = 0;
        config->is_active = true;
        meta.setLive(true);
        meta.markUpdated(0);
      }

      bool beginPage(uint32_t firstKey, IPC::sharedPage & page) {
        Util::RelAccX & pages = meta.pages(0);
        const uint64_t index = pages.getEndPos();
        pages.setInt("firstkey", firstKey, index);
        pages.setInt("size", 4096, index);
        pages.setInt("keycount", 0, index);
        pages.setInt("avail", 0, index);
        pages.addRecords(1);
        const bool started = bufferStart(0, firstKey, page, meta);
        page.master = true;
        return started;
      }

      void publishFirstKey() {
        meta.pages(0).setInt("keycount", 1, 0);
        meta.pages(0).setInt("avail", 1, 0);
        meta.update(0, 0, 0, 1, 0, true, 1);
      }

      void retireFirstKey() { meta.keys(0).deleteRecords(1); }

      void evictUnused() { removeUnused(); }

      size_t cachedPages() const { return M.pages(0).getPresent(); }

      size_t expected(const JSON::Value & processes, bool & resolved) const {
        return expectedProcessingOutputTracks(processes, resolved);
      }

      bool mayMatchTranscode(const JSON::Value & process, const JSON::Value & processes) const {
        return processingProcessMayMatchTranscodeOutput(process, processes);
      }

      bool matchesSource(const JSON::Value & process) const { return processingProcessMatchesSource(process); }

      void bindRunning(const JSON::Value & process, pid_t pid) {
        JSON::Value keyed = process;
        keyed["source"] = streamName;
        runningProcs[keyed.toString()] = pid;
      }

      size_t reconcileProcesses(const JSON::Value & processes) {
        checkProcesses(processes);
        return runningProcs.size();
      }

      void clearRunningFixtures() { runningProcs.clear(); }

      void retireHard(const JSON::Value & process) {
        JSON::Value keyed = process;
        keyed["source"] = streamName;
        procHardFailed.insert(keyed.toString());
      }

      void retireDisabled(const JSON::Value & process) {
        JSON::Value keyed = process;
        keyed["source"] = streamName;
        procBoots[keyed.toString()] = 1;
      }

      void publishExpected(const JSON::Value & processes) { publishProcessingOutputExpectation(processes); }

      void addCompletedOutput(uint8_t mask = TRACK_VALID_ALL) {
        const size_t track = meta.addTrack();
        meta.setID(track, track + 1);
        meta.setType(track, "video");
        meta.setCodec(track, "JPEG");
        meta.setSourceTrack(track, 0);
        meta.validateTrack(track, mask);
        meta.update(0, 0, track, 1, 0, true, 1);
        meta.breakClaim(track);
      }

      bool expectationResolved() const { return streamStatus.mapped[STRMSTATE_PROCESS_OUTPUTS_RESOLVED_OFFSET] != 0; }

      bool feedPaused() const { return streamStatus.mapped[STRMSTATE_PROCESS_FEED_PAUSED_OFFSET] != 0; }

      uint16_t publishedExpected() const {
        uint16_t value = 0;
        memcpy(&value, streamStatus.mapped + STRMSTATE_PROCESS_OUTPUTS_EXPECTED_OFFSET, sizeof(value));
        return value;
      }
  };
} // namespace

int main() {
  {
    Util::Config pageConfig("input-buffer-page-publication");
    InputBufferProbe pageInput(&pageConfig);
    pageInput.initPageFixture("pagepub" + std::to_string(getpid()));
    IPC::sharedPage firstPage;
    if (!pageInput.beginPage(0, firstPage)) { return fail("could not start first-page publication fixture"); }
    pageInput.evictUnused();
    if (!firstPage.exists() || pageInput.cachedPages() != 1) {
      return fail("buffer eviction deleted the publisher's page before its first key was published");
    }
    pageInput.publishFirstKey();
    IPC::sharedPage nextPage;
    if (!pageInput.beginPage(1, nextPage)) { return fail("could not start next-page publication fixture"); }
    pageInput.retireFirstKey();
    pageInput.evictUnused();
    if (firstPage.exists()) { return fail("buffer eviction must still remove a page whose keys expired"); }
    if (!nextPage.exists() || pageInput.cachedPages() != 1) {
      return fail("retiring the previous page erased the next publisher page before its first key was published");
    }
  }

  char pageName[NAME_BUFFER_SIZE];
  snprintf(pageName, sizeof(pageName), "/MstBufferLifecycleTest_%d", getpid());

  Util::Config config("input-buffer-lifecycle-unit");
  InputBufferProbe input(&config);
  if (!input.openStatePage(pageName)) { return fail("could not create stream-state test page"); }

  input.reset(true, false, false, false, 0);
  if (input.tick() != STRMSTAT_WAIT || !input.active()) {
    return fail("a process-controlled stream must not drain before its first producer");
  }

  input.reset(true, false, true, true, 0);
  if (input.tick() != STRMSTAT_READY || !input.active()) { return fail("an active producer must publish READY"); }

  input.reset(true, false, true, false, 1);
  if (input.tick() != STRMSTAT_WAIT || !input.active()) {
    return fail("producer EOF must wait while a processing consumer is active");
  }

  input.drainConsumerUsers = 0;
  if (input.tick() != STRMSTAT_SHUTDOWN || !input.active()) {
    return fail("the last processing consumer must transition the stream to drain without deactivating it");
  }
  if (input.tick() != STRMSTAT_SHUTDOWN || !input.active()) {
    return fail("the process-controlled drain state must be sticky");
  }

  input.reset(true, true, true, false, 0);
  if (input.tick() != STRMSTAT_SHUTDOWN || !input.active()) {
    return fail("resume-enabled process feeders must still signal final drain");
  }

  input.reset(false, true, true, false, 0);
  if (input.tick() != STRMSTAT_WAIT || !input.active()) {
    return fail("ordinary resume-enabled streams must wait for another producer");
  }

  input.reset(false, false, true, false, 0);
  if (input.tick() != STRMSTAT_SHUTDOWN || input.active()) {
    return fail("ordinary non-resumable producer EOF must stop the input");
  }

  input.initMetadata("processing-expectation-test");
  input.reset(true, false, true, true, 0);
  JSON::Value av;
  av["process"] = "AV";
  JSON::Value remoteAv = av;
  remoteAv["sink"] = "another-stream";
  JSON::Value ffmpeg;
  ffmpeg["process"] = "FFmpeg";
  JSON::Value thumbs;
  thumbs["process"] = "Thumbs";
  JSON::Value livepeer;
  livepeer["process"] = "Livepeer";
  JSON::Value profile;
  profile["name"] = "one";
  livepeer["target_profiles"].append(profile);
  profile["name"] = "two";
  livepeer["target_profiles"].append(profile);
  JSON::Value onnx;
  onnx["process"] = "ONNX";
  onnx["annotated_video"] = true;
  bool resolved = true;

  JSON::Value recordingInvisible;
  JSON::Value viewerOnlyAv = av;
  viewerOnlyAv["target_mask"] = TRACK_VALID_EXT_HUMAN;
  recordingInvisible.append(viewerOnlyAv);
  JSON::Value processOnlyOnnx = onnx;
  processOnlyOnnx["target_mask"] = TRACK_VALID_INT_PROCESS;
  recordingInvisible.append(processOnlyOnnx);
  JSON::Value rawAv = av;
  rawAv["codec"] = "I420";
  recordingInvisible.append(rawAv);
  if (input.expected(recordingInvisible, resolved) != 0 || !resolved) {
    return fail("recording readiness must ignore viewer-only and processing-only derived tracks");
  }
  JSON::Value viewerOnlyThumbs = thumbs;
  viewerOnlyThumbs["target_mask"] = TRACK_VALID_EXT_HUMAN;
  recordingInvisible.append(viewerOnlyThumbs);
  if (input.expected(recordingInvisible, resolved) != 0 || !resolved) {
    return fail("viewer-only thumbnails must not gate recordings");
  }
  JSON::Value thumbnailOnly;
  thumbnailOnly.append(thumbs);
  if (input.expected(thumbnailOnly, resolved) != 3 || !resolved) {
    return fail("recordings must await both JPEG tracks and the VTT track");
  }

  JSON::Value rawIntermediate = av;
  rawIntermediate["codec"] = "NV12";
  rawIntermediate["x-LSP-kind"] = "video";
  rawIntermediate["target_mask"] = TRACK_VALID_INT_PROCESS;
  JSON::Value downstreamOnnx = onnx;
  downstreamOnnx["track_select"] = "video=NV12&audio=none";
  downstreamOnnx["target_mask"] = TRACK_VALID_EXT_PUSH;
  JSON::Value processingChain;
  processingChain.append(rawIntermediate);
  processingChain.append(downstreamOnnx);
  if (!input.matchesSource(rawIntermediate)) {
    return fail("the AV intermediate fixture does not match the original H264 source");
  }
  if (!input.mayMatchTranscode(downstreamOnnx, processingChain) || input.expected(processingChain, resolved) != 0 || resolved) {
    return fail(
      "a downstream ONNX selector must remain expected while its configured AV intermediate is still pending");
  }
  input.publishExpected(processingChain);
  if (input.expectationResolved() || input.feedPaused()) {
    return fail("the feed must not pause before any process output exists: only a lagging recorder holds it");
  }
  JSON::Value impossibleOnnx = downstreamOnnx;
  impossibleOnnx["track_select"] = "video=VP9&audio=none";
  JSON::Value impossibleChain;
  impossibleChain.append(rawIntermediate);
  impossibleChain.append(impossibleOnnx);
  if (input.mayMatchTranscode(impossibleOnnx, impossibleChain) || input.expected(impossibleChain, resolved) != 0 || !resolved) {
    return fail(
      "an ONNX selector that no source or configured transcode can satisfy must not wedge recording readiness");
  }

  JSON::Value pushVisible;
  JSON::Value pushOnlyAv = av;
  pushOnlyAv["target_mask"] = TRACK_VALID_EXT_PUSH;
  pushVisible.append(pushOnlyAv);
  JSON::Value processingAndPushAv = av;
  processingAndPushAv["target_mask"] = TRACK_VALID_INT_PROCESS | TRACK_VALID_EXT_PUSH;
  pushVisible.append(processingAndPushAv);
  if (input.expected(pushVisible, resolved) != 2 || !resolved) {
    return fail("recording readiness must count every push-visible derived track");
  }

  JSON::Value processes;
  processes.append(av);
  processes.append(remoteAv);
  processes.append(ffmpeg);
  processes.append(thumbs);
  processes.append(livepeer);
  processes.append(onnx);
  processes.append(JSON::Value("invalid"));
  if (input.expected(processes, resolved) != 7 || resolved) {
    return fail("processing output expectation must remain unresolved until ONNX publishes its contract");
  }
  input.publishExpected(processes);
  if (input.expectationResolved()) {
    return fail("recording readiness must stay unresolved while the ONNX output contract is unavailable");
  }

  char procPageName[NAME_BUFFER_SIZE];
  snprintf(procPageName, sizeof(procPageName), SHM_PROC_STATE, getpid());
  IPC::sharedPage procPage(procPageName, sizeof(ProcState), true, false);
  if (!procPage.mapped) { return fail("could not create process-state test page"); }
  ProcState::initPage(procPage);
  ProcState::publishStartup(procPage, 1.0, PRC_RESOURCE_CPU);
  ProcState::publishOutputContract(procPage, 1, PRC_INPUT_AUDIO);
  input.bindRunning(onnx, getpid());
  if (input.expected(processes, resolved) != 8 || !resolved) {
    return fail("audio ONNX must publish one output even when annotated_video is configured");
  }

  input.bindRunning(downstreamOnnx, getpid());
  input.publishExpected(processingChain);
  if (!input.expectationResolved() || input.feedPaused() || input.publishedExpected() != 1) {
    return fail("the VOD feeder must resume after downstream ONNX publishes its output contract");
  }

  input.retireHard(ffmpeg);
  if (input.expected(processes, resolved) != 7 || !resolved) {
    return fail("hard-failed processes must leave the recording output expectation");
  }

  JSON::Value disabledOnnx = onnx;
  disabledOnnx["restart_type"] = "disabled";
  JSON::Value disabledProcesses;
  disabledProcesses.append(disabledOnnx);
  input.retireDisabled(disabledOnnx);
  if (input.expected(disabledProcesses, resolved) != 0 || !resolved) {
    return fail("completed restart-disabled processes must leave the recording output expectation");
  }

  input.publishExpected(processes);
  if (!input.expectationResolved() || input.publishedExpected() != 7) {
    return fail("the input buffer must publish its revised processing output expectation");
  }

  {
    InputBufferProbe completed(&config);
    completed.initMetadata("completed-output-expectation-test");
    JSON::Value completedThumbs = thumbs;
    completedThumbs["restart_type"] = "disabled";
    completed.retireDisabled(completedThumbs);
    for (size_t i = 0; i < 3; ++i) { completed.addCompletedOutput(); }
    JSON::Value pending;
    pending.append(completedThumbs);
    pending.append(av);
    if (completed.expected(pending, resolved) != 4 || !resolved) {
      return fail("three completed thumbnail tracks must not satisfy a pending video output");
    }
    completed.addCompletedOutput(TRACK_VALID_INT_PROCESS);
    if (completed.expected(pending, resolved) != 4 || !resolved) {
      return fail("recording-invisible completed tracks must not affect its expectation");
    }
    completed.retireHard(av);
    if (completed.expected(pending, resolved) != 3 || !resolved) {
      return fail("a failed producer must unblock without discarding completed output expectations");
    }
    JSON::Value inhibited = av;
    inhibited["track_inhibit"] = "video=JPEG";
    if (!completed.matchesSource(inhibited)) {
      return fail("a derived track must not inhibit a process; only source tracks do, as in the supervisor");
    }
  }

  {
    InputBufferProbe replacing(&config);
    replacing.initMetadata("replaced-output-expectation-test");
    if (!replacing.openStatePage(pageName)) { return fail("could not map replacement expectation page"); }
    replacing.reset(true, false, true, true, 0);
    JSON::Value keyed = livepeer;
    keyed["source"] = replacing.streamName;
    JSON::Value replacement = av;
    replacement["codec"] = "H264";
    replacement["resolution"] = "640x360";
    replacing.processReplacements[keyed.toString()].append(replacement);
    replacement["resolution"] = "854x480";
    replacing.processReplacements[keyed.toString()].append(replacement);
    replacing.retireHard(livepeer);
    for (size_t i = 0; i < 4; ++i) { replacing.addCompletedOutput(); }
    JSON::Value original;
    original.append(livepeer);
    // checkProcesses has chosen replacements, but its caller still holds the
    // old effective list until the next supervisor pass starts them.
    replacing.publishExpected(original);
    if (!replacing.expectationResolved() || replacing.publishedExpected() != 6) {
      return fail("the tick retiring Livepeer must still gate a recording on its two scheduled replacement outputs");
    }
    replacing.publishExpected(replacing.applyProcessReplacements(original));
    if (!replacing.expectationResolved() || replacing.publishedExpected() != 6) {
      return fail("an already replaced process list must not count its replacement contracts twice");
    }
    replacing.hasPush = false;
    replacing.publishExpected(original);
    if (!replacing.expectationResolved() || replacing.publishedExpected() != 6) {
      return fail("source EOF must not discard replacement outputs before their producers have started");
    }
    jsonForEachConst (replacing.processReplacements[keyed.toString()], process) {
      JSON::Value replacementKey = *process;
      replacementKey["source"] = replacing.streamName;
      replacing.procBoots[replacementKey.toString()] = 1;
    }
    replacing.publishExpected(original);
    if (!replacing.expectationResolved() || replacing.publishedExpected() != 4) {
      return fail("replacement producers that finished after source EOF must release missing-output requirements");
    }
    replacing.processReplacements.clear();
    replacing.publishExpected(original);
    if (!replacing.expectationResolved() || replacing.publishedExpected() != 4) {
      return fail("a retired producer without a replacement must still release its missing-output gate");
    }
    replacing.streamStatus.master = false;
  }

  std::deque<std::string> sleeper;
  sleeper.push_back("/bin/sleep");
  sleeper.push_back("30");
  const pid_t firstSleeper = Util::Procs::StartPiped(sleeper, 0, 0, 0);
  const pid_t secondSleeper = Util::Procs::StartPiped(sleeper, 0, 0, 0);
  if (!firstSleeper || !secondSleeper) {
    if (firstSleeper) { Util::Procs::Stop(firstSleeper); }
    if (secondSleeper) { Util::Procs::Stop(secondSleeper); }
    return fail("could not start supervisor lifecycle fixtures");
  }
  JSON::Value firstProcess;
  firstProcess["process"] = "FixtureOne";
  JSON::Value secondProcess;
  secondProcess["process"] = "FixtureTwo";
  input.clearRunningFixtures();
  input.bindRunning(firstProcess, firstSleeper);
  input.bindRunning(secondProcess, secondSleeper);
  JSON::Value noProcesses;
  noProcesses.append(JSON::Value());
  noProcesses.shrink(0);
  const size_t remainingProcesses = input.reconcileProcesses(noProcesses);
  if (Util::Procs::isActive(firstSleeper)) { Util::Procs::Stop(firstSleeper); }
  if (Util::Procs::isActive(secondSleeper)) { Util::Procs::Stop(secondSleeper); }
  if (remainingProcesses) { return fail("one reconciliation tick must stop every process removed from configuration"); }

  input.streamStatus.master = false;
  input.streamStatus.close();
  shm_unlink(pageName);
  procPage.master = false;
  shm_unlink(procPageName);
  return 0;
}
