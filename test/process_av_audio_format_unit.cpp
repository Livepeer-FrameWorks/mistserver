#include "../src/process/process_av_audio.h"

#include <cstdio>

int main() {
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
