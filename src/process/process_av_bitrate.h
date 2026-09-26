#pragma once

#include <cstdint>

namespace Mist {
  /// Target bitrate MistProcAV uses when the process config sets none. An audio
  /// encoder must not inherit the video default.
  inline int64_t defaultAVBitrate(bool isVideo) {
    return isVideo ? 2000000 : 128000;
  }

  /// Nearest bitrate the Opus encoder accepts.
  inline int64_t opusBitrate(int64_t requested) {
    if (requested < 500) { return 500; }
    if (requested > 256000) { return 256000; }
    return requested;
  }
} // namespace Mist
