#include "../lib/offline_attempt.h"
#include "../lib/stream.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace {
  int fail(const char *message) {
    std::cerr << message << std::endl;
    return 1;
  }
} // namespace

int main() {
  const std::string outerPageName = "/MstAttOuter_" + std::to_string(getpid());
  IPC::sharedPage outerPage(outerPageName, 1, true, false);
  if (!outerPage) { return fail("could not create inherited offline-attempt page"); }
  outerPage.mapped[0] = 0;
  setenv("MIST_OFFLINE_RESULT_PAGE", outerPageName.c_str(), 1);

  bool offline = false;
  std::string innerPageName;
  {
    Util::OfflineAttemptResult attempt(&offline);
    innerPageName = attempt.name();
    if (innerPageName.empty() || innerPageName == outerPageName) {
      return fail("each nested start attempt must have a distinct result page");
    }
    pid_t child = attempt.runWithAdvertisement([&]() {
      pid_t spawned = fork();
      if (!spawned) {
        Util::reportAttemptOffline();
        _exit(0);
      }
      return spawned;
    });
    if (child <= 0) { return fail("could not fork offline-attempt reporter"); }
    const char *restored = getenv("MIST_OFFLINE_RESULT_PAGE");
    if (!restored || outerPageName != restored) {
      return fail("spawn must restore an inherited parent attempt immediately");
    }
    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      return fail("offline-attempt reporter child failed");
    }
  }
  if (!offline) { return fail("child-side offline result did not reach its owning attempt"); }
  if (outerPage.mapped[0] != 0) { return fail("nested attempt leaked its result into the parent attempt"); }

  Util::reportAttemptOffline();
  if (outerPage.mapped[0] != 1) { return fail("restored parent attempt cannot receive a later offline result"); }
  unsetenv("MIST_OFFLINE_RESULT_PAGE");

  bool directOffline = false;
  {
    Util::OfflineAttemptResult direct(&directOffline);
    direct.markOffline();
  }
  if (!directOffline) { return fail("same-process offline decisions must set the attempt result"); }

  {
    Util::OfflineAttemptResult disabled(NULL);
    if (disabled.runWithAdvertisement([]() { return 7; }) != 7) {
      return fail("disabled attempt wrapper changed the spawn result");
    }
  }

  bool unusedResult = false;
  {
    Util::OfflineAttemptResult plain(&unusedResult);
    const int spawnResult = plain.runWithAdvertisement([]() { return getenv("MIST_OFFLINE_RESULT_PAGE") ? 9 : -1; });
    if (spawnResult != 9) { return fail("attempt page was not advertised during spawn"); }
    if (getenv("MIST_OFFLINE_RESULT_PAGE")) {
      return fail("spawn without a parent attempt must clear its advertisement immediately");
    }
  }

  return 0;
}
