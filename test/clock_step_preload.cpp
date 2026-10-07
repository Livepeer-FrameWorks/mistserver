// LD_PRELOAD helper: CLOCK_MONOTONIC reads one second later from the moment main() starts, as
// when a second boundary passes between a program's static initialisation and its main().
#include <dlfcn.h>
#include <time.h>

namespace {
  bool mainStarted = false;
  int (*realMain)(int, char **, char **) = 0;

  int steppedMain(int argc, char **argv, char **envp) {
    mainStarted = true;
    return realMain(argc, argv, envp);
  }
} // namespace

extern "C" int clock_gettime(clockid_t clk, struct timespec *tp) {
  typedef int (*ClockGettime)(clockid_t, struct timespec *);
  static ClockGettime realClockGettime = (ClockGettime)dlsym(RTLD_NEXT, "clock_gettime");
  int ret = realClockGettime(clk, tp);
  if (!ret && mainStarted && clk == CLOCK_MONOTONIC) { tp->tv_sec += 1; }
  return ret;
}

extern "C" int __libc_start_main(int (*main)(int, char **, char **), int argc, char **argv, void (*init)(void),
                                 void (*fini)(void), void (*rtldFini)(void), void *stackEnd) {
  typedef int (*StartMain)(int (*)(int, char **, char **), int, char **, void (*)(void), void (*)(void), void (*)(void), void *);
  StartMain realStart = (StartMain)dlsym(RTLD_NEXT, "__libc_start_main");
  realMain = main;
  return realStart(steppedMain, argc, argv, init, fini, rtldFini, stackEnd);
}
