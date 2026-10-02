#include <mist/socket.h>
#include <mist/timing.h>

#include <cassert>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>

static int calls = 0;
static int transientFailures = 0;
static int failure = 0;

// Fails the first `transientFailures` lookups with `failure`, then resolves
// every name to the loopback address.
static int fakeResolver(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res) {
  ++calls;
  if (calls <= transientFailures) { return failure; }
  return getaddrinfo(node ? "127.0.0.1" : 0, service, hints, res);
}

static void reset(int fails, int err) {
  calls = 0;
  transientFailures = fails;
  failure = err;
}

int main() {
  int listener = socket(AF_INET, SOCK_STREAM, 0);
  assert(listener >= 0);
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  assert(bind(listener, (struct sockaddr *)&addr, sizeof(addr)) == 0);
  assert(listen(listener, 8) == 0);
  socklen_t len = sizeof(addr);
  assert(getsockname(listener, (struct sockaddr *)&addr, &len) == 0);
  int port = ntohs(addr.sin_port);

  Socket::setAddrInfoResolver(fakeResolver);

  // A resolver that is briefly unavailable does not fail the connection.
  reset(2, EAI_AGAIN);
  Socket::Connection flaky("gateway.invalid", port, false);
  assert(flaky.connected());
  assert(calls == 3);
  flaky.close();

  // UDP destinations resolve through the same retry.
  reset(1, EAI_AGAIN);
  assert(Socket::getAddrs("gateway.invalid", port, AF_INET).size());
  assert(calls == 2);

  // A permanent failure is reported at once.
  reset(10, EAI_NONAME);
  uint64_t start = Util::bootMS();
  Socket::Connection missing("gateway.invalid", port, false);
  assert(!missing.connected());
  assert(calls == 1);
  assert(Util::bootMS() - start < 100);

  // A resolver that stays unavailable is retried three times, 850 ms apart in total.
  reset(10, EAI_AGAIN);
  start = Util::bootMS();
  Socket::Connection down("gateway.invalid", port, false);
  assert(!down.connected());
  assert(calls == 4);
  assert(Util::bootMS() - start >= 850);

  Socket::setAddrInfoResolver(0);
  close(listener);
  return 0;
}
