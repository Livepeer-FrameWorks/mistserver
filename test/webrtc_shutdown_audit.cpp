#include <dlfcn.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>

static int (*realShutdown)(int, int) = 0;
static int auditFD = -1;
__attribute__((constructor)) static void initializeAudit() {
  realShutdown = (int (*)(int, int))dlsym(RTLD_NEXT, "shutdown");
  const char *path = getenv("MIST_SHUTDOWN_AUDIT");
  if (path) { auditFD = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600); }
}
extern "C" int shutdown(int fd, int mode) noexcept {
  if (auditFD >= 0) {
    const char marker = fd == 0 ? '0' : 'x';
    const ssize_t ignored = write(auditFD, &marker, 1);
    (void)ignored;
  }
  return realShutdown ? realShutdown(fd, mode) : -1;
}
