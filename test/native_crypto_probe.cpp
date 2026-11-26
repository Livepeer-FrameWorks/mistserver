#include <cstdio>
#include <cstring>
#include <srtp2/srtp.h>

namespace {
  bool check(srtp_err_status_t result, const char *operation) {
    if (result == srtp_err_status_ok) { return true; }
    std::fprintf(stderr, "%s failed: SRTP status %d\n", operation, int(result));
    return false;
  }

  bool roundTrip(srtp_t sender, srtp_t receiver, bool rtcp) {
    const unsigned char rtp[] = {0x80, 96, 0, 1, 0, 0, 0, 1, 0, 0, 0, 7, 'm', 'e', 'd', 'i', 'a'};
    const unsigned char report[] = {0x80, 201, 0, 1, 0, 0, 0, 7};
    const unsigned char *original = rtcp ? report : rtp;
    const int originalSize = rtcp ? sizeof(report) : sizeof(rtp);
    unsigned char packet[256] = {};
    std::memcpy(packet, original, originalSize);
    int size = originalSize;
    if (!check(rtcp ? srtp_protect_rtcp(sender, packet, &size) : srtp_protect(sender, packet, &size), "protect")) {
      return false;
    }
    if (size <= originalSize) {
      std::fprintf(stderr, "SRTP protection did not add authentication data\n");
      return false;
    }
    if (!check(rtcp ? srtp_unprotect_rtcp(receiver, packet, &size) : srtp_unprotect(receiver, packet, &size), "unprotect")) {
      return false;
    }
    if (size != originalSize || std::memcmp(packet, original, originalSize)) {
      std::fprintf(stderr, "SRTP round trip changed the original packet\n");
      return false;
    }
    return true;
  }

  bool profile(srtp_profile_t cipher) {
    unsigned char key[30];
    for (unsigned int i = 0; i < sizeof(key); ++i) { key[i] = i + 1; }
    srtp_policy_t policy = {};
    if (!check(srtp_crypto_policy_set_from_profile_for_rtp(&policy.rtp, cipher), "RTP policy") ||
        !check(srtp_crypto_policy_set_from_profile_for_rtcp(&policy.rtcp, cipher), "RTCP policy")) {
      return false;
    }
    policy.key = key;
    policy.ssrc.type = ssrc_any_outbound;
    srtp_t sender = nullptr;
    srtp_t receiver = nullptr;
    if (!check(srtp_create(&sender, &policy), "sender creation")) { return false; }
    policy.ssrc.type = ssrc_any_inbound;
    bool success = check(srtp_create(&receiver, &policy), "receiver creation");
    if (success) { success = roundTrip(sender, receiver, false) && roundTrip(sender, receiver, true); }
    if (receiver) { success = check(srtp_dealloc(receiver), "receiver teardown") && success; }
    success = check(srtp_dealloc(sender), "sender teardown") && success;
    return success;
  }
} // namespace

int main() {
  if (!check(srtp_init(), "SRTP initialization")) { return 1; }
  const bool success = profile(srtp_profile_aes128_cm_sha1_80) && profile(srtp_profile_aes128_cm_sha1_32);
  const bool shutdown = check(srtp_shutdown(), "SRTP shutdown");
  if (success && shutdown) { std::puts("SRTP and SRTCP authenticated round trips passed for both WebRTC profiles"); }
  return success && shutdown ? 0 : 1;
}
