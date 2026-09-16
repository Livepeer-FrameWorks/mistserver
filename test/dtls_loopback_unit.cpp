#include <mist/certificate.h>
#include <mist/socket.h>

#include <chrono>
#include <cstdio>
#include <thread>

namespace {
  bool require(bool condition, const char *message) {
    if (!condition) { std::fprintf(stderr, "%s\n", message); }
    return condition;
  }
} // namespace

int main() {
  Certificate certificate;
  if (!require(certificate.init("NL", "MistServer", "webrtc-loopback") == 0, "could not generate the DTLS certificate")) {
    return 1;
  }

  Socket::UDPConnection server(false, AF_INET);
  Socket::UDPConnection client(false, AF_INET);
  if (!require(server.bind(0, "127.0.0.1"), "could not bind the server UDP socket") ||
      !require(client.bind(0, "127.0.0.1"), "could not bind the client UDP socket")) {
    return 1;
  }

  server.SetDestination("127.0.0.1", client.getBoundAddr().port());
  client.SetDestination("127.0.0.1", server.getBoundAddr().port());

  server.initDTLS(&certificate.cert, &certificate.key);
  client.initDTLS(&certificate.cert, &certificate.key, true);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline && (!server.handshakeComplete() || !client.handshakeComplete())) {
    server.Receive();
    client.Receive();
    if (server.timeToNextPace() != std::string::npos) { server.sendPaced(0); }
    if (client.timeToNextPace() != std::string::npos) { client.sendPaced(0); }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  if (!require(server.handshakeComplete(), "server DTLS handshake did not complete") ||
      !require(client.handshakeComplete(), "client DTLS handshake did not complete") ||
      !require(!server.cipher.empty(), "server did not negotiate an SRTP profile") ||
      !require(!client.cipher.empty(), "client did not negotiate an SRTP profile")) {
    return 1;
  }
  return 0;
}
