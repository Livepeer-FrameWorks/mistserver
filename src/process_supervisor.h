#pragma once

#include <mist/defines.h>

#include <cstdint>
#include <string>

namespace Mist {
  inline const char *processExitStatus(int exitCode, const std::string & restartType, uint32_t bootCount) {
    if (exitCode == 2) { return "unrecoverable"; }
    if (exitCode == 0) { return "clean"; }
    if (restartType == "disabled" && bootCount) { return "disabled"; }
    return "retrying";
  }

  inline bool processSupervisorMayStart(bool active, uint8_t streamState, bool sourceEof) {
    return active && !sourceEof && streamState != STRMSTAT_SHUTDOWN && streamState != STRMSTAT_OFF;
  }

  inline std::string processExitTriggerPayload(const std::string & streamName, const std::string & processType,
                                               const std::string & processConfig, uint64_t pid, int exitCode,
                                               uint32_t bootCount, const std::string & status,
                                               const std::string & shortReason, const std::string & longReason) {
    return streamName + "\n" + processType + "\n" + processConfig + "\n" + std::to_string(pid) + "\n" +
      std::to_string(exitCode) + "\n" + std::to_string(bootCount) + "\n" + status + "\n" + shortReason + "\n" + longReason;
  }
} // namespace Mist
