#include <mist/socket.h>

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

static int fail(const char *message) {
  fprintf(stderr, "%s\n", message);
  return 1;
}

int main() {
  int pair[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) { return fail("socketpair failed"); }
  Socket::Connection output(pair[0]);
  output.SendNow("HEADER");
  output.skipBytes(2);
  output.setSendLimit(3);
  output.SendNow("a");
  output.SendNow("bcdefgh");
  output.SendNow("discarded");
  if (!output.sendLimitReached() || output.dataUp() != 9) { return fail("bounded write count is incorrect"); }
  output.setSendLimit(UINT64_MAX);
  output.SendNow("TAIL");
  output.close();
  char bytes[128];
  std::string first;
  ssize_t size;
  while ((size = read(pair[1], bytes, sizeof(bytes))) > 0) { first.append(bytes, size); }
  close(pair[1]);
  if (size != 0 || first != "HEADERcdeTAIL") { return fail("prefix skip, body limit or reset failed"); }

  if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) { return fail("socketpair failed"); }
  Socket::Connection buffered(pair[0]);
  if (fcntl(pair[1], F_SETFL, fcntl(pair[1], F_GETFL) | O_NONBLOCK) == -1) { return fail("nonblocking setup failed"); }
  buffered.setBlocking(false);
  int capacity = 4096;
  setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &capacity, sizeof(capacity));
  const size_t limit = 100000;
  const std::string payload(200000, 'x');
  buffered.skipBytes(17);
  buffered.setSendLimit(limit);
  buffered.SendNow(payload);
  std::string received;
  bool sawBuffered = buffered.sendingBlocked(payload.size()) != 0;
  for (size_t attempt = 0; attempt < 10000; ++attempt) {
    const ssize_t count = read(pair[1], bytes, sizeof(bytes));
    if (count > 0) { received.append(bytes, count); }
    if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK) { return fail("read failed"); }
    buffered.SendNow("", 0);
    if (received.size() == limit && !buffered.sendingBlocked(payload.size())) { break; }
  }
  if (!sawBuffered || received != payload.substr(17, limit) || !buffered.sendLimitReached() || buffered.dataUp() != limit) {
    return fail("partial/nonblocking writes exceeded or lost the permitted body");
  }
  buffered.setSendLimit(0);
  buffered.SendNow("must not be sent");
  buffered.close();
  if (read(pair[1], bytes, sizeof(bytes)) != 0) { return fail("suffix bytes escaped after the limit"); }
  close(pair[1]);
  return 0;
}
