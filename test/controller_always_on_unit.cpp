#include "../src/controller/controller_always_on_policy.h"

#include <mist/defines.h>
#include <mist/shared_memory.h>
#include <mist/stream.h>
#include <mist/timing.h>

#include <fcntl.h>
#include <iostream>
#include <string>
#include <unistd.h>

namespace {
  int failures = 0;

  void check(bool ok, const std::string & message) {
    if (ok) { return; }
    std::cerr << "FAIL: " << message << std::endl;
    ++failures;
  }
} // namespace

int main() {
  using namespace Controller;

  // Decision table.
  check(alwaysOnAction(true, STRMSTAT_OFF, false, false, 0) == ALWAYS_ON_NONE, "a tracked running input needs no action");
  check(alwaysOnAction(false, STRMSTAT_OFF, false, false, 0) == ALWAYS_ON_START, "an unserved always-on stream must be started");
  check(alwaysOnAction(false, STRMSTAT_OFFLINE, false, false, 0) == ALWAYS_ON_START,
        "an offline always-on stream without an input must be restarted");
  check(alwaysOnAction(false, STRMSTAT_OFF, false, true, 4242) == ALWAYS_ON_ADOPT,
        "a pull-locked stream (SRT listener after a rolling restart) must be adopted, not started");
  check(alwaysOnAction(false, STRMSTAT_OFF, true, false, 4242) == ALWAYS_ON_ADOPT, "an input-locked stream must be adopted, not started");
  check(alwaysOnAction(false, STRMSTAT_OFF, false, true, 0) == ALWAYS_ON_SKIP,
        "a pull-locked stream without a readable PID must be skipped, not started");
  check(alwaysOnAction(false, STRMSTAT_INVALID, false, false, 0) == ALWAYS_ON_SKIP,
        "a non-terminal stream state means the stream is served");
  check(alwaysOnAction(false, STRMSTAT_READY, false, false, 4242) == ALWAYS_ON_ADOPT,
        "an online stream with a readable input PID must be adopted");

  // Real lock and PID-page probes, as used by checkStream.
  const std::string stream = "alwaysOnUnit" + std::to_string(getpid());
  check(!Util::streamPullAlive(stream), "no pull lock exists yet");
  check(Util::streamInputPid(stream) == 0, "no PID page exists yet");

  const std::string pullName = "/MstSemPull_" + stream;
  IPC::semaphore pullLock(pullName.c_str(), O_CREAT | O_RDWR, ACCESSPERMS, 1);
  check((bool)pullLock, "could not create the pull lock fixture");
  check(!Util::streamPullAlive(stream), "an unheld pull lock must not mark the stream as served");
  check(pullLock.tryWait(), "could not take the pull lock fixture");

  const uint64_t start = Util::bootMS();
  check(Util::streamPullAlive(stream), "a held pull lock must mark the stream as served");
  check(Util::bootMS() - start < 1000, "probing a held pull lock must return promptly");

  char pageName[NAME_BUFFER_SIZE];
  snprintf(pageName, NAME_BUFFER_SIZE, SHM_STREAM_PPID, stream.c_str());
  IPC::sharedPage pidPage(pageName, 8, true, false);
  check((bool)pidPage, "could not create the pull PID page fixture");
  if (pidPage) { *(uint64_t *)(pidPage.mapped) = getpid(); }
  check(Util::streamInputPid(stream) == getpid(), "the running pull process PID must be readable for adoption");
  check(alwaysOnAction(false, Util::getStreamStatus(stream), false, Util::streamPullAlive(stream),
                       Util::streamInputPid(stream)) == ALWAYS_ON_ADOPT,
        "a live pull input unknown to the controller must be adopted instead of spawning a duplicate");
  // A stale PID page of an exited process must not be adopted.
  if (pidPage) { *(uint64_t *)(pidPage.mapped) = 0x7ffffff0; }
  check(Util::streamInputPid(stream) == 0, "a PID page naming a dead process must not be adopted");

  pullLock.post();
  check(!Util::streamPullAlive(stream), "a released pull lock must no longer mark the stream as served");
  pullLock.unlink();
  pidPage.master = true;
  pidPage.close();

  if (failures) {
    std::cerr << failures << " failure(s)" << std::endl;
    return 1;
  }
  return 0;
}
