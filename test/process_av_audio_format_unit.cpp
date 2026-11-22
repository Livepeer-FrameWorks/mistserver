#include "../src/process/process_av_audio.h"

#include <cstdio>

static bool rateAndFrameGrowth() {
  AVCodecContext *encoder = avcodec_alloc_context3(nullptr);
  AVFrame *output = nullptr;
  int capacity = 0;
  encoder->sample_rate = 48000;
  encoder->sample_fmt = AV_SAMPLE_FMT_FLT;
  encoder->frame_size = 960;
  Mist::setAudioChannelLayout(encoder, 2);
  int64_t expected = 0, produced = 0;
  for (const int rate : {44100, 8000}) {
    SwrContext *resampler = nullptr;
#if LIBAVUTIL_VERSION_MAJOR < 57 || (LIBAVUTIL_VERSION_MAJOR == 57 && LIBAVUTIL_VERSION_MINOR < 24)
    resampler = swr_alloc_set_opts(nullptr, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_FLT, 48000, AV_CH_LAYOUT_STEREO,
                                   AV_SAMPLE_FMT_FLTP, rate, 0, nullptr);
#else
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    if (swr_alloc_set_opts2(&resampler, &stereo, AV_SAMPLE_FMT_FLT, 48000, &stereo, AV_SAMPLE_FMT_FLTP, rate, 0, nullptr) < 0) {
      return false;
    }
#endif
    if (!resampler || swr_init(resampler) < 0) { return false; }
    float samples[2][1152] = {};
    const uint8_t *input[] = {reinterpret_cast<const uint8_t *>(samples[0]), reinterpret_cast<const uint8_t *>(samples[1])};
    const int count = rate == 44100 ? 1024 : 1152;
    for (int i = 0; i < 1000; ++i) {
      if (Mist::prepareAudioResampleFrame(output, capacity, encoder, resampler, count) < 0) { return false; }
      const int converted = swr_convert(resampler, output->extended_data, capacity, input, count);
      if (converted < 0 || swr_get_delay(resampler, 48000) > 200) {
        fprintf(stderr, "resampler accumulated input after rate change\n");
        return false;
      }
      produced += converted;
      // The encoder owns the submitted frame until a later call; resizing must preserve it.
      AVFrame *retained = av_frame_clone(output);
      if (!retained) { return false; }
      output->nb_samples = 960;
      if (Mist::prepareAudioResampleFrame(output, capacity, encoder, resampler, 0) < 0) { return false; }
      av_frame_free(&retained);
    }
    expected += av_rescale_rnd(int64_t(count) * 1000, 48000, rate, AV_ROUND_NEAR_INF);
    for (;;) {
      if (Mist::prepareAudioResampleFrame(output, capacity, encoder, resampler, 0) < 0) { return false; }
      const int converted = swr_convert(resampler, output->extended_data, capacity, nullptr, 0);
      if (converted < 0) { return false; }
      if (!converted) { break; }
      produced += converted;
    }
    swr_free(&resampler);
  }
  av_frame_free(&output);
  avcodec_free_context(&encoder);
  if (std::abs(produced - expected) > 1) {
    fprintf(stderr, "resampled duration differs: %lld != %lld\n", (long long)produced, (long long)expected);
    return false;
  }
  return true;
}

int main() {
  if (!rateAndFrameGrowth()) { return 1; }
  Mist::AudioInputFormat remembered;
  AVFrame *frame = av_frame_alloc();
  if (!frame) { return 2; }
  frame->sample_rate = 48000;
  frame->format = AV_SAMPLE_FMT_FLTP;
#if LIBAVUTIL_VERSION_MAJOR < 57 || (LIBAVUTIL_VERSION_MAJOR == 57 && LIBAVUTIL_VERSION_MINOR < 24)
  frame->channels = 6;
  frame->channel_layout = AV_CH_LAYOUT_5POINT1;
#else
  av_channel_layout_from_mask(&frame->ch_layout, AV_CH_LAYOUT_5POINT1);
#endif
  if (!remembered.remember(frame) || !remembered.matches(frame)) { return 1; }
  frame->sample_rate = 44100;
  if (remembered.matches(frame)) {
    fprintf(stderr, "sample-rate change reused the old audio resampler\n");
    return 1;
  }
  if (!remembered.remember(frame) || !remembered.matches(frame)) { return 1; }
#if LIBAVUTIL_VERSION_MAJOR < 57 || (LIBAVUTIL_VERSION_MAJOR == 57 && LIBAVUTIL_VERSION_MINOR < 24)
  frame->channel_layout = AV_CH_LAYOUT_5POINT1_BACK;
#else
  av_channel_layout_uninit(&frame->ch_layout);
  av_channel_layout_from_mask(&frame->ch_layout, AV_CH_LAYOUT_5POINT1_BACK);
#endif
  if (remembered.matches(frame)) {
    fprintf(stderr, "same-channel-count speaker-layout change reused the old audio resampler\n");
    return 1;
  }
  if (!remembered.remember(frame) || !remembered.matches(frame)) { return 1; }
  frame->format = AV_SAMPLE_FMT_S16;
  if (remembered.matches(frame)) {
    fprintf(stderr, "sample-format change reused the old audio resampler\n");
    return 1;
  }
  if (!remembered.remember(frame) || !remembered.matches(frame)) { return 1; }
  av_frame_free(&frame);
  printf("PASS resampler detects rate, equal-count speaker layout and sample format changes\n");
}
