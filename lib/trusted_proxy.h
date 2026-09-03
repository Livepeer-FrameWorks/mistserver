#pragma once

#include "shared_memory.h"

#include <set>
#include <string>

namespace Util {
  /// Replaces the named shared-memory page with a ready trusted-proxy list.
  bool publishTrustedProxyList(IPC::sharedPage & page, const std::string & pageName, const std::string & trustedList);

  /// Reads a ready trusted-proxy list. Incomplete or malformed pages return an empty set.
  std::set<std::string> readTrustedProxyList(const IPC::sharedPage & page);
} // namespace Util
