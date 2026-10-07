#include <mist/config.h>

#include <cassert>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace {
  volatile sig_atomic_t diagnosticCalls = 0;
  volatile sig_atomic_t unrelatedCalls = 0;

  void diagnosticHandler(int) {
    ++diagnosticCalls;
  }
  void unrelatedHandler(int) {
    ++unrelatedCalls;
  }

  void installHandler(int signal, void (*handler)(int)) {
    struct sigaction action;
    action.sa_handler = handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    assert(sigaction(signal, &action, 0) == 0);
  }

  struct sigaction getHandler(int signal) {
    struct sigaction action;
    assert(sigaction(signal, 0, &action) == 0);
    return action;
  }

  void runChild(void (*scenario)()) {
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
      scenario();
      _exit(0);
    }

    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
  }

  void defaultDiagnosticIsIgnoredOnActivation() {
    installHandler(SIGUSR2, SIG_DFL);
    Util::Config config("config-signal-test");
    assert(getHandler(SIGUSR2).sa_handler == SIG_DFL);
    config.activate();
    assert(getHandler(SIGUSR2).sa_handler == SIG_IGN);
    raise(SIGUSR2);
    assert(config.is_active);
  }

  void noArgumentConstructionHasNoSignalSideEffect() {
    installHandler(SIGUSR2, SIG_DFL);
    Util::Config config;
    assert(getHandler(SIGUSR2).sa_handler == SIG_DFL);
    config.activate();
    assert(getHandler(SIGUSR2).sa_handler == SIG_IGN);
  }

  void customDiagnosticHandlerIsPreserved() {
    diagnosticCalls = 0;
    installHandler(SIGUSR2, diagnosticHandler);
    Util::Config config("config-signal-test");
    config.activate();
    assert(getHandler(SIGUSR2).sa_handler == diagnosticHandler);
    raise(SIGUSR2);
    assert(diagnosticCalls == 1);
    assert(config.is_active);
  }

  void explicitIgnoreIsPreserved() {
    installHandler(SIGUSR2, SIG_IGN);
    Util::Config config("config-signal-test");
    config.activate();
    assert(getHandler(SIGUSR2).sa_handler == SIG_IGN);
  }

  void shutdownAndUnrelatedSignalsKeepTheirContracts() {
    unrelatedCalls = 0;
    installHandler(SIGUSR1, unrelatedHandler);
    Util::Config config("config-signal-test");
    config.activate();
    assert(getHandler(SIGUSR1).sa_handler == unrelatedHandler);
    raise(SIGUSR1);
    assert(unrelatedCalls == 1);
    assert(config.is_active);
    raise(SIGTERM);
    assert(!config.is_active);
  }

  /// A process that is already stopping keeps receiving stop signals: MistUtilNuke repeats its stop
  /// every 10 ms, and a buffer's exit handler adds its own. Each one may interrupt the heap
  /// allocator, which the exiting process uses (destructors, the gcov dump at exit). The handler
  /// must then return without touching the heap: an allocation from the handler deadlocks on the
  /// allocator lock or corrupts the heap. A debug build still writes the backtrace of a process
  /// that receives a fifth stop signal while stopping.
  void repeatedStopSignalsDuringHeapUseAreHarmless() {
    int done[2], ready[2];
    assert(pipe(done) == 0);
    assert(pipe(ready) == 0);
    char logPath[] = "/tmp/config-signal-log.XXXXXX";
    int log = mkstemp(logPath);
    assert(log >= 0);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
      dup2(log, STDERR_FILENO);
      close(log);
      close(done[1]);
      close(ready[0]);
      fcntl(done[0], F_SETFL, O_NONBLOCK);
      Util::Config config("config-signal-test");
      config.activate();
      char byte = 0;
      if (write(ready[1], &byte, 1) != 1) { _exit(2); }
      while (read(done[0], &byte, 1) != 1) {
        for (size_t size = 16; size < 65536; size *= 2) { free(malloc(size)); }
      }
      _exit(config.is_active ? 1 : 0);
    }
    close(done[0]);
    close(ready[1]);
    char byte;
    assert(read(ready[0], &byte, 1) == 1);
    close(ready[0]);
    signal(SIGPIPE, SIG_IGN);
    for (int i = 0; i < 1000; ++i) {
      assert(kill(child, SIGTERM) == 0);
      usleep(200);
    }
    if (write(done[1], "x", 1) != 1) { perror("write"); }
    close(done[1]);

    // A deadlocked child never exits: give it 20 s, then fail.
    int status = 0;
    pid_t ret = 0;
    for (int i = 0; i < 2000 && !(ret = waitpid(child, &status, WNOHANG)); ++i) { usleep(10000); }
    if (!ret) {
      kill(child, SIGKILL);
      waitpid(child, &status, 0);
    }
    if (ret != child || !WIFEXITED(status) || WEXITSTATUS(status)) {
      fprintf(stderr, "child %s (status %d)\n", ret == child ? "ended badly" : "deadlocked", status);
    }
    assert(ret == child);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);

    std::string written;
    char buf[65536];
    ssize_t got;
    assert(lseek(log, 0, SEEK_SET) == 0);
    while ((got = read(log, buf, sizeof(buf))) > 0) { written.append(buf, got); }
    close(log);
    unlink(logPath);
#if DEBUG >= DLVL_DEVEL && defined(HASEXECINFO)
    const std::string header = "Backtrace of a stopping process that received another stop signal:\n";
    size_t at = written.find(header);
    if (at == std::string::npos) { fprintf(stderr, "no backtrace was written after repeated stop signals\n"); }
    assert(at != std::string::npos);
    // Frames follow the header, one per line, each naming its address.
    size_t frameEnd = written.find('\n', at + header.size());
    assert(frameEnd != std::string::npos);
    assert(written.substr(at + header.size(), frameEnd - at - header.size()).find("[0x") != std::string::npos);
#else
    assert(written.find("Backtrace") == std::string::npos);
#endif
  }

  void componentConstructionCannotOverwriteProcessType() {
    Util::Config::binaryType = Util::UNSET;
    assert(Util::Config::claimBinaryType(Util::INPUT) == Util::INPUT);
    assert(Util::Config::claimBinaryType(Util::OUTPUT) == Util::INPUT);

    Util::Config::binaryType = Util::PROCESS;
    assert(Util::Config::claimBinaryType(Util::INPUT) == Util::PROCESS);
    assert(Util::Config::claimBinaryType(Util::OUTPUT) == Util::PROCESS);
  }
} // namespace

int main() {
  runChild(defaultDiagnosticIsIgnoredOnActivation);
  runChild(noArgumentConstructionHasNoSignalSideEffect);
  runChild(customDiagnosticHandlerIsPreserved);
  runChild(explicitIgnoreIsPreserved);
  runChild(shutdownAndUnrelatedSignalsKeepTheirContracts);
  runChild(componentConstructionCannotOverwriteProcessType);
  repeatedStopSignalsDuringHeapUseAreHarmless();
  return 0;
}
