#include <mist/defines.h>
#include <mist/dtsc.h>

#include <cstdint>
#include <iostream>
#include <unistd.h>
#include <vector>

namespace {

  bool verifyLastFrame(const DTSC::Meta & meta, size_t track, size_t expectedIndex, uint64_t expectedTime,
                       unsigned char expectedByte, size_t frameSize) {
    uint64_t time = 0;
    if (!meta.getEmbeddedTime(track, expectedIndex, time) || time != expectedTime) {
      std::cerr << "embedded frame has invalid timestamp " << time << std::endl;
      return false;
    }
    char *data = 0;
    size_t dataSize = 0;
    if (!meta.getEmbeddedData(track, expectedIndex, data, dataSize) || dataSize != frameSize) {
      std::cerr << "embedded frame is unavailable" << std::endl;
      return false;
    }
    for (size_t index = 0; index < dataSize; ++index) {
      if (static_cast<unsigned char>(data[index]) != expectedByte) {
        std::cerr << "embedded frame payload was corrupted at byte " << index << ": got "
                  << static_cast<unsigned int>(static_cast<unsigned char>(data[index])) << ", expected "
                  << static_cast<unsigned int>(expectedByte) << std::endl;
        return false;
      }
    }
    return true;
  }

} // namespace

int main() {
  DTSC::Meta meta;
  meta.reInit("", true);

  const size_t frameSize = 257;
  const size_t track = meta.addTrack(0, 0, 0, 0, true, frameSize);
  if (track == INVALID_TRACK_ID || !meta.hasEmbeddedFrames(track)) {
    std::cerr << "failed to allocate embedded-frame track" << std::endl;
    return 1;
  }

  std::vector<char> frame(frameSize);
  for (size_t index = 0; index < RAW_FRAME_COUNT * 2; ++index) {
    std::fill(frame.begin(), frame.end(), static_cast<char>(index));
    meta.storeFrame(track, index * 20, frame.data(), frame.size());
  }

  uint64_t time = 0;
  const size_t firstReadable = RAW_FRAME_COUNT * 2 - static_cast<size_t>(RAW_FRAME_COUNT * 0.75);
  if (meta.getEmbeddedTime(track, firstReadable - 1, time)) {
    std::cerr << "overwritten embedded frame remained readable" << std::endl;
    return 1;
  }
  if (!meta.getEmbeddedTime(track, firstReadable, time) || time != firstReadable * 20) {
    std::cerr << "first retained embedded frame has invalid timestamp " << time << std::endl;
    return 1;
  }

  const size_t last = RAW_FRAME_COUNT * 2 - 1;
  if (!verifyLastFrame(meta, track, last, last * 20, static_cast<unsigned char>(last), frameSize)) { return 1; }

  meta.applyLimiter(firstReadable * 20, last * 20);
  DTSC::Keys limitedFrames = meta.getKeys(track);
  if (limitedFrames.getFirstValid() != firstReadable || limitedFrames.getTime(limitedFrames.getFirstValid()) != firstReadable * 20) {
    std::cerr << "embedded-frame limiter corrupted its first timestamp" << std::endl;
    return 1;
  }
  meta.removeLimiter();

  const std::string streamName = "embedded-frame-unit-" + std::to_string(getpid());
  DTSC::Meta sharedWriter(streamName, true);
  const size_t sharedTrack = sharedWriter.addTrack(0, 0, 0, 0, true, frameSize);
  sharedWriter.storeFrame(sharedTrack, last * 20, frame.data(), frame.size());

  DTSC::Meta sharedReader(streamName, false);
  sharedReader.reloadReplacedPagesIfNeeded();
  if (!sharedReader.trackLoaded(sharedTrack) || !sharedReader.hasEmbeddedFrames(sharedTrack) ||
      !verifyLastFrame(sharedReader, sharedTrack, 0, last * 20, static_cast<unsigned char>(last), frameSize)) {
    std::cerr << "shared embedded-frame reader did not match its writer" << std::endl;
    return 1;
  }

  return 0;
}
