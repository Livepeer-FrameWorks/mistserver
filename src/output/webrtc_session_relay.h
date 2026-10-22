#pragma once

#include <mist/socket.h>

#include <deque>
#include <netinet/in.h>
#include <stdint.h>

namespace Mist {
  /// Opens the socket through which the shared WebRTC port relays STUN usernames and DELETE commands to the session
  /// processes, which listen on 224.0.0.224:<port>. The socket is connected to that group so the kernel picks the
  /// route's source address and drops every inbound datagram; left unconnected, the first send would bind it to
  /// 0.0.0.0:<ephemeral>, where it would collect datagrams that nothing ever reads.
  /// Returns null when the group address does not resolve or cannot be routed to.
  inline Socket::UDPConnection *openWebRTCSessionRelay(uint16_t port) {
    std::deque<Socket::Address> mcAddrs = Socket::getAddrs("224.0.0.224", port, AF_INET);
    if (!mcAddrs.size()) { return 0; }
    sockaddr_in anyLocal = {};
    anyLocal.sin_family = AF_INET;
    anyLocal.sin_addr.s_addr = htonl(INADDR_ANY);
    Socket::Address local(&anyLocal, sizeof(anyLocal));
    Socket::UDPConnection *relay = new Socket::UDPConnection(*mcAddrs.begin(), local);
    if (!relay->connect()) {
      delete relay;
      return 0;
    }
    return relay;
  }
} // namespace Mist
