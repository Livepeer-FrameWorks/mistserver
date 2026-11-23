#pragma once
#include <mist/config.h>
#include <mist/json.h>
#include <mist/procs.h>
#include <mist/timing.h>
#include <mist/triggers.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>

namespace Mist {
  /// Bound the entire handler chain, including DNS/TLS, EXEC startup and stdin writes.
  inline bool runProcessReplacementRequest(const std::string & stream, const std::string & payload, std::string & response,
                                           const std::string & worker, uint64_t timeoutMs = 15000) {
    if (!Triggers::shouldTrigger("PROCESS_REPLACE", stream)) { return false; }
    const uint64_t deadline = Util::bootMS() + timeoutMs;
    int input = -1, output = -1, error = STDERR_FILENO;
    const pid_t pid = Util::Procs::StartPiped(std::deque<std::string>{worker, stream}, &input, &output, &error);
    if (!pid) { return false; }
    fcntl(input, F_SETFL, fcntl(input, F_GETFL) | O_NONBLOCK);
    fcntl(output, F_SETFL, fcntl(output, F_GETFL) | O_NONBLOCK);
    std::string reply;
    size_t sent = 0;
    bool eof = false, complete = false;
    int exitCode = -1;
    while (Util::bootMS() < deadline) {
      bool progress = false;
      if (input >= 0) {
        if (sent < payload.size()) {
          const ssize_t written = write(input, payload.data() + sent, std::min<size_t>(65536, payload.size() - sent));
          if (written > 0) {
            sent += written;
            progress = true;
          } else if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            break;
          }
        }
        if (sent == payload.size()) {
          close(input);
          input = -1;
        }
      }
      char bytes[65536];
      const ssize_t received = read(output, bytes, sizeof(bytes));
      if (received > 0) {
        reply.append(bytes, received);
        progress = true;
      } else if (!received) {
        eof = true;
      } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        break;
      }
      if (reply.size() > 16 * 1024 * 1024) { break; }
      if (eof) { Util::Procs::childRunning(pid); }
      if (eof && Util::Procs::getExitCode(pid, exitCode)) {
        complete = true;
        break;
      }
      if (!progress) { Util::sleep(2); }
    }
    if (input >= 0) { close(input); }
    close(output);
    if (!complete) {
      // The worker acknowledges its private process group before starting a handler.
      // A deadline kills only this request and the EXEC children it owns.
      if (!reply.empty() && reply[0] == 'R') { kill(-pid, SIGKILL); }
      if (Util::Procs::childRunning(pid)) { Util::Procs::Murder(pid); }
      const uint64_t reapDeadline = Util::bootMS() + 100;
      do {
        Util::Procs::childRunning(pid);
        if (Util::Procs::getExitCode(pid, exitCode)) { break; }
        Util::sleep(2);
      } while (Util::bootMS() < reapDeadline);
      if (Util::Procs::childRunning(pid)) { Util::Procs::ignoreExitCode(pid); }
      WARN_MSG("PROCESS_REPLACE request exceeded its execution budget or failed its worker");
      return false;
    }
    if (exitCode || reply.empty() || reply[0] != 'R') { return false; }
    const JSON::Value result = JSON::fromString(reply.substr(1));
    if (!result["success"].asBool() || !result["response"].isString() || result["response"].asString().empty()) {
      return false;
    }
    response = result["response"].asString();
    return true;
  }
} // namespace Mist
