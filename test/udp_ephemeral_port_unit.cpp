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
#include <vector>

// Ports this process holds model the sockets of other Mist processes on the host, such as a live SRT rendezvous
// socket or a WebRTC session port. With 400 of the roughly 28000 ephemeral ports held, 2000 binds that may land on
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

  // A per-peer socket still joins the port an ephemeral bind holds, as WebRTC and Foghorn punching do, and takes the
  // datagrams its peer sends there.
  Socket::UDPConnection & listener = *held.front();
  Socket::UDPConnection peer(false, AF_INET);
  if (!peer.bind(0, "127.0.0.1")) { return fail("could not bind the peer socket"); }
  Socket::UDPConnection joined(peer.getBoundAddr(), listener.getBoundAddr());
  if (!joined.connect()) { return fail("a per-peer socket could not join the port of an ephemeral bind"); }
  peer.setDestination(listener.getBoundAddr());
  const std::string payload("joined");
  peer.SendNow(payload);
  if (!receivedWithin(joined, payload, 2000)) {
    return fail("the per-peer socket did not receive its peer's datagram");
  }

  std::cout << "ephemeral UDP ports stay exclusive across " << bindCount << " binds and " << bindCount << " sends" << std::endl;
  return 0;
}
