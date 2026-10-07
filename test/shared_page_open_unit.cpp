// A page's creator makes it, then sizes it. A reader that opens it in between, with backoff (as a
// connection opens its session's page while the session process boots), must wait for the size
// instead of failing on an empty page: on a loaded host the creator can be descheduled between the
// two steps for longer than any fixed window.
#include <mist/shared_memory.h>
#include <mist/stream.h>
#include <mist/timing.h>

#include <cstdio>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace {
  int failures = 0;

  void check(bool ok, const char *message) {
    if (ok) { return; }
    fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
  }

  /// Makes the page the way its creator does, without sizing it yet.
  int createUnsized(const std::string & name) {
#ifdef SHM_ENABLED
    return shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, ACCESSPERMS);
#else
    return open((Util::getTmpFolder() + name).c_str(), O_CREAT | O_EXCL | O_RDWR, (mode_t)0600);
#endif
  }

  void removePage(const std::string & name) {
#ifdef SHM_ENABLED
    shm_unlink(name.c_str());
#else
    unlink((Util::getTmpFolder() + name).c_str());
#endif
  }
} // namespace

int main() {
  const std::string name = "/MstPageOpenTest" + std::to_string(getpid());
  const size_t size = 4096;
  removePage(name);
  int fd = createUnsized(name);
  if (fd < 0) {
    perror("create page");
    return 2;
  }

  // Without backoff, an unsized page is not opened.
  {
    IPC::sharedPage early(name, 0, false, false);
    check(!early.mapped, "a page opened without backoff before its creator sized it was mapped");
    early.master = false;
  }

  // The creator sizes the page well after any short fixed wait would have given up.
  std::thread creator([fd, size]() {
    Util::sleep(1500);
    if (ftruncate(fd, size)) { perror("size page"); }
  });
  {
    IPC::sharedPage reader(name, 0, false, true);
    check(reader.mapped, "a reader that opened the page before its creator sized it gave up instead of waiting");
    check(reader.len == size, "the reader did not map the page at its size");
    reader.master = false;
  }
  creator.join();
  close(fd);
  removePage(name);
  return failures ? 1 : 0;
}
