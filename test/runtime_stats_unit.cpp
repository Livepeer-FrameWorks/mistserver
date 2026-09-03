#include "../src/controller/runtime_stats.h"

#include <cstdio>
#include <sstream>

namespace {
  int fail(const char *message) {
    fprintf(stderr, "%s\n", message);
    return 1;
  }

  Controller::RuntimeStats::PsiTotals parse(const char *text) {
    std::istringstream input(text);
    return Controller::RuntimeStats::parsePsiTotals(input);
  }

  Controller::RuntimeStats::PsiSample sample(uint64_t timeMs, uint64_t base) {
    Controller::RuntimeStats::PsiSample result;
    result.timeMs = timeMs;
    result.cpu = parse(("some avg10=0.00 avg60=0.00 avg300=0.00 total=" + std::to_string(base + 100) + "\n").c_str());
    result.memory =
      parse(("some avg10=0.00 total=" + std::to_string(base + 200) + "\nfull avg10=0.00 total=" + std::to_string(base + 300) + "\n")
              .c_str());
    result.io =
      parse(("full avg10=0.00 total=" + std::to_string(base + 500) + "\nsome avg10=0.00 total=" + std::to_string(base + 400) + "\n")
              .c_str());
    return result;
  }
} // namespace

int main() {
  using namespace Controller::RuntimeStats;

  PsiTotals parsed = parse("some avg10=0.10 avg60=0.20 total=123456\nfull avg10=0.01 total=789\n");
  if (!parsed.hasSome || !parsed.hasFull || parsed.some != 123456 || parsed.full != 789) {
    return fail("Linux PSI totals were not parsed exactly");
  }
  parsed = parse("some avg10=0.10 total=broken\nfull avg10=0.01 total=42garbage\n");
  if (parsed.hasSome || parsed.hasFull) { return fail("malformed PSI totals were accepted"); }

  PsiDeltaTracker tracker;
  PsiRatios ratios;
  if (tracker.update(sample(1000, 0), ratios)) { return fail("the first PSI sample must establish a baseline"); }
  if (!tracker.update(sample(2000, 100000), ratios) || ratios.cpuSome != 6553 || ratios.memorySome != 6553 ||
      ratios.memoryFull != 6553 || ratios.ioSome != 6553 || ratios.ioFull != 6553) {
    return fail("PSI counter deltas were not normalized over the sampling interval");
  }
  PsiSample incomplete = sample(3000, 200000);
  incomplete.memory.hasFull = false;
  if (tracker.update(incomplete, ratios)) { return fail("an incomplete PSI sample was published"); }
  if (tracker.update(sample(4000, 300000), ratios)) {
    return fail("a sample after a read failure must rebuild its baseline instead of publishing a spike");
  }
  if (!tracker.update(sample(5000, 400000), ratios) || ratios.cpuSome != 6553) {
    return fail("PSI publication did not recover after rebuilding its baseline");
  }
  if (tracker.update(sample(6000, 10), ratios)) {
    return fail("rolled-back PSI counters must rebuild their baseline instead of publishing");
  }
  if (pressureRatioQ0_16(2000000, 0, 1000000) != 65535) {
    return fail("PSI ratios above one second per second were not saturated");
  }

  if (cpuUsePermille(1000, 400, 2000, 650, 17) != 750 || cpuUsePermille(0, 0, 100, 10, 17) != 17 ||
      cpuUsePermille(1000, 400, 900, 300, 17) != 17 || cpuUsePermille(1000, 400, 2000, 1500, 17) != 17) {
    return fail("platform CPU tick accounting did not preserve or calculate utilization correctly");
  }
  if (usedMemoryMiB(16000, 2000, 3000) != 11000 || usedMemoryMiB(100, 80, 30) != 0) {
    return fail("platform memory accounting underflowed or returned the wrong used amount");
  }
  return 0;
}
