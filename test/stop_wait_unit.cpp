// A process told to stop gives up waiting for what it would no longer use: a session that does not
// start, a shared page that does not appear, a stream that does not become available, and a
// stream's track list lock that a holder that died never releases (which a process also gives up on
// once the stream shuts down). It is told to stop by a signal (the handler Util::Config::activate
// installs) or by Util::Config::requestStop. Each case runs in a process of its own, since a stop is
// never taken back.
#include <mist/comms.h>
#include <mist/config.h>
#include <mist/defines.h>
#include <mist/dtsc.h>
#include <mist/shared_memory.h>
#include <mist/stream.h>
#include <mist/timing.h>

#include <csignal>
#include <cstdio>
#include <functional>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {
  int failures = 0;

  void expect(bool ok, const std::string & what) {
    if (!ok) {
      std::cerr << "FAIL: " << what << std::endl;
      ++failures;
    }
  }

  enum StopBy { NO_STOP, SIGNAL, REQUEST };

  /// Runs body in a child process that is told to stop stopAfterMs into it (before it, at 0).
  /// Returns how long the body ran; 0 when it was still running after limitMs. The body's result
  /// becomes the child's exit code.
  uint64_t runChild(StopBy stopBy, uint64_t stopAfterMs, const std::function<int()> & body, int & exitCode, uint64_t limitMs = 5000) {
    int took[2];
    if (pipe(took)) { exit(2); }
    pid_t child = fork();
    if (!child) {
      close(took[0]);
      if (stopBy == SIGNAL) {
        Util::Config conf;
        conf.activate();
      }
      auto stop = [stopBy]() {
        if (stopBy == SIGNAL) { kill(getpid(), SIGTERM); }
        if (stopBy == REQUEST) { Util::Config::requestStop(); }
      };
      std::thread stopper;
      if (stopBy != NO_STOP) {
        if (!stopAfterMs) {
          stop();
        } else {
          stopper = std::thread([stop, stopAfterMs]() {
            Util::sleep(stopAfterMs);
            stop();
          });
        }
      }
      const uint64_t start = Util::bootMS();
      const int result = body();
      const uint64_t ran = Util::bootMS() - start + 1;
      if (write(took[1], &ran, sizeof(ran)) != sizeof(ran)) { _exit(2); }
      if (stopper.joinable()) { stopper.join(); }
      _exit(result);
    }
    close(took[1]);
    const uint64_t start = Util::bootMS();
    int status = 0;
    bool done = false;
    while (Util::bootMS() - start < limitMs) {
      if (waitpid(child, &status, WNOHANG) == child) {
        done = true;
        break;
      }
      Util::sleep(20);
    }
    if (!done) {
      kill(child, SIGKILL);
      waitpid(child, &status, 0);
    }
    uint64_t ran = 0;
    if (!done || read(took[0], &ran, sizeof(ran)) != sizeof(ran)) { ran = 0; }
    close(took[0]);
    exitCode = (done && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    return ran;
  }

  std::string ms(uint64_t ran) {
    return ran ? std::to_string(ran) + " ms" : "still running after its limit";
  }

  /// Waits for a session that never appears; 0 when it was reported as not started.
  int waitForMissingSession(const std::string & sessionId) {
    Comms::Connections conn;
    conn.reload(sessionId, false, false);
    return (!conn && conn.sessionStartFailed()) ? 0 : 3;
  }

  void sessionWait() {
    const std::string base = "nowait" + std::to_string(getpid());
    int code = 0;
    uint64_t ran = runChild(REQUEST, 0, [&]() { return waitForMissingSession(base + "a"); }, code);
    expect(ran && ran < 1000, "a stopped process does not wait for its session (" + ms(ran) + ")");
    expect(!code, "a session a stopped process did not wait for is reported as not started");

    ran = runChild(SIGNAL, 500, [&]() { return waitForMissingSession(base + "b"); }, code);
    expect(ran >= 400 && ran < 2000, "a process signalled to stop 500 ms into its session wait gives up at once (" + ms(ran) + ")");
    expect(!code, "a session wait ended by a stop is reported as not started");
  }

  void sharedPageWait() {
    const std::string pageName = "/MstStopWaitUnit_" + std::to_string(getpid());
    int code = 0;
    const uint64_t ran = runChild(REQUEST, 300, [&]() {
      IPC::sharedPage page(pageName, 0, false, true);
      return page ? 3 : 0;
    }, code);
    expect(ran && ran < 2000, "opening a page that does not appear gives up once stopped after 300 ms (" + ms(ran) + ")");
    expect(!code, "a page that was never created does not open");
  }

  void startInputWait() {
    const std::string streamName = "stopwait" + std::to_string(getpid() % 100000);
    char statePageName[NAME_BUFFER_SIZE];
    snprintf(statePageName, sizeof(statePageName), SHM_STREAM_STATE, streamName.c_str());
    IPC::sharedPage statePage(statePageName, STRMSTATE_PAGE_LEN, true, false);
    if (!statePage) {
      expect(false, "the stream state page is created");
      return;
    }
    statePage.mapped[0] = STRMSTAT_BOOT;
    int code = 0;
    const uint64_t ran = runChild(SIGNAL, 300, [&]() { return Util::startInput(streamName, "", true, false) ? 3 : 0; }, code);
    expect(ran && ran < 2000, "waiting for a booting stream gives up once signalled to stop after 300 ms (" + ms(ran) + ")");
    expect(!code, "a stopped caller does not report the stream as started");
  }

  /// Takes the track list lock of the stream and dies holding it.
  void dieHoldingTheLock(const std::string & streamName) {
    pid_t holder = fork();
    if (!holder) {
      char name[NAME_BUFFER_SIZE];
      snprintf(name, NAME_BUFFER_SIZE, SEM_TRACKLIST, streamName.c_str());
      IPC::semaphore lock(name, O_CREAT | O_RDWR, ACCESSPERMS, 1);
      lock.wait();
      _exit(0);
    }
    waitpid(holder, 0, 0);
  }

  void removeStream(const std::string & streamName) {
    char name[NAME_BUFFER_SIZE];
    snprintf(name, NAME_BUFFER_SIZE, SHM_STREAM_META, streamName.c_str());
    IPC::sharedPage meta(name, 0, false, false);
    if (meta) { meta.master = true; }
    snprintf(name, NAME_BUFFER_SIZE, SEM_TRACKLIST, streamName.c_str());
    IPC::semaphore trackLock(name, O_CREAT | O_RDWR, ACCESSPERMS, 1);
    trackLock.unlink();
  }

  void trackListLockWait() {
    const std::string streamName = "lw" + std::to_string(getpid() % 100000);
    {
      DTSC::Meta buffer(streamName, true);
      buffer.setLive(true);
      dieHoldingTheLock(streamName);
      int code = 0;
      const uint64_t ran = runChild(REQUEST, 300, [&]() {
        DTSC::Meta M(streamName, false, false);
        return M.addTrack() == INVALID_TRACK_ID ? 0 : 3;
      }, code);
      expect(ran && ran < 1500, "waiting for the track list lock gives up once stopped after 300 ms (" + ms(ran) + ")");
      expect(!code, "a process that gave up waiting for the track list lock adds no track");
    }
    removeStream(streamName);
  }

  void trackListLockStreamEnds() {
    const std::string streamName = "le" + std::to_string(getpid() % 100000);
    {
      DTSC::Meta buffer(streamName, true);
      buffer.setLive(true);
      buffer.addTrack();
      buffer.breakClaim(0);
      dieHoldingTheLock(streamName);
      std::thread shutdown([&buffer]() {
        Util::sleep(300);
        buffer.clear(); // the buffer shuts the stream down
      });
      int code = 0;
      const uint64_t ran = runChild(NO_STOP, 0, [&]() {
        DTSC::Meta M(streamName, false, false);
        return M.claimTrack(0, false) ? 3 : 0;
      }, code);
      shutdown.join();
      expect(ran && ran < 2500, "waiting for the track list lock gives up once the stream shuts down (" + ms(ran) + ")");
      expect(!code, "a claimant that gave up waiting claims nothing");
    }
    removeStream(streamName);
  }
} // namespace

int main() {
  sessionWait();
  sharedPageWait();
  startInputWait();
  trackListLockWait();
  trackListLockStreamEnds();
  if (failures) { return 1; }
  std::cout << "a process gives up its waits once told to stop" << std::endl;
  return 0;
}
