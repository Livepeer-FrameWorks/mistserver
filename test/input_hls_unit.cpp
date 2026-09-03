#include "../src/input/input_hls.h"

#include <atomic>
#include <cassert>
#include <thread>

int main() {
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
