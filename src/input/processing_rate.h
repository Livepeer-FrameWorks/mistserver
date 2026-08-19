#pragma once

#include <mist/proc_stats.h>

#include <algorithm>
#include <cstdint>

namespace Mist {
  enum ProcFeedVote : uint8_t {
    PROC_FEED_ALLOW = 0,
    PROC_FEED_SLOW = 1,
    PROC_FEED_HARD = 2,
    PROC_FEED_HARD_LOCKOUT = 3,
  };

  inline ProcFeedVote classifyProcFeedVote(uint16_t flags, uint8_t reason, uint8_t canAcceptMore, uint16_t pressureQ0_16) {
    if (reason == PRC_REASON_RETRY || reason == PRC_REASON_QUEUE_FULL) { return PROC_FEED_HARD_LOCKOUT; }
    // Source starvation explains low achieved throughput; it does not mean the
    // processor lacks capacity. Queue/retry signals above still take priority.
    bool sourceStarved = (flags & PRC_FLAG_SOURCE_LIMITED) && reason == PRC_REASON_SOURCE_WAIT;
    if (!canAcceptMore && !sourceStarved) { return PROC_FEED_HARD; }
    if (pressureQ0_16 > (uint16_t)(0.7 * 65535.0) && !sourceStarved) { return PROC_FEED_SLOW; }
    return PROC_FEED_ALLOW;
  }

  /// Inputs to the generic feed-rate transition. Proc-specific knowledge stays
  /// on the ProcState page; this only combines normalized contract signals.
  struct ProcessingRateInput {
      uint64_t current = 0;
      uint64_t target = 1;
      bool hardSlow = false;
      bool regularSlow = false;
      bool nodeSlow = false;
      bool nodeHold = false;
      bool freshVoteRound = false;
      bool contractsReady = false;
      bool rampLocked = false;
  };

  struct ProcessingRateResult {
      uint64_t speed;
      bool ramped;
  };

  inline ProcessingRateResult decideProcessingRate(const ProcessingRateInput & in) {
    uint64_t speed = in.current ? in.current : std::max((uint64_t)1, in.target);
    bool ramped = false;
    if (in.hardSlow) {
      speed = 1;
    } else if (in.regularSlow || in.nodeSlow) {
      speed = std::max((uint64_t)1, (uint64_t)((double)speed * 0.8));
    } else if (in.target < speed) {
      speed = std::max((uint64_t)1, in.target);
    } else if (in.freshVoteRound && in.contractsReady && !in.nodeHold && !in.rampLocked && speed < in.target) {
      // A complete vote round may add at most 50%; this keeps equal jobs from
      // all leaping to their individual maximum on the same node-pressure tick.
      uint64_t raised = std::max(speed + 1, (speed * 3 + 1) / 2);
      speed = std::min(raised, in.target);
      ramped = true;
    }
    return {speed, ramped};
  }
} // namespace Mist
