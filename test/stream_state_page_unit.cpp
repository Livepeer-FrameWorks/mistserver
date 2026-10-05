// Outputs ask for their stream's state many times a second. The state page stays mapped between
// those calls instead of being opened for each one, and a page the stream's next input made
// after the previous one ended (unlinking its page) is still seen at once.
#include <mist/defines.h>
#include <mist/shared_memory.h>
#include <mist/stream.h>

#include <cstdio>
#include <iostream>
#include <string>
#include <unistd.h>

#ifdef __linux__
#include <dlfcn.h>
#include <fcntl.h>

namespace {
  size_t opens = 0;
}

extern "C" int shm_open(const char *name, int oflag, mode_t mode) {
  static int (*real)(const char *, int, mode_t) = (int (*)(const char *, int, mode_t))dlsym(RTLD_NEXT, "shm_open");
  ++opens;
  return real(name, oflag, mode);
}
#endif

namespace {
  int failures = 0;

  void expect(bool ok, const std::string & what) {
    if (!ok) {
      std::cerr << "FAIL: " << what << std::endl;
      ++failures;
    }
  }

  /// Makes the stream's state page as an input does, with the given state.
  void startInput(const std::string & pageName, uint8_t state) {
    IPC::sharedPage page(pageName, STRMSTATE_PAGE_LEN, true, false);
    page.mapped[0] = state;
    page.master = false;
  }

  /// Removes the stream's state page as an input that ends does.
  void endInput(const std::string & pageName) {
    IPC::sharedPage page(pageName, 0, false, false);
    page.master = true;
  }
} // namespace

int main() {
  const std::string streamName = "statepage" + std::to_string(getpid() % 100000);
  char pageName[NAME_BUFFER_SIZE];
  snprintf(pageName, NAME_BUFFER_SIZE, SHM_STREAM_STATE, streamName.c_str());

  expect(Util::getStreamStatus(streamName) == STRMSTAT_OFF, "a stream without a state page is off");
  startInput(pageName, STRMSTAT_READY);
  expect(Util::getStreamStatus(streamName) == STRMSTAT_READY, "a stream whose input started is seen at once");
#ifdef __linux__
  opens = 0;
#endif
  bool allReady = true;
  for (size_t i = 0; i < 1000; ++i) { allReady &= Util::getStreamStatus(streamName) == STRMSTAT_READY; }
  expect(allReady, "every read sees the state");
#ifdef __linux__
  expect(opens == 0, "reading the state 1000 times opens no page (opened " + std::to_string(opens) + ")");
#endif
  {
    IPC::sharedPage page(pageName, 0, false, false);
    page.mapped[0] = STRMSTAT_SHUTDOWN;
  }
  expect(Util::getStreamStatus(streamName) == STRMSTAT_SHUTDOWN, "a state change is seen at once");

  endInput(pageName);
  expect(Util::getStreamStatus(streamName) == STRMSTAT_OFF, "a stream whose input ended is off at once");
  startInput(pageName, STRMSTAT_BOOT);
  expect(Util::getStreamStatus(streamName) == STRMSTAT_BOOT, "the next input's state page is seen at once");
  endInput(pageName);

  if (failures) { return 1; }
  std::cout << "a stream's state page stays mapped between reads and is replaced as its inputs come and go" << std::endl;
  return 0;
}
