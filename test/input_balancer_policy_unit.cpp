#include "../src/input/input_balancer_policy.h"

#include <iostream>

namespace {
  int fail(const char *message) {
    std::cerr << message << std::endl;
    return 1;
  }
} // namespace

int main() {
  if (Mist::classifyBalancerResponse(false, "", false) != Mist::BALANCER_KEEP_FALLBACK ||
      Mist::classifyBalancerResponse(false, "dtsc://remote/live", false) != Mist::BALANCER_KEEP_FALLBACK ||
      Mist::classifyBalancerResponse(true, "dtsc://127.0.0.1/live", true) != Mist::BALANCER_KEEP_FALLBACK) {
    return fail("failed or local balancer results must retain the configured fallback source");
  }
  if (Mist::classifyBalancerResponse(true, "", false) != Mist::BALANCER_OFFLINE ||
      Mist::classifyBalancerResponse(true, "offline:no active broadcast", false) != Mist::BALANCER_OFFLINE) {
    return fail("empty and explicit offline balancer results must be authoritative");
  }
  if (Mist::classifyBalancerResponse(true, "dtsc://remote/live", false) != Mist::BALANCER_USE_RESPONSE) {
    return fail("a remote balancer result must replace the configured fallback source");
  }

  if (Mist::classifyBalancerSource(false, true, true) != Mist::BALANCER_SOURCE_BOOTABLE ||
      Mist::classifyBalancerSource(true, true, false) != Mist::BALANCER_SOURCE_BOOTABLE) {
    return fail("a source bootable in the current context must be selected");
  }
  if (Mist::classifyBalancerSource(false, false, true) != Mist::BALANCER_SOURCE_PROVIDER_ONLY) {
    return fail("a push/provider-only source must report deliberate offline to a playback attempt");
  }
  if (Mist::classifyBalancerSource(false, false, false) != Mist::BALANCER_SOURCE_UNSUPPORTED ||
      Mist::classifyBalancerSource(true, false, true) != Mist::BALANCER_SOURCE_UNSUPPORTED) {
    return fail("an unsupported source must remain a fallback-eligible startup failure");
  }

  return 0;
}
