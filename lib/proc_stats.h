#pragma once
#include "config.h"
#include "defines.h"
#include "shared_memory.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

#define SHM_PROC_STATE "/MstProcState_%d" // format with PID

/// Process state shared via SHM between MistProc processes and InputBuffer.
/// Timing stats used for rate control; exit state read after process dies.
struct ProcState {
    // Timing stats (all times in microseconds, cumulative since process start)
    uint64_t totalWork; // cumulative active processing time (decode+encode+transform+inference+etc)
    uint64_t totalSourceSleep; // cumulative time waiting for input data (KEY rate control signal)
    uint64_t totalSinkSleep; // cumulative time waiting to write output
    uint64_t frameCount; // frames/items processed
    uint64_t lastUpdateMs; // Util::bootMS() of last write
    // Exit state (written by process before exiting, read by buffer after process dies)
    char shortReason[32]; // ER_* constant string (e.g. "FORMAT_SPECIFIC")
    char longReason[256]; // Human-readable exit reason

    /// Write exit reason into the SHM fields
    void setExitReason(const char *shortStr, const char *longStr) {
      if (shortStr) {
        strncpy(shortReason, shortStr, sizeof(shortReason) - 1);
        shortReason[sizeof(shortReason) - 1] = '\0';
      }
      if (longStr) {
        strncpy(longReason, longStr, sizeof(longReason) - 1);
        longReason[sizeof(longReason) - 1] = '\0';
      }
    }
};

/// Process-wide exit reason aggregator. Thread-safe, severity-based.
/// Any thread can record an exit reason; the highest-severity exit wins:
/// unrecoverable (2) > retryable (1) > clean (0).
/// For equal severities, first write wins.
class ProcExitState {
    std::mutex mtx;
    bool isSet;
    int exitCode; ///< 0 = clean, 1 = retryable error, 2 = unrecoverable
    char shortReason[32];
    char longReason[256];

  public:
    ProcExitState() : isSet(false), exitCode(0) {
      shortReason[0] = '\0';
      longReason[0] = '\0';
    }
    /// Record exit reason from any thread. Higher-severity reasons replace lower-severity ones.
    /// For equal severities, the first recorded reason is kept.
    /// code: 0 = clean, 1 = retryable error, 2 = unrecoverable.
    /// Also calls Util::logExitReason for the log message (thread-local) and captures
    /// the thread-local exit reason so lower layers can override generic wrapper messages.
    void log(const char *shortStr, int code, const char *fmt, ...) {
      va_list args;
      va_start(args, fmt);
      char buf[256];
      vsnprintf(buf, sizeof(buf), fmt, args);
      va_end(args);
      Util::logExitReason(shortStr, "%s", buf);
      const char *capturedShort = Util::mRExitReason;
      const char *capturedLong = Util::exitReason[0] ? Util::exitReason : buf;
      std::lock_guard<std::mutex> guard(mtx);
      if (isSet && code <= exitCode) { return; }
      isSet = true;
      exitCode = code;
      if (capturedShort) {
        strncpy(shortReason, capturedShort, sizeof(shortReason) - 1);
        shortReason[sizeof(shortReason) - 1] = '\0';
      }
      strncpy(longReason, capturedLong, sizeof(longReason) - 1);
      longReason[sizeof(longReason) - 1] = '\0';
    }
    /// Write aggregated state to SHM page, relinquish ownership, return exit code.
    /// Falls back to main thread's Util::exitReason if no thread recorded a reason.
    int flush(IPC::sharedPage & page) {
      std::lock_guard<std::mutex> guard(mtx);
      if (page.mapped) {
        ProcState *s = (ProcState *)page.mapped;
        if (isSet) {
          s->setExitReason(shortReason, longReason);
        } else {
          s->setExitReason(Util::mRExitReason, Util::exitReason);
        }
        page.master = false;
      }
      return exitCode;
    }
};
