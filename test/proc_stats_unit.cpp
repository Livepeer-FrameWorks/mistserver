#include "../src/input/processing_rate.h"

#include <mist/proc_stats.h>

#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace {
  int fail(const char *message) {
    fprintf(stderr, "%s\n", message);
    return 1;
  }
} // namespace

int main() {
  char pageName[NAME_BUFFER_SIZE];
  snprintf(pageName, sizeof(pageName), "/MstProcStateTest_%d", getpid());
  IPC::sharedPage page(pageName, sizeof(ProcState), true, false);
  if (!page.mapped) { return fail("could not create ProcState test page"); }
  ProcState::initPage(page);
  if (!ProcState::isValid(page)) { return fail("fresh ProcState page is invalid"); }

  ProcState::publishStartup(page, 8.0, PRC_RESOURCE_CPU);
  ProcState snapshot;
  if (!ProcState::readSnapshot(page, snapshot)) { return fail("could not read startup snapshot"); }
  if (snapshot.phase != PRC_PHASE_STARTUP || snapshot.primaryResource != PRC_RESOURCE_CPU ||
      snapshot.recommendedFeedQ16_16 != ProcState::speedToQ16(8.0)) {
    return fail("startup contract did not round-trip");
  }

  ProcState *writer = (ProcState *)page.mapped;
  writer->beginPublish();
  if (ProcState::readSnapshot(page, snapshot)) { return fail("reader accepted an in-progress snapshot"); }
  writer->capacitySpeedQ16_16 = ProcState::speedToQ16(12.5);
  writer->flags |= PRC_FLAG_CAPACITY_VALID;
  writer->endPublish();
  if (!ProcState::readSnapshot(page, snapshot) || snapshot.capacitySpeedQ16_16 != ProcState::speedToQ16(12.5)) {
    return fail("completed snapshot did not round-trip");
  }

  NodePressureState node;
  memset(&node, 0, sizeof(node));
  node.flags = NODE_PRESSURE_HAS_PSI;
  node.cpuSomeQ0_16 = (uint16_t)(0.09 * 65535.0);
  if (node.cpuVerdict() != 0) { return fail("low PSI should permit ramping"); }
  node.cpuSomeQ0_16 = (uint16_t)(0.15 * 65535.0);
  if (node.cpuVerdict() != 1) { return fail("medium PSI should hold"); }
  node.cpuSomeQ0_16 = (uint16_t)(0.30 * 65535.0);
  if (node.cpuVerdict() != 2) { return fail("high PSI should slow down"); }

  memset(&node, 0, sizeof(node));
  node.cpuUseQ0_16 = (uint16_t)(0.90 * 65535.0);
  if (node.cpuVerdict() != 1) { return fail("fallback CPU threshold should hold"); }
  node.cpuUseQ0_16 = (uint16_t)(0.97 * 65535.0);
  if (node.cpuVerdict() != 2) { return fail("fallback CPU threshold should slow down"); }

  if (Mist::classifyProcFeedVote(PRC_FLAG_SOURCE_LIMITED, PRC_REASON_SOURCE_WAIT, 0, 65535) != Mist::PROC_FEED_ALLOW) {
    return fail("source starvation must not score as processor pressure");
  }
  if (Mist::classifyProcFeedVote(PRC_FLAG_SOURCE_LIMITED, PRC_REASON_RETRY, 0, 65535) != Mist::PROC_FEED_HARD_LOCKOUT) {
    return fail("retry pressure must override source starvation");
  }

  // A source-limited sample remains eligible for the next bounded ramp.
  Mist::ProcessingRateInput rate;
  rate.target = 8;
  Mist::ProcessingRateResult decision = Mist::decideProcessingRate(rate);
  if (decision.speed != 8 || decision.ramped) {
    return fail("the first complete contract should apply its startup seed immediately");
  }

  rate = Mist::ProcessingRateInput();
  rate.current = 4;
  rate.target = 12;
  rate.freshVoteRound = true;
  rate.contractsReady = true;
  decision = Mist::decideProcessingRate(rate);
  if (decision.speed != 6 || !decision.ramped) {
    return fail("source-limited samples should remain eligible for a bounded ramp");
  }

  rate.current = 10;
  rate.target = 20;
  rate.regularSlow = true;
  decision = Mist::decideProcessingRate(rate);
  if (decision.speed != 8 || decision.ramped) { return fail("processor pressure should back off by 20 percent"); }

  rate = Mist::ProcessingRateInput();
  rate.current = 10;
  rate.target = 20;
  rate.hardSlow = true;
  decision = Mist::decideProcessingRate(rate);
  if (decision.speed != 1) { return fail("hard proc pressure should return to realtime"); }

  rate = Mist::ProcessingRateInput();
  rate.current = 6;
  rate.target = 20;
  rate.freshVoteRound = true;
  rate.contractsReady = true;
  rate.nodeHold = true;
  decision = Mist::decideProcessingRate(rate);
  if (decision.speed != 6 || decision.ramped) { return fail("node pressure hold should block a ramp"); }

  return 0;
}
