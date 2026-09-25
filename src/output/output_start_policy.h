#pragma once

#include <mist/defines.h>
#include <mist/triggers.h>

#include <string>

namespace Mist {
  enum PlayRewriteOutcome { PLAY_REWRITE_UNCHANGED, PLAY_REWRITE_CHANGED, PLAY_REWRITE_DENIED };

  class PlayRewriteGate {
    public:
      PlayRewriteGate() : handled(false) {}

      bool begin(bool enabled) {
        if (!enabled || handled) { return false; }
        handled = true;
        return true;
      }

    private:
      bool handled;
  };

  inline std::string playRewriteTarget(const std::string & current, const Triggers::Result & result) {
    if (result.action == Triggers::ACT_DENY) { return ""; }
    if (result.action == Triggers::ACT_KEEP || result.action == Triggers::ACT_CONFIGURED) { return current; }
    return result.response;
  }

  inline bool statusAllowsFallback(bool startAttemptWasOffline, const std::string & original, const std::string & current) {
    return !startAttemptWasOffline && original == current;
  }

  inline uint8_t effectiveStatus(uint8_t observed, bool startAttemptWasOffline) {
    return startAttemptWasOffline ? STRMSTAT_OFFLINE : observed;
  }

  /// A buffer that is still booting can expose its meta page before any track
  /// is valid or the meta is marked live; an output attached in that window
  /// would select nothing and end at once, so it waits for the buffer as well.
  /// Processing readers attach to a waiting buffer on purpose and never wait.
  /// Bound on how long an output waits for a booting buffer: first for its
  /// meta page, then for playable tracks.
  static const uint64_t OUTPUT_BUFFER_BOOT_WAIT_MS = 45000;

  /// The buffer creates its meta page some time after it takes the input lock.
  /// An output that reached a starting stream keeps polling for that page
  /// while the stream is alive or still booting, instead of giving up after a
  /// burst of immediate retries and never reaching the booting-buffer wait.
  inline bool outputWaitsForMetaPage(bool hasMeta, bool streamAlive, uint8_t streamStatus, uint64_t waitedMs) {
    if (hasMeta || waitedMs >= OUTPUT_BUFFER_BOOT_WAIT_MS) { return false; }
    return streamAlive || streamStatus == STRMSTAT_INIT || streamStatus == STRMSTAT_BOOT || streamStatus == STRMSTAT_WAIT;
  }

  inline bool outputWaitsForBootingBuffer(uint8_t streamStatus, size_t validTracks, bool processingReader) {
    if (processingReader || validTracks) { return false; }
    return streamStatus == STRMSTAT_INIT || streamStatus == STRMSTAT_BOOT || streamStatus == STRMSTAT_WAIT;
  }
} // namespace Mist
