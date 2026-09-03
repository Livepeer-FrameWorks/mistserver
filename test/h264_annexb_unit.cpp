#include <mist/dtsc.h>
#include <mist/h264.h>

#include <iostream>
#include <string>

namespace {
  const char sps[] = {0x67,       0x64, 0x00, 0x1f,       (char)0xac, (char)0xd9, 0x40, 0x50, 0x05,
                      (char)0xbb, 0x01, 0x10, 0x00,       0x00,       0x03,       0x00, 0x10, 0x00,
                      0x00,       0x03, 0x03, (char)0xc0, (char)0xf1, (char)0x83, 0x19, 0x60};
  const char pps[] = {0x68, (char)0xeb, (char)0xec, (char)0xb2, 0x2c};

  std::string expectedAvcc() {
    std::string result = {1, 0x64, 0x00, 0x1f, (char)0xff, (char)0xe1, 0, sizeof(sps)};
    result.append(sps, sizeof(sps));
    result.append({1, 0, sizeof(pps)});
    result.append(pps, sizeof(pps));
    return result;
  }

  std::string annexB(bool fourByteSps, bool fourBytePps) {
    std::string result(fourByteSps ? "\0\0\0\1" : "\0\0\1", fourByteSps ? 4 : 3);
    result.append(sps, sizeof(sps));
    result.append(fourBytePps ? "\0\0\0\1" : "\0\0\1", fourBytePps ? 4 : 3);
    result.append(pps, sizeof(pps));
    return result;
  }

  bool expectConversion(bool fourByteSps, bool fourBytePps) {
    const std::string input = annexB(fourByteSps, fourBytePps);
    const std::string actual = h264::initFromAnnexB(input.data(), input.size());
    if (actual != expectedAvcc()) {
      std::cerr << "Annex B helper rejected " << (fourByteSps ? 4 : 3) << "/" << (fourBytePps ? 4 : 3)
                << "-byte start codes" << std::endl;
      return false;
    }
    return true;
  }

  bool expectMetadataConversion(bool fourByteStartCode) {
    DTSC::Meta meta;
    meta.reInit("", true);
    const size_t track = meta.addTrack(8, 8, 16, 2, true);
    meta.setType(track, "video");
    meta.setCodec(track, "H264");
    const std::string input = annexB(fourByteStartCode, !fourByteStartCode);
    meta.setInit(track, input);
    if (meta.getInit(track) != expectedAvcc()) {
      std::cerr << "DTSC metadata did not normalize an Annex B init beginning with a " << (fourByteStartCode ? 4 : 3)
                << "-byte start code" << std::endl;
      return false;
    }
    return true;
  }
} // namespace

int main() {
  if (!expectConversion(false, false) || !expectConversion(false, true) || !expectConversion(true, false) ||
      !expectConversion(true, true)) {
    return 1;
  }
  if (!expectMetadataConversion(false) || !expectMetadataConversion(true)) { return 1; }
  if (!h264::initFromAnnexB("not annex b", 11).empty()) {
    std::cerr << "non-Annex-B data was accepted" << std::endl;
    return 1;
  }
  const std::string missingPps("\0\0\1\x67\x64\0\x1f", 7);
  if (!h264::initFromAnnexB(missingPps.data(), missingPps.size()).empty()) {
    std::cerr << "initialization without a PPS was accepted" << std::endl;
    return 1;
  }
  const std::string shortSps("\0\0\1\x67\x64\0\0\1\x68\xee", 10);
  if (!h264::initFromAnnexB(shortSps.data(), shortSps.size()).empty()) {
    std::cerr << "SPS without profile/compatibility/level bytes was accepted" << std::endl;
    return 1;
  }
  return 0;
}
