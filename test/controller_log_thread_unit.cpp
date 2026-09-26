#include "../src/controller/controller_log_thread.h"

#include <mist/util.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <thread>
#include <unistd.h>

// Checks are explicit rather than assert(): release builds define NDEBUG,
// which would compile every assertion, and the calls inside it, away.
namespace {
  int failures = 0;

  void check(bool ok, const char *what) {
    if (!ok) {
      fprintf(stderr, "FAIL: %s\n", what);
      ++failures;
    }
  }

  struct PipeReader {
      PipeReader() : finished(false) { check(pipe(fds) == 0, "create pipe"); }

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
    check(logThread.joinable(), "a running log thread is joinable");
    return 17;
  }
} // namespace

int main() {
  PipeReader earlyReturn;
  check(returnWhileLoggerIsRunning(earlyReturn) == 17, "scope exit returns the function result");
  check(earlyReturn.finished, "scope exit joins the log thread");

  PipeReader explicitStop;
  Controller::LogThread logThread(explicitStop.thread(), explicitStop.fds[0], explicitStop.fds[1]);
  logThread.stop();
  check(explicitStop.finished, "stop joins the log thread");
  check(!logThread.joinable(), "a stopped log thread is not joinable");

  // A repeated stop must not close an unrelated descriptor that reused one of the old numbers.
  int probe = open("/dev/null", O_RDONLY);
  check(probe >= 0, "open probe descriptor");
  logThread.stop();
  errno = 0;
  check(fcntl(probe, F_GETFD) >= 0, "a repeated stop leaves reused descriptors open");
  close(probe);

  // Production shape: the reader is the real log parser and another writer
  // (every child's stderr, here a second descriptor) keeps the pipe open, so
  // it never reaches end-of-file. Closing the descriptors does not wake a
  // blocked read on Linux; stop() must still end the reader.
  {
    int fds[2];
    check(pipe(fds) == 0, "create log pipe");
    int otherWriter = dup(fds[1]);
    check(otherWriter >= 0, "hold a second writer");
    int sink = open("/dev/null", O_WRONLY);
    check(sink >= 0, "open log sink");
    std::atomic<bool> stopFlag(false);
    std::atomic<bool> parserReturned(false);
    Controller::LogThread parserThread(std::thread([&]() {
      Util::logParser(fds[0], sink, false, 0, &stopFlag);
      parserReturned = true;
    }),
                                       fds[0], fds[1], &stopFlag);
    check(write(otherWriter, "INFO|test|1||x\n", 15) == 15, "write a log line");
    parserThread.stop();
    check(parserReturned, "stop ends a log parser whose pipe has another writer");
    close(otherWriter);
    close(sink);
  }
  return failures ? 1 : 0;
}
