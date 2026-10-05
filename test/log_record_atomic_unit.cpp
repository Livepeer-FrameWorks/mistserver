// Every Mist process logs into the controller's one log pipe. A record must arrive whole even while
// other processes log at the same time, or the controller attributes one process's message to
// another's stream and PID, or drops it. Several children here log records concurrently into one
// pipe; every line read back must be one complete record of the process it names. The records stay
// within PIPE_BUF on every platform (512 bytes on macOS). A record longer than the logger's stack
// buffer must arrive whole as well, not cut.
#include <mist/defines.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace {
  const int writers = 4;
  const int recordsPerWriter = 400;
  const size_t payloadSize = 400;

  void logRecords(int id) {
    std::string payload(payloadSize, (char)('a' + id));
    for (int i = 0; i < recordsPerWriter; ++i) { LOG_MSG("INFO", "%d:%s", id, payload.c_str()); }
  }
  /// Reads everything from fd until every writer closed it.
  std::string readAll(int fd) {
    std::string all;
    char buf[65536];
    ssize_t got;
    while ((got = read(fd, buf, sizeof(buf))) != 0) {
      if (got < 0) {
        if (errno == EINTR) { continue; }
        break;
      }
      all.append(buf, got);
    }
    return all;
  }

  /// One record of 10000 bytes, more than the logger's stack buffer, arrives as one whole line.
  int longRecordArrivesWhole() {
    int fds[2];
    if (pipe(fds)) {
      std::cerr << "pipe failed" << std::endl;
      return 1;
    }
    const std::string payload(10000, 'z');
    pid_t pid = fork();
    if (pid < 0) {
      std::cerr << "fork failed" << std::endl;
      return 1;
    }
    if (!pid) {
      close(fds[0]);
      dup2(fds[1], STDERR_FILENO);
      close(fds[1]);
      LOG_MSG("INFO", "long:%s", payload.c_str());
      _exit(0);
    }
    close(fds[1]);
    const std::string all = readAll(fds[0]);
    close(fds[0]);
    waitpid(pid, 0, 0);
    const std::string tail = "|long:" + payload + "\n";
    if (all.compare(0, 5, "INFO|") || all.size() < tail.size() ||
        all.compare(all.size() - tail.size(), tail.size(), tail) || all.find('\n') != all.size() - 1) {
      std::cerr << "a " << payload.size() << " byte record arrived as " << all.size() << " bytes, not as one whole line"
                << std::endl;
      return 1;
    }
    return 0;
  }
} // namespace

int main() {
  if (longRecordArrivesWhole()) { return 1; }
  int fds[2];
  if (pipe(fds)) {
    std::cerr << "pipe failed" << std::endl;
    return 1;
  }
  std::map<pid_t, int> ids;
  for (int id = 0; id < writers; ++id) {
    pid_t pid = fork();
    if (pid < 0) {
      std::cerr << "fork failed" << std::endl;
      return 1;
    }
    if (!pid) {
      close(fds[0]);
      dup2(fds[1], STDERR_FILENO);
      close(fds[1]);
      logRecords(id);
      _exit(0);
    }
    ids[pid] = id;
  }
  close(fds[1]);

  const std::string all = readAll(fds[0]);
  close(fds[0]);
  for (size_t i = 0; i < ids.size(); ++i) { wait(0); }

  // INFO|<prog>|<pid>|<file>:<line>|<stream>|<id>:<payload of id's letter>
  std::map<int, int> whole;
  size_t broken = 0, pos = 0;
  while (pos < all.size()) {
    size_t end = all.find('\n', pos);
    if (end == std::string::npos) { end = all.size(); }
    std::string line = all.substr(pos, end - pos);
    pos = end + 1;
    size_t f[5], at = 0;
    bool fieldsOk = true;
    for (int k = 0; k < 5; ++k) {
      at = line.find('|', at);
      if (at == std::string::npos) {
        fieldsOk = false;
        break;
      }
      f[k] = at++;
    }
    if (!fieldsOk || line.compare(0, 5, "INFO|")) {
      ++broken;
      continue;
    }
    pid_t pid = atoi(line.substr(f[1] + 1, f[2] - f[1] - 1).c_str());
    std::string message = line.substr(f[4] + 1);
    if (!ids.count(pid)) {
      ++broken;
      continue;
    }
    int id = ids[pid];
    std::string expected = std::to_string(id) + ":" + std::string(payloadSize, (char)('a' + id));
    if (message != expected) {
      ++broken;
      continue;
    }
    ++whole[id];
  }

  int failures = 0;
  if (broken) {
    std::cerr << broken << " log lines were not one whole record of the process they name" << std::endl;
    ++failures;
  }
  for (int id = 0; id < writers; ++id) {
    if (whole[id] != recordsPerWriter) {
      std::cerr << "writer " << id << ": " << whole[id] << " of " << recordsPerWriter << " records arrived whole" << std::endl;
      ++failures;
    }
  }
  if (failures) { return 1; }
  std::cout << "all " << writers * recordsPerWriter << " concurrent records and one long record arrived whole" << std::endl;
  return 0;
}
