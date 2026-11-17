#include "../src/process/process_av_progress.h"

#include <iostream>

namespace {
  int failures = 0;

  void expect(uint64_t actual, uint64_t wanted, const char *message) {
    if (actual != wanted) {
      std::cerr << message << ": got " << actual << ", expected " << wanted << '\n';
      ++failures;
    }
  }
} // namespace

int main() {
  Mist::AVMediaProgress live;
  expect(live.total(), 0, "a processor that has not seen media has no billable progress");
  live.observe(18332277976ULL);
  expect(live.total(), 0, "joining a live audio timeline must not bill its 212-day timestamp offset");
  live.observe(18332278976ULL);
  live.observe(18332285976ULL);
  expect(live.total(), 8000, "eight seconds of actual advancement on an offset timeline bills eight seconds");
  expect(live.total(), 8000, "an idle reporting tick must not add media work");

  Mist::AVMediaProgress vod;
  vod.observe(0);
  vod.observe(40);
  vod.observe(80);
  expect(vod.total(), 80, "timestamp zero is a real first packet, not an uninitialized sentinel");

  Mist::AVMediaProgress resumed;
  resumed.observe(53918648);
  resumed.observe(53919648);
  resumed.observe(100);
  expect(resumed.total(), 1000, "a timeline regression cannot underflow the unsigned advancement counter");
  resumed.observe(1100);
  expect(resumed.total(), 2000, "media following a timeline regression resumes from its new baseline");

  Mist::AVMediaProgress fast;
  fast.observe(900000);
  fast.observe(960000);
  expect(fast.total(), 60000, "fast VOD processing keeps real media duration without a wall-clock clamp");

  Mist::AVMediaProgress source, sink;
  source.observe(100000);
  source.observe(104000);
  expect(sink.total(), 0, "an output without a first packet has no progress");
  sink.observe(102000);
  sink.observe(104000);
  expect(source.total(), 4000, "input progress is independent of the output timeline origin");
  expect(sink.total(), 2000, "the delayed output establishes its own first-packet baseline");
  return failures ? 1 : 0;
}
