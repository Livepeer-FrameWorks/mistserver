#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace Mist {

  /// One prepared segment as it was when its upload started. The source thread reuses the slot for
  /// the next segment (or the tail) as soon as the upload hands it back, so everything reported
  /// about an upload comes from this copy.
  struct LivepeerSegmentInfo {
      uint64_t keyNo;
      uint64_t time;
      uint64_t duration;
      uint64_t width;
      uint64_t height;
      uint64_t bytes;
  };

  /// Whether an upload whose response was parsed completed its segment: every rendition it
  /// carried was inserted. A stop during an insert wait leaves renditions uninserted; such an
  /// upload neither completes its insert turn nor reports the segment as complete.
  inline bool livepeerSegmentInserted(size_t renditions, size_t inserted) {
    return inserted == renditions;
  }

  class LivepeerInsertOrder {
    public:
      explicit LivepeerInsertOrder(size_t slotCount) : slots(slotCount), turn(0) {}

      bool isCurrent(size_t slot) const { return turn.load(std::memory_order_acquire) == slot; }

      bool complete(size_t slot) {
        size_t expected = slot;
        return turn.compare_exchange_strong(expected, (slot + 1) % slots, std::memory_order_acq_rel, std::memory_order_acquire);
      }

      size_t current() const { return turn.load(std::memory_order_acquire); }

    private:
      const size_t slots;
      std::atomic<size_t> turn;
  };

} // namespace Mist
