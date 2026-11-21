#include <mist/ebml_socketglue.h>
#include <mist/mp4_generic.h>

#include <cstdio>

int main() {
  EBML::trackPredictor predictor;
  const uint64_t presentation[] = {0, 120, 40, 80, 240, 160, 200};
  for (uint64_t time : presentation) { predictor.add(time, 1, 10, 1, time == 0, true); }
  predictor.finish();
  uint64_t previous = 0;
  for (size_t i = 0; i < 7; ++i) {
    const EBML::packetData & packet = predictor.getPacketData(true);
    if ((i && packet.time <= previous) || int64_t(packet.time) + int64_t(packet.offset) != int64_t(presentation[i])) {
      fprintf(stderr, "B-frame decode timestamps clamped or presentation shifted: frame=%zu DTS=%llu offset=%lld PTS=%llu\n",
              i, (unsigned long long)packet.time, (long long)packet.offset, (unsigned long long)presentation[i]);
      return 1;
    }
    previous = packet.time;
    predictor.remove();
  }
  MP4::TRUN run;
  run.setFlags(MP4::trunsampleOffsets);
  MP4::trunSampleInformation sample = {};
  sample.sampleOffset = -40;
  run.setSampleInformation(sample, 0);
  if (run.getVersion() != 1 || run.getSampleInformation(0).sampleOffset != -40) {
    fprintf(stderr, "negative CMAF composition offset was written unsigned\n");
    return 1;
  }
  MP4::CTTS offsets;
  MP4::CTTSEntry entry = {1, -40};
  offsets.setCTTSEntry(entry, 0);
  if (offsets.getVersion() != 1 || offsets.getCTTSEntry(0).sampleOffset != -40) {
    fprintf(stderr, "negative MP4 composition offset was written unsigned\n");
    return 1;
  }
  puts("PASS monotonic DTS and original presentation times with signed MP4/CMAF offsets");
}
