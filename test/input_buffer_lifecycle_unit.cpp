#define private public
#define protected public
#include "../src/input/input_buffer.h"
#undef protected
#undef private

#include <mist/http_parser.h>

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

      std::string key(const JSON::Value & process) const {
        JSON::Value keyed = process;
        keyed["source"] = streamName;
        return keyed.toString();
      }

      /// Stands in for the process's --describe-outputs answer.
      void declare(const JSON::Value & process, const JSON::Value & declaration) {
        processDeclarations[key(process)] = declaration;
      }

      void addSourceData() { meta.update(0, 0, 0, 1, 0, true, 1); }

      /// Publishes the graph of these processes, then reports how many expected outputs a recording
      /// that takes every track selects from the published page, and how many of them carry data.
      bool recorderExpects(const JSON::Value & processes, size_t & selected, size_t & ready) {
        publishProcessGraph(resolveProcessGraph(processes));
        JSON::Value graph;
        if (!Mist::readProcessGraphPage(processGraphPage, graph) || !graph["resolved"].asBool()) { return false; }
        std::map<std::string, std::string> everything;
        everything["video"] = "all";
        everything["audio"] = "all";
        everything["meta"] = "all";
        JSON::Value capa;
        capa["codecs"][0u][0u].append("*");
        const uint8_t oldMask = DTSC::trackValidMask;
        DTSC::trackValidMask = TRACK_VALID_EXT_PUSH;
        Mist::recordingExpectedOutputs(meta, graph["outputs"], everything, capa, "", selected, ready);
        DTSC::trackValidMask = oldMask;
        return true;
      }

      size_t addOutput(const JSON::Value & process, const std::string & output, const std::string & type,
                       const std::string & codec, uint8_t mask, bool withData) {
        const size_t track = meta.addTrack();
        meta.setID(track, track + 1);
        meta.setType(track, type);
        meta.setCodec(track, codec);
        meta.setSourceTrack(track, 0);
        meta.setOutputKey(track, DTSC::outputKey(DTSC::processIdentity(key(process)), output));
        meta.validateTrack(track, mask);
        if (withData) { meta.update(0, 0, track, 1, 0, true, 1); }
        meta.breakClaim(track);
        return track;
      }

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

      bool feedPaused() const { return streamStatus.mapped[STRMSTATE_PROCESS_FEED_PAUSED_OFFSET] != 0; }

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

  input.initMetadata("pgexpect");
  input.addSourceData();
  input.reset(true, false, true, true, 0);
  JSON::Value av;
  av["process"] = "AV";
  JSON::Value thumbs;
  thumbs["process"] = "Thumbs";
  JSON::Value onnx;
  onnx["process"] = "ONNX";
  auto declaration = [](const std::string & selector, const std::string & codecs, const JSON::Value & outputs) {
    JSON::Value declared;
    std::map<std::string, std::string> params;
    HTTP::parseVars(selector, params);
    for (const auto & param : params) { declared["select"]["target"][param.first] = param.second; }
    declared["select"]["codecs"][0u][0u] = JSON::fromString(codecs);
    declared["outputs"] = outputs;
    return declared;
  };
  auto output = [](const std::string & name, const std::string & type, const std::string & codec, int mask) {
    JSON::Value out;
    out["output"] = name;
    out["type"] = type;
    out["codec"] = codec;
    out["mask"] = mask;
    return out;
  };
  auto one = [](const JSON::Value & out) {
    JSON::Value list;
    list.append(out);
    return list;
  };
  size_t selected = 0;
  size_t ready = 0;

  // Outputs a recording cannot select (viewer-only, processing-only, raw) never gate it.
  JSON::Value viewerOnlyAv = av;
  viewerOnlyAv["target_mask"] = TRACK_VALID_EXT_HUMAN;
  input.declare(viewerOnlyAv,
                declaration("video=H264", R"(["H264"])", one(output("video", "video", "H264", TRACK_VALID_EXT_HUMAN))));
  JSON::Value processOnlyOnnx = onnx;
  processOnlyOnnx["target_mask"] = TRACK_VALID_INT_PROCESS;
  input.declare(processOnlyOnnx,
                declaration("video=H264", R"(["H264"])", one(output("default/results", "meta", "JSON", TRACK_VALID_INT_PROCESS))));
  JSON::Value rawAv = av;
  rawAv["codec"] = "I420";
  input.declare(rawAv,
                declaration("video=H264", R"(["H264"])",
                            one(output("video", "video", "I420", TRACK_VALID_EXT_HUMAN | TRACK_VALID_INT_PROCESS))));
  JSON::Value recordingInvisible;
  recordingInvisible.append(viewerOnlyAv);
  recordingInvisible.append(processOnlyOnnx);
  recordingInvisible.append(rawAv);
  if (!input.recorderExpects(recordingInvisible, selected, ready) || selected != 0) {
    return fail("recording readiness must ignore viewer-only and processing-only derived tracks");
  }

  JSON::Value thumbOutputs;
  thumbOutputs.append(output("sprite", "video", "JPEG", 3));
  thumbOutputs.append(output("vtt", "meta", "thumbvtt", 3));
  thumbOutputs.append(output("preview", "video", "JPEG", 3));
  input.declare(thumbs, declaration("", R"(["H264","AV1","JPEG"])", thumbOutputs));
  JSON::Value thumbnailOnly;
  thumbnailOnly.append(thumbs);
  if (!input.recorderExpects(thumbnailOnly, selected, ready) || selected != 3 || ready != 0) {
    return fail("recordings must await both JPEG tracks and the VTT track");
  }

  // A chain is expected as soon as the source tracks exist, without any of its processes running.
  JSON::Value rawIntermediate = av;
  rawIntermediate["codec"] = "NV12";
  rawIntermediate["target_mask"] = TRACK_VALID_INT_PROCESS;
  input.declare(rawIntermediate,
                declaration("video=H264", R"(["H264"])", one(output("video", "video", "NV12", TRACK_VALID_INT_PROCESS))));
  JSON::Value downstreamOnnx = onnx;
  downstreamOnnx["track_select"] = "video=NV12&audio=none";
  downstreamOnnx["target_mask"] = TRACK_VALID_EXT_PUSH;
  input.declare(downstreamOnnx,
                declaration("video=NV12&audio=none", R"(["NV12"])",
                            one(output("default/results", "meta", "JSON", TRACK_VALID_EXT_PUSH))));
  JSON::Value processingChain;
  processingChain.append(rawIntermediate);
  processingChain.append(downstreamOnnx);
  if (!input.recorderExpects(processingChain, selected, ready) || selected != 1 || ready != 0) {
    return fail("a downstream ONNX output must be expected before its AV intermediate exists");
  }
  input.publishFeedPaused();
  if (input.feedPaused()) {
    return fail("the feed must not pause before any process output exists: only a lagging recorder holds it");
  }
  JSON::Value impossibleOnnx = downstreamOnnx;
  impossibleOnnx["track_select"] = "video=VP9&audio=none";
  input.declare(impossibleOnnx,
                declaration("video=VP9&audio=none", R"(["NV12"])",
                            one(output("default/results", "meta", "JSON", TRACK_VALID_EXT_PUSH))));
  JSON::Value impossibleChain;
  impossibleChain.append(rawIntermediate);
  impossibleChain.append(impossibleOnnx);
  if (!input.recorderExpects(impossibleChain, selected, ready) || selected != 0) {
    return fail("an ONNX selector that no source or declared output can satisfy must not wedge recording readiness");
  }

  JSON::Value pushOnlyAv = av;
  pushOnlyAv["target_mask"] = TRACK_VALID_EXT_PUSH;
  input.declare(pushOnlyAv, declaration("video=H264", R"(["H264"])", one(output("video", "video", "H264", TRACK_VALID_EXT_PUSH))));
  JSON::Value processingAndPushAv = av;
  processingAndPushAv["target_mask"] = TRACK_VALID_INT_PROCESS | TRACK_VALID_EXT_PUSH;
  input.declare(processingAndPushAv,
                declaration("video=H264", R"(["H264"])",
                            one(output("video", "video", "H264", TRACK_VALID_INT_PROCESS | TRACK_VALID_EXT_PUSH))));
  JSON::Value pushVisible;
  pushVisible.append(pushOnlyAv);
  pushVisible.append(processingAndPushAv);
  if (!input.recorderExpects(pushVisible, selected, ready) || selected != 2) {
    return fail("recording readiness must count every push-visible derived track");
  }

  // Retired processes leave the expectation: hard-failed, or restart-disabled after their run.
  input.retireHard(pushOnlyAv);
  if (!input.recorderExpects(pushVisible, selected, ready) || selected != 1) {
    return fail("hard-failed processes must leave the recording output expectation");
  }
  JSON::Value disabledAv = processingAndPushAv;
  disabledAv["restart_type"] = "disabled";
  input.declare(disabledAv, declaration("video=H264", R"(["H264"])", one(output("video", "video", "H264", TRACK_VALID_EXT_PUSH))));
  input.retireDisabled(disabledAv);
  JSON::Value disabledProcesses;
  disabledProcesses.append(disabledAv);
  if (!input.recorderExpects(disabledProcesses, selected, ready) || selected != 0) {
    return fail("completed restart-disabled processes must leave the recording output expectation");
  }

  {
    // A producer that finished after the source ended leaves its tracks; they need no waiting.
    InputBufferProbe completed(&config);
    completed.initMetadata("pgdone");
    completed.addSourceData();
    completed.processControlledRealtime = true;
    completed.everHadPush = true;
    completed.hasPush = false;
    JSON::Value completedThumbs = thumbs;
    completedThumbs["restart_type"] = "disabled";
    completed.declare(completedThumbs, declaration("", R"(["H264","AV1","JPEG"])", thumbOutputs));
    completed.procBoots[completed.key(completedThumbs)] = 1;
    completed.addOutput(completedThumbs, "sprite", "video", "JPEG", TRACK_VALID_ALL, true);
    completed.addOutput(completedThumbs, "vtt", "meta", "thumbvtt", TRACK_VALID_ALL, true);
    completed.addOutput(completedThumbs, "preview", "video", "JPEG", TRACK_VALID_ALL, true);
    completed.declare(pushOnlyAv,
                      declaration("video=H264", R"(["H264"])", one(output("video", "video", "H264", TRACK_VALID_EXT_PUSH))));
    JSON::Value pending;
    pending.append(completedThumbs);
    pending.append(pushOnlyAv);
    if (!completed.recorderExpects(pending, selected, ready) || selected != 1 || ready != 0) {
      return fail("completed thumbnail tracks must not satisfy a pending video output");
    }
    completed.addOutput(pushOnlyAv, "video", "video", "H264", TRACK_VALID_EXT_PUSH, true);
    if (!completed.recorderExpects(pending, selected, ready) || selected != 1 || ready != 1) {
      return fail("an expected output must count as ready once its track carries data");
    }
    JSON::Value inhibited = av;
    inhibited["track_inhibit"] = "video=JPEG";
    if (Mist::processInhibitReason(inhibited, completed.meta, std::set<std::string>()).size()) {
      return fail("a derived track must not inhibit a process; only source tracks do");
    }
  }

  {
    // Replacements a hard failure scheduled are expected on the tick that retires the original.
    InputBufferProbe replacing(&config);
    replacing.initMetadata("pgreplace");
    replacing.addSourceData();
    replacing.processControlledRealtime = true;
    replacing.everHadPush = true;
    replacing.hasPush = true;
    JSON::Value livepeer;
    livepeer["process"] = "Livepeer";
    JSON::Value replacement = av;
    replacement["codec"] = "H264";
    replacement["resolution"] = "640x360";
    replacing.declare(replacement, declaration("video=H264", R"(["H264"])", one(output("video", "video", "H264", 3))));
    replacing.processReplacements[replacing.key(livepeer)].append(replacement);
    replacement["resolution"] = "854x480";
    replacing.declare(replacement, declaration("video=H264", R"(["H264"])", one(output("video", "video", "H264", 3))));
    replacing.processReplacements[replacing.key(livepeer)].append(replacement);
    replacing.retireHard(livepeer);
    JSON::Value original;
    original.append(livepeer);
    if (!replacing.recorderExpects(replacing.applyProcessReplacements(original), selected, ready) || selected != 2) {
      return fail("a recording must wait for the two scheduled replacement outputs of a retired Livepeer");
    }
    replacing.hasPush = false;
    if (!replacing.recorderExpects(replacing.applyProcessReplacements(original), selected, ready) || selected != 2) {
      return fail("source EOF must not discard replacement outputs before their producers have started");
    }
    jsonForEachConst (replacing.processReplacements[replacing.key(livepeer)], process) {
      replacing.procBoots[replacing.key(*process)] = 1;
    }
    if (!replacing.recorderExpects(replacing.applyProcessReplacements(original), selected, ready) || selected != 0) {
      return fail("replacement producers that finished after source EOF must release missing-output requirements");
    }
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
  return 0;
}
