#pragma once

#include <cstdint>
#include <string>

namespace Mist {
  // Single byte ranges use inclusive endpoints. Reject unsupported multiple
  // ranges and overflowing integers instead of silently serving another range.
  inline bool parseSingleByteRange(const std::string & header, uint64_t & start, uint64_t & end) {
    const uint64_t last = end;
    end = 0;
    if (header.compare(0, 6, "bytes=") != 0) { return false; }
    const size_t dash = header.find('-', 6);
    if (dash == std::string::npos) { return false; }
    const auto number = [&](size_t begin, size_t finish, uint64_t & value) {
      if (begin == finish) { return false; }
      value = 0;
      for (size_t i = begin; i < finish; ++i) {
        if (header[i] < '0' || header[i] > '9') { return false; }
        const unsigned digit = header[i] - '0';
        if (value > (UINT64_MAX - digit) / 10) { return false; }
        value = value * 10 + digit;
      }
      return true;
    };
    uint64_t first, final;
    if (dash == 6) {
      uint64_t suffix;
      if (!number(dash + 1, header.size(), suffix) || !suffix) { return false; }
      first = suffix > last ? 0 : last - suffix + 1;
      final = last;
    } else {
      if (!number(6, dash, first) || first > last) { return false; }
      final = last;
      if (dash + 1 < header.size()) {
        if (!number(dash + 1, header.size(), final) || final < first) { return false; }
        if (final > last) { final = last; }
      }
    }
    start = first;
    end = final;
    return true;
  }
} // namespace Mist
