#include "../src/output/webrtc_output_policy.h"

#include <cstdio>
#include <vector>

namespace {
  int fail(const char *message) {
    fprintf(stderr, "%s\n", message);
    return 1;
  }
} // namespace

int main() {
  using namespace Mist;

  if (!isWebRTCOutputVideoCodec("H264") || !isWebRTCOutputVideoCodec("VP8") || !isWebRTCOutputVideoCodec("VP9") ||
      !isWebRTCOutputVideoCodec("AV1") || !isWebRTCOutputVideoCodec("HEVC") || isWebRTCOutputVideoCodec("JPEG") ||
      !isWebRTCOutputAudioCodec("opus") || !isWebRTCOutputAudioCodec("ALAW") || !isWebRTCOutputAudioCodec("ULAW") ||
      isWebRTCOutputAudioCodec("AAC")) {
    return fail("WebRTC output codec filtering must match the packetizers exposed by the connector");
  }

  std::vector<WebRTCOutputTrackCandidate> candidates;
  candidates.push_back(WebRTCOutputTrackCandidate{10, "video", "JPEG"});
  candidates.push_back(WebRTCOutputTrackCandidate{11, "video", "H264"});
  candidates.push_back(WebRTCOutputTrackCandidate{12, "video", "VP8"});
  candidates.push_back(WebRTCOutputTrackCandidate{20, "audio", "AAC"});
  candidates.push_back(WebRTCOutputTrackCandidate{21, "audio", "opus"});
  candidates.push_back(WebRTCOutputTrackCandidate{22, "audio", "ALAW"});
  candidates.push_back(WebRTCOutputTrackCandidate{30, "meta", "subtitle"});
  candidates.push_back(WebRTCOutputTrackCandidate{31, "meta", "JSON"});
  candidates.push_back(WebRTCOutputTrackCandidate{32, "meta", "JSON"});

  const WebRTCOutputTrackSelection selection = selectWebRTCOutputTracks(candidates);
  if (!selection.hasVideo || selection.videoTrack != 11 || selection.videoCodec != "H264" || !selection.hasAudio ||
      selection.audioTrack != 21 || selection.audioCodec != "opus" || !selection.hasMeta || selection.metaTrack != 31 ||
      selection.metaCodec != "JSON") {
    return fail("WebRTC output must select the first supported track of each media type");
  }
  if (!webRTCOutputPacketMatchesSelection("video", 11, selection) || webRTCOutputPacketMatchesSelection("video", 12, selection) ||
      !webRTCOutputPacketMatchesSelection("audio", 21, selection) || webRTCOutputPacketMatchesSelection("audio", 22, selection) ||
      !webRTCOutputPacketMatchesSelection("meta", 32, selection)) {
    return fail("WebRTC output must not remap packets from unnegotiated A/V tracks");
  }

  std::vector<WebRTCOutputTrackCandidate> unsupported;
  unsupported.push_back(WebRTCOutputTrackCandidate{1, "video", "JPEG"});
  unsupported.push_back(WebRTCOutputTrackCandidate{2, "audio", "AAC"});
  unsupported.push_back(WebRTCOutputTrackCandidate{3, "meta", "JSON"});
  const WebRTCOutputTrackSelection metaOnly = selectWebRTCOutputTracks(unsupported);
  if (metaOnly.hasVideo || metaOnly.hasAudio || !metaOnly.hasMeta ||
      webRTCOutputHasPrimaryMedia(metaOnly.hasVideo, metaOnly.hasAudio)) {
    return fail("metadata alone must not make a WHEP playback negotiation succeed");
  }
  if (!webRTCOutputHasPrimaryMedia(true, false) || !webRTCOutputHasPrimaryMedia(false, true)) {
    return fail("either negotiated video or audio must make WHEP playback valid");
  }

  WebRTCPlayheadPosition playhead = webRTCPlayheadPosition(false, true, false, 4321, 0, 0, 0);
  if (!playhead.expose || playhead.exposeUTC || playhead.millis != 4321) {
    return fail("VOD WHEP responses must expose media time without inventing wall-clock time");
  }
  playhead = webRTCPlayheadPosition(false, true, false, 4321, 1700000000000ULL, 0, 0);
  if (!playhead.exposeUTC || playhead.unixMillis != 1700000004321ULL) {
    return fail("explicit media UTC offsets must anchor the WHEP playhead timestamp");
  }
  playhead = webRTCPlayheadPosition(false, true, true, 4321, 0, 250, 1700000000000ULL);
  if (!playhead.exposeUTC || playhead.unixMillis != 1700000004571ULL) {
    return fail("live WHEP playheads must derive wall-clock time from stream and system boot offsets");
  }
  if (webRTCPlayheadPosition(true, true, true, 1, 2, 3, 4).expose ||
      webRTCPlayheadPosition(false, false, true, 1, 2, 3, 4).expose) {
    return fail("push sessions and missing metadata must not expose playback headers");
  }

  return 0;
}
