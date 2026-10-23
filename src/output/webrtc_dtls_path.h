#pragma once

#include <mist/socket.h>

#include <stddef.h>
#include <stdint.h>

namespace Mist {
  /// DTLS record content types (RFC 7983 demultiplexing range 20..63).
  inline bool isDTLSRecord(const char *data, size_t len) {
    return len && (uint8_t)data[0] > 19 && (uint8_t)data[0] < 64;
  }

  /// Picks the socket that processes the DTLS record in the receive buffer of path, a candidate-pair socket that holds
  /// no DTLS context. A WebRTC session has one DTLS association for all of its candidate pairs: the peer sends its
  /// handshake flights and data-channel records over whichever pair its ICE agent selected, and it switches pairs
  /// whenever it prefers another one, also halfway through the handshake. A context per pair would answer such a peer
  /// from a second, unrelated handshake (a fresh HelloVerifyRequest or ServerHello), which browsers reject as an
  /// unexpected message.
  /// - Without an association (owner is null), a handshake record makes path the association's socket. Only the DTLS
  ///   server takes one this way; a client starts its association itself.
  /// - With an association on owner, owner takes over the address pair of path by swapping their descriptors, so its
  ///   replies, SRTP and SCTP traffic follow the pair the peer now uses, and path keeps the pair owner had.
  /// Returns the socket whose next Receive() yields the record, or null when no association takes it.
  inline Socket::UDPConnection *routeDTLSRecord(Socket::UDPConnection *owner, Socket::UDPConnection & path, bool server,
                                                mbedtls_x509_crt *cert, mbedtls_pk_context *key) {
    if (path.dtlsEnabled() || !isDTLSRecord(path.data, path.data.size())) { return 0; }
    if (!owner) {
      // Content type 22: handshake. Anything else cannot open an association.
      if (!server || (uint8_t)path.data[0] != 22) { return 0; }
      path.initDTLS(cert, key);
      path.injectReceived(path.data, path.data.size());
      return &path;
    }
    if (owner == &path) { return 0; }
    owner->swapSocket(path);
    owner->injectReceived(path.data, path.data.size());
    return owner;
  }
} // namespace Mist
