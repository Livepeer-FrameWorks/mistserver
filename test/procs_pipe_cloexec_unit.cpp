#include <mist/procs.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

int main(int argc, char **argv) {
  signal(SIGPIPE, SIG_IGN);
  if (argc > 1 && !strcmp(argv[1], "closed-stdin")) { close(STDIN_FILENO); }
  int input = -1, output = -1, error = -1;
  const char *args[] = {"/bin/cat", NULL};
  const pid_t child = Util::Procs::StartPiped(args, &input, &output, &error);
  if (!child) {
    fprintf(stderr, "cat fixture did not start\n");
    return 1;
  }
  bool inherited = false;
  for (int fd : {input, output, error}) {
    if (!(fcntl(fd, F_GETFD) & FD_CLOEXEC)) { inherited = true; }
  }
  const char payload[] = "processor media pipe\n";
  bool complete = write(input, payload, sizeof(payload)) == sizeof(payload);
  close(input);
  char buffer[sizeof(payload)];
  size_t received = 0;
  while (received < sizeof(buffer)) {
    const ssize_t count = read(output, buffer + received, sizeof(buffer) - received);
    if (count < 0 && errno == EINTR) { continue; }
    if (count <= 0) { break; }
    received += count;
  }
  close(output);
  close(error);
  if (inherited) {
    fprintf(stderr, "FFmpeg process pipes leak into subsequently executed children\n");
    return 1;
  }
  if (!complete || received != sizeof(payload) || memcmp(buffer, payload, sizeof(payload))) {
    fprintf(stderr, "close-on-exec pipes broke the intended child's standard streams\n");
    return 1;
  }
  printf("PASS processor pipe descriptors close on exec while child standard streams carry media\n");
  return 0;
}
