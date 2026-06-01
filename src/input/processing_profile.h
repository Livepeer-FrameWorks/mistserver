#pragma once

#include <mist/json.h>
#include <mist/proc_stats.h>

#include <cstdint>
#include <string>
#include <vector>

namespace Mist {

  /// Kind of work the processing job is doing. Drives start/max speed defaults.
  /// "MIXED" is what the classifier returns when multiple processes are present;
  /// the most restrictive caps win.
  ///
  /// Numeric values MUST match `ProcNegotiatedKind` (lib/proc_stats.h); procs
  /// publish their negotiated kind by that wire enum and the controller
  /// re-classifies via this enum.
  enum ProcessingProfileKind : uint8_t {
    PP_KIND_UNKNOWN = 0,
    PP_KIND_THUMBS = 1,
    PP_KIND_AUDIO = 2,
    PP_KIND_AV_SW_VIDEO = 3,
    PP_KIND_AV_HW_VIDEO = 4,
    PP_KIND_LIVEPEER = 5,
    PP_KIND_MIXED = 6,
  };

  static_assert((uint8_t)PP_KIND_UNKNOWN == (uint8_t)PRC_KIND_UNKNOWN, "kind sync");
  static_assert((uint8_t)PP_KIND_THUMBS == (uint8_t)PRC_KIND_THUMBS, "kind sync");
  static_assert((uint8_t)PP_KIND_AUDIO == (uint8_t)PRC_KIND_AUDIO, "kind sync");
  static_assert((uint8_t)PP_KIND_AV_SW_VIDEO == (uint8_t)PRC_KIND_AV_SW_VIDEO, "kind sync");
  static_assert((uint8_t)PP_KIND_AV_HW_VIDEO == (uint8_t)PRC_KIND_AV_HW_VIDEO, "kind sync");
  static_assert((uint8_t)PP_KIND_LIVEPEER == (uint8_t)PRC_KIND_LIVEPEER, "kind sync");
  static_assert((uint8_t)PP_KIND_MIXED == (uint8_t)PRC_KIND_MIXED, "kind sync");

  /// Resolved per-job processing speed profile. Lives on the InputBuffer
  /// instance, never on shared `processing.*` config.
  struct ProcessingProfile {
      uint64_t startSpeed; ///< multiplier at which to begin feeding (>= 1)
      uint64_t maxSpeed; ///< upper bound the controller may ramp to (>= startSpeed)
      ProcessingProfileKind kind;
      const char *name; ///< short label for logs (static string, do not free)
  };

  /// Classify the resolved processes array (as stored in stream config
  /// `processes`) into a ProcessingProfile. Pure: no SHM access, no logging.
  /// Empty array / unrecognized contents return the conservative default
  /// (startSpeed=1, maxSpeed=1) so callers stay safe.
  ///
  /// Static-config classifier: cannot distinguish HW vs SW AV encode (libav
  /// negotiates that at runtime). Use `classifyFromNegotiatedKinds()` once
  /// procs have published their negotiatedKind to ProcState.
  ProcessingProfile classifyProcessingProfile(const JSON::Value & processesArray);

  /// Classify from the kinds procs reported once their negotiation finished.
  /// Picks the tightest (smallest maxSpeed) across all kinds present, so e.g.
  /// Livepeer + audio + thumbs becomes max=16 (audio's cap), not the
  /// conservative SW-video MIXED fallback.
  ProcessingProfile classifyFromNegotiatedKinds(const std::vector<ProcessingProfileKind> & kinds);

  /// Default profile (1x/1x). Returned when classification has no data.
  ProcessingProfile defaultProcessingProfile();
} // namespace Mist
