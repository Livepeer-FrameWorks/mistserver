#include <mist/defines.h>
#include <mist/shared_memory.h>
#include <mist/stream.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <unistd.h>

namespace {
  int fail(const char *message) {
    std::cerr << message << std::endl;
    return 1;
  }
} // namespace

int main() {
  const std::string streamName = "fork-audit-offline-state-" + std::to_string(getpid());
  char statePageName[NAME_BUFFER_SIZE];
  snprintf(statePageName, sizeof(statePageName), SHM_STREAM_STATE, streamName.c_str());

  Util::setStreamOffline(streamName);
  IPC::sharedPage statePage(statePageName, STRMSTATE_PAGE_LEN, false, false);
  if (!statePage || statePage.len < STRMSTATE_PAGE_LEN) {
    return fail("offline state must not create a page smaller than the processing diagnostics ABI");
  }
  statePage.master = true;
  if (Util::getStreamStatus(streamName) != STRMSTAT_OFFLINE || Util::getStreamStatusPercentage(streamName) != 0) {
    return fail("setStreamOffline must publish OFFLINE with zero progress");
  }

  statePage.mapped[1] = 73;
  Util::setStreamOffline(streamName);
  if (Util::getStreamStatusPercentage(streamName) != 0) {
    return fail("re-publishing OFFLINE must clear stale progress");
  }

  Util::clearStreamOffline(streamName);
  if (Util::getStreamStatus(streamName) != STRMSTAT_OFF || Util::getStreamStatusPercentage(streamName) != 0) {
    return fail("clearStreamOffline must reset only the deliberate offline state");
  }
  Util::clearStreamOffline(streamName);
  if (Util::getStreamStatus(streamName) != STRMSTAT_OFF) { return fail("clearStreamOffline must be idempotent"); }

  const std::string attemptPageName = "/MstAttResUnit_" + std::to_string(getpid());
  IPC::sharedPage attemptPage(attemptPageName, 1, true, false);
  if (!attemptPage) { return fail("could not create offline-attempt result page"); }
  attemptPage.mapped[0] = 0;
  setenv("MIST_OFFLINE_RESULT_PAGE", attemptPageName.c_str(), 1);
  Util::reportAttemptOffline();
  unsetenv("MIST_OFFLINE_RESULT_PAGE");
  if (attemptPage.mapped[0] != 1) {
    return fail("child-side offline reporting must reach the advertised attempt page");
  }

  return 0;
}
