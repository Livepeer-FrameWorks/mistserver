#include <mist/procs.h>
#include <mist/timing.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>

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
  const pid_t child = Util::Procs::StartPiped({"/bin/cat"}, &input, &output, &error);
  if (!child) { return 1; }
  const std::string result =
    Util::Procs::getOutputOf({argv[0], std::to_string(input), std::to_string(output), std::to_string(error)});
  const char payload[] = "parent can still use child pipes\n";
  bool usable = write(input, payload, sizeof(payload)) == sizeof(payload);
  char data[sizeof(payload)];
  usable = usable && read(output, data, sizeof(data)) == sizeof(data);
  close(input);
  close(output);
  close(error);
  Util::Procs::Stop(child);
  if (result != "closed\n" || !usable) {
    fprintf(stderr, "unrelated exec inherited process pipes: %s usable=%d\n", result.c_str(), usable);
    return 1;
  }
  puts("PASS exec closes unrelated pipes and preserves selected stdio");
  return 0;
}
