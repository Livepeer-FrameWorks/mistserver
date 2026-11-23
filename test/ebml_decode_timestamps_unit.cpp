#include <mist/ebml_socketglue.h>

#include <algorithm>
#include <cstdio>
#include <vector>

int main() {
  const uint64_t presentation[] = {0,   120, 80,  40,  240, 200, 160, 360, 320, 280,
                                   480, 440, 400, 600, 560, 520, 720, 680, 640, 840};
  for (uint64_t origin : {1000, 0}) {
    EBML::trackPredictor predictor;
    std::vector<uint64_t> sorted;
    for (uint64_t time : presentation) {
      predictor.add(origin + time, 1, 4, 0, time == 0, true);
      sorted.push_back(origin + time);
    }
    std::sort(sorted.begin(), sorted.end());
    const uint64_t delay = sorted[predictor.maxDelay] - sorted[0];
    predictor.finish();
    uint64_t previous = 0;
    size_t count = 0;
    while (predictor.hasPackets()) {
      const EBML::packetData & packet = predictor.getPacketData(true);
      if ((count && packet.time <= previous) || packet.offset < 0 ||
          int64_t(packet.time) + packet.offset != int64_t(origin + presentation[count])) {
        fprintf(stderr, "Matroska reorder clock violated DTS/offset/PTS: frame=%zu DTS=%llu previous=%llu offset=%lld PTS=%llu\n",
                count, (unsigned long long)packet.time, (unsigned long long)previous, (long long)packet.offset,
                (unsigned long long)(origin + presentation[count]));
        return 1;
      }
      if (sorted[count] >= origin + delay + count && packet.time != sorted[count] - delay) {
        fprintf(stderr, "Matroska decode clock does not trail by its fixed buffer span: frame=%zu DTS=%llu sorted=%llu delay=%llu\n",
                count, (unsigned long long)packet.time, (unsigned long long)sorted[count], (unsigned long long)delay);
        return 1;
      }
      previous = packet.time;
      ++count;
      predictor.remove();
    }
    if (count != sorted.size()) { return 1; }
    // Seeking reuses the same predictor clock without keeping its last decode time.
    predictor.flush();
    for (uint64_t time : presentation) { predictor.add(origin + time, 1, 4, 0, time == 0, true); }
    predictor.finish();
    const EBML::packetData & first = predictor.getPacketData(true);
    if (first.time != origin) {
      fprintf(stderr, "Matroska seek retained the previous decode position\n");
      return 1;
    }
    predictor.flush();
    for (uint64_t time : presentation) { predictor.add(origin + 10000 + time, 1, 4, 0, time == 0, true); }
    predictor.finish();
    const EBML::packetData & sought = predictor.getPacketData(true);
    if (sought.time != origin + 10000 - delay) {
      fprintf(stderr, "Matroska seek restarted its decode clock: DTS=%llu expected=%llu\n",
              (unsigned long long)sought.time, (unsigned long long)(origin + 10000 - delay));
      return 1;
    }
  }
  puts(
    "PASS Matroska input: fixed buffer-span decode lag, nonnegative offsets, monotonic startup/seek and original PTS");
}
