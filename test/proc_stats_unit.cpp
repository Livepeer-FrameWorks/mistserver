#include "../src/input/processing_rate.h"

#include <mist/proc_stats.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>

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

  ProcState *writer = (ProcState *)page.mapped;
  writer->schemaVersion++;
  if (ProcState::isValid(page)) { return fail("a mismatched ProcState version was accepted"); }
  writer->schemaVersion = PROC_STATE_VERSION;
  writer->structSize = sizeof(ProcState) - 1;
  if (ProcState::isValid(page)) { return fail("an undersized ProcState schema was accepted"); }
  writer->structSize = sizeof(ProcState);

  if (ProcState::speedToQ16(-1.0) != 0 || ProcState::speedToQ16(0.0) != 0 || ProcState::speedToQ16(1.5) != 98304 ||
      ProcState::speedToQ16(70000.0) != 0xFFFFFFFFu) {
    return fail("processing speed conversion did not clamp or encode correctly");
  }

  ProcState::publishStartup(page, 0.5, PRC_RESOURCE_CPU);
  ProcState snapshot;
  if (!ProcState::readSnapshot(page, snapshot)) { return fail("could not read startup snapshot"); }
  if (snapshot.phase != PRC_PHASE_STARTUP || snapshot.primaryResource != PRC_RESOURCE_CPU ||
      snapshot.recommendedFeedQ16_16 != ProcState::speedToQ16(1.0)) {
    return fail("startup contract did not round-trip");
  }

  ProcState::publishOutputContract(page, 2, PRC_INPUT_VIDEO);
  if (!ProcState::readSnapshot(page, snapshot) || !(snapshot.flags & PRC_FLAG_OUTPUT_CONTRACT_VALID) ||
      snapshot.expectedOutputTracks != 2 || snapshot.inputModality != PRC_INPUT_VIDEO) {
    return fail("process output contract did not round-trip");
  }
  ProcState::publishStartup(page, 1.0, PRC_RESOURCE_GPU);
  if (!ProcState::readSnapshot(page, snapshot) || !(snapshot.flags & PRC_FLAG_OUTPUT_CONTRACT_VALID) ||
      snapshot.expectedOutputTracks != 2 || snapshot.inputModality != PRC_INPUT_VIDEO) {
    return fail("startup publication discarded the resolved output contract");
  }

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

  char nodePageName[NAME_BUFFER_SIZE];
  snprintf(nodePageName, sizeof(nodePageName), "/MstNodePressureTest_%d", getpid());
  IPC::sharedPage nodePage(nodePageName, sizeof(NodePressureState), true, false);
  if (!nodePage.mapped) { return fail("could not create NodePressureState test page"); }
  NodePressureState::initPage(nodePage);
  NodePressureState *nodeWriter = (NodePressureState *)nodePage.mapped;
  nodeWriter->beginPublish();
  if (NodePressureState::readSnapshot(nodePage, node)) {
    return fail("node-pressure reader accepted an in-progress snapshot");
  }
  nodeWriter->cpuUseQ0_16 = 12345;
  nodeWriter->lastUpdateMs = 67890;
  nodeWriter->endPublish();
  if (!NodePressureState::readSnapshot(nodePage, node) || node.cpuUseQ0_16 != 12345 || node.lastUpdateMs != 67890) {
    return fail("completed node-pressure snapshot did not round-trip");
  }

  if (Mist::classifyProcFeedVote(0, PRC_REASON_UNKNOWN, 1, 0) != Mist::PROC_FEED_ALLOW ||
      Mist::classifyProcFeedVote(0, PRC_REASON_CPU, 1, (uint16_t)(0.8 * 65535.0)) != Mist::PROC_FEED_SLOW ||
      Mist::classifyProcFeedVote(0, PRC_REASON_CPU, 0, 0) != Mist::PROC_FEED_HARD ||
      Mist::classifyProcFeedVote(0, PRC_REASON_QUEUE_FULL, 1, 0) != Mist::PROC_FEED_HARD_LOCKOUT) {
    return fail("normalized processor pressure did not map to the expected feed vote");
  }
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

  rate.regularSlow = false;
  rate.nodeSlow = true;
  decision = Mist::decideProcessingRate(rate);
  if (decision.speed != 8 || decision.ramped) { return fail("node pressure should use the bounded slowdown"); }

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

  rate.nodeHold = false;
  rate.rampLocked = true;
  decision = Mist::decideProcessingRate(rate);
  if (decision.speed != 6 || decision.ramped) { return fail("a hard-pressure lockout should block a ramp"); }

  rate.rampLocked = false;
  rate.contractsReady = false;
  decision = Mist::decideProcessingRate(rate);
  if (decision.speed != 6 || decision.ramped) { return fail("incomplete contracts should block a ramp"); }

  rate.contractsReady = true;
  rate.freshVoteRound = false;
  decision = Mist::decideProcessingRate(rate);
  if (decision.speed != 6 || decision.ramped) { return fail("stale votes should block a ramp"); }

  rate = Mist::ProcessingRateInput();
  rate.current = 10;
  rate.target = 4;
  decision = Mist::decideProcessingRate(rate);
  if (decision.speed != 4 || decision.ramped) { return fail("a lower processor target should apply immediately"); }

  rate = Mist::ProcessingRateInput();
  rate.current = 5;
  rate.target = 6;
  rate.freshVoteRound = true;
  rate.contractsReady = true;
  decision = Mist::decideProcessingRate(rate);
  if (decision.speed != 6 || !decision.ramped) { return fail("a ramp should stop at the processor target"); }

  ProcExitState exitState;
  exitState.log("CLEAN", 0, "clean exit");
  exitState.log("RETRY", 1, "retryable exit");
  exitState.log("RETRY_LATE", 1, "later equal-severity exit");
  exitState.log("FATAL", 2, "fatal exit");
  exitState.log("CLEAN_LATE", 0, "later clean exit");
  if (exitState.flush(page) != 2 || !ProcState::readSnapshot(page, snapshot) || snapshot.phase != PRC_PHASE_DRAINING ||
      std::string(snapshot.shortReason) != "FATAL" || std::string(snapshot.longReason) != "fatal exit") {
    return fail("process exit aggregation did not preserve the highest-severity first reason");
  }

  ProcState::initPage(page);
  ProcExitState concurrentExit;
  std::vector<std::thread> exitWriters;
  for (size_t i = 0; i < 8; ++i) {
    exitWriters.emplace_back([&concurrentExit, i]() {
      if (i == 5) {
        concurrentExit.log("FATAL_CONCURRENT", 2, "fatal concurrent exit");
      } else {
        concurrentExit.log("RETRY_CONCURRENT", 1, "retryable concurrent exit %zu", i);
      }
    });
  }
  for (std::thread & writerThread : exitWriters) { writerThread.join(); }
  if (concurrentExit.flush(page) != 2 || !ProcState::readSnapshot(page, snapshot) ||
      std::string(snapshot.shortReason) != "FATAL_CONCURRENT" || std::string(snapshot.longReason) != "fatal concurrent exit") {
    return fail("concurrent process exits did not retain the fatal reason");
  }

  nodePage.master = false;
  shm_unlink(nodePageName);
  shm_unlink(pageName);

  return 0;
}
