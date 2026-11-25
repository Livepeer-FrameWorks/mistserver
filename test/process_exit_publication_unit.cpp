#include <mist/procs.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <sys/resource.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {
  std::mutex mutex;
  std::condition_variable changed;
  pid_t expectedPid;
  bool reaped = false, releaseReaper = false, observerEntered = false, observerDone = false;
} // namespace

// Defined in the executable so it also replaces the waitpid that a shared libmist calls;
// a link-time --wrap only rewrites references inside this executable.
extern "C" pid_t waitpid(pid_t pid, int *status, int flags) {
  const pid_t result = wait4(pid, status, flags, nullptr);
  if (pid == -1 && result == expectedPid) {
    std::unique_lock<std::mutex> lock(mutex);
    reaped = true;
    changed.notify_all();
    changed.wait(lock, [] { return releaseReaper; });
  }
  return result;
}

int main() {
  expectedPid = fork();
  if (expectedPid < 0) { return 2; }
  if (!expectedPid) { _exit(17); }
  siginfo_t info = {};
  int result;
  do { result = waitid(P_PID, expectedPid, &info, WEXITED | WNOWAIT); } while (result && errno == EINTR);
  if (result) { return 2; }
  std::thread reaper([] { Util::Procs::reap(); });
  {
    std::unique_lock<std::mutex> lock(mutex);
    changed.wait(lock, [] { return reaped; });
  }
  bool running = true, available = false;
  int exitCode = -1;
  std::thread observer([&] {
    {
      std::lock_guard<std::mutex> lock(mutex);
      observerEntered = true;
      changed.notify_all();
    }
    running = Util::Procs::childRunning(expectedPid);
    available = Util::Procs::getExitCode(expectedPid, exitCode);
    std::lock_guard<std::mutex> lock(mutex);
    observerDone = true;
    changed.notify_all();
  });
  bool reportedBeforePublication;
  {
    std::unique_lock<std::mutex> lock(mutex);
    changed.wait(lock, [] { return observerEntered; });
    reportedBeforePublication = changed.wait_for(lock, std::chrono::milliseconds(100), [] { return observerDone; });
    releaseReaper = true;
    changed.notify_all();
  }
  reaper.join();
  observer.join();
  int unused;
  Util::Procs::getExitCode(expectedPid, unused);
  if (reportedBeforePublication || running || !available || exitCode != 17) {
    std::cerr << "FAIL: observed before publication=" << reportedBeforePublication << ", running=" << running
              << ", exit available=" << available << ", code=" << exitCode << std::endl;
    return 1;
  }
  if (Util::Procs::childRunning(0) || Util::Procs::childRunning(-1)) {
    std::cerr << "FAIL: nonpositive child handle treated as a process group" << std::endl;
    return 1;
  }
  std::cout << "Child liveness cannot overtake the reaper's exit-status publication" << std::endl;
  return 0;
}
