// A session that cannot be started must be reported at once, not waited out on the semaphore
// its process would have created, and as a start failure rather than a refusal: only viewers
// stop on it, internal process readers retry on their next stats update.
//
// The binary runs a copy of itself from a fresh directory, so getMyPath() + "MistSession"
// does not exist and every session start fails.
#include <mist/comms.h>
#include <mist/config.h>
#include <mist/timing.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
  int fail(const std::string & message) {
    std::cerr << message << std::endl;
    return 1;
  }

  int runWithoutSession() {
    for (int attempt = 0; attempt < 3; ++attempt) {
      Comms::Connections conn;
      const uint64_t start = Util::bootMS();
      conn.reload("nosession", std::string(16, '\0'),
                  "nosession" + std::to_string(getpid()) + "x" + std::to_string(attempt), "NOSESSION", "url");
      const uint64_t took = Util::bootMS() - start;
      if (conn) { return fail("a connection without a session process must not be valid"); }
      if (took > 2000) {
        return fail("a session that could not be started was waited on for " + std::to_string(took) + " ms");
      }
      if (!conn.sessionStartFailed()) { return fail("a session that could not be started was not reported as such"); }
    }
    if (!Comms::missingSessionEndsConnection(false, true) || !Comms::missingSessionEndsConnection(false, false) ||
        !Comms::missingSessionEndsConnection(true, false)) {
      return fail("viewers without a session, and readers whose session refused them, must stop");
    }
    if (Comms::missingSessionEndsConnection(true, true)) {
      return fail("a process reader whose session could not be started must keep running and retry");
    }
    std::cout << "session start failures are reported at once and stop only viewers" << std::endl;
    return 0;
  }
} // namespace

int main(int argc, char **argv) {
  if (argc > 1 && std::string(argv[1]) == "run") { return runWithoutSession(); }

  char tmpl[] = "/tmp/mist-nosession.XXXXXX";
  const char *dir = mkdtemp(tmpl);
  if (!dir) { return fail("could not create a staging directory"); }
  const std::string staged = std::string(dir) + "/nosession";
  {
    std::ifstream in(Util::getMyPathWithBin().c_str(), std::ios::binary);
    std::ofstream out(staged.c_str(), std::ios::binary);
    out << in.rdbuf();
  }
  chmod(staged.c_str(), 0755);
  int status = 1;
  pid_t child = fork();
  if (!child) {
    execl(staged.c_str(), staged.c_str(), "run", (char *)NULL);
    _exit(127);
  }
  if (child > 0) {
    int raw = 0;
    if (waitpid(child, &raw, 0) == child && WIFEXITED(raw)) { status = WEXITSTATUS(raw); }
  }
  unlink(staged.c_str());
  rmdir(dir);
  return status;
}
