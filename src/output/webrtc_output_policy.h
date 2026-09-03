#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace Mist {
  struct WebRTCOutputTrackCandidate {
      size_t id;
      std::string type;
      std::string codec;
  };

  struct WebRTCOutputTrackSelection {
      WebRTCOutputTrackSelection()
        : hasVideo(false), hasAudio(false), hasMeta(false), videoTrack(0), audioTrack(0), metaTrack(0) {}

      bool hasVideo;
      bool hasAudio;
      bool hasMeta;
      size_t videoTrack;
      size_t audioTrack;
      size_t metaTrack;
      std::string videoCodec;
      std::string audioCodec;
      std::string metaCodec;
  };

  inline bool isWebRTCOutputVideoCodec(const std::string & codec) {
    return codec == "H264" || codec == "VP8" || codec == "VP9" || codec == "AV1" || codec == "HEVC";
  }

  inline bool isWebRTCOutputAudioCodec(const std::string & codec) {
    return codec == "opus" || codec == "ALAW" || codec == "ULAW";
  }

  inline WebRTCOutputTrackSelection selectWebRTCOutputTracks(const std::vector<WebRTCOutputTrackCandidate> & candidates) {
    WebRTCOutputTrackSelection result;
    for (std::vector<WebRTCOutputTrackCandidate>::const_iterator it = candidates.begin(); it != candidates.end(); ++it) {
      if (it->type == "video" && !result.hasVideo && isWebRTCOutputVideoCodec(it->codec)) {
        result.hasVideo = true;
        result.videoTrack = it->id;
        result.videoCodec = it->codec;
      } else if (it->type == "audio" && !result.hasAudio && isWebRTCOutputAudioCodec(it->codec)) {
        result.hasAudio = true;
        result.audioTrack = it->id;
        result.audioCodec = it->codec;
      } else if (it->type == "meta" && !result.hasMeta && it->codec == "JSON") {
        result.hasMeta = true;
        result.metaTrack = it->id;
        result.metaCodec = it->codec;
      }
    }
    return result;
  }

  inline bool webRTCOutputPacketMatchesSelection(const std::string & type, size_t track, const WebRTCOutputTrackSelection & selection) {
    if (type == "video") { return selection.hasVideo && track == selection.videoTrack; }
    if (type == "audio") { return selection.hasAudio && track == selection.audioTrack; }
    return true;
  }

  inline bool webRTCOutputHasPrimaryMedia(bool videoEnabled, bool audioEnabled) {
    return videoEnabled || audioEnabled;
  }

  struct WebRTCPlayheadPosition {
      WebRTCPlayheadPosition() : expose(false), exposeUTC(false), millis(0), unixMillis(0) {}

      bool expose;
      bool exposeUTC;
      uint64_t millis;
      uint64_t unixMillis;
  };

  inline WebRTCPlayheadPosition webRTCPlayheadPosition(bool pushing, bool hasMetadata, bool live, uint64_t currentMillis,
                                                       uint64_t utcOffset, uint64_t bootMillisOffset, uint64_t systemBootMillis) {
    WebRTCPlayheadPosition result;
    if (pushing || !hasMetadata) { return result; }
    result.expose = true;
    result.millis = currentMillis;
    if (live || utcOffset) {
      result.exposeUTC = true;
      result.unixMillis = utcOffset ? utcOffset + currentMillis : bootMillisOffset + systemBootMillis + currentMillis;
    }
    return result;
  }
} // namespace Mist
