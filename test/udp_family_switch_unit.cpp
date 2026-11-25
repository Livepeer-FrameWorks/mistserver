#include <mist/defines.h>
#include <mist/socket.h>

#include <arpa/inet.h>
#include <deque>
#include <iostream>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <type_traits>
#include <unistd.h>

// An address passed where a port is expected must not compile: it used to become port 1 through operator bool.
static_assert(!std::is_convertible<Socket::Address, int>::value, "Socket::Address converts to an integer");

static int fail(const std::string & message) {
  std::cerr << "FAIL: " << message << std::endl;
  return 1;
}

/// Binds a raw receiver on host and returns its port, or 0.
static uint16_t bindReceiver(int & sock, int family, const char *host) {
  sock = socket(family, SOCK_DGRAM, 0);
  if (sock < 0) { return 0; }
  sockaddr_storage addr = {};
  socklen_t len = 0;
  if (family == AF_INET) {
    sockaddr_in & a = (sockaddr_in &)addr;
    a.sin_family = AF_INET;
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) { return 0; }
    len = sizeof a;
  } else {
    sockaddr_in6 & a = (sockaddr_in6 &)addr;
    a.sin6_family = AF_INET6;
    if (inet_pton(AF_INET6, host, &a.sin6_addr) != 1) { return 0; }
    len = sizeof a;
  }
  if (::bind(sock, (sockaddr *)&addr, len) || getsockname(sock, (sockaddr *)&addr, &len)) { return 0; }
  return ntohs(family == AF_INET ? ((sockaddr_in &)addr).sin_port : ((sockaddr_in6 &)addr).sin6_port);
}

/// Returns the source port of the next datagram on sock, or 0 when none arrives within two seconds.
static uint16_t sourcePort(int sock) {
  pollfd readable = {sock, POLLIN, 0};
  if (poll(&readable, 1, 2000) != 1) { return 0; }
  char buf[64];
  sockaddr_storage from = {};
  socklen_t len = sizeof from;
  if (recvfrom(sock, buf, sizeof buf, 0, (sockaddr *)&from, &len) <= 0) { return 0; }
  if (from.ss_family == AF_INET) { return ntohs(((sockaddr_in &)from).sin_port); }
  return ntohs(((sockaddr_in6 &)from).sin6_port);
}

/// Binds a Mist socket on bindHost, points it at a destination of the other address family, and requires the
/// rebound socket to keep its port: the datagram it then sends must arrive from that port.
static int checkSwitch(const std::string & name, const char *bindHost, int receiverFamily, const char *receiverHost,
                       const std::string & destHost, int destFamily) {
  int receiver = -1;
  const uint16_t receiverPort = bindReceiver(receiver, receiverFamily, receiverHost);
  if (!receiverPort) { return fail(name + ": could not bind the receiver"); }
  Socket::UDPConnection sock(false, destFamily == AF_INET ? AF_INET6 : AF_INET);
  const uint16_t port = sock.bind(0, bindHost);
  if (!port) { return fail(name + ": could not bind the switching socket"); }
  const sa_family_t boundFamily = sock.getBoundAddr().family();
  if (boundFamily == destFamily) { return fail(name + ": the switching socket was bound in the destination family"); }
  std::deque<Socket::Address> dest = Socket::getAddrs(destHost, receiverPort, destFamily, destFamily == AF_INET6);
  if (!dest.size()) { return fail(name + ": could not resolve the destination"); }
  if (!sock.setDestination(dest.front())) { return fail(name + ": switching the address family failed"); }
  if (sock.getBoundAddr().family() != destFamily || sock.getBoundAddr().port() != port) {
    return fail(name + ": after the switch the socket is bound to " + sock.getBoundAddr().toString() +
                " instead of port " + std::to_string(port));
  }
  sock.SendNow("switched");
  const uint16_t from = sourcePort(receiver);
  close(receiver);
  if (from != port) {
    return fail(name + ": the datagram came from port " + std::to_string(from) + " instead of " + std::to_string(port));
  }
  std::cout << name << ": kept port " << port << std::endl;
  return 0;
}

int main() {
  Util::printDebugLevel = DLVL_WARN;
  // A specific IPv4 address moves to its IPv4-mapped IPv6 form when the destination is IPv4-mapped IPv6.
  if (checkSwitch("IPv4 address to IPv6", "127.0.0.1", AF_INET, "127.0.0.1", "127.0.0.1", AF_INET6)) { return 1; }
  // The dual-stack IPv6 wildcard becomes the IPv4 wildcard.
  if (checkSwitch("IPv6 wildcard to IPv4", "::", AF_INET, "127.0.0.1", "127.0.0.1", AF_INET)) { return 1; }
  // The IPv4 wildcard becomes the dual-stack IPv6 wildcard; this needs IPv6 loopback.
  int probe = -1;
  const bool haveV6Loopback = bindReceiver(probe, AF_INET6, "::1");
  if (probe >= 0) { close(probe); }
  if (haveV6Loopback) {
    if (checkSwitch("IPv4 wildcard to IPv6", "0.0.0.0", AF_INET6, "::1", "::1", AF_INET6)) { return 1; }
  } else {
    std::cout << "IPv4 wildcard to IPv6: skipped, no IPv6 loopback" << std::endl;
  }
  return 0;
}
