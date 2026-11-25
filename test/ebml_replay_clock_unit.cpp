#include <mist/ebml_socketglue.h>

#include <cstdio>
#include <utility>
#include <vector>
using Clock = std::vector<std::pair<uint64_t, int64_t>>;
static bool read(EBML::trackPredictor & p, const std::vector<uint64_t> & presentation, Clock & clock) {
  size_t emitted = 0;
  bool valid = true;
  const auto drain = [&]() {
    while (p.hasPackets()) {
      const auto & packet = p.getPacketData(true);
      if ((emitted && packet.time <= clock.back().first) || packet.offset < 0 ||
          int64_t(packet.time) + packet.offset != int64_t(presentation[emitted])) {
        fprintf(stderr, "invalid presentation/decode clock at frame %zu\n", emitted);
        valid = false;
      }
      clock.push_back({packet.time, packet.offset});
      ++emitted;
      p.remove();
    }
  };
  for (const auto pts : presentation) {
    p.add(pts, 1, 4, 0, false, true);
    drain();
  }
  p.finish();
  drain();
  return valid && clock.size() == presentation.size();
}
int main() {
  for (size_t depth : {2, 3, 5, 16}) {
    for (size_t seed = 0; seed < 12; ++seed) {
      std::vector<uint64_t> chronological(1 + depth * 60), coding;
      for (size_t i = 1; i < chronological.size(); ++i) {
        const uint64_t period = (seed % 3 == 0) ? 40 : 20 * (1 + ((i / (depth * 4) + seed) % 4));
        chronological[i] = chronological[i - 1] + period;
      }
      coding.push_back(0);
      for (size_t ref = depth; ref < chronological.size(); ref += depth) {
        coding.push_back(chronological[ref]);
        for (size_t n = 1; n < depth; ++n) { coding.push_back(chronological[ref - n]); }
      }
      EBML::trackPredictor predictor;
      predictor.maxDelay = depth;
      Clock index, replay;
      if (!read(predictor, coding, index)) { return 1; }
      predictor.flush();
      if (!read(predictor, coding, replay) || replay != index) {
        fprintf(stderr, "full replay differs at depth=%zu seed=%zu\n", depth, seed);
        return 1;
      }
      if (seed % 3 == 0 && predictor.decodeDelays.size() != 1) {
        fprintf(stderr, "constant frame cadence inflated the lag timeline\n");
        return 1;
      }
      for (size_t group : {2, 15, 31}) {
        const size_t first = 1 + (group - 1) * depth;
        EBML::trackPredictor sought;
        sought.maxDelay = predictor.maxDelay;
        sought.initialized = true;
        sought.decodeOrigin = predictor.decodeOrigin;
        sought.decodeDelay = predictor.decodeDelay;
        sought.decodeDelays = predictor.decodeDelays;
        std::vector<uint64_t> tail(coding.begin() + first, coding.end());
        Clock actual, expected(index.begin() + first, index.end());
        if (!read(sought, tail, actual) || actual != expected) {
          fprintf(stderr, "mid-file seek differs at depth=%zu seed=%zu group=%zu\n", depth, seed, group);
          return 1;
        }
      }
    }
  }
  puts("PASS 48 CFR/VFR clocks, full replay and 144 mid-file seeks at reorder depths 2/3/5/16");
}
