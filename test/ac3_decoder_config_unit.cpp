#include <mist/ac3.h>
#include <mist/bitstream.h>

#include <cstdio>

static std::string accessUnit(unsigned int streamType, unsigned int id, unsigned int locations = 0) {
  Utils::bitWriter bits;
  bits.append(0x0b77, 16);
  bits.append(streamType, 2);
  bits.append(id, 3);
  bits.append(127, 11);
  bits.append(0, 2);
  bits.append(3, 2);
  bits.append(streamType ? 2 : 7, 3);
  bits.append(streamType ? 0 : 1, 1);
  bits.append(16, 5);
  bits.append(0, 5);
  bits.append(0, 1);
  if (streamType) {
    bits.append(1, 1);
    bits.append(locations, 16);
  } else {
    bits.append(0, 1);
    bits.append(1, 1);
    bits.append(3, 3);
  }
  std::string data = bits.str();
  data.resize(256);
  return data;
}

int main() {
  const std::string frames = accessUnit(0, 0) + accessUnit(1, 0, 1u << 10) + accessUnit(1, 1, 1u << 5);
  const AC3::Header parsed = AC3::parse(frames.data(), frames.size());
  if (!parsed.valid || !parsed.enhanced || parsed.channels != 10 || parsed.dependentSubstreams != 2 ||
      parsed.dependentLocations != 0x021) {
    fprintf(stderr, "EAC-3 access unit lost dependent speaker locations\n");
    return 1;
  }
  const std::string config = AC3::makeDEC3(parsed);
  const AC3::Header stored = AC3::parseDEC3(config.data(), config.size());
  if (!stored.valid || stored.channels != 10 || stored.rate != 48000 || stored.dependentLocations != 0x021 ||
      stored.dependentSubstreams != 2) {
    fprintf(stderr, "EAC-3 decoder configuration lost its source layout\n");
    return 1;
  }
  printf("PASS independent + two dependent EAC-3 substreams retain 10 channels through dec3\n");
}
