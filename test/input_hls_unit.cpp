#include "../src/input/input_hls.h"

#include <atomic>
#include <cassert>
#include <thread>

class SequentialHLSProbe : public Mist::InputHLS {
  public:
    explicit SequentialHLSProbe(Util::Config *config) : Mist::InputHLS(config) { meta.reInit("", true); }
    size_t addMediaTrack(uint32_t playlist, uint32_t pid, const char *type, const char *codec) {
      const size_t idx = meta.addTrack();
      meta.setID(idx, getPacketID(playlist, pid));
      meta.setType(idx, type);
      meta.setCodec(idx, codec);
      return idx;
    }
    size_t seekAnchorTrack() { return muxedPlaylistSeekAnchorTrack(); }
};

int main() {
  Util::Config config("hls-sequential-unit");
  SequentialHLSProbe input(&config);
  assert(input.seekAnchorTrack() == INVALID_TRACK_ID);
  const size_t video = input.addMediaTrack(1, 256, "video", "H264");
  input.addMediaTrack(1, 257, "audio", "AAC");
  // Both tracks share the playlist; video is only the seek anchor, not a filter.
  assert(input.seekAnchorTrack() == video);
  input.addMediaTrack(2, 258, "audio", "AAC");
  assert(input.seekAnchorTrack() == INVALID_TRACK_ID);

  {
    std::lock_guard<std::mutex> guard(Mist::entryMutex);
    Mist::listEntries.clear();
    Mist::playListEntries entry;
    entry.filename = "segment-a.ts";
    entry.bytePos = 101;
    entry.timeOffset = -42;
    Mist::listEntries[7].push_back(entry);
  }

  Mist::playListEntries snapshot;
  assert(Mist::snapshotPlaylistEntry(7, 0, snapshot));
  assert(snapshot.filename == "segment-a.ts");
  assert(snapshot.bytePos == 101);
  assert(snapshot.timeOffset == -42);
  assert(!Mist::snapshotPlaylistEntry(7, 1, snapshot));
  assert(!Mist::snapshotPlaylistEntry(8, 0, snapshot));

  std::atomic<bool> running(true);
  std::thread writer([&running]() {
    for (size_t i = 0; i < 10000; ++i) {
      std::lock_guard<std::mutex> guard(Mist::entryMutex);
      Mist::playListEntries replacement;
      replacement.filename = (i & 1) ? "odd.ts" : "even.ts";
      replacement.bytePos = (i & 1) ? 1 : 2;
      Mist::listEntries[7][0] = replacement;
    }
    running = false;
  });
  while (running) {
    assert(Mist::snapshotPlaylistEntry(7, 0, snapshot));
    assert((snapshot.filename == "segment-a.ts" && snapshot.bytePos == 101) ||
           (snapshot.filename == "odd.ts" && snapshot.bytePos == 1) || (snapshot.filename == "even.ts" && snapshot.bytePos == 2));
  }
  writer.join();

  {
    std::lock_guard<std::mutex> guard(Mist::entryMutex);
    Mist::listEntries[7].clear();
  }
  assert(!Mist::snapshotPlaylistEntry(7, 0, snapshot));
  return 0;
}
