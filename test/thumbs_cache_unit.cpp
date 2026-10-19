#include "../src/process/thumbs_cache.h"

#include <cstdint>
#include <cstdio>
#include <deque>
#include <vector>

namespace {
  struct Frame {
      uint64_t timeMs;
  };

  int fail(const char *message) {
    fprintf(stderr, "%s\n", message);
    return 1;
  }

  // The sprite samples a 100-cell grid evenly by index from the cache, as composeAndBuffer does.
  std::vector<uint64_t> spriteCells(const std::deque<Frame> & cache, size_t cells) {
    std::vector<uint64_t> out;
    if (cache.size() <= cells) {
      for (const Frame & f : cache) { out.push_back(f.timeMs); }
      return out;
    }
    for (size_t c = 0; c < cells; ++c) { out.push_back(cache[c * (cache.size() - 1) / (cells - 1)].timeMs); }
    return out;
  }

  uint64_t largestGap(const std::vector<uint64_t> & cells) {
    uint64_t gap = 0;
    for (size_t i = 1; i < cells.size(); ++i) {
      if (cells[i] - cells[i - 1] > gap) { gap = cells[i] - cells[i - 1]; }
    }
    return gap;
  }

  // Feeds `keyframes` keyframes `stepMs` apart the way the thumbnail source thread caches them.
  std::deque<Frame> feed(size_t keyframes, uint64_t stepMs, uint64_t windowMs, bool vod, bool processControlled, size_t cells) {
    const size_t cap = cells * 3;
    std::deque<Frame> cache;
    for (size_t k = 0; k < keyframes; ++k) {
      uint64_t now = k * stepMs;
      uint64_t bufferFirst = now > windowMs ? now - windowMs : 0;
      cache.push_back({now});
      if (Mist::thumbCacheFollowsBufferWindow(vod, processControlled)) {
        while (!cache.empty() && cache.front().timeMs < bufferFirst) { cache.pop_front(); }
      }
      if (cache.size() > cap) {
        bool finite = !Mist::thumbCacheFollowsBufferWindow(vod, processControlled);
        Mist::thinThumbCache(cache, cap, finite ? 0 : cells / 2);
      }
    }
    return cache;
  }
} // namespace

int main() {
  const size_t cells = 100;

  // A 30-minute VoD processed through a live buffer with a 50 s window, one keyframe per 2 s.
  {
    std::deque<Frame> cache = feed(900, 2000, 50000, false, true, cells);
    if (cache.size() > cells * 3) { return fail("a finite source's thumbnail cache outgrew its cap"); }
    std::vector<uint64_t> sprite = spriteCells(cache, cells);
    if (sprite.front() != 0) {
      fprintf(stderr, "sprite starts at %llu ms\n", (unsigned long long)sprite.front());
      return fail("a processed VoD's sprite must start at the first keyframe, not at the buffer window");
    }
    if (sprite.back() != 899 * 2000) { return fail("a processed VoD's sprite must end at the last keyframe"); }
    uint64_t gap = largestGap(sprite);
    // 100 cells over 1798 s average 18 s apart.
    if (gap > 3 * 18200) {
      fprintf(stderr, "largest gap between sprite cells: %llu ms\n", (unsigned long long)gap);
      return fail("a processed VoD's sprite must spread its cells over the whole source");
    }
  }

  // A 4-hour VoD, one keyframe per second: memory stays capped and coverage stays even.
  {
    std::deque<Frame> cache = feed(4 * 3600, 1000, 50000, true, false, cells);
    if (cache.size() > cells * 3) { return fail("a long VoD's thumbnail cache outgrew its cap"); }
    std::vector<uint64_t> sprite = spriteCells(cache, cells);
    uint64_t gap = largestGap(sprite);
    if (sprite.front() != 0 || gap > 3 * 145000) {
      fprintf(stderr, "first cell %llu ms, largest gap %llu ms\n", (unsigned long long)sprite.front(), (unsigned long long)gap);
      return fail("a long VoD's sprite must cover the whole source evenly");
    }
  }

  // A live stream keeps only its buffer window.
  {
    std::deque<Frame> cache = feed(600, 2000, 50000, false, false, cells);
    if (cache.front().timeMs < 1198000 - 50000) { return fail("a live stream's sprite must follow its buffer window"); }
  }

  // A live stream with a one-hour DVR window, two hours in: the sprite covers the whole window,
  // with the newest half of the cells kept dense near the live edge.
  {
    std::deque<Frame> cache = feed(3600, 2000, 3600000, false, false, cells);
    if (cache.size() > cells * 3) { return fail("a live stream's thumbnail cache outgrew its cap"); }
    std::vector<uint64_t> sprite = spriteCells(cache, cells);
    if (sprite.front() > 7198000 - 3600000 + 2000) { return fail("a live sprite must start at its window"); }
    uint64_t gap = largestGap(sprite);
    // The older 250 cached thumbnails span ~3500 s; ~84 of the cells sample them.
    if (gap > 3 * 42000) {
      fprintf(stderr, "largest gap between live sprite cells: %llu ms\n", (unsigned long long)gap);
      return fail("a live sprite must cover its whole DVR window without a hole");
    }
  }

  // A 3x2 grid caps a two-minute processed source at 18 thumbnails from its first to its last keyframe.
  {
    std::deque<Frame> cache = feed(60, 2000, 50000, false, true, 3 * 2);
    if (cache.size() != 18 || cache.front().timeMs != 0 || cache.back().timeMs != 118000) {
      return fail("a finite source thinned to its cap keeps its first and last keyframe");
    }
  }
  return 0;
}
