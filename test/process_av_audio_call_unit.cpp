extern "C" {
#include <libswresample/swresample.h>
}

static int observeResampleDelay(SwrContext *context, uint8_t **output, int capacity, const uint8_t **input, int samples);
#define swr_convert observeResampleDelay
#define main avProcessorMain
#include "../src/process/process_av.cpp"
#undef main
#undef swr_convert

#include <cstdio>
#include <fstream>
#include <iterator>

static int64_t maximumResampleDelay = 0;

static int observeResampleDelay(SwrContext *context, uint8_t **output, int capacity, const uint8_t **input, int samples) {
  if (getenv("MIST_AUDIO_BACKLOG_CONTROL") && samples) { capacity = 1; }
  const int result = swr_convert(context, output, capacity, input, samples);
  maximumResampleDelay = std::max(maximumResampleDelay, swr_get_delay(context, 48000));
  return result;
}

int main(int argc, char **argv) {
  if (argc != 3) { return 2; }
  DTSC::trackValidMask = TRACK_VALID_INT_PROCESS;
  isVideo = false;
  codecIn = "EAC-3";
  codecOut = "opus";
  Util::Config config("av-audio-call-test");
  JSON::Value capa;
  Mist::ProcessSource::init(&config, capa);
  Socket::Connection connection;
  bool passed = true;
  {
    Mist::ProcessSource source(connection, config, capa);
    codec_in = avcodec_find_decoder(AV_CODEC_ID_EAC3);
    context_in = avcodec_alloc_context3(codec_in);
    if (!context_in || avcodec_open2(context_in, codec_in, 0) < 0) { return 2; }
    context_out = avcodec_alloc_context3(0);
    context_out->sample_rate = 48000;
    context_out->sample_fmt = AV_SAMPLE_FMT_FLTP;
    context_out->frame_size = 960;
    av_channel_layout_default(&context_out->ch_layout, 2);
    // Inspect available samples before EOF draining or encoding can conceal a backlog.
    conf.is_active = co.is_active = false;
    int64_t expected = 0, produced = 0;
    for (int phase = 1; phase < 3 && passed; ++phase) {
      std::ifstream file(argv[phase], std::ios::binary);
      const std::string bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
      if (bytes.empty()) { return 2; }
      size_t offset = 0;
      for (int packet = 0; packet < (phase == 1 ? 3 : 1334); ++packet) {
        if (offset + 6 > bytes.size()) { offset = 0; }
        if (offset + 6 > bytes.size() || (uint8_t)bytes[offset] != 0x0b || (uint8_t)bytes[offset + 1] != 0x77) {
          return 2;
        }
        const size_t size = ((((uint8_t)bytes[offset + 2] & 7) << 8) | (uint8_t)bytes[offset + 3]) * 2 + 2;
        if (offset + size > bytes.size()) { return 2; }
        const unsigned int code = (uint8_t)bytes[offset + 4];
        const unsigned int rateCode = code >> 6;
        if (rateCode == 3) { return 2; }
        const unsigned int rates[] = {48000, 44100, 32000}, blocks[] = {1, 2, 3, 6};
        const unsigned int rate = rates[rateCode];
        const unsigned int samples = blocks[(code >> 4) & 3] * 256;
        if ((phase == 1 && (rate != 48000 || samples != 256)) || (phase == 2 && (rate != 32000 || samples != 1536))) {
          return 2;
        }
        if (!source.decodeFrame((char *)bytes.data() + offset, size) || !source.transformAudioFrame()) { return 2; }
        if (maximumResampleDelay > 128) {
          fprintf(stderr, "swr_get_delay exceeds128 samples before EOF: %lld\n", (long long)maximumResampleDelay);
          passed = false;
          break;
        }
        if (source.receiveDecodedFrame()) { return 2; }
        expected += samples * 48000 / rate;
        const int available = av_audio_fifo_size(audioBuffer);
        produced += available;
        av_audio_fifo_drain(audioBuffer, available);
        if (expected - produced > 128 || produced > expected) {
          fprintf(stderr, "actual ProcAV resampler accumulated a backlog before EOF: phase=%d packet=%d expected=%lld produced=%lld lag=%lld samples\n",
                  phase, packet, (long long)expected, (long long)produced, (long long)(expected - produced));
          passed = false;
          break;
        }
        if (packet % 100 == 0) {
          printf("RESAMPLER phase=%d packet=%d expected=%lld produced=%lld lag=%lld samples\n", phase, packet,
                 (long long)expected, (long long)produced, (long long)(expected - produced));
        }
        offset += size;
      }
    }
  }
  av_audio_fifo_free(audioBuffer);
  audioBuffer = 0;
  avcodec_free_context(&context_in);
  avcodec_free_context(&context_out);
  if (!passed) { return 1; }
  printf("PASS direct swr_get_delay bound: maximum=%lld samples at48000Hz\n", (long long)maximumResampleDelay);
  puts("PASS actual ProcAV EAC3 decoder/resampler keeps 64 seconds of 256@48k ->1536@32k output within128 samples "
       "before EOF");
  return 0;
}
