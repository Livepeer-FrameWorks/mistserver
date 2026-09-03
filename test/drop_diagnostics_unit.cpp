#include "../src/packet_drop_limiter.h"
#include "../src/process/livepeer_diagnostics.h"

#include <iostream>

namespace {
  int fail(const char *message) {
    std::cerr << message << std::endl;
    return 1;
  }
} // namespace

int main() {
  Mist::PacketDropLogLimiter limiter;
  if (!limiter.shouldLog(7, 0) || limiter.shouldLog(7, 0) || limiter.shouldLog(7, 9999) || !limiter.shouldLog(7, 10000) ||
      !limiter.shouldLog(8, 10000) || !limiter.shouldLog(7, 3) || limiter.shouldLog(7, 4)) {
    return fail("packet-drop warnings must be independently rate-limited per track and survive clock rollback");
  }

  if (Mist::livepeerSegmentParseExhausted(9, 10, false) || Mist::livepeerSegmentParseExhausted(10, 10, true) ||
      !Mist::livepeerSegmentParseExhausted(10, 10, false) || !Mist::livepeerSegmentParseExhausted(11, 10, false)) {
    return fail("Livepeer must discard a segment only after parsing consumed all bytes and has no queued packet");
  }
  if (!Mist::livepeerSegmentProducedNoPackets(true, false) || Mist::livepeerSegmentProducedNoPackets(false, false) ||
      Mist::livepeerSegmentProducedNoPackets(true, true)) {
    return fail("Livepeer must diagnose only exhausted segments that never established packet timing");
  }

  return 0;
}
