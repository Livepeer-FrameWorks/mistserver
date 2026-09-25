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
  inline bool outputWaitsForBootingBuffer(uint8_t streamStatus, size_t validTracks, bool processingReader) {
    if (processingReader || validTracks) { return false; }
    return streamStatus == STRMSTAT_INIT || streamStatus == STRMSTAT_BOOT || streamStatus == STRMSTAT_WAIT;
  }
} // namespace Mist
