#include <mist/sdp_media.h>
#include <mist/socket.h>
#include <mist/timing.h>

#include <iostream>
#include <netdb.h>
#include <string>
#include <vector>

namespace {
  bool contains(const std::string & haystack, const std::string & needle) {
    if (haystack.find(needle) != std::string::npos) { return true; }
    std::cerr << "Missing SDP fragment: " << needle << std::endl;
    return false;
  }

  size_t count(const std::string & haystack, const std::string & needle) {
    size_t result = 0;
    size_t offset = 0;
    while ((offset = haystack.find(needle, offset)) != std::string::npos) {
      ++result;
      offset += needle.size();
    }
    return result;
  }

  SDP::Media offeredMedia(const std::string & type, const std::string & mid, const std::string & payloadTypes) {
    SDP::Media media;
    media.type = type;
    media.mediaID = mid;
    media.payloadTypes = payloadTypes;
    return media;
  }

  void configureFormat(SDP::Media & media, SDP::MediaFormat & format, const std::string & type,
                       const std::string & codec, uint64_t payloadType) {
    media.type = type;
    format.encodingName = codec;
    format.payloadType = payloadType;
    format.iceUFrag = "local-user";
    format.icePwd = "local-password";
    format.rtpmap = "rtpmap:" + std::to_string(payloadType) + " " + codec + (type == "audio" ? "/48000/2" : "/90000");
  }

  bool oneMediaPerTypeAndCompleteBundle() {
    SDP::Answer answer;
    answer.direction = "sendonly";
    answer.candidates.push_back("127.0.0.1");
    answer.port = 5000;
    answer.fingerprint = "00:11";
    answer.isVideoEnabled = true;
    answer.isAudioEnabled = true;

    answer.sdpOffer.medias.push_back(offeredMedia("video", "video-main", "96"));
    answer.sdpOffer.medias.push_back(offeredMedia("video", "video-backup", "97"));
    answer.sdpOffer.medias.push_back(offeredMedia("audio", "audio-main", "111"));
    answer.sdpOffer.medias.push_back(offeredMedia("audio", "audio-backup", "112"));

    configureFormat(answer.answerVideoMedia, answer.answerVideoFormat, "video", "VP8", 96);
    configureFormat(answer.answerAudioMedia, answer.answerAudioFormat, "audio", "OPUS", 111);
    answer.answerVideoMedia.mediaID = "video-main";
    answer.answerAudioMedia.mediaID = "audio-main";

    const std::string sdp = answer.toString();
    bool ok = true;
    ok &= contains(sdp, "a=group:BUNDLE video-main audio-main\r\n");
    ok &= count(sdp, "m=video 9 ") == 1;
    ok &= count(sdp, "m=video 0 ") == 1;
    ok &= count(sdp, "m=audio 9 ") == 1;
    ok &= count(sdp, "m=audio 0 ") == 1;
    ok &= contains(sdp, "a=mid:video-main\r\n");
    ok &= contains(sdp, "a=mid:video-backup\r\n");
    ok &= contains(sdp, "a=mid:audio-main\r\n");
    ok &= contains(sdp, "a=mid:audio-backup\r\n");
    return ok;
  }

  bool singleEnabledMediaIsBundled() {
    SDP::Answer answer;
    answer.direction = "sendonly";
    answer.candidates.push_back("127.0.0.1");
    answer.port = 5000;
    answer.fingerprint = "00:11";
    answer.isVideoEnabled = true;
    answer.sdpOffer.medias.push_back(offeredMedia("video", "only-video", "96"));
    configureFormat(answer.answerVideoMedia, answer.answerVideoFormat, "video", "VP8", 96);

    return contains(answer.toString(), "a=group:BUNDLE only-video\r\n");
  }

  std::string offerWithSetup(const std::string & sessionSetup, const std::string & audioSetup, const std::string & videoSetup) {
    std::string sdp = "v=0\r\no=- 1 2 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\na=group:BUNDLE 0 1\r\n";
    if (!sessionSetup.empty()) { sdp += "a=setup:" + sessionSetup + "\r\n"; }
    sdp += "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\nc=IN IP4 0.0.0.0\r\na=mid:0\r\na=sendonly\r\n";
    if (!audioSetup.empty()) { sdp += "a=setup:" + audioSetup + "\r\n"; }
    sdp += "a=rtpmap:111 opus/48000/2\r\n";
    sdp += "m=video 9 UDP/TLS/RTP/SAVPF 106\r\nc=IN IP4 0.0.0.0\r\na=mid:1\r\na=sendonly\r\n";
    if (!videoSetup.empty()) { sdp += "a=setup:" + videoSetup + "\r\n"; }
    sdp += "a=rtpmap:106 H264/90000\r\n";
    return sdp;
  }

  // Returns our negotiated a=setup value, or "error: <reason>".
  std::string negotiated(const std::string & offer) {
    SDP::Answer answer;
    if (!answer.parseOffer(offer)) { return "error: unparsable"; }
    std::string error;
    if (!answer.negotiateSetup(error)) { return "error: " + error; }
    return answer.setup;
  }

  bool expect(const std::string & what, const std::string & got, const std::string & want) {
    if (got == want) { return true; }
    std::cerr << what << ": negotiated '" << got << "', expected '" << want << "'" << std::endl;
    return false;
  }

  bool setupRoleFollowsTheOffer() {
    bool ok = true;
    ok &= expect("actpass offer", negotiated(offerWithSetup("", "actpass", "actpass")), "passive");
    ok &= expect("active offer", negotiated(offerWithSetup("", "active", "active")), "passive");
    ok &= expect("offer without a=setup", negotiated(offerWithSetup("", "", "")), "passive");
    ok &= expect("passive offer (ffmpeg WHIP)", negotiated(offerWithSetup("", "passive", "passive")), "active");
    ok &= expect("session-level passive offer", negotiated(offerWithSetup("passive", "", "")), "active");
    ok &= expect("media overrides the session setup", negotiated(offerWithSetup("passive", "actpass", "actpass")), "passive");
    const std::string holdconn = negotiated(offerWithSetup("", "holdconn", "holdconn"));
    ok &= expect("holdconn offer", holdconn.substr(0, 7), "error: ");
    ok &= contains(holdconn, "holdconn");
    const std::string mixed = negotiated(offerWithSetup("", "passive", "actpass"));
    ok &= expect("conflicting media roles", mixed.substr(0, 7), "error: ");
    return ok;
  }

  bool answerAnnouncesTheNegotiatedRole() {
    SDP::Answer answer;
    answer.direction = "recvonly";
    answer.candidates.push_back("127.0.0.1");
    answer.port = 5000;
    answer.fingerprint = "00:11";
    answer.isVideoEnabled = true;
    answer.sdpOffer.medias.push_back(offeredMedia("video", "1", "106"));
    configureFormat(answer.answerVideoMedia, answer.answerVideoFormat, "video", "H264", 106);
    bool ok = contains(answer.toString(), "a=setup:passive\r\n");
    answer.setup = "active";
    const std::string sdp = answer.toString();
    ok &= contains(sdp, "a=setup:active\r\n");
    ok &= count(sdp, "a=setup:passive") == 0;
    return ok;
  }
} // namespace

namespace {
  std::vector<std::string> resolved;

  // Fails a .local name the way unicast DNS does (EAI_AGAIN, "Temporary failure in name
  // resolution"); resolves everything else normally.
  int recordingResolver(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res) {
    resolved.push_back(node ? node : "");
    if (node && SDP::isMdnsCandidateHost(node)) { return EAI_AGAIN; }
    return getaddrinfo(node, service, hints, res);
  }

  // A browser's offer carries an mDNS host candidate next to a reflexive one. Only the address is
  // resolved; the .local name is skipped instead of retried against unicast DNS for seconds.
  bool mdnsCandidatesAreSkipped() {
    const std::string offer =
      "v=0\r\n"
      "o=- 1 2 IN IP4 127.0.0.1\r\n"
      "s=-\r\n"
      "t=0 0\r\n"
      "a=candidate:1 1 udp 2122260223 5b37985c-06ed-4a63-bc6d-929adb7789ac.local 54321 typ host\r\n"
      "a=candidate:2 1 udp 1686052607 192.0.2.10 54322 typ srflx raddr 0.0.0.0 rport 0\r\n"
      "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
      "a=mid:0\r\n"
      "a=rtpmap:96 VP8/90000\r\n";
    resolved.clear();
    Socket::setAddrInfoResolver(recordingResolver);
    SDP::Session session;
    const uint64_t began = Util::bootMS();
    session.parseSDP(offer);
    const uint64_t took = Util::bootMS() - began;
    Socket::setAddrInfoResolver(0);
    bool ok = true;
    for (const std::string & name : resolved) {
      if (SDP::isMdnsCandidateHost(name)) {
        std::cerr << "The mDNS candidate " << name << " was resolved through unicast DNS" << std::endl;
        ok = false;
      }
    }
    if (session.candidates.size() != 1) {
      std::cerr << "Expected the reflexive candidate only, got " << session.candidates.size() << " candidates" << std::endl;
      ok = false;
    }
    if (took > 1000) {
      std::cerr << "Parsing the offer took " << took << " ms" << std::endl;
      ok = false;
    }
    if (!SDP::isMdnsCandidateHost("abc.LOCAL") || SDP::isMdnsCandidateHost("local") ||
        SDP::isMdnsCandidateHost("192.0.2.10") || SDP::isMdnsCandidateHost("host.localdomain")) {
      std::cerr << "isMdnsCandidateHost misclassifies a host" << std::endl;
      ok = false;
    }
    return ok;
  }
} // namespace

int main() {
  if (!mdnsCandidatesAreSkipped()) { return 1; }
  if (!oneMediaPerTypeAndCompleteBundle()) { return 1; }
  if (!singleEnabledMediaIsBundled()) { return 1; }
  if (!setupRoleFollowsTheOffer()) { return 1; }
  if (!answerAnnouncesTheNegotiatedRole()) { return 1; }
  return 0;
}
