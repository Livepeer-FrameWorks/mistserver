// An output fills in only the fields of a buffered packet that its kind of track uses: a seek into
// a raw track's embedded frames sets its track, part and time, and nothing else. The other fields
// must start out cleared, whatever memory the entry is made in. A packet left marked unavailable
// is skipped by packetSorter::nonGhost, so an output that is behind waits between every frame
// instead of catching up, and falls out of the frame window.
#include <mist/stream.h>

#include <cstring>
#include <iostream>
#include <new>
#include <string>

namespace {
  int failures = 0;

  void expect(bool ok, const std::string & what) {
    if (!ok) {
      std::cerr << "FAIL: " << what << std::endl;
      ++failures;
    }
  }
} // namespace

int main() {
  alignas(Util::sortedPageInfo) unsigned char dirty[sizeof(Util::sortedPageInfo)];
  memset(dirty, 0xFF, sizeof(dirty));
  Util::sortedPageInfo *entry = new (dirty) Util::sortedPageInfo;
  unsigned char flags[2];
  memcpy(flags, &entry->ghostPacket, 1);
  memcpy(flags + 1, &entry->unavailable, 1);
  expect(!flags[0] && !flags[1], "a new entry made in used memory is neither a ghost nor unavailable");
  expect(!entry->tid && !entry->time && !entry->offset && !entry->partIndex, "a new entry has no position");

  // As Output::seek buffers the first embedded frame of a raw track.
  entry->tid = 1;
  entry->partIndex = 7;
  entry->time = 3660;
  Util::packetSorter buffer;
  buffer.insert(*entry);
  expect(buffer.nonGhost() == 3660, "a sought embedded frame counts as a packet that is ready");

  if (failures) { return 1; }
  std::cout << "buffered packets start out with every field cleared" << std::endl;
  return 0;
}
