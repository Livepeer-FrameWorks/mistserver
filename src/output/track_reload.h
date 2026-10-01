#pragma once

#include <mist/dtsc.h>

namespace Mist {
  /// True for a track the stream still lists as valid while this process has no
  /// mapping of its metadata page: its first load failed because the writer was
  /// replacing the page. The track still exists and a later
  /// reloadReplacedPagesIfNeeded call loads it, so outputs wait for that instead
  /// of treating the track as removed.
  inline bool trackAwaitingReload(const DTSC::Meta & M, size_t trackIdx) {
    return M.trackValid(trackIdx) && !M.trackLoaded(trackIdx);
  }
} // namespace Mist
