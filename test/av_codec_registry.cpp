#include <iostream>

extern "C" {
#include <libavcodec/avcodec.h>
}

struct RequiredCodec {
    const char *name;
    AVCodecID id;
};

static bool requireEncoder(const RequiredCodec & codec) {
  const AVCodec *encoder = avcodec_find_encoder(codec.id);
  if (encoder) { return true; }
  std::cerr << "Missing FFmpeg encoder: " << codec.name << std::endl;
  return false;
}

static bool requireDecoder(const RequiredCodec & codec) {
  const AVCodec *decoder = avcodec_find_decoder(codec.id);
  if (decoder) { return true; }
  std::cerr << "Missing FFmpeg decoder: " << codec.name << std::endl;
  return false;
}

int main() {
  static const RequiredCodec requiredEncoders[] = {
    {"H264", AV_CODEC_ID_H264},
    {"HEVC", AV_CODEC_ID_HEVC},
    {"AV1", AV_CODEC_ID_AV1},
    {"VP8", AV_CODEC_ID_VP8},
    {"VP9", AV_CODEC_ID_VP9},
    {"MJPEG", AV_CODEC_ID_MJPEG},
    {"AAC", AV_CODEC_ID_AAC},
    {"Opus", AV_CODEC_ID_OPUS},
    {"MP3", AV_CODEC_ID_MP3},
    {"FLAC", AV_CODEC_ID_FLAC},
    {"Vorbis", AV_CODEC_ID_VORBIS},
    {"PCM S16BE", AV_CODEC_ID_PCM_S16BE},
    {"PCM S32BE", AV_CODEC_ID_PCM_S32BE},
    {"rawvideo", AV_CODEC_ID_RAWVIDEO},
  };

  static const RequiredCodec requiredDecoders[] = {
    {"H264", AV_CODEC_ID_H264},
    {"HEVC", AV_CODEC_ID_HEVC},
    {"AV1", AV_CODEC_ID_AV1},
    {"VP8", AV_CODEC_ID_VP8},
    {"VP9", AV_CODEC_ID_VP9},
    {"MJPEG", AV_CODEC_ID_MJPEG},
    {"AAC", AV_CODEC_ID_AAC},
    {"Opus", AV_CODEC_ID_OPUS},
    {"MP3", AV_CODEC_ID_MP3},
    {"FLAC", AV_CODEC_ID_FLAC},
    {"Vorbis", AV_CODEC_ID_VORBIS},
    {"PCM S16BE", AV_CODEC_ID_PCM_S16BE},
    {"PCM S32BE", AV_CODEC_ID_PCM_S32BE},
    {"rawvideo", AV_CODEC_ID_RAWVIDEO},
  };

  bool ok = true;
  for (const RequiredCodec & codec : requiredEncoders) { ok &= requireEncoder(codec); }
  for (const RequiredCodec & codec : requiredDecoders) { ok &= requireDecoder(codec); }
  return ok ? 0 : 1;
}
