#include "../src/input/buffer_hold.h"
#include "../src/input/processing_rate.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace {
  int fail(const char *message) {
    fprintf(stderr, "%s\n", message);
    return 1;
  }

  void scan(Mist::BufferHoldTracker & t, size_t user, size_t track, uint64_t keyNum, uint64_t now) {
    t.beginScan();
    t.observe(user, 100 + user, track, keyNum, now);
    t.endScan(now);
  }
} // namespace

int main() {
  using namespace Mist;

  // Pin: a freshly selected recorder sits at key 0 and holds from the first key.
  BufferHoldTracker holds;
  uint64_t minKey = 99;
  scan(holds, 7, 1, 0, 1000);
  if (!holds.held(1, minKey) || minKey != 0) { return fail("a recorder waiting for its header must hold from key 0"); }
  if (keyRemovalAllowed(0, true, minKey)) { return fail("the first key must stay while a recorder has not read it"); }
  if (holds.held(2, minKey)) { return fail("a hold covers only the tracks the recorder reads"); }

  // Progress: keys before the recorder's position may go, its own key may not.
  scan(holds, 7, 1, 12, 2000);
  if (!holds.held(1, minKey) || minKey != 12) { return fail("the hold must follow the recorder's key"); }
  if (!keyRemovalAllowed(11, true, minKey) || keyRemovalAllowed(12, true, minKey) || keyRemovalAllowed(13, true, minKey)) {
    return fail("eviction must stop at the lowest key a recorder still needs");
  }
  if (!keyRemovalAllowed(500, false, 0)) { return fail("an unheld track evicts as before"); }

  // The slowest of several recorders decides.
  holds.beginScan();
  holds.observe(7, 107, 1, 30, 3000);
  holds.observe(8, 108, 1, 20, 3000);
  holds.endScan(3000);
  if (!holds.held(1, minKey) || minKey != 20) { return fail("the lowest key over all holding readers must win"); }

  // Release on disconnect: a record that is no longer scanned stops holding.
  scan(holds, 7, 1, 30, 4000);
  if (!holds.held(1, minKey) || minKey != 30) { return fail("a disconnected recorder must release its hold"); }
  holds.beginScan();
  holds.endScan(5000);
  if (holds.held(1, minKey) || holds.heldTracks().size()) { return fail("no scanned readers means no holds"); }

  // A reader waiting at the live point has consumed everything.
  scan(holds, 9, 1, (uint64_t)std::string::npos, 6000);
  if (holds.held(1, minKey)) { return fail("a reader at the live point must not hold the buffer"); }

  // Stale: a key position that does not move for the bound stops pinning, and
  // moving again restores the hold.
  BufferHoldTracker stale;
  scan(stale, 3, 1, 5, 10000);
  scan(stale, 3, 1, 5, 10000 + BUFFER_HOLD_STALE_MS);
  if (!stale.held(1, minKey)) { return fail("a hold within the stale bound must stay"); }
  scan(stale, 3, 1, 5, 10001 + BUFFER_HOLD_STALE_MS);
  if (stale.held(1, minKey)) { return fail("a reader stuck beyond the stale bound must release the buffer"); }
  scan(stale, 3, 1, 6, 10002 + BUFFER_HOLD_STALE_MS);
  if (!stale.held(1, minKey) || minKey != 6) { return fail("a stale reader that moves again must hold again"); }

  // A recorder waiting at the live point of a rendition is waiting on its
  // producer: its lead is the smallest over its tracks, so it does not pause
  // the feed that producer needs. A recorder behind on every track does.
  {
    std::map<size_t, uint64_t> waitingOnRendition;
    waitingOnRendition[0] = 30000; // source: behind by 30 s
    waitingOnRendition[2] = 0; // rendition: at its live point
    if (recorderLeadMs(waitingOnRendition) != 0) { return fail("a recorder waiting on a producer is not lagging"); }
    std::map<size_t, uint64_t> slowWriter;
    slowWriter[0] = 46000;
    slowWriter[1] = 45000;
    if (recorderLeadMs(slowWriter) != 45000) {
      return fail("a recorder behind on every track lags by its smallest lead");
    }
    BufferHoldTracker positions;
    positions.beginScan();
    positions.observe(1, 500, 0, 3, 1000);
    positions.observe(2, 500, 2, (uint64_t)std::string::npos, 1000);
    positions.endScan(1000);
    if (positions.positions().size() != 2 || !BufferHoldTracker::atLivePoint(positions.positions()[1].keyNum)) {
      return fail("live-point records must stay visible for the lead while holding no keys");
    }
    if (!positions.held(0, minKey) || positions.held(2, minKey)) {
      return fail("only records behind the live point hold keys");
    }
  }

  // Threshold: two target durations short of the buffer window.
  if (consumerHoldThreshold(50000, 3000) != 44000) { return fail("threshold must be bufferTime - 2x target"); }
  if (consumerHoldThreshold(10000, 3000) != 5000) { return fail("a window under 4 targets holds at half of it"); }

  // Hysteresis: hold at the threshold, release below half of it.
  ConsumerLagHold lag;
  if (lag.update(43999, 44000)) { return fail("below the threshold the feed runs"); }
  if (!lag.update(44000, 44000)) { return fail("at the threshold the feed must hold"); }
  if (!lag.update(30000, 44000) || !lag.update(22000, 44000)) {
    return fail("the hold must last until half the threshold");
  }
  if (lag.update(21999, 44000)) { return fail("below half the threshold the feed must resume"); }
  if (lag.update(30000, 44000)) { return fail("a resumed feed must not re-hold before the threshold"); }
  lag.update(50000, 44000);
  if (lag.update(50000, 0)) { return fail("without holding readers there is nothing to wait for"); }

  // The unconstrained ramp stops once the feed leads the recorder by the
  // threshold: simulate one-second ticks where the recorder writes at 4x and
  // the source advances at the effective speed.
  {
    ProcessingRateInput rate;
    ConsumerLagHold feedLag;
    uint64_t speed = 1, sourceMs = 0, readerMs = 0, maxSpeed = 0, maxLead = 0;
    const uint64_t threshold = consumerHoldThreshold(50000, 3000);
    bool everHeld = false;
    for (int tick = 0; tick < 120; ++tick) {
      rate = ProcessingRateInput();
      rate.current = speed;
      rate.target = PROCESSING_UNCONSTRAINED_SPEED;
      rate.freshVoteRound = true;
      rate.contractsReady = true;
      rate.consumerHold = feedLag.held();
      ProcessingRateResult r = decideProcessingRate(rate);
      if (feedLag.held() && r.speed > speed) { return fail("the ramp must not rise while the consumer hold is set"); }
      speed = r.speed;
      if (speed > maxSpeed) { maxSpeed = speed; }
      // The buffer re-evaluates the lead every loop; model it at 100 ms.
      for (int step = 0; step < 10; ++step) {
        if (!feedLag.held()) { sourceMs += speed * 100; }
        readerMs = std::min(sourceMs, readerMs + 400);
        uint64_t lead = sourceMs - readerMs;
        if (lead > maxLead) { maxLead = lead; }
        if (feedLag.update(lead, threshold)) { everHeld = true; }
      }
    }
    if (!everHeld) { return fail("a feed outrunning its recorder must reach the consumer hold"); }
    if (maxLead >= 50000) { return fail("the feed must never lead its recorder by the whole buffer window"); }
    if (maxSpeed > PROCESSING_UNCONSTRAINED_SPEED) {
      return fail("the ramp must stay under the unconstrained ceiling");
    }
  }

  return 0;
}
