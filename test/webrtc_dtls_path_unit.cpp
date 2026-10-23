#include "../src/output/webrtc_dtls_path.h"

#include <mist/certificate.h>
#include <mist/socket.h>

#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

// A browser reaches a WebRTC session over several candidate pairs (here: through a port proxy and directly) and sends
// its DTLS flights over whichever pair its ICE agent currently selected. This test puts a forwarder between a DTLS
// client and two server-side pair sockets, sends the client's datagrams over pair A until a chosen datagram and over
// pair B after it, and checks that the session completes one handshake and keeps decrypting records on either pair.

namespace {
  int fail(const char *message) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    return 1;
  }

  struct RawSocket {
      int fd = -1;
      sockaddr_in addr = {};
      bool open() {
        fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) { return false; }
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t len = sizeof(addr);
        return !bind(fd, (sockaddr *)&addr, sizeof(addr)) && !getsockname(fd, (sockaddr *)&addr, &len);
      }
      ~RawSocket() {
        if (fd >= 0) { ::close(fd); }
      }
      uint16_t port() const { return ntohs(addr.sin_port); }
      // Moves every waiting datagram to the destination, sending it from out (or from this socket).
      size_t forward(const RawSocket *out, uint16_t destPort) {
        size_t count = 0;
        char buf[4096];
        sockaddr_in dest = {};
        dest.sin_family = AF_INET;
        dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        dest.sin_port = htons(destPort);
        ssize_t r;
        while ((r = recv(fd, buf, sizeof(buf), MSG_DONTWAIT)) > 0) {
          sendto((out ? out : this)->fd, buf, r, 0, (sockaddr *)&dest, sizeof(dest));
          ++count;
        }
        return count;
      }
  };

  void flush(Socket::UDPConnection & s) {
    while (s.timeToNextPace() != std::string::npos) { s.sendPaced(0); }
  }

  struct Session {
      Certificate & cert;
      bool perPairContexts; ///< The broken design: every pair socket runs its own DTLS association.
      Socket::UDPConnection *owner = 0;
      std::vector<std::string> received;

      Session(Certificate & c, bool perPair) : cert(c), perPairContexts(perPair) {}

      void collect(Socket::UDPConnection & s) {
        if (s.wasEncrypted) { received.push_back(std::string(s.data, s.data.size())); }
      }

      void pump(Socket::UDPConnection & s) {
        while (s.Receive()) {
          if (s.dtlsEnabled()) {
            collect(s);
            continue;
          }
          if (perPairContexts) { continue; }
          Socket::UDPConnection *dst = Mist::routeDTLSRecord(owner, s, true, &cert.cert, &cert.key);
          if (!dst) { continue; }
          owner = dst;
          if (dst == &s) { continue; }
          while (dst->Receive()) { collect(*dst); }
        }
        flush(s);
        if (owner) { flush(*owner); }
      }
  };

  /// Runs one client against a session reached over pairs A and B. The client's datagrams up to and including number
  /// switchAfter take pair A, later ones pair B; switchBackAfter (when nonzero) moves it back to pair A after that
  /// datagram. After the handshake the client sends two records, one on each side of the last switch.
  int run(Certificate & cert, bool perPairContexts, size_t switchAfter, size_t switchBackAfter, std::string & why) {
    RawSocket clientSide, pairA, pairB;
    if (!clientSide.open() || !pairA.open() || !pairB.open()) {
      why = "could not open the forwarder sockets";
      return -1;
    }

    Socket::UDPConnection serverA(false, AF_INET), serverB(false, AF_INET), client(false, AF_INET);
    if (!serverA.bind(0, "127.0.0.1") || !serverB.bind(0, "127.0.0.1") || !client.bind(0, "127.0.0.1")) {
      why = "could not bind the UDP sockets";
      return -1;
    }
    serverA.SetDestination("127.0.0.1", pairA.port());
    serverB.SetDestination("127.0.0.1", pairB.port());
    client.SetDestination("127.0.0.1", clientSide.port());

    Session session(cert, perPairContexts);
    if (perPairContexts) {
      serverA.initDTLS(&cert.cert, &cert.key);
      serverB.initDTLS(&cert.cert, &cert.key);
    }
    client.initDTLS(&cert.cert, &cert.key, true);

    size_t sent = 0;
    bool wroteRecords = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
      flush(client);
      // Route the client's datagrams one at a time so each takes the pair selected at that moment.
      char buf[4096];
      ssize_t r;
      while ((r = recv(clientSide.fd, buf, sizeof(buf), MSG_DONTWAIT)) > 0) {
        ++sent;
        bool useB = sent > switchAfter && !(switchBackAfter && sent > switchBackAfter);
        const RawSocket & pair = useB ? pairB : pairA;
        sockaddr_in dest = {};
        dest.sin_family = AF_INET;
        dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        dest.sin_port = htons(useB ? serverB.getBoundAddr().port() : serverA.getBoundAddr().port());
        sendto(pair.fd, buf, r, 0, (sockaddr *)&dest, sizeof(dest));
      }
      session.pump(serverA);
      session.pump(serverB);
      // The client accepts the session's datagrams from either pair.
      pairA.forward(&clientSide, client.getBoundAddr().port());
      pairB.forward(&clientSide, client.getBoundAddr().port());
      while (client.Receive()) {}

      if (client.handshakeComplete() && !wroteRecords) {
        wroteRecords = true;
        client.sendPaced("record-one", 10);
        flush(client);
        client.sendPaced("record-two", 10);
        flush(client);
      }
      if (wroteRecords && session.received.size() >= 2) { break; }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (!client.handshakeComplete()) {
      why = "the client's DTLS handshake did not complete";
      return 1;
    }
    if (!perPairContexts) {
      if (!session.owner || !session.owner->handshakeComplete()) {
        why = "the session holds no completed DTLS association";
        return 1;
      }
      if (serverA.dtlsEnabled() + serverB.dtlsEnabled() != 1) {
        why = "the session runs more than one DTLS association";
        return 1;
      }
      if (session.owner->cipher.empty()) {
        why = "the session association negotiated no SRTP profile";
        return 1;
      }
    }
    // The two records may take different pairs, so they may arrive in either order.
    std::sort(session.received.begin(), session.received.end());
    if (session.received.size() != 2 || session.received[0] != "record-one" || session.received[1] != "record-two") {
      why = "the session did not decrypt the client's records on the pair the client used";
      return 1;
    }
    return 0;
  }
} // namespace

int main() {
  Certificate cert;
  if (cert.init("NL", "MistServer", "webrtc-dtls-path") != 0) {
    return fail("could not generate the DTLS certificate");
  }

  std::string why;
  // The client's first two datagrams are its ClientHello and, after the HelloVerifyRequest, its ClientHello with the
  // cookie; both take pair A, which answers with its ServerHello. The client's next flight takes pair B: a switch
  // halfway through the handshake. A context per pair has no handshake in progress on pair B, so it never completes.
  int r = run(cert, true, 2, 0, why);
  if (r < 0) { return fail(why.c_str()); }
  if (!r) { return fail("control: a context per pair completed a handshake that switched pairs halfway"); }

  struct Case {
      size_t switchAfter, switchBackAfter;
      const char *name;
  } cases[] = {
    {0, 0, "all datagrams over pair B, which carries the first ClientHello"},
    {1, 0, "switch to pair B after the first ClientHello"},
    {2, 0, "switch to pair B after the ClientHello with the cookie"},
    {1, 2, "switch to pair B after the first ClientHello and back to pair A after the next"},
    {1000, 0, "every datagram over pair A"},
  };
  for (const Case & c : cases) {
    why.clear();
    r = run(cert, false, c.switchAfter, c.switchBackAfter, why);
    if (r) {
      std::fprintf(stderr, "case: %s\n", c.name);
      return fail(why.c_str());
    }
    std::printf("ok: %s\n", c.name);
  }

  // After the handshake the client's data-channel records move to pair B and back to pair A.
  // The handshake takes a few datagrams; the last switch lands on the records that follow it.
  for (size_t afterHandshake : {3, 4, 5}) {
    why.clear();
    r = run(cert, false, afterHandshake, afterHandshake + 1, why);
    if (r) {
      std::fprintf(stderr, "case: records after datagram %zu over pair B, then pair A\n", afterHandshake);
      return fail(why.c_str());
    }
  }
  std::printf("ok: records follow the client between pairs after the handshake\n");
  return 0;
}
