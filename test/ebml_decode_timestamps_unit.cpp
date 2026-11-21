#include <mist/ebml_socketglue.h>

#include <cstdio>

int main() {
  EBML::trackPredictor predictor;
  const uint64_t presentation[] = {0,   120, 80,  40,  240, 200, 160, 360, 320, 280,
                                   480, 440, 400, 600, 560, 520, 720, 680, 640, 840};
  for (uint64_t time : presentation) { predictor.add(time, 1, 4, 0, time == 0, true); }
  predictor.finish();
  uint64_t previous = 0;
  size_t count = 0;
  while (predictor.hasPackets()) {
    const EBML::packetData & packet = predictor.getPacketData(true);
    if ((count && packet.time <= previous) || int64_t(packet.time) + packet.offset != int64_t(presentation[count])) {
      fprintf(stderr, "B-frame DTS moved backwards or changed PTS at frame %zu: previous=%llu DTS=%llu offset=%llu PTS=%llu\n",
              count, (unsigned long long)previous, (unsigned long long)packet.time, (unsigned long long)packet.offset,
              (unsigned long long)presentation[count]);
      return 1;
    }
    previous = packet.time;
    ++count;
    predictor.remove();
  }
  if (count != sizeof(presentation) / sizeof(*presentation)) { return 1; }
  printf("PASS %zu reordered frames retain presentation timestamps and monotonic nonnegative DTS\n", count);
}
