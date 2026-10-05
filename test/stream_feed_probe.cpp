#include <mist/defines.h>
#include <mist/shared_memory.h>
#include <mist/timing.h>

#include <cstdio>
#include <cstdlib>
#include <string>

/// Watches the feed-paused flag a process-controlled stream's buffer publishes on its state page
/// for the given number of milliseconds, and prints for how many of them the feed was paused.
int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: %s stream milliseconds\n", argv[0]);
    return 2;
  }
  const std::string stream = argv[1];
  const uint64_t watchMs = strtoull(argv[2], 0, 10);
  char pageName[NAME_BUFFER_SIZE];
  snprintf(pageName, sizeof(pageName), SHM_STREAM_STATE, stream.c_str());
  IPC::sharedPage page(pageName, STRMSTATE_PAGE_LEN, false, false);
  if (!page.mapped || page.len <= STRMSTATE_PROCESS_FEED_PAUSED_OFFSET) {
    fprintf(stderr, "stream %s has no state page\n", stream.c_str());
    return 1;
  }
  page.master = false;
  const uint64_t start = Util::bootMS();
  uint64_t last = start;
  uint64_t paused = 0;
  while (Util::bootMS() < start + watchMs) {
    Util::sleep(5);
    const uint64_t now = Util::bootMS();
    if (page.mapped[STRMSTATE_PROCESS_FEED_PAUSED_OFFSET]) { paused += now - last; }
    last = now;
  }
  printf("%llu\n", (unsigned long long)paused);
  return 0;
}
