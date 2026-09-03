#pragma once

#include <cstddef>
#include <cstdint>

namespace Mist {
  class LookaheadWaitDiagnostics {
    public:
      bool shouldLog(size_t track, uint64_t nowMs, uint64_t intervalMs = 5000) {
        if (!waiting || waitingTrack != track) {
          waiting = true;
          waitingTrack = track;
          waitSinceMs = nowMs;
          lastLogMs = nowMs;
          return false;
        }
        if (nowMs < lastLogMs) {
          waitSinceMs = nowMs;
          lastLogMs = nowMs;
          return false;
        }
        if (nowMs - lastLogMs < intervalMs) { return false; }
        lastLogMs = nowMs;
        return true;
      }

      uint64_t elapsedMs(uint64_t nowMs) const {
        if (!waiting || nowMs < waitSinceMs) { return 0; }
        return nowMs - waitSinceMs;
      }

      void clear() { waiting = false; }
      bool active() const { return waiting; }
      size_t track() const { return waitingTrack; }

    private:
      bool waiting = false;
      size_t waitingTrack = 0;
      uint64_t waitSinceMs = 0;
      uint64_t lastLogMs = 0;
  };
} // namespace Mist
