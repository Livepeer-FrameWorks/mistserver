#include "mpeg.h"

#include "bitstream.h"

#include <algorithm>

namespace Mpeg{
  MPEG4Info parseMPEG4Headers(const char *data, size_t length) {
    MPEG4Info result;
    if (!data || length < 4) { return result; }
    size_t firstHeader = length;
    for (size_t offset = 0; offset + 4 <= length; ++offset) {
      if (data[offset] || data[offset + 1] || (unsigned char)data[offset + 2] != 1) { continue; }
      const unsigned char code = data[offset + 3];
      if (code == 0xB6) {
        if (offset + 4 < length) { result.keyFrame = !((unsigned char)data[offset + 4] >> 6); }
        if (firstHeader < offset) { result.init.assign(data + firstHeader, offset - firstHeader); }
        break;
      }
      if (firstHeader == length) { firstHeader = offset; }
      if (code < 0x20 || code > 0x2F) { continue; }
      Utils::bitstream bits;
      bits.append(data + offset + 4, std::min<size_t>(64, length - offset - 4));
      bool complete = true;
      const auto get = [&](size_t count) -> uint64_t {
        if (count > 64 || count > bits.size()) {
          complete = false;
          return 0;
        }
        return bits.get(count);
      };
      const auto skip = [&](size_t count) {
        if (count > bits.size()) {
          complete = false;
          return;
        }
        bits.skip(count);
      };
      skip(9);
      unsigned int version = 1;
      if (get(1)) {
        version = get(4);
        skip(3);
      }
      if (get(4) == 15) { skip(16); }
      if (get(1)) {
        skip(3);
        if (get(1)) { skip(79); }
      }
      const unsigned int shape = get(2);
      if (shape == 3 && version != 1) { skip(4); }
      if (get(1) != 1) { continue; }
      const unsigned int resolution = get(16);
      if (!resolution || get(1) != 1) { continue; }
      unsigned int increment = 0;
      if (get(1)) {
        unsigned int count = 1;
        while (count < 16 && (1u << count) < resolution) { ++count; }
        increment = get(count);
      }
      if (shape != 0 || get(1) != 1) { continue; }
      const unsigned int width = get(13);
      if (get(1) != 1) { continue; }
      const unsigned int height = get(13);
      if (!complete || get(1) != 1 || !width || !height) { continue; }
      result.width = width;
      result.height = height;
      if (increment) { result.fps = (double)resolution / increment; }
    }
    if (result.init.empty() && firstHeader < length) { result.init.assign(data + firstHeader, length - firstHeader); }
    return result;
  }

  MP2Info parseMP2Header(const std::string &hdr){return parseMP2Header(hdr.c_str());}

  MP2Info parseMP2Header(const char *hdr){
    MP2Info res;
    // mpeg version is on the bits 0x18 of header[1], but only 0x08 is important --> 0 is version 2,
    // 1 is version 1
    // leads to 2 - value == version, -1 to get the right index for the array
    int mpegVersion = 1 - ((hdr[1] >> 3) & 0x01);
    // samplerate is encoded in bits 0x0C of header[2];
    res.sampleRate = sampleRates[mpegVersion][((hdr[2] >> 2) & 0x03)] * 1000;
    res.channels = 2 - (hdr[3] >> 7);
    res.layer = 4 - ((hdr[1] >> 1) & 0x03);
    return res;
  }

  void MPEG2Info::clear(){
    width = 0;
    height = 0;
    fps = 0;
    tempSeq = 0;
    frameType = 0;
    isHeader = false;
  }

  MPEG2Info parseMPEG2Header(const std::string &hdr){return parseMPEG2Header(hdr.data());}

  MPEG2Info parseMPEG2Header(const char *hdr){
    MPEG2Info res;
    parseMPEG2Header(hdr, res);
    return res;
  }

  bool parseMPEG2Header(const char *hdr, MPEG2Info &mpInfo){
    // Check for start code
    if (hdr[0] != 0 || hdr[1] != 0 || hdr[2] != 1){return false;}
    if (hdr[3] == 0xB3){// Sequence header
      mpInfo.isHeader = true;
      mpInfo.width = (hdr[4] << 4) | ((hdr[5] >> 4) & 0x0F);
      mpInfo.height = ((hdr[5] & 0x0F) << 8) | hdr[6];
      mpInfo.fps = frameRate[hdr[7] & 0x0F];
      return true;
    }
    if (hdr[3] == 0x00){// Picture header
      mpInfo.tempSeq = ((uint16_t)hdr[4] << 2) | (hdr[5] >> 6);
      mpInfo.frameType = (hdr[5] & 0x38) >> 3;
      return true;
    }
    // Not parsed
    return false;
  }

  void parseMPEG2Headers(const char *hdr, uint32_t len, MPEG2Info &mpInfo){
    mpInfo.clear();
    const char *offset = hdr;
    char *maxData = (char *)hdr + len - 5;
    if (maxData - hdr > 250){maxData = (char *)hdr + 250;}
    while (offset < maxData){
      if (offset[2] > 1){
        // We have no zero in the third byte, so we need to skip at least 3 bytes forward
        offset += 3;
        continue;
      }
      if (!offset[2]){
        // We skip forward 1 or 2 bytes depending on contents of the second byte
        offset += (offset[1] ? 2 : 1);
        continue;
      }
      if (!offset[0] && !offset[1]){parseMPEG2Header(offset, mpInfo);}
      // We have no zero in the third byte, so we need to skip at least 3 bytes forward
      offset += 3;
    }
  }

  MPEG2Info parseMPEG2Headers(const char *hdr, uint32_t len){
    MPEG2Info res;
    parseMPEG2Headers(hdr, len, res);
    return res;
  }
}// namespace Mpeg
