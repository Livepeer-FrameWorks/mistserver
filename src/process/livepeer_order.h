#pragma once

#include <atomic>
#include <cstddef>

namespace Mist {

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
