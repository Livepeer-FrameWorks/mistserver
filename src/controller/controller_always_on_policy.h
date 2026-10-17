#pragma once

#include <mist/stream_status.h>

#include <cstdint>
#include <sys/types.h>

namespace Controller {
  enum AlwaysOnAction {
    ALWAYS_ON_NONE, ///< The input this controller started is still running.
    ALWAYS_ON_START, ///< Nothing serves the stream: start the always-on input.
    ALWAYS_ON_ADOPT, ///< An input this controller did not start serves the stream: track its PID.
    ALWAYS_ON_SKIP, ///< The stream looks served, but no running input published its PID (yet): wait.
    ALWAYS_ON_RECLAIM ///< It looked served without a running input for too long: release its locks, start it.
  };

  /// How long an always-on stream may look served (a held input or pull lock, a non-terminal
  /// state) without a running input having published its PID, before those signals count as left
  /// behind by an input that died without releasing them (killed, say). A starting input publishes
  /// its PID right after taking its lock, and a pull input waits at most 5 s for the lock first.
  static const uint64_t ALWAYS_ON_ABANDONED_LOCK_MS = 15000;

  /// Decides the always-on branch of checkStream.
  /// A controller started by a rolling restart has no record of the inputs the previous generation
  /// started, and those inputs keep running. Starting another one would only produce a duplicate
  /// that blocks the controller loop inside startInput, so any sign of a live input (a non-terminal
  /// stream state, a held input lock or a held pull lock) means the stream is already served.
  /// Those signals outlive an input that died without releasing them (a POSIX semaphore stays taken,
  /// a state page stays), so once they have shown no running input for ALWAYS_ON_ABANDONED_LOCK_MS
  /// (servedWithoutInputMs) the stream is reclaimed instead of skipped for good.
  inline AlwaysOnAction alwaysOnAction(bool trackedInputRunning, uint8_t streamStatus, bool inputLockHeld,
                                       bool pullLockHeld, pid_t runningInputPid, uint64_t servedWithoutInputMs = 0) {
    if (trackedInputRunning) { return ALWAYS_ON_NONE; }
    const bool served = !Util::streamStatusIsTerminal(streamStatus) || inputLockHeld || pullLockHeld;
    if (!served) { return ALWAYS_ON_START; }
    if (runningInputPid > 1) { return ALWAYS_ON_ADOPT; }
    return servedWithoutInputMs >= ALWAYS_ON_ABANDONED_LOCK_MS ? ALWAYS_ON_RECLAIM : ALWAYS_ON_SKIP;
  }
} // namespace Controller
