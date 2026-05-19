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

/// ProcState schema version. Bump on any layout change.
/// Old/new MistProc binaries on the same node are unsupported; readers reject pages
/// with a different version or a structSize smaller than their own sizeof(ProcState).
static constexpr uint32_t PROC_STATE_VERSION = 2;

/// Normalized pressure reason. Ordered by aggregation priority (higher = stronger backoff).
enum ProcPressureReason : uint8_t {
  PRC_REASON_UNKNOWN = 0,
  PRC_REASON_CPU = 1,
  PRC_REASON_SOURCE_WAIT = 2,
  PRC_REASON_SINK_WAIT = 3,
  PRC_REASON_HW_SLOT = 4,
  PRC_REASON_EXTERNAL_WAIT = 5,
  PRC_REASON_QUEUE_FULL = 6,
  PRC_REASON_RETRY = 7,
};

/// Negotiated processing kind as observed by the proc after init (e.g. AV
/// reports whether it ended up on HW or SW encode, which the static config
/// classifier cannot know). These numeric values match
/// `Mist::ProcessingProfileKind` (src/input/processing_profile.h); a
/// static_assert there keeps the two in sync.
enum ProcNegotiatedKind : uint8_t {
  PRC_KIND_UNKNOWN = 0,
  PRC_KIND_THUMBS = 1,
  PRC_KIND_AUDIO = 2,
  PRC_KIND_AV_SW_VIDEO = 3,
  PRC_KIND_AV_HW_VIDEO = 4,
  PRC_KIND_LIVEPEER = 5,
  PRC_KIND_MIXED = 6, // never reported by a proc; classifier-only
};

/// Process state shared via SHM between MistProc processes and InputBuffer.
/// Two kinds of data live here:
///   * Raw counters (totalWork / totalSourceWait / ...): debug/support, NOT
///     required truth. Procs may leave any of them zero.
///   * Normalized pressure (canAcceptMore, pressureQ0_16, reasonCode,
///     observedSpeedQ16_16, queueDepth, inflight, retryCount): first-class
///     signal the controller acts on. Each proc fills what it can measure.
/// Exit state (short/longReason) is written by the process before exiting and
/// read by InputBuffer after the process dies.
struct ProcState {
    // --- Header (always at offset 0; layout-stable across versions) ---
    uint32_t schemaVersion; ///< must equal PROC_STATE_VERSION
    uint32_t structSize; ///< sizeof(ProcState) at the time of writing

    // --- Timing ---
    uint64_t lastUpdateMs; ///< Util::bootMS() of last write (0 = never written)
    uint64_t frameCount; ///< frames/items processed (cumulative)

    // --- Raw counters (microseconds, cumulative; optional per-proc) ---
    uint64_t totalWork; ///< active processing time (decode+encode+transform+...)
    uint64_t totalSourceWait; ///< time waiting for input data
    uint64_t totalSinkWait; ///< time waiting to write output
    uint64_t totalExternalWait; ///< time waiting on external service (e.g. Livepeer gateway)

    // --- Normalized pressure (first-class) ---
    uint32_t observedSpeedQ16_16; ///< measured throughput as multiple of realtime (Q16.16)
    uint16_t pressureQ0_16; ///< 0..65535 -> 0.0..1.0 (1.0 = max pressure / cannot keep up)
    uint8_t canAcceptMore; ///< 0 = hard stop / do not feed faster, 1 = OK
    uint8_t reasonCode; ///< ProcPressureReason enum

    uint32_t queueDepth; ///< pending work units (proc-defined unit)
    uint32_t inflight; ///< work items currently in-flight (e.g. Livepeer segments)
    uint32_t retryCount; ///< rolling count of retriable failures

    // Negotiated kind: proc fills this once it knows what it actually became.
    // The controller uses it to re-derive the effective speed profile on each
    // tick. For example, HW-accelerated AV that the static config classifier could
    // not detect gets its real cap, not the SW-video fallback. 0 = unknown.
    uint8_t negotiatedKind; ///< ProcNegotiatedKind value
    uint8_t _pad0;
    uint8_t _pad1;
    uint8_t _pad2;

    // --- Exit state ---
    char shortReason[32]; ///< ER_* constant string (e.g. "FORMAT_SPECIFIC")
    char longReason[256]; ///< human-readable exit reason

    /// Initialize a freshly-mapped SHM page: zero everything, then stamp version/size.
    /// Call this once from the process side right after init() of the SHM page.
    static void initPage(IPC::sharedPage & page) {
      if (!page.mapped || page.len < sizeof(ProcState)) { return; }
      memset(page.mapped, 0, sizeof(ProcState));
      ProcState *s = (ProcState *)page.mapped;
      s->schemaVersion = PROC_STATE_VERSION;
      s->structSize = (uint32_t)sizeof(ProcState);
    }

    /// True if a mapped page looks like a valid current-version ProcState.
    /// Readers should call this and skip / log-once on false.
    static bool isValid(const IPC::sharedPage & page) {
      if (!page.mapped || page.len < sizeof(ProcState)) { return false; }
      const ProcState *s = (const ProcState *)page.mapped;
      if (s->schemaVersion != PROC_STATE_VERSION) { return false; }
      if (s->structSize < sizeof(ProcState)) { return false; }
      return true;
    }

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
