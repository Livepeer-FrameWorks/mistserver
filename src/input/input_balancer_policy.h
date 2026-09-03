#pragma once

#include <string>

namespace Mist {
  enum BalancerResponseKind { BALANCER_KEEP_FALLBACK, BALANCER_OFFLINE, BALANCER_USE_RESPONSE };

  inline BalancerResponseKind classifyBalancerResponse(bool requestSucceeded, const std::string & response, bool responseIsLocal) {
    if (!requestSucceeded) { return BALANCER_KEEP_FALLBACK; }
    if (response.empty() || response.compare(0, 8, "offline:") == 0) { return BALANCER_OFFLINE; }
    if (responseIsLocal) { return BALANCER_KEEP_FALLBACK; }
    return BALANCER_USE_RESPONSE;
  }

  enum BalancerSourceKind { BALANCER_SOURCE_BOOTABLE, BALANCER_SOURCE_PROVIDER_ONLY, BALANCER_SOURCE_UNSUPPORTED };

  inline BalancerSourceKind classifyBalancerSource(bool providerContext, bool bootableInContext, bool bootableAsProvider) {
    if (bootableInContext) { return BALANCER_SOURCE_BOOTABLE; }
    if (!providerContext && bootableAsProvider) { return BALANCER_SOURCE_PROVIDER_ONLY; }
    return BALANCER_SOURCE_UNSUPPORTED;
  }
} // namespace Mist
