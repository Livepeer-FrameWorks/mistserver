#include <mist/defines.h>
#include <mist/socket.h>
#include <mist/timing.h>

#include <arpa/inet.h>
#include <iostream>
#include <memory>
#include <netinet/in.h>
#include <poll.h>
#include <set>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

// Ports this process holds model the non-sharing ephemeral sockets of other Mist processes on the host, such as
// RTP ports or send sockets. With 400 of the roughly 28000 ephemeral ports held, 2000 binds that may land on
// held ports hit one about 28 times.
static const size_t heldCount = 400;
static const size_t bindCount = 2000;

static int fail(const std::string & message) {
  std::cerr << "FAIL: " << message << std::endl;
  return 1;
}

static uint16_t localPort(int sock) {
  sockaddr_in addr = {};
  socklen_t len = sizeof addr;
  if (getsockname(sock, (sockaddr *)&addr, &len) || addr.sin_family != AF_INET) { return 0; }
  return ntohs(addr.sin_port);
}

static bool receivedWithin(Socket::UDPConnection & sock, const std::string & payload, uint32_t timeoutMs) {
  const uint64_t deadline = Util::bootMS() + timeoutMs;
  while (Util::bootMS() < deadline) {
    pollfd readable = {sock.getSock(), POLLIN, 0};
    poll(&readable, 1, 50);
    if (sock.Receive() && std::string(sock.data, sock.data.size()) == payload) { return true; }
  }
  return false;
}

int main() {
  Util::printDebugLevel = DLVL_WARN;

  std::vector<std::unique_ptr<Socket::UDPConnection>> held;
  std::set<uint16_t> heldPorts;
  for (size_t i = 0; i < heldCount; ++i) {
    std::unique_ptr<Socket::UDPConnection> sock(new Socket::UDPConnection(false, AF_INET));
    const uint16_t port = sock->bind(0, "127.0.0.1");
    if (!port) { return fail("could not bind a held ephemeral UDP socket"); }
    heldPorts.insert(port);
    held.push_back(std::move(sock));
  }

  // An ephemeral bind must never be handed a port another socket holds: the newer socket on an address receives
  // every unicast datagram sent there, so the holder silently stops receiving.
  size_t bindCollisions = 0;
  for (size_t i = 0; i < bindCount; ++i) {
    Socket::UDPConnection sock(false, AF_INET);
    const uint16_t port = sock.bind(0, "127.0.0.1");
    if (!port) { return fail("could not bind an ephemeral UDP socket"); }
    if (heldPorts.count(port)) { ++bindCollisions; }
  }
  if (bindCollisions) {
    return fail("ephemeral binds landed on held ports " + std::to_string(bindCollisions) + " times out of " +
                std::to_string(bindCount));
  }

  // A send-only socket takes its local port from the kernel on its first send; that port must not be a held one.
  Socket::UDPConnection sink(false, AF_INET);
  const uint16_t sinkPort = sink.bind(0, "127.0.0.1");
  if (!sinkPort) { return fail("could not bind the sink socket"); }
  size_t sendCollisions = 0;
  for (size_t i = 0; i < bindCount; ++i) {
    Socket::UDPConnection sender(false, AF_INET);
    sender.SetDestination("127.0.0.1", sinkPort);
    sender.SendNow("x", 1);
    const uint16_t port = localPort(sender.getSock());
    if (!port) { return fail("a send-only socket has no local port after sending"); }
    if (heldPorts.count(port)) { ++sendCollisions; }
    while (sink.Receive()) {}
  }
  if (sendCollisions) {
    return fail("send-only sockets were given held ports " + std::to_string(sendCollisions) + " times out of " +
                std::to_string(bindCount));
  }

  // A foreign program that sets SO_REUSEADDR before bind(0) must not be handed a held Mist port either.
  size_t foreignCollisions = 0;
  for (size_t i = 0; i < bindCount; ++i) {
    const int sock = socket(AF_INET, SOCK_DGRAM, 0);
    const int on = 1;
    sockaddr_in any = {};
    any.sin_family = AF_INET;
    any.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (sock < 0 || setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on) || ::bind(sock, (sockaddr *)&any, sizeof any)) {
      return fail("could not bind a foreign SO_REUSEADDR socket");
    }
    if (heldPorts.count(localPort(sock))) { ++foreignCollisions; }
    close(sock);
  }
  if (foreignCollisions) {
    return fail("foreign SO_REUSEADDR binds landed on held ports " + std::to_string(foreignCollisions) +
                " times out of " + std::to_string(bindCount));
  }

  // Only a socket that opted into port sharing can be joined by a per-peer socket.
  Socket::UDPConnection peer(false, AF_INET);
  if (!peer.bind(0, "127.0.0.1")) { return fail("could not bind the peer socket"); }
  Socket::UDPConnection intruder(peer.getBoundAddr(), held.front()->getBoundAddr());
  if (intruder.connect()) { return fail("a per-peer socket joined an ephemeral port that does not share"); }

  // A per-peer socket joins the ephemeral port of a sharing socket, as WebRTC sessions, shared-port listeners and
  // Foghorn punching do, and takes the datagrams its peer sends there.
  Socket::UDPConnection listener(false, AF_INET);
  listener.setPortSharing(true);
  if (!listener.bind(0, "127.0.0.1")) { return fail("could not bind the sharing listener socket"); }
  Socket::UDPConnection joined(peer.getBoundAddr(), listener.getBoundAddr());
  if (!joined.connect()) { return fail("a per-peer socket could not join the port of a sharing ephemeral bind"); }
  peer.setDestination(listener.getBoundAddr());
  const std::string payload("joined");
  peer.SendNow(payload);
  if (!receivedWithin(joined, payload, 2000)) {
    return fail("the per-peer socket did not receive its peer's datagram");
  }

  std::cout << "ephemeral UDP ports stay exclusive across " << bindCount << " Mist binds, " << bindCount
            << " sends and " << bindCount << " foreign SO_REUSEADDR binds" << std::endl;
  return 0;
}
