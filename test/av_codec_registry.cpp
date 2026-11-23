#include <iostream>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
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

int main(int argc, char **argv) {
  if (argc > 1 && std::string(argv[1]) == "hardware-linux") {
    bool ok = true;
    for (const char *name : {"h264_qsv", "hevc_qsv", "av1_qsv", "vp9_qsv", "mpeg2_qsv", "h264_cuvid", "hevc_cuvid", "av1_cuvid"}) {
      if (!avcodec_find_decoder_by_name(name)) {
        std::cerr << "Missing packaged hardware decoder: " << name << std::endl;
        ok = false;
      }
    }
    for (const char *name : {"h264_qsv", "av1_qsv", "mjpeg_qsv", "h264_nvenc", "av1_nvenc", "h264_vaapi", "mjpeg_vaapi"}) {
      if (!avcodec_find_encoder_by_name(name)) {
        std::cerr << "Missing packaged hardware encoder: " << name << std::endl;
        ok = false;
      }
    }
    for (const char *name : {"vpp_qsv", "scale_qsv", "scale_cuda", "scale_vaapi", "hwdownload", "hwupload"}) {
      if (!avfilter_get_by_name(name)) {
        std::cerr << "Missing packaged hardware filter: " << name << std::endl;
        ok = false;
      }
    }
    return ok ? 0 : 1;
  }
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
    {"MPEG1", AV_CODEC_ID_MPEG1VIDEO},
    {"MPEG2", AV_CODEC_ID_MPEG2VIDEO},
    {"MPEG4", AV_CODEC_ID_MPEG4},
    {"AC3", AV_CODEC_ID_AC3},
    {"EAC3", AV_CODEC_ID_EAC3},
    {"MP2", AV_CODEC_ID_MP2},
    {"Theora", AV_CODEC_ID_THEORA},
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
