#pragma once
#include "config.h"
#include "defines.h"
#include "shared_memory.h"
#include "timing.h"

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <unistd.h>

#define SHM_PROC_STATE "/MstProcState_%d" // format with PID
#define SHM_NODE_PRESSURE "/MstNodePressure"

/// ProcState schema version. Bump on any layout change.
/// Old/new MistProc binaries on the same node are unsupported; readers reject pages
/// with a different version or a structSize smaller than their own sizeof(ProcState).
static constexpr uint32_t PROC_STATE_VERSION = 3;
static constexpr uint32_t NODE_PRESSURE_VERSION = 1;

enum ProcLifecyclePhase : uint8_t {
  PRC_PHASE_BOOTING = 0,
  PRC_PHASE_STARTUP = 1,
  PRC_PHASE_MEASURING = 2,
  PRC_PHASE_READY = 3,
  PRC_PHASE_DRAINING = 4,
};

enum ProcPrimaryResource : uint8_t {
  PRC_RESOURCE_UNKNOWN = 0,
  PRC_RESOURCE_CPU = 1,
  PRC_RESOURCE_GPU = 2,
  PRC_RESOURCE_EXTERNAL = 3,
  PRC_RESOURCE_IO = 4,
};

enum ProcStateFlags : uint16_t {
  PRC_FLAG_SOURCE_LIMITED = 1 << 0,
  PRC_FLAG_PROCESSOR_LIMITED = 1 << 1,
  PRC_FLAG_CAPACITY_VALID = 1 << 2,
};

enum NodePressureFlags : uint16_t {
  NODE_PRESSURE_HAS_PSI = 1 << 0,
};

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
    uint32_t publishSeq; ///< seqlock: odd while publishing, even when complete
    uint8_t phase; ///< ProcLifecyclePhase
    uint8_t primaryResource; ///< ProcPrimaryResource
    uint16_t flags; ///< ProcStateFlags

    // --- Proc-authored feed contract ---
    uint32_t recommendedFeedQ16_16; ///< requested feeder rate; startup seed until measured
    uint32_t capacitySpeedQ16_16; ///< sustainable proc capacity, excluding source starvation
    uint32_t inputSpeedQ16_16; ///< achieved source media-time / wall-time
    uint32_t outputSpeedQ16_16; ///< achieved sink media-time / wall-time
    uint16_t confidenceQ0_16; ///< confidence in measured capacity; 0 during startup
    uint16_t _contractPad;

    // --- Timing ---
    uint64_t lastUpdateMs; ///< Util::bootMS() of last write (0 = never written)
    uint64_t frameCount; ///< frames/items processed (cumulative)

    // --- Raw counters (microseconds, cumulative; optional per-proc) ---
    uint64_t totalWork; ///< active processing time (decode+encode+transform+...)
    uint64_t totalSourceWait; ///< time waiting for input data
    uint64_t totalSinkWait; ///< proc-defined sink-side wait/idle time; diagnostic only
    uint64_t totalExternalWait; ///< time waiting on external service (e.g. Livepeer gateway)

    // --- Normalized pressure (first-class) ---
    uint32_t observedSpeedQ16_16; ///< measured throughput as multiple of realtime (Q16.16)
    uint16_t pressureQ0_16; ///< 0..65535 -> 0.0..1.0 (1.0 = max pressure / cannot keep up)
    uint8_t canAcceptMore; ///< 0 = hard stop / do not feed faster, 1 = OK
    uint8_t reasonCode; ///< ProcPressureReason enum

    uint32_t queueDepth; ///< pending work units (proc-defined unit)
    uint32_t inflight; ///< work items currently in-flight (e.g. Livepeer segments)
    uint32_t retryCount; ///< rolling count of retriable failures

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
      s->phase = PRC_PHASE_BOOTING;
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

    void beginPublish() {
      uint32_t seq = __atomic_load_n(&publishSeq, __ATOMIC_RELAXED);
      if (seq & 1) { ++seq; }
      __atomic_store_n(&publishSeq, seq + 1, __ATOMIC_RELEASE);
    }

    void endPublish() {
      uint32_t seq = __atomic_load_n(&publishSeq, __ATOMIC_RELAXED);
      if (!(seq & 1)) { ++seq; }
      __atomic_store_n(&publishSeq, seq + 1, __ATOMIC_RELEASE);
    }

    static bool readSnapshot(const IPC::sharedPage & page, ProcState & out) {
      if (!isValid(page)) { return false; }
      const ProcState *s = (const ProcState *)page.mapped;
      for (size_t attempt = 0; attempt < 3; ++attempt) {
        uint32_t before = __atomic_load_n(&s->publishSeq, __ATOMIC_ACQUIRE);
        if (before & 1) { continue; }
        memcpy(&out, s, sizeof(ProcState));
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        uint32_t after = __atomic_load_n(&s->publishSeq, __ATOMIC_ACQUIRE);
        if (before == after && !(after & 1)) { return true; }
      }
      return false;
    }

    static uint32_t speedToQ16(double speed) {
      if (speed <= 0.0) { return 0; }
      if (speed >= 65535.0) { return 0xFFFFFFFFu; }
      return (uint32_t)(speed * 65536.0);
    }

    static void publishStartup(IPC::sharedPage & page, double feedSpeed, ProcPrimaryResource resource) {
      if (!isValid(page)) { return; }
      ProcState *s = (ProcState *)page.mapped;
      s->beginPublish();
      s->phase = PRC_PHASE_STARTUP;
      s->primaryResource = resource;
      s->recommendedFeedQ16_16 = speedToQ16(feedSpeed < 1.0 ? 1.0 : feedSpeed);
      s->capacitySpeedQ16_16 = 0;
      s->inputSpeedQ16_16 = 0;
      s->outputSpeedQ16_16 = 0;
      s->confidenceQ0_16 = 0;
      s->flags = 0;
      s->lastUpdateMs = Util::bootMS();
      s->canAcceptMore = 1;
      s->endPublish();
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

/// Node-wide pressure sampled by the Controller and consumed by all InputBuffers.
/// PSI values are fractions of the last sampling interval. On platforms without
/// PSI, cpuUseQ0_16 is still populated and NODE_PRESSURE_HAS_PSI is clear.
struct NodePressureState {
    uint32_t schemaVersion;
    uint32_t structSize;
    uint32_t publishSeq;
    uint32_t _pad0;
    uint64_t lastUpdateMs;
    uint16_t flags;
    uint16_t cpuUseQ0_16;
    uint16_t cpuSomeQ0_16;
    uint16_t memorySomeQ0_16;
    uint16_t memoryFullQ0_16;
    uint16_t ioSomeQ0_16;
    uint16_t ioFullQ0_16;
    uint16_t _pad1;

    static void initPage(IPC::sharedPage & page) {
      if (!page.mapped || page.len < sizeof(NodePressureState)) { return; }
      memset(page.mapped, 0, sizeof(NodePressureState));
      NodePressureState *s = (NodePressureState *)page.mapped;
      s->schemaVersion = NODE_PRESSURE_VERSION;
      s->structSize = (uint32_t)sizeof(NodePressureState);
    }

    static bool isValid(const IPC::sharedPage & page) {
      if (!page.mapped || page.len < sizeof(NodePressureState)) { return false; }
      const NodePressureState *s = (const NodePressureState *)page.mapped;
      return s->schemaVersion == NODE_PRESSURE_VERSION && s->structSize >= sizeof(NodePressureState);
    }

    void beginPublish() {
      uint32_t seq = __atomic_load_n(&publishSeq, __ATOMIC_RELAXED);
      if (seq & 1) { ++seq; }
      __atomic_store_n(&publishSeq, seq + 1, __ATOMIC_RELEASE);
    }

    void endPublish() {
      uint32_t seq = __atomic_load_n(&publishSeq, __ATOMIC_RELAXED);
      if (!(seq & 1)) { ++seq; }
      __atomic_store_n(&publishSeq, seq + 1, __ATOMIC_RELEASE);
    }

    static bool readSnapshot(const IPC::sharedPage & page, NodePressureState & out) {
      if (!isValid(page)) { return false; }
      const NodePressureState *s = (const NodePressureState *)page.mapped;
      for (size_t attempt = 0; attempt < 3; ++attempt) {
        uint32_t before = __atomic_load_n(&s->publishSeq, __ATOMIC_ACQUIRE);
        if (before & 1) { continue; }
        memcpy(&out, s, sizeof(NodePressureState));
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        uint32_t after = __atomic_load_n(&s->publishSeq, __ATOMIC_ACQUIRE);
        if (before == after && !(after & 1)) { return true; }
      }
      return false;
    }

    /// 0 = allow ramp, 1 = hold, 2 = slow down.
    uint8_t cpuVerdict() const {
      if (flags & NODE_PRESSURE_HAS_PSI) {
        if (cpuSomeQ0_16 >= (uint16_t)(0.25 * 65535.0)) { return 2; }
        if (cpuSomeQ0_16 >= (uint16_t)(0.10 * 65535.0)) { return 1; }
        return 0;
      }
      if (cpuUseQ0_16 >= (uint16_t)(0.95 * 65535.0)) { return 2; }
      if (cpuUseQ0_16 >= (uint16_t)(0.85 * 65535.0)) { return 1; }
      return 0;
    }
};

/// Minimal contract publisher for MistProc implementations that do not yet
/// have workload-specific measurements. It creates the PID page immediately,
/// publishes a conservative startup seed, and keeps the contract fresh.
class ProcStateHeartbeat {
    IPC::sharedPage page;
    std::atomic<bool> active;
    std::thread worker;
    std::mutex publishMutex;

  public:
    ProcStateHeartbeat() : active(true) {
      char name[NAME_BUFFER_SIZE];
      snprintf(name, sizeof(name), SHM_PROC_STATE, getpid());
      page.init(name, sizeof(ProcState), true, false);
      ProcState::initPage(page);
      page.master = false;
      worker = std::thread([this]() {
        while (active.load(std::memory_order_acquire)) {
          if (ProcState::isValid(page)) {
            std::lock_guard<std::mutex> guard(publishMutex);
            ProcState *s = (ProcState *)page.mapped;
            s->beginPublish();
            s->lastUpdateMs = Util::bootMS();
            s->endPublish();
          }
          for (size_t i = 0; i < 4 && active.load(std::memory_order_acquire); ++i) { Util::sleep(250); }
        }
      });
    }

    ~ProcStateHeartbeat() {
      active.store(false, std::memory_order_release);
      if (worker.joinable()) { worker.join(); }
      page.master = false;
    }

    void publishStartup(double speed = 1.0, ProcPrimaryResource resource = PRC_RESOURCE_UNKNOWN) {
      std::lock_guard<std::mutex> guard(publishMutex);
      ProcState::publishStartup(page, speed, resource);
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
        s->beginPublish();
        s->phase = PRC_PHASE_DRAINING;
        s->lastUpdateMs = Util::bootMS();
        if (isSet) {
          s->setExitReason(shortReason, longReason);
        } else {
          s->setExitReason(Util::mRExitReason, Util::exitReason);
        }
        s->endPublish();
        page.master = false;
      }
      return exitCode;
    }
};
