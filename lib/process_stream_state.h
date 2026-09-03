#pragma once

#include <mist/defines.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace Mist {
  struct ProcessStreamStateTick {
      uint64_t effectiveSpeed = 0;
      bool hardSlow = false;
      bool regularSlow = false;
      bool ramped = false;
      bool lockout = false;
      bool staleHold = false;
      bool warmup = false;
      bool sourceLimited = false;
      bool processorLimited = false;
      bool nodeLimited = false;
      uint32_t inputSpeedQ16 = 0;
      uint32_t outputSpeedQ16 = 0;
      uint32_t capacitySpeedQ16 = 0;
  };

  /// Process lifecycle and rate-controller diagnostics carried in
  /// SHM_STREAM_STATE. Status and startup bytes remain owned by InputBuffer.
  struct ProcessStreamState {
      bool sourceEof = false;
      bool processProducersFinished = false;
      uint32_t ticks = 0;
      uint32_t speedMin = 0;
      uint32_t speedMax = 0;
      uint32_t hardSlowTicks = 0;
      uint32_t regularSlowTicks = 0;
      uint32_t rampUps = 0;
      uint32_t lockoutTicks = 0;
      uint32_t staleHoldTicks = 0;
      uint64_t speedSum = 0;
      uint32_t warmupTicks = 0;
      uint32_t sourceLimitedTicks = 0;
      uint32_t processorLimitedTicks = 0;
      uint32_t nodeLimitedTicks = 0;
      uint32_t capacitySamples = 0;
      uint64_t inputSpeedSumQ16 = 0;
      uint64_t outputSpeedSumQ16 = 0;
      uint64_t capacitySpeedSumQ16 = 0;

      void recordTick(const ProcessStreamStateTick & tick) {
        ++ticks;
        speedSum += tick.effectiveSpeed;
        if (!speedMin || tick.effectiveSpeed < speedMin) { speedMin = tick.effectiveSpeed; }
        if (tick.effectiveSpeed > speedMax) { speedMax = tick.effectiveSpeed; }
        if (tick.hardSlow) { ++hardSlowTicks; }
        if (tick.regularSlow) { ++regularSlowTicks; }
        if (tick.ramped) { ++rampUps; }
        if (tick.lockout) { ++lockoutTicks; }
        if (tick.staleHold) { ++staleHoldTicks; }
        if (tick.warmup) { ++warmupTicks; }
        if (tick.sourceLimited) { ++sourceLimitedTicks; }
        if (tick.processorLimited) { ++processorLimitedTicks; }
        if (tick.nodeLimited) { ++nodeLimitedTicks; }
        inputSpeedSumQ16 += tick.inputSpeedQ16;
        outputSpeedSumQ16 += tick.outputSpeedQ16;
        if (tick.capacitySpeedQ16) {
          capacitySpeedSumQ16 += tick.capacitySpeedQ16;
          ++capacitySamples;
        }
      }

      bool read(const char *page, size_t length) {
        if (!page || length < STRMSTATE_PAGE_LEN) { return false; }
        sourceEof = page[STRMSTATE_PROCESS_SOURCE_EOF_OFFSET];
        processProducersFinished = page[STRMSTATE_PROCESS_PRODUCERS_FINISHED_OFFSET];
        copyFrom(page, STRMSTATE_SPEED_TICKS_OFFSET, ticks);
        copyFrom(page, STRMSTATE_SPEED_MIN_OFFSET, speedMin);
        copyFrom(page, STRMSTATE_SPEED_MAX_OFFSET, speedMax);
        copyFrom(page, STRMSTATE_HARD_SLOW_TICKS_OFFSET, hardSlowTicks);
        copyFrom(page, STRMSTATE_REGULAR_SLOW_TICKS_OFFSET, regularSlowTicks);
        copyFrom(page, STRMSTATE_RAMP_UPS_OFFSET, rampUps);
        copyFrom(page, STRMSTATE_LOCKOUT_TICKS_OFFSET, lockoutTicks);
        copyFrom(page, STRMSTATE_STALE_HOLD_TICKS_OFFSET, staleHoldTicks);
        copyFrom(page, STRMSTATE_SPEED_SUM_OFFSET, speedSum);
        copyFrom(page, STRMSTATE_WARMUP_TICKS_OFFSET, warmupTicks);
        copyFrom(page, STRMSTATE_SOURCE_LIMITED_TICKS_OFFSET, sourceLimitedTicks);
        copyFrom(page, STRMSTATE_PROCESSOR_LIMITED_TICKS_OFFSET, processorLimitedTicks);
        copyFrom(page, STRMSTATE_NODE_LIMITED_TICKS_OFFSET, nodeLimitedTicks);
        copyFrom(page, STRMSTATE_CAPACITY_SAMPLES_OFFSET, capacitySamples);
        copyFrom(page, STRMSTATE_INPUT_SPEED_SUM_OFFSET, inputSpeedSumQ16);
        copyFrom(page, STRMSTATE_OUTPUT_SPEED_SUM_OFFSET, outputSpeedSumQ16);
        copyFrom(page, STRMSTATE_CAPACITY_SPEED_SUM_OFFSET, capacitySpeedSumQ16);
        return true;
      }

      bool writeStatistics(char *page, size_t length) const {
        if (!page || length < STRMSTATE_PAGE_LEN) { return false; }
        copyTo(page, STRMSTATE_SPEED_TICKS_OFFSET, ticks);
        copyTo(page, STRMSTATE_SPEED_MIN_OFFSET, speedMin);
        copyTo(page, STRMSTATE_SPEED_MAX_OFFSET, speedMax);
        copyTo(page, STRMSTATE_HARD_SLOW_TICKS_OFFSET, hardSlowTicks);
        copyTo(page, STRMSTATE_REGULAR_SLOW_TICKS_OFFSET, regularSlowTicks);
        copyTo(page, STRMSTATE_RAMP_UPS_OFFSET, rampUps);
        copyTo(page, STRMSTATE_LOCKOUT_TICKS_OFFSET, lockoutTicks);
        copyTo(page, STRMSTATE_STALE_HOLD_TICKS_OFFSET, staleHoldTicks);
        copyTo(page, STRMSTATE_SPEED_SUM_OFFSET, speedSum);
        copyTo(page, STRMSTATE_WARMUP_TICKS_OFFSET, warmupTicks);
        copyTo(page, STRMSTATE_SOURCE_LIMITED_TICKS_OFFSET, sourceLimitedTicks);
        copyTo(page, STRMSTATE_PROCESSOR_LIMITED_TICKS_OFFSET, processorLimitedTicks);
        copyTo(page, STRMSTATE_NODE_LIMITED_TICKS_OFFSET, nodeLimitedTicks);
        copyTo(page, STRMSTATE_CAPACITY_SAMPLES_OFFSET, capacitySamples);
        copyTo(page, STRMSTATE_INPUT_SPEED_SUM_OFFSET, inputSpeedSumQ16);
        copyTo(page, STRMSTATE_OUTPUT_SPEED_SUM_OFFSET, outputSpeedSumQ16);
        copyTo(page, STRMSTATE_CAPACITY_SPEED_SUM_OFFSET, capacitySpeedSumQ16);
        return true;
      }

    private:
      template<typename T> static void copyFrom(const char *page, size_t offset, T & value) {
        memcpy(&value, page + offset, sizeof(T));
      }
      template<typename T> static void copyTo(char *page, size_t offset, const T & value) {
        memcpy(page + offset, &value, sizeof(T));
      }
  };
} // namespace Mist
