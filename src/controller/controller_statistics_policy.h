#pragma once

#include <mist/json.h>

#include <cstdint>
#include <set>

namespace Controller {
  inline JSON::Value sourcePidList(const std::set<uint64_t> & claimedPids) {
    JSON::Value result;
    // JSON::Value has no explicit array constructor. Appending and removing a
    // placeholder preserves the authoritative empty-array type.
    result.append();
    result.shrink(0);
    for (std::set<uint64_t>::const_iterator it = claimedPids.begin(); it != claimedPids.end(); ++it) {
      if (*it) { result.append(*it); }
    }
    return result;
  }

  inline void accumulateInterfaceCounters(bool isLinkLayer, bool isLoopback, bool hasCounterData, uint64_t transmittedBytes,
                                          uint64_t receivedBytes, uint64_t & totalUp, uint64_t & totalDown) {
    if (!isLinkLayer || isLoopback || !hasCounterData) { return; }
    totalUp += transmittedBytes;
    totalDown += receivedBytes;
  }
} // namespace Controller
