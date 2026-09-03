#include "jpeg_framing.h"

namespace Mist {
  namespace JPEG {
    static ScanResult incomplete(const uint8_t *data, size_t size, size_t maxFrameSize) {
      if (size <= maxFrameSize) { return ScanResult{NEED_MORE, 0}; }
      for (size_t i = 2; i + 1 < size; ++i) {
        if (data[i] == 0xFF && data[i + 1] == 0xD8) { return ScanResult{DISCARD_OVERSIZED, i}; }
      }
      return ScanResult{DISCARD_OVERSIZED, size - (data[size - 1] == 0xFF)};
    }

    static ScanResult invalid(const uint8_t *data, size_t size, size_t from) {
      for (size_t i = from; i + 1 < size; ++i) {
        if (data[i] == 0xFF && data[i + 1] == 0xD8) { return ScanResult{DISCARD_INVALID, i}; }
      }
      return ScanResult{DISCARD_INVALID, size - (size && data[size - 1] == 0xFF)};
    }

    ScanResult scanFrame(const uint8_t *data, size_t size, size_t maxFrameSize) {
      if (!size) { return ScanResult{NEED_MORE, 0}; }
      if (size < 2) {
        if (data[0] == 0xFF) { return incomplete(data, size, maxFrameSize); }
        return ScanResult{DISCARD_INVALID, 1};
      }
      if (data[0] != 0xFF || data[1] != 0xD8) { return invalid(data, size, 0); }

      size_t pos = 2;
      bool entropy = false;
      while (true) {
        if (entropy) {
          while (pos < size && data[pos] != 0xFF) { ++pos; }
          if (pos + 1 >= size) { return incomplete(data, size, maxFrameSize); }
          const size_t markerStart = pos++;
          while (pos < size && data[pos] == 0xFF) { ++pos; }
          if (pos >= size) { return incomplete(data, size, maxFrameSize); }
          const uint8_t marker = data[pos];
          if (marker == 0x00 || (marker >= 0xD0 && marker <= 0xD7)) {
            ++pos;
            continue;
          }
          if (marker == 0xD9) {
            const size_t frameSize = pos + 1;
            if (frameSize > maxFrameSize) { return ScanResult{DISCARD_OVERSIZED, frameSize}; }
            return ScanResult{FRAME_READY, frameSize};
          }
          if (marker == 0xD8) { return ScanResult{DISCARD_INVALID, markerStart}; }
          pos = markerStart;
          entropy = false;
          continue;
        }

        if (pos >= size) { return incomplete(data, size, maxFrameSize); }
        if (data[pos] != 0xFF) { return invalid(data, size, pos + 1); }
        const size_t markerStart = pos++;
        while (pos < size && data[pos] == 0xFF) { ++pos; }
        if (pos >= size) { return incomplete(data, size, maxFrameSize); }
        const uint8_t marker = data[pos++];

        if (marker == 0xD9) {
          if (pos > maxFrameSize) { return ScanResult{DISCARD_OVERSIZED, pos}; }
          return ScanResult{FRAME_READY, pos};
        }
        if (marker == 0xD8) { return ScanResult{DISCARD_INVALID, markerStart}; }
        if (marker == 0x00) { return invalid(data, size, pos); }
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) { continue; }
        if (pos + 2 > size) { return incomplete(data, size, maxFrameSize); }

        const size_t segmentSize = ((size_t)data[pos] << 8) | data[pos + 1];
        if (segmentSize < 2) { return invalid(data, size, pos + 2); }
        if (segmentSize > maxFrameSize || pos > maxFrameSize - segmentSize) {
          return ScanResult{DISCARD_OVERSIZED, invalid(data, size, markerStart + 2).bytes};
        }
        if (pos + segmentSize > size) { return incomplete(data, size, maxFrameSize); }
        pos += segmentSize;
        if (marker == 0xDA) { entropy = true; }
      }
    }
  } // namespace JPEG
} // namespace Mist
