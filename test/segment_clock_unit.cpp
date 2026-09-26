#include "../src/output/segment_clock.h"

#include <cstdio>

namespace {
  int fail(const char *message) {
    fprintf(stderr, "%s\n", message);
    return 1;
  }
} // namespace

// The dev-stack recording tail: segment 33 starts at 31.491 s and lasts 6 s;
// after the publisher's end-of-stream burst shifted the boot offset, the
// final segment mapped to 35.473 s, 2018 ms before segment 33 ended.
int main() {
  Mist::SegmentClock clock;
  const uint64_t base = 1790416591491ull;
  if (clock.stamp(base, 6000) != base) { return fail("the first segment keeps its mapped start"); }
  if (clock.stamp(base + 3982, 4344) != base + 6000) {
    return fail("a segment mapped before the previous end starts at that end");
  }
  // A real gap (the next segment maps after the previous end) is kept.
  if (clock.stamp(base + 20000, 6000) != base + 20000) { return fail("a forward gap is not closed"); }
  // No mapping is written as none and does not move the clock.
  if (clock.stamp(0, 6000) != 0) { return fail("an unmapped segment gets no start"); }
  if (clock.stamp(base + 26000, 6000) != base + 26000) { return fail("the clock moved on an unmapped segment"); }
  return 0;
}
