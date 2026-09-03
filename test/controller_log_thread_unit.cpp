#include "../src/controller/controller_log_thread.h"

#include <atomic>
#include <cassert>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace {
  struct PipeReader {
      PipeReader() : finished(false) { assert(pipe(fds) == 0); }

      std::thread thread() {
        return std::thread([this]() {
          char byte = 0;
          while (read(fds[0], &byte, 1) > 0) {}
          finished = true;
        });
      }

      int fds[2];
      std::atomic<bool> finished;
  };

  int returnWhileLoggerIsRunning(PipeReader & pipeReader) {
    Controller::LogThread logThread(pipeReader.thread(), pipeReader.fds[0], pipeReader.fds[1]);
    assert(logThread.joinable());
    return 17;
  }
} // namespace

int main() {
  PipeReader earlyReturn;
  assert(returnWhileLoggerIsRunning(earlyReturn) == 17);
  assert(earlyReturn.finished);

  PipeReader explicitStop;
  Controller::LogThread logThread(explicitStop.thread(), explicitStop.fds[0], explicitStop.fds[1]);
  logThread.stop();
  assert(explicitStop.finished);
  assert(!logThread.joinable());

  // A repeated stop must not close an unrelated descriptor that reused one of the old numbers.
  int probe = open("/dev/null", O_RDONLY);
  assert(probe >= 0);
  logThread.stop();
  errno = 0;
  assert(fcntl(probe, F_GETFD) >= 0);
  close(probe);
  return 0;
}
