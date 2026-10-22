#include "../src/output/webrtc_session_relay.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
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

static bool receiveWithin(Socket::UDPConnection & socket, uint32_t timeoutMs) {
  fd_set readSet;
  FD_ZERO(&readSet);
  FD_SET(socket.getSock(), &readSet);
  struct timeval timeout;
  timeout.tv_sec = timeoutMs / 1000;
  timeout.tv_usec = (timeoutMs % 1000) * 1000;
  return select(socket.getSock() + 1, &readSet, 0, 0, &timeout) > 0 && socket.Receive();
}

int main() {
  // Hosts without a route to 224.0.0.0/4 cannot relay to session processes at all; skip there.
  int probe = socket(AF_INET, SOCK_DGRAM, 0);
  sockaddr_in group = {};
  group.sin_family = AF_INET;
  group.sin_port = htons(9);
  inet_pton(AF_INET, "224.0.0.224", &group.sin_addr);
  if (probe < 0 || connect(probe, (sockaddr *)&group, sizeof(group))) {
    std::cerr << "SKIP: no route to 224.0.0.224: " << strerror(errno) << std::endl;
    return 77;
  }
  close(probe);

  // A session process listens on the group the way OutWebRTC::setupIncomingMainSocket binds it.
  Socket::UDPConnection session;
  uint16_t port = session.bind(0, "224.0.0.224");
  if (!port) { return fail("the session socket could not bind to 224.0.0.224"); }
  session.allocateDestination();

  Socket::UDPConnection *relay = Mist::openWebRTCSessionRelay(port);
  if (!relay || !*relay) { return fail("the relay socket could not be opened"); }
  const std::string payload = "stunuser";
  relay->SendNow(payload);

  sockaddr_in local = {};
  socklen_t localLen = sizeof(local);
  if (getsockname(relay->getSock(), (sockaddr *)&local, &localLen)) {
    return fail(std::string("getsockname failed: ") + strerror(errno));
  }
  char localStr[INET_ADDRSTRLEN] = {0};
  inet_ntop(AF_INET, &local.sin_addr, localStr, sizeof(localStr));
  if (local.sin_addr.s_addr == htonl(INADDR_ANY)) {
    return fail(std::string("the relay sits bound to the wildcard address ") + localStr + ":" +
                std::to_string(ntohs(local.sin_port)) + ", where it collects datagrams nobody reads");
  }
  sockaddr_in peer = {};
  socklen_t peerLen = sizeof(peer);
  if (getpeername(relay->getSock(), (sockaddr *)&peer, &peerLen)) {
    return fail(std::string("the relay is not connected to its group: ") + strerror(errno));
  }
  if (peer.sin_addr.s_addr != group.sin_addr.s_addr || ntohs(peer.sin_port) != port) {
    return fail("the relay is connected somewhere other than 224.0.0.224:" + std::to_string(port));
  }

  if (!receiveWithin(session, 2000)) { return fail("the session socket did not receive the relayed datagram"); }
  if (std::string(session.data, session.data.size()) != payload) {
    return fail("the session socket received a different datagram than was relayed");
  }

  relay->close();
  delete relay;
  std::cout << "relay bound to " << localStr << ":" << ntohs(local.sin_port) << ", connected to 224.0.0.224:" << port
            << " and delivered the datagram" << std::endl;
  return 0;
}
