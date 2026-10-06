#include <mist/config.h>
#include <mist/socket.h>
#include <mist/timing.h>

#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>

static int calls = 0;
static int transientFailures = 0;
static int failure = 0;
static uint64_t failUntil = 0;
static int failures = 0;

// Checks are explicit rather than assert(): release builds define NDEBUG,
// which would compile every assertion, and the calls inside it, away.
static void check(bool ok, const char *what) {
  if (!ok) {
    fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

// Fails the first `transientFailures` lookups, and every lookup before
// `failUntil` (boot ms), with `failure`; then resolves every name to the
// loopback address.
static int fakeResolver(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res) {
  ++calls;
  if (calls <= transientFailures || Util::bootMS() < failUntil) { return failure; }
  return getaddrinfo(node ? "127.0.0.1" : 0, service, hints, res);
}

static void reset(int fails, int err, uint64_t failForMs = 0) {
  calls = 0;
  transientFailures = fails;
  failure = err;
  failUntil = failForMs ? Util::bootMS() + failForMs : 0;
}

int main() {
  int listener = socket(AF_INET, SOCK_STREAM, 0);
  check(listener >= 0, "create listener");
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  check(bind(listener, (struct sockaddr *)&addr, sizeof(addr)) == 0, "bind listener");
  check(listen(listener, 8) == 0, "listen");
  socklen_t len = sizeof(addr);
  check(getsockname(listener, (struct sockaddr *)&addr, &len) == 0, "read listener port");
  int port = ntohs(addr.sin_port);

  Socket::setAddrInfoResolver(fakeResolver);

  // A resolver that is briefly unavailable does not fail the connection.
  reset(2, EAI_AGAIN);
  Socket::Connection flaky("gateway.invalid", port, false);
  check(flaky.connected(), "connect after two temporary failures");
  check(calls == 3, "three lookups for two temporary failures");
  flaky.close();

  // UDP destinations resolve through the same retry.
  reset(1, EAI_AGAIN);
  check(Socket::getAddrs("gateway.invalid", port, AF_INET).size() > 0, "UDP lookup after a temporary failure");
  check(calls == 2, "two UDP lookups for one temporary failure");

  // A resolver that is unavailable for a few seconds, as when the system
  // resolver retries a dropped DNS query, does not fail the connection.
  reset(0, EAI_AGAIN, 2500);
  uint64_t start = Util::bootMS();
  Socket::Connection slow("gateway.invalid", port, false);
  check(slow.connected(), "connect after 2.5 s of temporary failures");
  check(Util::bootMS() - start >= 2500, "connect waited for the resolver");
  slow.close();

  // A permanent failure is reported at once.
  reset(1000, EAI_NONAME);
  start = Util::bootMS();
  Socket::Connection missing("gateway.invalid", port, false);
  check(!missing.connected(), "permanent failure does not connect");
  check(calls == 1, "permanent failure is not retried");
  check(Util::bootMS() - start < 100, "permanent failure fails fast");

  // A resolver that stays unavailable is retried for the connect timeout, then fails.
  reset(1000, EAI_AGAIN);
  start = Util::bootMS();
  Socket::Connection down("gateway.invalid", port, false);
  uint64_t elapsed = Util::bootMS() - start;
  check(!down.connected(), "unavailable resolver does not connect");
  check(elapsed >= 5000 && elapsed < 6500, "unavailable resolver is retried for the connect timeout");

  // A stop request ends the retries.
  reset(1000, EAI_AGAIN);
  start = Util::bootMS();
  std::thread stopper([]() {
    Util::sleep(300);
    Util::Config::requestStop();
  });
  Socket::Connection stopped("gateway.invalid", port, false);
  elapsed = Util::bootMS() - start;
  stopper.join();
  check(!stopped.connected(), "stopped lookup does not connect");
  check(elapsed < 1000, "stop request ends the retries");

  Socket::setAddrInfoResolver(0);
  close(listener);
  return failures ? 1 : 0;
}
