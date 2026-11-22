#include "../lib/av_codec_map.h"
#include "../src/process/process_av_audio.h"

#include <iostream>

int main() {
  for (const std::string & codec : {"H264", "HEVC", "AV1", "VP8", "VP9", "MPEG2", "MPEG4", "JPEG"}) {
    if (AVCodecMap::codecID(codec) == AV_CODEC_ID_NONE) {
      std::cerr << codec << " has no decoder mapping\n";
      return 1;
    }
  }
  for (const std::string & codec : {"PCM", "opus", "AAC", "AC3", "EAC-3", "MP3", "MP2", "FLAC", "vorbis"}) {
    if (AVCodecMap::codecID(codec) == AV_CODEC_ID_NONE) {
      std::cerr << codec << " has no decoder mapping\n";
      return 1;
    }
  }
  if (AVCodecMap::codecID("unknown") != AV_CODEC_ID_NONE || AVCodecMap::codecID("hevc") != AV_CODEC_ID_HEVC ||
      AVCodecMap::codecID("MPEG4") != AV_CODEC_ID_MPEG4 || AVCodecMap::codecID("EAC-3") != AV_CODEC_ID_EAC3 ||
      AVCodecMap::codecID("PCM", 32) != AV_CODEC_ID_PCM_S32BE) {
    return 1;
  }
  for (unsigned int channels = 1; channels <= 8; ++channels) {
    if (Mist::encodedAudioChannels("AAC", channels) != (channels == 7 ? 8u : channels) ||
        Mist::encodedAudioChannels("opus", channels) != channels) {
      std::cerr << "Opus changed " << channels << " source channels to " << Mist::encodedAudioChannels("opus", channels)
                << std::endl;
      return 1;
    }
    AVCodecContext *context = avcodec_alloc_context3(NULL);
    Mist::setAudioChannelLayout(context, channels);
#if LIBAVUTIL_VERSION_MAJOR < 57 || (LIBAVUTIL_VERSION_MAJOR == 57 && LIBAVUTIL_VERSION_MINOR < 24)
    const bool valid = context->channels == (int)channels && context->channel_layout;
#else
    const bool valid = context->ch_layout.nb_channels == (int)channels && av_channel_layout_check(&context->ch_layout);
#endif
    avcodec_free_context(&context);
    if (!valid) { return 1; }
  }
  for (unsigned int channels : {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u, 16u}) {
    const AVCodec *encoder = avcodec_find_encoder_by_name("libopus");
    if (!encoder) { return 1; }
    AVCodecContext *context = avcodec_alloc_context3(encoder);
    AVDictionary *options = 0;
    Mist::setAudioChannelLayout(context, Mist::encodedAudioChannels("opus", channels), "opus", &options);
    context->sample_rate = 48000;
    context->sample_fmt = AV_SAMPLE_FMT_S16;
    context->bit_rate = 128000;
    const int result = avcodec_open2(context, encoder, &options);
    const bool correct = result >= 0 && context->extradata_size > 18 && context->extradata[9] == channels &&
      (context->extradata[18] ==
       (channels <= 2     ? 0
          : channels <= 8 ? 1
                          : 255));
    if (correct) {
      std::cout << "PASS Opus channels=" << channels << " mapping_family=" << (unsigned int)context->extradata[18] << "\n";
    }
    av_dict_free(&options);
    avcodec_free_context(&context);
    if (!correct) {
      std::cerr << "Opus encoder lost " << channels << " channels or mapping family" << std::endl;
      return 1;
    }
  }
  if (Mist::limitedAudioChannels("opus", 6, true, 2) != 2 || Mist::limitedAudioChannels("opus", 1, false, 2) != 1 ||
      Mist::limitedAudioChannels("AAC", 6, true, 0) != 6) {
    std::cerr << "Platform audio limit lost stereo Opus, mono preservation or AAC surround" << std::endl;
    return 1;
  }
  if (Mist::encodedAudioChannels("AAC", 3, true) != 6 || Mist::encodedAudioChannels("AAC", 6, true) != 6) { return 1; }
  for (const unsigned int channels : {3u, 7u}) {
    const AVCodec *encoder = avcodec_find_encoder(AV_CODEC_ID_AAC);
    AVCodecContext *context = avcodec_alloc_context3(encoder);
    Mist::setAudioChannelLayout(context, Mist::encodedAudioChannels("AAC", channels), "AAC");
    context->sample_rate = 48000;
    context->sample_fmt = AV_SAMPLE_FMT_FLTP;
    context->bit_rate = 192000;
    if (avcodec_open2(context, encoder, NULL) < 0) { return 1; }
    const unsigned int configuration = context->extradata_size >= 2 ? (context->extradata[1] >> 3) & 15 : 0;
    avcodec_free_context(&context);
    if (!configuration) {
      std::cerr << "AAC output requires an unavailable ADTS program config for " << channels << " channels\n";
      return 1;
    }
  }
  return 0;
}
