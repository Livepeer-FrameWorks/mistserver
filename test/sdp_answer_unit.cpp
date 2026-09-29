#include <mist/sdp_media.h>

#include <iostream>
#include <string>

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

int main() {
  if (!oneMediaPerTypeAndCompleteBundle()) { return 1; }
  if (!singleEnabledMediaIsBundled()) { return 1; }
  if (!setupRoleFollowsTheOffer()) { return 1; }
  if (!answerAnnouncesTheNegotiatedRole()) { return 1; }
  return 0;
}
