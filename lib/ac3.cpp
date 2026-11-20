#include "ac3.h"

#include "bitstream.h"

namespace AC3 {
  static const unsigned int rates[] = {48000, 44100, 32000};
  static const unsigned int channelCounts[] = {2, 1, 2, 3, 3, 4, 4, 5};

  static unsigned int dependentChannels(unsigned int locations) {
    static const unsigned int counts[] = {2, 2, 1, 1, 2, 2, 2, 1, 1};
    unsigned int count = 0;
    for (size_t bit = 0; bit < 9; ++bit) {
      if (locations & (1u << bit)) { count += counts[bit]; }
    }
    return count;
  }

  Header parse(const void *data, size_t bytes) {
    Header h;
    if (!data || bytes < 7) { return h; }
    const unsigned char *p = (const unsigned char *)data;
    Utils::bitstream bits;
    bits.append((const char *)data, bytes);
    bool complete = true;
    const auto get = [&](size_t count) -> uint64_t {
      if (count > 64 || count > bits.size()) {
        complete = false;
        return 0;
      }
      return bits.get(count);
    };
    const auto skip = [&](size_t count) {
      if (count > bits.size()) { complete = false; }
      bits.skip(count);
    };
    if (get(16) != 0x0B77) { return h; }
    h.bsid = p[5] >> 3;
    if (h.bsid <= 10) {
      skip(16);
      h.fscod = get(2);
      const unsigned int sizeCode = get(6);
      if (h.fscod == 3 || sizeCode >= 38) { return h; }
      h.bsid = get(5);
      h.bsmod = get(3);
      h.acmod = get(3);
      if (h.acmod == 2) {
        skip(2);
      } else {
        if (h.acmod & 1 && h.acmod != 1) { skip(2); }
        if (h.acmod & 4) { skip(2); }
      }
      h.lfe = get(1);
      static const unsigned int bitRates[] = {32,  40,  48,  56,  64,  80,  96,  112, 128, 160,
                                              192, 224, 256, 320, 384, 448, 512, 576, 640};
      const unsigned int kbps = bitRates[sizeCode / 2];
      const unsigned int shift = h.bsid > 8 ? h.bsid - 8 : 0;
      h.rate = rates[h.fscod] >> shift;
      h.bitRate = (kbps * 1000) >> shift;
      h.samples = 1536;
      h.frameBytes = h.fscod == 0 ? kbps * 4
        : h.fscod == 2            ? kbps * 6
                                  : ((1536 * kbps * 1000 / (44100 * 16)) + (sizeCode & 1)) * 2;
    } else if (h.bsid <= 16) {
      h.enhanced = true;
      h.streamType = get(2);
      h.substream = get(3);
      h.frameBytes = (get(11) + 1) * 2;
      if (h.streamType == 3 || h.frameBytes < 7) { return h; }
      h.fscod = get(2);
      unsigned int blocks = 6;
      if (h.fscod == 3) {
        const unsigned int code = get(2);
        if (code == 3) { return h; }
        h.rate = rates[code] / 2;
      } else {
        static const unsigned int blockCounts[] = {1, 2, 3, 6};
        blocks = blockCounts[get(2)];
        h.rate = rates[h.fscod];
      }
      h.acmod = get(3);
      h.lfe = get(1);
      h.bsid = get(5);
      for (unsigned int program = 0; program < (h.acmod ? 1u : 2u); ++program) {
        skip(5);
        if (get(1)) { skip(8); }
      }
      if (h.streamType == 1 && get(1)) { h.channelMap = get(16); }
      if (get(1)) {
        if (h.acmod > 2) { skip(2); }
        if (h.acmod & 1 && h.acmod > 2) { skip(6); }
        if (h.acmod & 4) { skip(6); }
        if (h.lfe && get(1)) { skip(5); }
        if (h.streamType == 0) {
          for (unsigned int program = 0; program < (h.acmod ? 1u : 2u); ++program) {
            if (get(1)) { skip(6); }
          }
          if (get(1)) { skip(6); }
          const unsigned int mixdef = get(2);
          if (mixdef == 1) { skip(5); }
          if (mixdef == 2) { skip(12); }
          if (mixdef == 3) {
            const unsigned int length = get(5);
            skip((length + 2) * 8);
          }
          if (h.acmod < 2) {
            for (unsigned int program = 0; program < (h.acmod ? 1u : 2u); ++program) {
              if (get(1)) { skip(14); }
            }
          }
          if (get(1)) {
            for (unsigned int block = 0; block < blocks; ++block) {
              if (blocks == 1 || get(1)) { skip(5); }
            }
          }
        }
      }
      if (get(1)) { h.bsmod = get(3); }
      h.samples = blocks * 256;
      h.bitRate = uint64_t(h.frameBytes) * 8 * h.rate / h.samples;
    } else {
      return h;
    }
    h.channels = channelCounts[h.acmod] + h.lfe;
    h.valid = complete;
    if (h.valid && h.enhanced && h.streamType == 0 && h.frameBytes <= bytes) {
      size_t offset = h.frameBytes;
      while (offset + 7 <= bytes && h.dependentSubstreams < 15) {
        const Header dependent = parse(p + offset, bytes - offset);
        if (!dependent.valid || dependent.streamType != 1 || dependent.substream != h.substream ||
            dependent.rate != h.rate || dependent.samples != h.samples || dependent.frameBytes > bytes - offset) {
          break;
        }
        // E.1.4 numbers chanmap from its MSB; F.6.1 numbers chan_loc from its LSB.
        for (unsigned int location = 0; location < 8; ++location) {
          if (dependent.channelMap & (1u << (10 - location))) { h.dependentLocations |= 1u << location; }
        }
        if (dependent.channelMap & 2) { h.dependentLocations |= 1u << 8; }
        ++h.dependentSubstreams;
        h.bitRate += dependent.bitRate;
        offset += dependent.frameBytes;
      }
      h.channels += dependentChannels(h.dependentLocations);
    }
    return h;
  }

  std::string makeDEC3(const Header & h) {
    if (!h.valid || !h.enhanced || h.streamType == 1 || h.substream) { return ""; }
    Utils::bitWriter bits;
    bits.append((h.bitRate + 999) / 1000, 13);
    bits.append(0, 3);
    bits.append(h.fscod, 2);
    bits.append(h.bsid, 5);
    bits.append(0, 2);
    bits.append(h.bsmod, 3);
    bits.append(h.acmod, 3);
    bits.append(h.lfe, 1);
    bits.append(0, 3);
    bits.append(h.dependentSubstreams, 4);
    bits.append(h.dependentLocations, h.dependentSubstreams ? 9 : 1);
    return bits.str();
  }

  Header parseDEC3(const void *data, size_t bytes) {
    Header h;
    if (!data || bytes < 5) { return h; }
    Utils::bitstream bits;
    bits.append((const char *)data, bytes);
    bool complete = true;
    const auto get = [&](size_t count) -> uint64_t {
      if (count > 64 || count > bits.size()) {
        complete = false;
        return 0;
      }
      return bits.get(count);
    };
    const auto skip = [&](size_t count) {
      if (count > bits.size()) { complete = false; }
      bits.skip(count);
    };
    h.enhanced = true;
    h.bitRate = get(13) * 1000;
    const unsigned int programs = get(3) + 1;
    for (unsigned int program = 0; program < programs; ++program) {
      const unsigned int fscod = get(2), bsid = get(5);
      skip(2);
      const unsigned int bsmod = get(3), acmod = get(3), lfe = get(1);
      skip(3);
      const unsigned int dependents = get(4);
      const unsigned int locations = get(dependents ? 9 : 1);
      if (!program) {
        h.fscod = fscod;
        h.bsid = bsid;
        h.bsmod = bsmod;
        h.acmod = acmod;
        h.lfe = lfe;
        h.rate = fscod < 3 ? rates[fscod] : 0;
        h.dependentSubstreams = dependents;
        h.dependentLocations = locations;
        h.channels = channelCounts[acmod] + lfe + dependentChannels(locations);
      }
    }
    h.valid = complete && h.bsid >= 11 && h.bsid <= 16;
    return h;
  }
} // namespace AC3
