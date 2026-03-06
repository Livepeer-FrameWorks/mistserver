#pragma once
#include <cstdint>

#define SHM_PROC_STATS "/MstProcStats_%d" // format with PID

/// Generic process timing stats for rate control.
/// All times in microseconds, cumulative since process start.
/// InputBuffer reads these to determine processing headroom.
struct ProcTimingStats{
  uint64_t totalWork;        // cumulative active processing time (decode+encode+transform+inference+etc)
  uint64_t totalSourceSleep; // cumulative time waiting for input data (KEY rate control signal)
  uint64_t totalSinkSleep;   // cumulative time waiting to write output
  uint64_t frameCount;       // frames/items processed
  uint64_t lastUpdateMs;     // Util::bootMS() of last write
};
