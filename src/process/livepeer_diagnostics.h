#pragma once

#include <cstddef>

namespace Mist {
  inline bool livepeerSegmentParseExhausted(size_t byteOffset, size_t dataSize, bool parserHasPacket) {
    return byteOffset >= dataSize && !parserHasPacket;
  }

  inline bool livepeerSegmentProducedNoPackets(bool parseExhausted, bool timestampOffsetCalculated) {
    return parseExhausted && !timestampOffsetCalculated;
  }
} // namespace Mist
