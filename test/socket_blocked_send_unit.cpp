#include <mist/socket.h>
#include <mist/timing.h>

#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

static int waitCallbacks = 0;

static void countWait() {
  ++waitCallbacks;
}

static int fail(const std::string & message) {
  std::cerr << "FAIL: " << message << std::endl;
  return 1;
}

int main() {
  // A blocking send into a pipe whose reader reads nothing for 2.5 s: with a blocked-send
  // callback, the send keeps calling it while it waits, and still delivers every byte.
  const size_t total = 1024 * 1024;
  int fds[2];
  if (pipe(fds)) { return fail("could not create a pipe"); }
  pid_t reader = fork();
  if (reader < 0) { return fail("could not fork the reader"); }
  if (reader == 0) {
    close(fds[1]);
    Util::sleep(2500);
    char buffer[65536];
    size_t received = 0;
    ssize_t r;
    while ((r = read(fds[0], buffer, sizeof(buffer))) > 0) { received += r; }
    _exit(received == total ? 0 : 1);
  }
  close(fds[0]);

  Socket::setBlockedSendCallback(countWait);
  Socket::Connection conn(fds[1], -1);
  conn.setBlocking(true);
  std::string payload(total, 'x');
  conn.SendNow(payload);
  const int flagsAfter = fcntl(fds[1], F_GETFL, 0);
  const uint64_t sent = conn.dataUp();
  conn.close();
  Socket::setBlockedSendCallback(0);

  int status = 0;
  waitpid(reader, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status)) { return fail("the reader did not receive every byte"); }
  if (sent != total) { return fail("the send reported " + std::to_string(sent) + " bytes written"); }
  if (waitCallbacks < 2) {
    return fail("the blocked send called back " + std::to_string(waitCallbacks) + " times while it waited 2.5 s");
  }
  if (flagsAfter == -1 || (flagsAfter & O_NONBLOCK)) { return fail("the send left the descriptor non-blocking"); }
  std::cout << "blocked send called back " << waitCallbacks << " times and delivered " << sent << " bytes" << std::endl;
  return 0;
}
