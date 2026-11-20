#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace AC3 {
  struct Header {
      bool valid = false;
      bool enhanced = false;
      unsigned int frameBytes = 0, rate = 0, samples = 0, channels = 0, bitRate = 0;
      unsigned int fscod = 0, bsid = 0, bsmod = 0, acmod = 0, lfe = 0;
      unsigned int streamType = 0, substream = 0, channelMap = 0;
      unsigned int dependentSubstreams = 0, dependentLocations = 0;
  };
  Header parse(const void *data, size_t bytes);
  std::string makeDEC3(const Header & header);
  Header parseDEC3(const void *data, size_t bytes);
  inline bool validDEC3(const std::string & data) {
    return parseDEC3(data.data(), data.size()).valid;
  }
} // namespace AC3
