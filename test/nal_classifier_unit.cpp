#include <mist/h264.h>
#include <mist/h265.h>

#include <cassert>
#include <cstdint>
#include <string>

/// Appends one NAL unit to a packet in the 4-byte length-prefixed layout the
/// classifiers walk. `header` bytes are the raw NAL bytes (header byte first).
static void appendNal(std::string & pkt, const std::string & nal) {
  uint32_t n = (uint32_t)nal.size();
  char len[4] = {(char)(n >> 24), (char)(n >> 16), (char)(n >> 8), (char)n};
  pkt.append(len, 4);
  pkt.append(nal);
}

// One NAL with the given first (header) byte plus filler so the length-validity
// guard (needs a few bytes past the prefix) is satisfied.
static std::string nal(uint8_t header) {
  std::string s;
  s.push_back((char)header);
  s.append(3, '\000');
  return s;
}

int main() {
  // ---- H264: drop only a non-IDR VCL slice with nal_ref_idc == 0 ----
  {
    // Leading non-reference slice (type 1, nal_ref_idc 0) => DROP.
    std::string p;
    appendNal(p, nal(0x01));
    assert(h264::isDroppableLeadingSlice(p.data(), p.size()));
  }
  {
    // Same, but with AUD + SPS + PPS in front: still finds the VCL slice => DROP.
    std::string p;
    appendNal(p, nal(0x09)); // AUD
    appendNal(p, nal(0x67)); // SPS
    appendNal(p, nal(0x68)); // PPS
    appendNal(p, nal(0x01)); // non-ref slice
    assert(h264::isDroppableLeadingSlice(p.data(), p.size()));
  }
  {
    // IDR slice (type 5) is a reference keyframe => KEEP.
    std::string p;
    appendNal(p, nal(0x65));
    assert(!h264::isDroppableLeadingSlice(p.data(), p.size()));
  }
  {
    // Reference non-IDR slice (type 1, nal_ref_idc 2) => KEEP.
    std::string p;
    appendNal(p, nal(0x41));
    assert(!h264::isDroppableLeadingSlice(p.data(), p.size()));
  }
  {
    // Parameter-set-only packet (no VCL slice) => KEEP.
    std::string p;
    appendNal(p, nal(0x67)); // SPS
    appendNal(p, nal(0x68)); // PPS
    assert(!h264::isDroppableLeadingSlice(p.data(), p.size()));
  }
  {
    // Truncated: length prefix claims more than is present => KEEP.
    std::string p;
    char bad[6] = {0, 0, 0, 100, 0x01, 0x00};
    p.append(bad, 6);
    assert(!h264::isDroppableLeadingSlice(p.data(), p.size()));
  }
  {
    // Empty => KEEP.
    assert(!h264::isDroppableLeadingSlice("", 0));
  }

  // ---- H265: drop only RASL leading pictures (types 8-9) ----
  {
    // RASL_N (type 8) => DROP.
    std::string p;
    appendNal(p, nal(0x10));
    assert(h265::isDroppableLeadingSlice(p.data(), p.size()));
  }
  {
    // RASL_R (type 9) behind VPS/SPS/PPS => DROP.
    std::string p;
    appendNal(p, nal(0x40)); // VPS (32)
    appendNal(p, nal(0x42)); // SPS (33)
    appendNal(p, nal(0x44)); // PPS (34)
    appendNal(p, nal(0x12)); // RASL_R (9)
    assert(h265::isDroppableLeadingSlice(p.data(), p.size()));
  }
  {
    // RADL (type 6) is decodable from the RAP => KEEP.
    std::string p;
    appendNal(p, nal(0x0C));
    assert(!h265::isDroppableLeadingSlice(p.data(), p.size()));
  }
  {
    // IDR_W_RADL (type 19) keyframe => KEEP.
    std::string p;
    appendNal(p, nal(0x26));
    assert(!h265::isDroppableLeadingSlice(p.data(), p.size()));
  }
  {
    // Trailing slice (type 0) => KEEP.
    std::string p;
    appendNal(p, nal(0x00));
    assert(!h265::isDroppableLeadingSlice(p.data(), p.size()));
  }
  {
    // Non-VCL only (VPS) => KEEP.
    std::string p;
    appendNal(p, nal(0x40));
    assert(!h265::isDroppableLeadingSlice(p.data(), p.size()));
  }

  return 0;
}
