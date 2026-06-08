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

  return 0;
}
