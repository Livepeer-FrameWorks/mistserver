#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>

namespace Mist {
  /// Whether the thumbnail cache follows the source buffer's window. A finite source (a VoD, or a
  /// process-controlled stream playing a file through a live buffer) is summarized whole in its
  /// final sprite, so its cache keeps thumbnails the buffer already evicted.
  inline bool thumbCacheFollowsBufferWindow(bool vod, bool processControlled) {
    return !vod && !processControlled;
  }

  /// Thins `cache` (ordered by ascending timeMs) to at most `cap` entries. The `recentKeep` newest
  /// entries stay as they are. Of the older entries, the first is always kept, and each removal
  /// takes the entry whose two neighbours lie closest together, so the remainder stays evenly
  /// spread over its whole time span however long the cache keeps growing.
  template<typename Frame> void thinThumbCache(std::deque<Frame> & cache, size_t cap, size_t recentKeep) {
    while (cache.size() > cap && cache.size() > recentKeep + 2) {
      size_t histEnd = cache.size() - recentKeep;
      if (histEnd > cache.size() - 1) { histEnd = cache.size() - 1; }
      size_t drop = 0;
      uint64_t dropSpan = 0;
      for (size_t i = 1; i < histEnd; ++i) {
        uint64_t span = cache[i + 1].timeMs - cache[i - 1].timeMs;
        if (!drop || span < dropSpan) {
          drop = i;
          dropSpan = span;
        }
      }
      if (!drop) { return; }
      cache.erase(cache.begin() + drop);
    }
  }
} // namespace Mist
