#include <mist/config.h>

#include <cassert>
#include <csignal>
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
  return 0;
}
