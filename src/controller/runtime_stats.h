#pragma once

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <istream>
#include <string>

namespace Controller {
  namespace RuntimeStats {
    struct PsiTotals {
        uint64_t some = 0;
        uint64_t full = 0;
        bool hasSome = false;
        bool hasFull = false;
    };

    inline PsiTotals parsePsiTotals(std::istream & input) {
      PsiTotals result;
      std::string line;
      while (std::getline(input, line)) {
        const size_t position = line.find("total=");
        if (position == std::string::npos) { continue; }
        const char *number = line.c_str() + position + 6;
        char *end = 0;
        errno = 0;
        const unsigned long long total = strtoull(number, &end, 10);
        if (errno || end == number || (*end && *end != ' ' && *end != '\t')) { continue; }
        if (line.rfind("some ", 0) == 0) {
          result.some = total;
          result.hasSome = true;
        } else if (line.rfind("full ", 0) == 0) {
          result.full = total;
          result.hasFull = true;
        }
      }
      return result;
    }

    struct PsiSample {
        uint64_t timeMs = 0;
        PsiTotals cpu;
        PsiTotals memory;
        PsiTotals io;

        bool complete() const { return cpu.hasSome && memory.hasSome && memory.hasFull && io.hasSome && io.hasFull; }
    };

    struct PsiRatios {
        uint16_t cpuSome = 0;
        uint16_t memorySome = 0;
        uint16_t memoryFull = 0;
        uint16_t ioSome = 0;
        uint16_t ioFull = 0;
    };

    inline uint16_t pressureRatioQ0_16(uint64_t current, uint64_t previous, uint64_t intervalUs) {
      if (!intervalUs || current < previous) { return 0; }
      const uint64_t delta = current - previous;
      if (delta >= intervalUs) { return 65535; }
      return (uint16_t)((long double)delta * 65535.0L / (long double)intervalUs);
    }

    class PsiDeltaTracker {
      public:
        bool update(const PsiSample & sample, PsiRatios & result) {
          result = PsiRatios();
          if (!sample.complete()) {
            haveBaseline = false;
            return false;
          }
          if (!haveBaseline || sample.timeMs <= previous.timeMs || !monotonic(sample)) {
            previous = sample;
            haveBaseline = true;
            return false;
          }
          const uint64_t intervalUs = (sample.timeMs - previous.timeMs) * 1000;
          result.cpuSome = pressureRatioQ0_16(sample.cpu.some, previous.cpu.some, intervalUs);
          result.memorySome = pressureRatioQ0_16(sample.memory.some, previous.memory.some, intervalUs);
          result.memoryFull = pressureRatioQ0_16(sample.memory.full, previous.memory.full, intervalUs);
          result.ioSome = pressureRatioQ0_16(sample.io.some, previous.io.some, intervalUs);
          result.ioFull = pressureRatioQ0_16(sample.io.full, previous.io.full, intervalUs);
          previous = sample;
          return true;
        }

      private:
        bool monotonic(const PsiSample & sample) const {
          return sample.cpu.some >= previous.cpu.some && sample.memory.some >= previous.memory.some &&
            sample.memory.full >= previous.memory.full && sample.io.some >= previous.io.some &&
            sample.io.full >= previous.io.full;
        }

        bool haveBaseline = false;
        PsiSample previous;
    };

    inline uint16_t cpuUsePermille(uint64_t previousTotal, uint64_t previousIdle, uint64_t currentTotal,
                                   uint64_t currentIdle, uint16_t fallback) {
      if (!previousTotal || currentTotal <= previousTotal || currentIdle < previousIdle) { return fallback; }
      const uint64_t totalDelta = currentTotal - previousTotal;
      const uint64_t idleDelta = currentIdle - previousIdle;
      if (idleDelta > totalDelta) { return fallback; }
      const uint64_t busyDelta = totalDelta - idleDelta;
      return (uint16_t)((long double)busyDelta * 1000.0L / (long double)totalDelta);
    }

    inline uint64_t usedMemoryMiB(uint64_t total, uint64_t free, uint64_t cache) {
      return free >= total || cache >= total - free ? 0 : total - free - cache;
    }
  } // namespace RuntimeStats
} // namespace Controller
