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
} // namespace

int main() {
  if (!oneMediaPerTypeAndCompleteBundle()) { return 1; }
  if (!singleEnabledMediaIsBundled()) { return 1; }
  return 0;
}
