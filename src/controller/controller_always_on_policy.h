#pragma once

#include <mist/stream_status.h>

#include <cstdint>
#include <sys/types.h>

namespace Controller {
  enum AlwaysOnAction {
    ALWAYS_ON_NONE, ///< The input this controller started is still running.
    ALWAYS_ON_START, ///< Nothing serves the stream: start the always-on input.
    ALWAYS_ON_ADOPT, ///< An input this controller did not start serves the stream: track its PID.
    ALWAYS_ON_SKIP ///< The stream is served, but its input PID is not readable: leave it alone.
  };

  /// Decides the always-on branch of checkStream.
  /// A controller started by a rolling restart has no record of the inputs the previous generation
  /// started, and those inputs keep running. Starting another one would only produce a duplicate
  /// that blocks the controller loop inside startInput, so any sign of a live input (a non-terminal
  /// stream state, a held input lock or a held pull lock) means the stream is already served.
  inline AlwaysOnAction alwaysOnAction(bool trackedInputRunning, uint8_t streamStatus, bool inputLockHeld,
                                       bool pullLockHeld, pid_t runningInputPid) {
    if (trackedInputRunning) { return ALWAYS_ON_NONE; }
    const bool served = !Util::streamStatusIsTerminal(streamStatus) || inputLockHeld || pullLockHeld;
    if (!served) { return ALWAYS_ON_START; }
    return runningInputPid > 1 ? ALWAYS_ON_ADOPT : ALWAYS_ON_SKIP;
  }
} // namespace Controller
