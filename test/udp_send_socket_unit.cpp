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

static std::string addrString(const sockaddr_in & addr) {
  char str[INET_ADDRSTRLEN] = {0};
  inet_ntop(AF_INET, &addr.sin_addr, str, sizeof(str));
  return std::string(str) + ":" + std::to_string(ntohs(addr.sin_port));
}

/// Checks that a send-only socket that just sent a datagram to the receiver on 127.0.0.1:port is bound to the loopback
/// source the route picks, is connected to the receiver, delivered the datagram, and does not take datagrams from a
/// socket other than the receiver.
static int checkSendSocket(const std::string & name, int fd, Socket::UDPConnection & receiver, uint16_t port,
                           const std::string & payload) {
  sockaddr_in local = {};
  socklen_t localLen = sizeof(local);
  if (getsockname(fd, (sockaddr *)&local, &localLen)) {
    return fail(name + ": getsockname failed: " + strerror(errno));
  }
  if (local.sin_family != AF_INET || local.sin_addr.s_addr == htonl(INADDR_ANY)) {
    return fail(name + " sits bound to the wildcard address " + addrString(local) + ", where it accepts datagrams from any interface");
  }
  if (local.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) {
    return fail(name + " is bound to " + addrString(local) + " instead of the loopback route's source address");
  }
  sockaddr_in peer = {};
  socklen_t peerLen = sizeof(peer);
  if (getpeername(fd, (sockaddr *)&peer, &peerLen)) {
    return fail(name + " is not connected to its destination: " + strerror(errno));
  }
  if (peer.sin_addr.s_addr != htonl(INADDR_LOOPBACK) || ntohs(peer.sin_port) != port) {
    return fail(name + " is connected to " + addrString(peer) + " instead of 127.0.0.1:" + std::to_string(port));
  }

  if (!readableWithin(receiver.getSock(), 2000) || !receiver.Receive()) {
    return fail(name + ": the receiver did not get the datagram");
  }
  if (std::string(receiver.data, receiver.data.size()) != payload) {
    return fail(name + ": the receiver got a different datagram than was sent");
  }

  // A datagram from any socket other than the destination must not reach the send socket.
  int stranger = socket(AF_INET, SOCK_DGRAM, 0);
  if (stranger < 0) { return fail(std::string("could not open a stranger socket: ") + strerror(errno)); }
  sockaddr_in target = local;
  target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  sendto(stranger, "stranger", 8, 0, (sockaddr *)&target, sizeof(target));
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
