#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace Mist {
  /// A HOLDBUFFER reader whose key position has not moved for this long stops
  /// pinning the buffer, so a wedged or abandoned recorder can neither block
  /// eviction nor pause the feed indefinitely.
  static const uint64_t BUFFER_HOLD_STALE_MS = 60000;

  /// Collects, per track, the lowest key that a HOLDBUFFER reader has not
  /// consumed yet. Fed once per user-page scan: beginScan, observe for every
  /// active HOLDBUFFER record, endScan.
  class BufferHoldTracker {
    public:
      /// One live (not stale) HOLDBUFFER record: a recorder's read position
      /// on one track. keyNum npos means waiting at that track's live point.
      struct Position {
          uint64_t reader;
          size_t track;
          uint64_t keyNum;
      };

      void beginScan() { seen.clear(); }

      /// keyNum is the reader's absolute key number; a freshly selected reader
      /// sits at 0 (hold from the first key). std::string::npos marks a reader
      /// waiting at the live point: it has consumed everything and holds nothing.
      /// reader identifies the recorder (its pid): one recorder holds one
      /// record per selected track.
      void observe(size_t userId, uint64_t reader, size_t track, uint64_t keyNum, uint64_t nowMs) {
        seen.insert(userId);
        std::map<size_t, Reader>::iterator it = readers.find(userId);
        if (it == readers.end() || it->second.track != track || it->second.keyNum != keyNum) {
          Reader & r = readers[userId];
          r.track = track;
          r.keyNum = keyNum;
          r.changedMs = nowMs;
        }
        readers[userId].reader = reader;
      }

      void endScan(uint64_t nowMs) {
        minKeys.clear();
        live.clear();
        for (std::map<size_t, Reader>::iterator it = readers.begin(); it != readers.end();) {
          if (!seen.count(it->first)) {
            readers.erase(it++);
            continue;
          }
          const Reader & r = it->second;
          ++it;
          if (nowMs > r.changedMs && nowMs - r.changedMs > BUFFER_HOLD_STALE_MS) { continue; }
          live.push_back({r.reader, r.track, r.keyNum});
          if (atLivePoint(r.keyNum)) { continue; }
          std::map<size_t, uint64_t>::iterator m = minKeys.find(r.track);
          if (m == minKeys.end() || r.keyNum < m->second) { minKeys[r.track] = r.keyNum; }
        }
      }

      /// True when a live HOLDBUFFER reader still needs keys of this track;
      /// minKey is then the lowest key any of them still needs.
      bool held(size_t track, uint64_t & minKey) const {
        std::map<size_t, uint64_t>::const_iterator it = minKeys.find(track);
        if (it == minKeys.end()) { return false; }
        minKey = it->second;
        return true;
      }

      const std::map<size_t, uint64_t> & heldTracks() const { return minKeys; }

      /// Every live HOLDBUFFER record of the last scan.
      const std::vector<Position> & positions() const { return live; }

      static bool atLivePoint(uint64_t keyNum) {
        return keyNum == (uint64_t)std::string::npos || keyNum == 0xFFFFFFFFull;
      }

    private:
      struct Reader {
          uint64_t reader;
          size_t track;
          uint64_t keyNum;
          uint64_t changedMs;
      };
      std::map<size_t, Reader> readers;
      std::set<size_t> seen;
      std::map<size_t, uint64_t> minKeys;
      std::vector<Position> live;
  };

  /// How far the feed leads a recorder: the smallest lead over the tracks it
  /// reads. A recorder waiting at the live point of any track (typically a
  /// rendition still being produced) is waiting on a producer, not lagging
  /// behind the source, and pausing the feed would starve that producer.
  /// trackLeads maps each of its tracks to lastms minus the read position.
  inline uint64_t recorderLeadMs(const std::map<size_t, uint64_t> & trackLeads) {
    uint64_t lead = 0;
    bool first = true;
    for (std::map<size_t, uint64_t>::const_iterator it = trackLeads.begin(); it != trackLeads.end(); ++it) {
      if (first || it->second < lead) { lead = it->second; }
      first = false;
    }
    return lead;
  }

  /// The buffer may evict its first key only while every holding reader is
  /// already past it.
  inline bool keyRemovalAllowed(uint64_t firstKey, bool held, uint64_t minHeldKey) {
    return !held || firstKey < minHeldKey;
  }

  /// Lead at which the feed must wait for its slowest holding reader: two
  /// target durations short of the buffer window, so eviction never has to
  /// choose between the window and a reader.
  inline uint64_t consumerHoldThreshold(uint64_t bufferTimeMs, uint64_t targetDurationMs) {
    if (bufferTimeMs <= 4 * targetDurationMs) { return bufferTimeMs / 2; }
    return bufferTimeMs - 2 * targetDurationMs;
  }

  /// Hysteresis on the feed lead over the slowest holding reader: hold at the
  /// threshold, release once the reader has caught up to half of it.
  class ConsumerLagHold {
    public:
      bool update(uint64_t leadMs, uint64_t thresholdMs) {
        if (!thresholdMs) {
          holding = false;
        } else if (!holding && leadMs >= thresholdMs) {
          holding = true;
        } else if (holding && leadMs < thresholdMs / 2) {
          holding = false;
        }
        return holding;
      }
      void reset() { holding = false; }
      bool held() const { return holding; }

    private:
      bool holding = false;
  };
} // namespace Mist
