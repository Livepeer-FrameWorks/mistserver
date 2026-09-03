#include <mist/process_stream_state.h>

#include <array>
#include <cstdio>

namespace {
  int fail(const char *message) {
    fprintf(stderr, "%s\n", message);
    return 1;
  }
} // namespace

int main() {
  Mist::ProcessStreamState accumulated;
  Mist::ProcessStreamStateTick firstTick;
  firstTick.effectiveSpeed = 4;
  firstTick.hardSlow = true;
  firstTick.lockout = true;
  firstTick.staleHold = true;
  firstTick.warmup = true;
  firstTick.sourceLimited = true;
  firstTick.nodeLimited = true;
  firstTick.inputSpeedQ16 = 3 << 16;
  firstTick.outputSpeedQ16 = 2 << 16;
  firstTick.capacitySpeedQ16 = 5 << 16;
  accumulated.recordTick(firstTick);

  Mist::ProcessStreamStateTick secondTick;
  secondTick.effectiveSpeed = 8;
  secondTick.regularSlow = true;
  secondTick.ramped = true;
  secondTick.processorLimited = true;
  secondTick.inputSpeedQ16 = 7 << 16;
  secondTick.outputSpeedQ16 = 6 << 16;
  accumulated.recordTick(secondTick);
  if (accumulated.ticks != 2 || accumulated.speedMin != 4 || accumulated.speedMax != 8 || accumulated.speedSum != 12 ||
      accumulated.hardSlowTicks != 1 || accumulated.regularSlowTicks != 1 || accumulated.rampUps != 1 ||
      accumulated.lockoutTicks != 1 || accumulated.staleHoldTicks != 1 || accumulated.warmupTicks != 1 ||
      accumulated.sourceLimitedTicks != 1 || accumulated.processorLimitedTicks != 1 || accumulated.nodeLimitedTicks != 1 ||
      accumulated.inputSpeedSumQ16 != (10ULL << 16) || accumulated.outputSpeedSumQ16 != (8ULL << 16) ||
      accumulated.capacitySamples != 1 || accumulated.capacitySpeedSumQ16 != (5ULL << 16)) {
    return fail("processing diagnostic ticks must retain every structured rate and bottleneck aggregate");
  }

  std::array<char, STRMSTATE_PAGE_LEN> page;
  page.fill((char)0x5a);

  Mist::ProcessStreamState written;
  written.ticks = 7;
  written.speedMin = 1;
  written.speedMax = 8;
  written.hardSlowTicks = 2;
  written.regularSlowTicks = 3;
  written.rampUps = 4;
  written.lockoutTicks = 5;
  written.staleHoldTicks = 6;
  written.speedSum = 29;
  written.warmupTicks = 9;
  written.sourceLimitedTicks = 10;
  written.processorLimitedTicks = 11;
  written.nodeLimitedTicks = 12;
  written.capacitySamples = 13;
  written.inputSpeedSumQ16 = 14ULL << 16;
  written.outputSpeedSumQ16 = 15ULL << 16;
  written.capacitySpeedSumQ16 = 16ULL << 16;

  page[STRMSTATE_PROCESS_SOURCE_EOF_OFFSET] = 1;
  page[STRMSTATE_PROCESS_PRODUCERS_FINISHED_OFFSET] = 1;
  if (!written.writeStatistics(page.data(), page.size())) {
    return fail("full stream-state pages must accept processing statistics");
  }
  if (page[STRMSTATE_PROCESS_SOURCE_EOF_OFFSET] != 1) {
    return fail("statistics publication must not overwrite the producer-EOF marker");
  }
  if (page[STRMSTATE_PROCESS_PRODUCERS_FINISHED_OFFSET] != 1) {
    return fail("statistics publication must not overwrite the processor-completion marker");
  }

  Mist::ProcessStreamState read;
  if (!read.read(page.data(), page.size()) || !read.sourceEof || !read.processProducersFinished ||
      read.ticks != written.ticks || read.speedMin != written.speedMin || read.speedMax != written.speedMax ||
      read.hardSlowTicks != written.hardSlowTicks || read.regularSlowTicks != written.regularSlowTicks ||
      read.rampUps != written.rampUps || read.lockoutTicks != written.lockoutTicks ||
      read.staleHoldTicks != written.staleHoldTicks || read.speedSum != written.speedSum ||
      read.warmupTicks != written.warmupTicks || read.sourceLimitedTicks != written.sourceLimitedTicks ||
      read.processorLimitedTicks != written.processorLimitedTicks || read.nodeLimitedTicks != written.nodeLimitedTicks ||
      read.capacitySamples != written.capacitySamples || read.inputSpeedSumQ16 != written.inputSpeedSumQ16 ||
      read.outputSpeedSumQ16 != written.outputSpeedSumQ16 || read.capacitySpeedSumQ16 != written.capacitySpeedSumQ16) {
    return fail("processing statistics must round-trip through the shared stream-state layout");
  }

  std::array<char, STRMSTATE_PAGE_LEN - 1> oldPage;
  oldPage.fill(0);
  if (written.writeStatistics(oldPage.data(), oldPage.size()) || read.read(oldPage.data(), oldPage.size())) {
    return fail("readers and writers must reject stream-state pages without the complete diagnostics block");
  }
  return 0;
}
