#include "../src/output/jpeg_framing.h"

#include <iostream>
#include <string>

static bool expect(bool condition, const char *message) {
  if (condition) { return true; }
  std::cerr << message << std::endl;
  return false;
}

static std::string fixture() {
  const uint8_t bytes[] = {0xFF, 0xD8, // SOI
                           0xFF, 0xEE, 0x00, 0x06, 0xFF, 0xD9, 0x01, 0x02, // APP14 containing a false EOI
                           0xFF, 0xDB, 0x00, 0x04, 0x03, 0x04, // DQT
                           0xFF, 0xDA, 0x00, 0x04, 0x05, 0x06, // SOS
                           0x11, 0xFF, 0x00, 0x22, 0xFF, 0xD3, 0x33, // stuffed byte and restart marker
                           0xFF, 0xC4, 0x00, 0x04, 0x07, 0x08, // progressive scan table
                           0xFF, 0xDA, 0x00, 0x04, 0x09, 0x0A, 0x44, // second scan
                           0xFF, 0xD9}; // EOI
  return std::string((const char *)bytes, sizeof(bytes));
}

int main() {
  bool ok = true;
  const std::string frame = fixture();
  for (size_t i = 0; i < frame.size(); ++i) {
    Mist::JPEG::ScanResult result = Mist::JPEG::scanFrame((const uint8_t *)frame.data(), i, 1024);
    ok &= expect(result.status == Mist::JPEG::NEED_MORE && !result.bytes, "a fragmented valid JPEG must remain buffered");
  }

  Mist::JPEG::ScanResult result = Mist::JPEG::scanFrame((const uint8_t *)frame.data(), frame.size(), 1024);
  ok &= expect(result.status == Mist::JPEG::FRAME_READY && result.bytes == frame.size(),
               "complete JPEG framing or marker-payload handling is wrong");

  const std::string joined = frame + frame;
  result = Mist::JPEG::scanFrame((const uint8_t *)joined.data(), joined.size(), 1024);
  ok &= expect(result.status == Mist::JPEG::FRAME_READY && result.bytes == frame.size(),
               "concatenated JPEGs must be emitted one at a time");

  const std::string junk = std::string("junk", 4) + frame;
  result = Mist::JPEG::scanFrame((const uint8_t *)junk.data(), junk.size(), 1024);
  ok &= expect(result.status == Mist::JPEG::DISCARD_INVALID && result.bytes == 4, "invalid prefixes must resynchronize at the next SOI");

  const uint8_t badLength[] = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x01, 0xFF, 0xD8};
  result = Mist::JPEG::scanFrame(badLength, sizeof(badLength), 1024);
  ok &= expect(result.status == Mist::JPEG::DISCARD_INVALID && result.bytes == 6, "malformed segment lengths must resynchronize safely");

  result = Mist::JPEG::scanFrame((const uint8_t *)frame.data(), frame.size(), frame.size() - 1);
  ok &= expect(result.status == Mist::JPEG::DISCARD_OVERSIZED && result.bytes == frame.size(),
               "oversized complete JPEGs must be rejected as one frame");

  const uint8_t trailingMarker[] = {0x12, 0x34, 0xFF};
  result = Mist::JPEG::scanFrame(trailingMarker, sizeof(trailingMarker), 1024);
  ok &= expect(result.status == Mist::JPEG::DISCARD_INVALID && result.bytes == 2,
               "resynchronization must retain a trailing marker prefix");

  return ok ? 0 : 1;
}
