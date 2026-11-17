#pragma once

#include <atomic>
#include <cstdint>

namespace Mist {
  // One media thread observes packet timestamps; the statistics thread reads accumulated work.
  class AVMediaProgress {
    public:
      AVMediaProgress() : previous(0), initialized(false), advanced(0) {}

      void observe(uint64_t timestamp) {
        // The first packet anchors the timeline; its absolute timestamp is not media work.
        if (initialized && timestamp > previous) {
          advanced.fetch_add(timestamp - previous, std::memory_order_relaxed);
        }
        previous = timestamp;
        initialized = true;
      }

      uint64_t total() const { return advanced.load(std::memory_order_relaxed); }

    private:
      uint64_t previous;
      bool initialized;
      std::atomic<uint64_t> advanced;
  };
} // namespace Mist
