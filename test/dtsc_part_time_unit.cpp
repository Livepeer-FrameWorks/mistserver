#include <mist/dtsc.h>

#include <iostream>

class PartTimeMeta : public DTSC::Meta {
  public:
    void dropFirstKey(size_t track) {
      DTSC::Track & t = tracks.at(track);
      uint64_t partsToDelete = t.keys.getInt(t.keyPartsField, t.keys.getDeleted());
      t.parts.deleteRecords(partsToDelete);
      t.keys.deleteRecords(1);
      setFirstms(track, t.keys.getInt(t.keyTimeField, t.keys.getDeleted()));
    }
};

int main() {
  PartTimeMeta meta;
  meta.reInit("", true);

  size_t track = meta.addTrack(8, 8, 16, 2, true);
  meta.setType(track, "video");
  meta.setCodec(track, "H264");

  meta.update(1000, 0, track, 100, 0, true, 100);
  meta.update(1500, 0, track, 100, 0, false, 100);
  meta.update(2000, 0, track, 100, 0, true, 100);
  meta.update(2500, 0, track, 100, 0, false, 100);
  meta.update(3000, 0, track, 100, 0, true, 100);
  meta.update(3500, 0, track, 100, 0, false, 100);

  meta.dropFirstKey(track);

  DTSC::Parts parts(meta.parts(track));
  uint64_t firstPartTime = meta.getPartTime(parts.getFirstValid(), track);
  if (firstPartTime != 2000) {
    std::cerr << "first valid part resolved to " << firstPartTime << ", expected 2000" << std::endl;
    return 1;
  }

  if (meta.getPartTime(parts.getFirstValid() - 1, track)) {
    std::cerr << "deleted part still resolves to a timestamp" << std::endl;
    return 1;
  }

  const size_t finalPart = parts.getEndValid() - 1;
  if (meta.getPartIndex(meta.getPartTime(finalPart, track), track) != finalPart) {
    std::cerr << "final valid part timestamp did not resolve to the final part" << std::endl;
    return 1;
  }
  const uint64_t finalEnd = meta.getPartTime(finalPart, track) + parts.getDuration(finalPart);
  if (meta.getPartIndex(finalEnd, track) != parts.getEndValid()) {
    std::cerr << "final part end did not resolve to the half-open range end" << std::endl;
    return 1;
  }

  PartTimeMeta reordered;
  reordered.reInit("", true);
  size_t reorderedTrack = reordered.addTrack(2, 2, 6, 1, true);
  reordered.setType(reorderedTrack, "video");
  reordered.setCodec(reorderedTrack, "H264");
  reordered.update(1000, 120, reorderedTrack, 100, 0, true, 40);
  reordered.update(1040, 200, reorderedTrack, 100, 0, false, 40);
  reordered.update(1080, 40, reorderedTrack, 100, 0, false, 40);
  reordered.update(1120, 80, reorderedTrack, 100, 0, false, 40);
  reordered.update(1160, 120, reorderedTrack, 100, 0, false, 40);
  reordered.update(1200, 120, reorderedTrack, 100, 0, true, 40);
  reordered.applyLimiter(1000, 1120);

  DTSC::Keys limitedKeys = reordered.getKeys(reorderedTrack);
  if (limitedKeys.getTotalPartCount() != 4) {
    std::cerr << "reordered limiter retained " << limitedKeys.getTotalPartCount()
              << " parts, expected 4 to preserve the final presentation frame" << std::endl;
    return 1;
  }

  PartTimeMeta reorderedAcrossKeys;
  reorderedAcrossKeys.reInit("", true);
  size_t acrossTrack = reorderedAcrossKeys.addTrack(3, 3, 11, 1, true);
  reorderedAcrossKeys.setType(acrossTrack, "video");
  reorderedAcrossKeys.setCodec(acrossTrack, "H264");
  for (size_t partNo = 0; partNo < 11; ++partNo) {
    const int64_t offsets[] = {120, 200, 40, 80, 120};
    reorderedAcrossKeys.update(1000 + partNo * 40, offsets[partNo % 5], acrossTrack, 100, 0,
                               partNo == 0 || partNo == 5 || partNo == 10, 40);
  }
  reorderedAcrossKeys.applyLimiter(1000, 1320);

  DTSC::Keys acrossKeys = reorderedAcrossKeys.getKeys(acrossTrack);
  if (acrossKeys.getTotalPartCount() != 9) {
    std::cerr << "multi-key reordered limiter retained " << acrossKeys.getTotalPartCount()
              << " parts, expected 9 to preserve the final presentation frame" << std::endl;
    return 1;
  }

  return 0;
}
