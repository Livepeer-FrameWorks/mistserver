#include <mist/dtsc.h>

#include <iostream>

// A live track's newest key only covers up to its last part, so a playback or recording start
// past the live edge must resolve to that newest key, never to the key slot one past it.
int main() {
  DTSC::Meta meta;
  meta.reInit("", true);
  meta.setLive(true);

  size_t track = meta.addTrack(8, 8, 32, 2, true);
  meta.setType(track, "video");
  meta.setCodec(track, "H264");
  for (uint64_t time = 1000; time < 4000; time += 250) { meta.update(time, 0, track, 100, 0, time % 1000 == 0, 100); }

  DTSC::Keys keys(meta.getKeys(track));
  const size_t lastKey = keys.getEndValid() - 1;
  if (keys.getTime(lastKey) != 3000) {
    std::cerr << "fixture's newest key starts at " << keys.getTime(lastKey) << ", expected 3000" << std::endl;
    return 1;
  }
  const uint64_t lastms = meta.getLastms(track);

  const uint64_t probes[] = {lastms, lastms + 1, lastms + 60000};
  for (uint64_t probe : probes) {
    const uint32_t keyIdx = meta.getKeyIndexForTime(track, probe);
    if (keyIdx != lastKey) {
      std::cerr << "time " << probe << " past the live edge (" << lastms << ") resolved to key " << keyIdx << " (time "
                << meta.getTimeForKeyIndex(track, keyIdx) << "), expected newest key " << lastKey << " at 3000" << std::endl;
      return 1;
    }
  }

  if (meta.getKeyIndexForTime(track, 2500) != lastKey - 1) {
    std::cerr << "time inside the second key did not resolve to it" << std::endl;
    return 1;
  }

  DTSC::Meta empty;
  empty.reInit("", true);
  size_t emptyTrack = empty.addTrack(8, 8, 32, 2, true);
  empty.setType(emptyTrack, "video");
  DTSC::Keys emptyKeys(empty.getKeys(emptyTrack));
  if (empty.getKeyIndexForTime(emptyTrack, 1000) != emptyKeys.getEndValid()) {
    std::cerr << "a track without keys did not resolve to its end position" << std::endl;
    return 1;
  }
  return 0;
}
