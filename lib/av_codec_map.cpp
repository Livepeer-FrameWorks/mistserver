#include "av_codec_map.h"

#include <algorithm>
#include <cctype>

namespace {
  struct Codec {
      const char *name;
      AVCodecID id;
  };
  const Codec codecs[] = {
    {"H264", AV_CODEC_ID_H264},     {"HEVC", AV_CODEC_ID_HEVC},     {"AV1", AV_CODEC_ID_AV1},
    {"VP8", AV_CODEC_ID_VP8},       {"VP9", AV_CODEC_ID_VP9},       {"MPEG2", AV_CODEC_ID_MPEG2VIDEO},
    {"JPEG", AV_CODEC_ID_MJPEG},    {"AAC", AV_CODEC_ID_AAC},
    {"opus", AV_CODEC_ID_OPUS},     {"AC3", AV_CODEC_ID_AC3},       {"EAC-3", AV_CODEC_ID_EAC3},
    {"MP3", AV_CODEC_ID_MP3},       {"MP2", AV_CODEC_ID_MP2},       {"FLAC", AV_CODEC_ID_FLAC},
    {"vorbis", AV_CODEC_ID_VORBIS}, {"theora", AV_CODEC_ID_THEORA},
  };

  std::string upper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::toupper(c); });
    return value;
  }
} // namespace

namespace AVCodecMap {
  AVCodecID codecID(const std::string & codec, unsigned int pcmDepth) {
    const std::string normalized = upper(codec);
    if (normalized == "PCM") {
      if (pcmDepth == 16) { return AV_CODEC_ID_PCM_S16BE; }
      if (pcmDepth == 32) { return AV_CODEC_ID_PCM_S32BE; }
      return AV_CODEC_ID_NONE;
    }
    for (const Codec & entry : codecs) {
      if (normalized == upper(entry.name)) { return entry.id; }
    }
    return AV_CODEC_ID_NONE;
  }

  const char *mistCodec(AVCodecID codec) {
    if (codec == AV_CODEC_ID_MPEG1VIDEO) { return "MPEG2"; }
    for (const Codec & entry : codecs) {
      if (codec == entry.id) { return entry.name; }
    }
    return "";
  }

} // namespace AVCodecMap
