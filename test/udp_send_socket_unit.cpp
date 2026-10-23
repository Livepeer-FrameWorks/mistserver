#include <mist/socket.h>
#include <mist/stream.h>
#include <mist/url.h>

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

static int fail(const std::string & message) {
  std::cerr << "FAIL: " << message << std::endl;
  return 1;
}

static bool readableWithin(int fd, uint32_t timeoutMs) {
  fd_set readSet;
  FD_ZERO(&readSet);
  FD_SET(fd, &readSet);
  struct timeval timeout;
  timeout.tv_sec = timeoutMs / 1000;
  timeout.tv_usec = (timeoutMs % 1000) * 1000;
  return select(fd + 1, &readSet, 0, 0, &timeout) > 0;
}

static std::string addrString(const sockaddr_storage & addr) {
  char str[INET6_ADDRSTRLEN] = {0};
  if (addr.ss_family == AF_INET) {
    const sockaddr_in & a = (const sockaddr_in &)addr;
    inet_ntop(AF_INET, &a.sin_addr, str, sizeof(str));
    return std::string(str) + ":" + std::to_string(ntohs(a.sin_port));
  }
  if (addr.ss_family == AF_INET6) {
    const sockaddr_in6 & a = (const sockaddr_in6 &)addr;
    inet_ntop(AF_INET6, &a.sin6_addr, str, sizeof(str));
    return "[" + std::string(str) + "]:" + std::to_string(ntohs(a.sin6_port));
  }
  return "address of family " + std::to_string(addr.ss_family);
}

static uint16_t portOf(const sockaddr_storage & addr) {
  if (addr.ss_family == AF_INET) { return ntohs(((const sockaddr_in &)addr).sin_port); }
  if (addr.ss_family == AF_INET6) { return ntohs(((const sockaddr_in6 &)addr).sin6_port); }
  return 0;
}

/// The IPv4 address an AF_INET address or an IPv4-mapped AF_INET6 address carries, in host order.
/// Returns false for any other address.
static bool ipv4Of(const sockaddr_storage & addr, uint32_t & ip) {
  if (addr.ss_family == AF_INET) {
    ip = ntohl(((const sockaddr_in &)addr).sin_addr.s_addr);
    return true;
  }
  if (addr.ss_family == AF_INET6) {
    const in6_addr & a = ((const sockaddr_in6 &)addr).sin6_addr;
    if (!IN6_IS_ADDR_V4MAPPED(&a)) { return false; }
    uint32_t mapped;
    memcpy(&mapped, a.s6_addr + 12, sizeof(mapped));
    ip = ntohl(mapped);
    return true;
  }
  return false;
}

/// True for 0.0.0.0, :: and ::ffff:0.0.0.0.
static bool isWildcard(const sockaddr_storage & addr) {
  uint32_t ip;
  if (ipv4Of(addr, ip)) { return ip == INADDR_ANY; }
  return addr.ss_family == AF_INET6 && IN6_IS_ADDR_UNSPECIFIED(&((const sockaddr_in6 &)addr).sin6_addr);
}

/// True for 127.0.0.1, ::ffff:127.0.0.1 and ::1. A dual-stack host gives a default (AF_INET6) socket that sends to
/// 127.0.0.1 the IPv4-mapped form.
static bool isLoopback(const sockaddr_storage & addr) {
  uint32_t ip;
  if (ipv4Of(addr, ip)) { return ip == INADDR_LOOPBACK; }
  return addr.ss_family == AF_INET6 && IN6_IS_ADDR_LOOPBACK(&((const sockaddr_in6 &)addr).sin6_addr);
}

static socklen_t addrLen(const sockaddr_storage & addr) {
  return addr.ss_family == AF_INET6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
}

/// Checks that a send-only socket that just sent a datagram to the receiver on 127.0.0.1:port is bound to the loopback
/// source the route picks, is connected to the receiver, delivered the datagram, and does not take datagrams from a
/// socket other than the receiver.
static int checkSendSocket(const std::string & name, int fd, Socket::UDPConnection & receiver, uint16_t port,
                           const std::string & payload) {
  sockaddr_storage local = {};
  socklen_t localLen = sizeof(local);
  if (getsockname(fd, (sockaddr *)&local, &localLen)) {
    return fail(name + ": getsockname failed: " + strerror(errno));
  }
  if (isWildcard(local)) {
    return fail(name + " sits bound to the wildcard address " + addrString(local) + ", where it accepts datagrams from any interface");
  }
  if (!isLoopback(local)) {
    return fail(name + " is bound to " + addrString(local) + " instead of the loopback route's source address");
  }
  sockaddr_storage peer = {};
  socklen_t peerLen = sizeof(peer);
  if (getpeername(fd, (sockaddr *)&peer, &peerLen)) {
    return fail(name + " is not connected to its destination: " + strerror(errno));
  }
  if (!isLoopback(peer) || portOf(peer) != port) {
    return fail(name + " is connected to " + addrString(peer) + " instead of 127.0.0.1:" + std::to_string(port));
  }

  if (!readableWithin(receiver.getSock(), 2000) || !receiver.Receive()) {
    return fail(name + ": the receiver did not get the datagram");
  }
  if (std::string(receiver.data, receiver.data.size()) != payload) {
    return fail(name + ": the receiver got a different datagram than was sent");
  }

  // A datagram from any socket other than the destination must not reach the send socket. The stranger uses the send
  // socket's own family and sends to the exact address it is bound to.
  int stranger = socket(local.ss_family, SOCK_DGRAM, 0);
  if (stranger < 0) { return fail(std::string("could not open a stranger socket: ") + strerror(errno)); }
  if (sendto(stranger, "stranger", 8, 0, (sockaddr *)&local, addrLen(local)) != 8) {
    int err = errno;
    close(stranger);
    return fail("the stranger could not send to " + addrString(local) + ": " + strerror(err));
  }
  close(stranger);
  if (readableWithin(fd, 300)) {
    return fail(name + " on " + addrString(local) + " accepted a datagram from a socket other than its destination");
  }
  std::cout << name << " bound to " << addrString(local) << ", connected to " << addrString(peer)
            << ", delivered the datagram and ignored a stranger" << std::endl;
  return 0;
}

int main() {
  Util::printDebugLevel = 0;

  // The controller binds its UDP API on a loopback address.
  Socket::UDPConnection api;
  uint16_t apiPort = api.bind(0, "127.0.0.1");
  if (!apiPort) { return fail("the UDP API receiver could not bind to 127.0.0.1"); }
  api.allocateDestination();

  std::unique_ptr<Socket::UDPConnection> apiSock =
    Util::openUDPApiSocket(HTTP::URL("udp://127.0.0.1:" + std::to_string(apiPort)));
  if (!apiSock || !*apiSock) { return fail("the UDP API send socket could not be opened"); }
  const std::string apiPayload = "{\"trigger_stat\":{}}";
  apiSock->SendNow(apiPayload);
  if (int r = checkSendSocket("UDP API send socket", apiSock->getSock(), api, apiPort, apiPayload)) { return r; }
  int flags = fcntl(apiSock->getSock(), F_GETFD);
  if (flags < 0 || !(flags & FD_CLOEXEC)) {
    return fail("the UDP API send socket is inherited by spawned children (no FD_CLOEXEC)");
  }

  // A UDP push (tsudp://, tsrtp:// and its FEC streams) sends to a destination set by SetDestination.
  Socket::UDPConnection pushReceiver;
  uint16_t pushPort = pushReceiver.bind(0, "127.0.0.1");
  if (!pushPort) { return fail("the push receiver could not bind to 127.0.0.1"); }
  pushReceiver.allocateDestination();

  Socket::UDPConnection pushSock;
  pushSock.SetDestination("127.0.0.1", pushPort);
  if (!pushSock.connectDestination()) { return fail("the push socket could not connect to its destination"); }
  const std::string pushPayload(188, 'G');
  pushSock.SendNow(pushPayload);
  if (int r = checkSendSocket("UDP push socket", pushSock.getSock(), pushReceiver, pushPort, pushPayload)) { return r; }
  return 0;
}
