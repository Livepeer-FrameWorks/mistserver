#pragma once

#include <cstdint>
#include <map>

namespace Mist {
  class PacketDropLogLimiter {
    public:
      bool shouldLog(uint32_t track, uint64_t nowMs, uint64_t intervalMs = 10000) {
        std::map<uint32_t, uint64_t>::iterator previous = lastLogMs.find(track);
        if (previous == lastLogMs.end()) {
          lastLogMs[track] = nowMs;
          return true;
        }
        if (nowMs >= previous->second && nowMs - previous->second < intervalMs) { return false; }
        previous->second = nowMs;
        return true;
      }

    private:
      std::map<uint32_t, uint64_t> lastLogMs;
  };
} // namespace Mist
