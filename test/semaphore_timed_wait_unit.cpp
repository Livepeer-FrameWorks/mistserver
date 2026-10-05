// IPC::semaphore's timed waits wait as long as asked for a semaphore another process holds, also
// when a signal interrupts them, and count each acquisition so every one can be released.
#include <mist/shared_memory.h>
#include <mist/timing.h>

#include <cinttypes>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace {
  int failures = 0;

  void expect(bool ok, const std::string & what) {
    if (!ok) {
      std::cerr << "FAIL: " << what << std::endl;
      ++failures;
    }
  }

  void onAlarm(int) {}
} // namespace

int main() {
  const std::string name = "/MstSemWaitUnit" + std::to_string(getpid() % 100000);
  IPC::semaphore held(name.c_str(), O_CREAT | O_RDWR, ACCESSPERMS, 1);
  held.wait();

  pid_t waiter = fork();
  if (!waiter) {
    // A signal that arrives during a wait interrupts it without restarting it.
    struct sigaction alarmAction = {};
    alarmAction.sa_handler = onAlarm;
    sigaction(SIGALRM, &alarmAction, 0);
    IPC::semaphore sem(name.c_str(), O_RDWR, ACCESSPERMS, 1);
    uint64_t start = Util::bootMS();
    const bool second = sem.tryWaitOneSecond();
    const uint64_t secondMs = Util::bootMS() - start;
    ualarm(200000, 0);
    start = Util::bootMS();
    const bool timed = sem.tryWait(700);
    const uint64_t timedMs = Util::bootMS() - start;
    printf("tryWaitOneSecond on a held semaphore: %s after %" PRIu64 " ms\n", second ? "acquired" : "gave up", secondMs);
    printf("tryWait(700) on a held semaphore, interrupted after 200 ms: %s after %" PRIu64 " ms\n",
           timed ? "acquired" : "gave up", timedMs);
    fflush(stdout);
    sem.abandon();
    _exit((!second && secondMs >= 900 && secondMs < 1500 ? 0 : 1) | (!timed && timedMs >= 650 && timedMs < 1200 ? 0 : 2));
  }
  int status = 0;
  waitpid(waiter, &status, 0);
  expect(WIFEXITED(status) && !(WEXITSTATUS(status) & 1), "tryWaitOneSecond waits a second for a held semaphore");
  expect(WIFEXITED(status) && !(WEXITSTATUS(status) & 2),
         "tryWait(ms) waits as long as asked for a held semaphore, also when a signal interrupts it");
  held.post();

  held.unlink();

  // A semaphore that can be acquired twice, through timed waits; both acquisitions are released.
  {
    const std::string twiceName = name + "b";
    IPC::semaphore twice(twiceName.c_str(), O_CREAT | O_RDWR, ACCESSPERMS, 2);
    expect(twice.tryWait(100) && twice.tryWait(100), "two timed waits acquire a semaphore with value 2");
    twice.post();
    twice.post();
    expect(twice.tryWait(100) && twice.tryWait(100), "both timed acquisitions were released, so both can be made again");
    twice.close();
    twice.open(twiceName.c_str(), O_RDWR, ACCESSPERMS, 2);
    twice.unlink();
  }

  if (failures) { return 1; }
  std::cout << "timed semaphore waits wait as long as asked and count what they acquire" << std::endl;
  return 0;
}
