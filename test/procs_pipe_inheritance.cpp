#include <mist/procs.h>
#include <mist/timing.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <thread>
#include <unistd.h>

static volatile sig_atomic_t interrupted = 0;
static void interruptRead(int) {
  interrupted = 1;
}

int main(int argc, char **argv) {
  if (argc == 4) {
    for (int i = 1; i < argc; ++i) {
      if (fcntl(atoi(argv[i]), F_GETFD) != -1 || errno != EBADF) {
        printf("inherited pipe %s\n", argv[i]);
        return 1;
      }
    }
    puts("closed");
    return 0;
  }
  int input = -1, output = -1, error = -1;
  const bool interrupt = argc == 2 && std::string(argv[1]) == "interrupt-read";
  std::deque<std::string> command{"/bin/cat"};
  if (interrupt) {
    command = {"/bin/sh", "-c", "sleep 0.1; exec cat"};
    struct sigaction action{};
    action.sa_handler = interruptRead;
    sigemptyset(&action.sa_mask);
    sigaction(SIGUSR1, &action, NULL);
  }
  const pid_t child = Util::Procs::StartPiped(command, &input, &output, &error);
  if (!child) { return 1; }
  const std::string result =
    Util::Procs::getOutputOf({argv[0], std::to_string(input), std::to_string(output), std::to_string(error)});
  const char payload[] = "parent can still use child pipes\n";
  size_t written = 0;
  while (written < sizeof(payload)) {
    const ssize_t count = write(input, payload + written, sizeof(payload) - written);
    if (count < 0 && errno == EINTR) { continue; }
    if (count <= 0) { break; }
    written += count;
  }
  bool usable = written == sizeof(payload);
  char data[sizeof(payload)];
  const pthread_t reader = pthread_self();
  std::thread signal;
  if (interrupt) {
    signal = std::thread([reader]() {
      usleep(10000);
      pthread_kill(reader, SIGUSR1);
    });
  }
  size_t received = 0;
  while (usable && received < sizeof(data)) {
    const ssize_t count = read(output, data + received, sizeof(data) - received);
    if (count < 0 && errno == EINTR) { continue; }
    if (count <= 0) { break; }
    received += count;
  }
  usable = usable && received == sizeof(payload) && !memcmp(data, payload, sizeof(payload));
  if (signal.joinable()) { signal.join(); }
  if (interrupt && !interrupted) { usable = false; }
  close(input);
  close(output);
  close(error);
  Util::Procs::Stop(child);
  if (result != "closed\n" || !usable) {
    fprintf(stderr, "pipe probe failed: exec=%s usable=%d\n", result.c_str(), usable);
    return 1;
  }
  puts("PASS exec closes unrelated pipes and preserves selected stdio");
  return 0;
}
