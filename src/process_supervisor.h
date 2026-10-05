#pragma once

#include <mist/defines.h>
#include <mist/json.h>

#include <cstdint>
#include <string>
#include <vector>

namespace Mist {
  /// How a collected process exit is handled. A clean exit the controller requested (the stop of
  /// the process's session) is "stopped": the process stays down until a new publisher session or
  /// a configuration change, since restarting it would undo that stop.
  inline const char *processExitStatus(int exitCode, const std::string & restartType, uint32_t bootCount,
                                       const std::string & shortReason = "") {
    if (exitCode == 2) { return "unrecoverable"; }
    if (exitCode == 0 && shortReason == ER_CLEAN_CONTROLLER_REQ) { return "stopped"; }
    if (exitCode == 0) { return "clean"; }
    if (restartType == "disabled" && bootCount) { return "disabled"; }
    return "retrying";
  }

  inline bool processSupervisorMayStart(bool active, uint8_t streamState, bool sourceEof) {
    return active && !sourceEof && streamState != STRMSTAT_SHUTDOWN && streamState != STRMSTAT_OFF;
  }

  /// Whether the source of a live stream ended: a publisher was connected and none is now. The
  /// buffer neither starts nor restarts processes then, since there is nothing left for them to
  /// process until a publisher returns. A process-controlled stream publishes the same state on
  /// its state page for its readers.
  inline bool processSourceEnded(bool processControlledRealtime, bool processingSourceEofFlag, bool everHadPush, bool hasPush) {
    if (processControlledRealtime) { return processingSourceEofFlag; }
    return everHadPush && !hasPush;
  }

  /// Whether the buffer checks its processes on this tick: on its regular interval (which also
  /// picks up configuration changes), and as soon as a process it started has exited or a delayed
  /// restart became due, so a producer that died is collected and restarted on the next tick.
  inline bool processSupervisorCheckDue(uint64_t nowMs, uint64_t lastCheckMs, uint64_t intervalMs, bool processExited, bool restartDue) {
    return processExited || restartDue || nowMs - lastCheckMs > intervalMs;
  }

  /// The outputs of an AV or Thumbs process that a processing buffer reserves a track for before
  /// starting it, by output name (see DTSC::outputKey). Raw AV outputs are intermediate tracks in
  /// their own track layout and are not reserved; other processes declare their outputs only once
  /// they run.
  inline std::vector<std::string> processReservableOutputs(const JSON::Value & proc) {
    std::vector<std::string> outputs;
    const std::string process = proc["process"].asString();
    if (process == "Thumbs") {
      outputs.push_back("sprite");
      outputs.push_back("vtt");
      outputs.push_back("preview");
    } else if (process == "AV") {
      const std::string codec = proc.isMember("codec") && proc["codec"].isString() ? proc["codec"].asString() : "";
      if (codec.empty() || codec == "H264" || codec == "AV1" || codec == "JPEG") { outputs.push_back("video"); }
      if (codec == "opus" || codec == "AAC") { outputs.push_back("audio"); }
    }
    return outputs;
  }

  /// How long after its (re)start a producer gets to register its outputs again. It covers the
  /// process boot (about a second), waiting for a source keyframe (GOPs of up to 10 s), and one
  /// segment's processing deadline with gateway retries (Livepeer: the segment duration plus one
  /// second per attempt).
  const uint64_t PRODUCER_RESUME_GRACE_MS = 30000;

  /// Until when the buffer keeps a track whose producer went away, for the restarted producer to
  /// continue it: the restart grace after the producer's next start, which is the loss itself or
  /// a configured restart delay later. A producer that keeps failing before it registers does not
  /// extend this: the deadline is fixed when the track loses its producer.
  inline uint64_t producerResumeDeadline(uint64_t lostAtMs, uint64_t nextStartMs) {
    return (nextStartMs > lostAtMs ? nextStartMs : lostAtMs) + PRODUCER_RESUME_GRACE_MS;
  }

  /// Whether the buffer keeps a track without a producer instead of erasing it when it idles:
  /// only while the process that produced it is still configured and will be restarted, and its
  /// resume deadline has not passed.
  inline bool trackHeldForProducer(bool producerRestartable, uint64_t deadlineMs, uint64_t nowMs) {
    return producerRestartable && nowMs < deadlineMs;
  }

  inline std::string processExitTriggerPayload(const std::string & streamName, const std::string & processType,
                                               const std::string & processConfig, uint64_t pid, int exitCode,
                                               uint32_t bootCount, const std::string & status,
                                               const std::string & shortReason, const std::string & longReason) {
    return streamName + "\n" + processType + "\n" + processConfig + "\n" + std::to_string(pid) + "\n" +
      std::to_string(exitCode) + "\n" + std::to_string(bootCount) + "\n" + status + "\n" + shortReason + "\n" + longReason;
  }

  inline std::string processReplaceTriggerPayload(const std::string & streamName, const std::string & processType,
                                                  const std::string & processConfig, int exitCode,
                                                  const std::string & shortReason, const std::string & longReason) {
    return streamName + "\n" + processType + "\n" + processConfig + "\n" + std::to_string(exitCode) + "\n" +
      shortReason + "\n" + longReason;
  }

  /// Parses a PROCESS_REPLACE response into the replacement process configs.
  /// Only objects naming a process are kept; an empty, non-array or unusable response yields an
  /// empty array, which means the failed process is not replaced.
  inline JSON::Value processReplacementConfigs(const std::string & response) {
    JSON::Value replacements;
    replacements.append(JSON::Value());
    replacements.shrink(0);
    JSON::Value parsed = JSON::fromString(response);
    if (!parsed.isArray()) { return replacements; }
    jsonForEachConst (parsed, it) {
      if (!it->isObject() || !(*it)["process"].isString() || (*it)["process"].asStringRef().empty()) { continue; }
      replacements.append(*it);
    }
    return replacements;
  }
} // namespace Mist
