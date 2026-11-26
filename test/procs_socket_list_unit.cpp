#include <mist/procs.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>

// Runs `sh -c script` with piped stdin/stdout and returns what it printed, or "" if it did not start.
static std::string run(const std::string & script) {
  int input = -1, output = -1;
  const char *args[] = {"/bin/sh", "-c", script.c_str(), NULL};
  if (!Util::Procs::StartPiped(args, &input, &output, NULL)) { return ""; }
  close(input);
  std::string printed;
  char buffer[64];
  ssize_t count;
  while ((count = read(output, buffer, sizeof(buffer))) != 0) {
    if (count < 0 && errno == EINTR) { continue; }
    if (count < 0) { break; }
    printed.append(buffer, count);
  }
  close(output);
  return printed;
}

int main() {
  signal(SIGPIPE, SIG_IGN);
  // A socket closed without leaving the list frees its number for the next child's pipes.
  const int stale = open("/dev/null", O_RDONLY);
  Util::Procs::socketList.insert(stale);
  close(stale);
  if (run("echo started") != "started\n") {
    fprintf(stderr, "a closed descriptor left in the socket list broke the child's stdio pipes\n");
    return 1;
  }
  // A listed socket that is still open must not reach the child.
  const int live = open("/dev/null", O_RDONLY);
  Util::Procs::socketList.insert(live);
  const std::string seen = run("if [ -e /dev/fd/" + std::to_string(live) + " ]; then echo leaked; else echo closed; fi");
  close(live);
  Util::Procs::socketList.erase(live);
  if (seen != "closed\n") {
    fprintf(stderr, "a listed open socket reached the child (%s)\n", seen.c_str());
    return 1;
  }
  printf("PASS spawned children close listed open sockets and keep their pipes when listed sockets went stale\n");
  return 0;
}
