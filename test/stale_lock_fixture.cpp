#include <mist/defines.h>
#include <mist/shared_memory.h>

#include <cstdio>
#include <fcntl.h>
#include <string>

// Leaves the named stream lock taken and exits without releasing it, the way an input that is
// killed leaves its lock: a POSIX semaphore keeps its count when its holder dies.
// Usage: stale_lock_fixture <semaphore name> | --release <semaphore name>
int main(int argc, char **argv) {
  if (argc == 3 && std::string(argv[1]) == "--release") {
    IPC::semaphore lock(argv[2], O_RDWR, ACCESSPERMS, 0, true);
    if (lock) { lock.unlink(); }
    return 0;
  }
  if (argc != 2) {
    fprintf(stderr, "usage: %s <semaphore name> | --release <semaphore name>\n", argv[0]);
    return 2;
  }
  IPC::semaphore lock(argv[1], O_CREAT | O_RDWR, ACCESSPERMS, 1);
  if (!lock) {
    fprintf(stderr, "could not open %s\n", argv[1]);
    return 1;
  }
  if (!lock.tryWait()) {
    fprintf(stderr, "%s is already taken\n", argv[1]);
    return 1;
  }
  lock.abandon();
  return 0;
}
