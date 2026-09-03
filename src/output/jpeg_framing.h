#pragma once

#include <cstddef>
#include <cstdint>

namespace Mist {
  namespace JPEG {
    enum ScanStatus { NEED_MORE, FRAME_READY, DISCARD_INVALID, DISCARD_OVERSIZED };

    struct ScanResult {
        ScanStatus status;
        size_t bytes;
    };

    /// Finds one complete JPEG image at the beginning of a byte stream.
    /// Invalid prefixes are returned as discardable bytes so callers can resynchronize.
    ScanResult scanFrame(const uint8_t *data, size_t size, size_t maxFrameSize);
  } // namespace JPEG
} // namespace Mist
