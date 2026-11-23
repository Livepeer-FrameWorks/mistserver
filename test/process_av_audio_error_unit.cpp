extern "C" {
#include <libavutil/audio_fifo.h>
#include <libavutil/error.h>
}
static int reportedError = 0;
static int rejectedAudioWrite(AVAudioFifo *, void **, int) {
  return AVERROR(ENOSPC);
}
static int recordAudioError(int code, char *buffer, size_t size) {
  reportedError = code;
  return av_strerror(code, buffer, size);
}
#define av_audio_fifo_write rejectedAudioWrite
#define av_strerror recordAudioError
#define main avProcessorMain
#include "../src/process/process_av.cpp"
#undef main
#undef av_strerror
#undef av_audio_fifo_write

int main() {
  isVideo = false;
  codecIn = "PCM";
  codecOut = "opus";
  Util::Config config("audio-fifo-error-test");
  JSON::Value capability;
  Mist::ProcessSource::init(&config, capability);
  Socket::Connection connection;
  bool passed = false;
  {
    Mist::ProcessSource source(connection, config, capability);
    context_out = avcodec_alloc_context3(0);
    context_out->sample_rate = 48000;
    context_out->sample_fmt = AV_SAMPLE_FMT_FLTP;
    context_out->frame_size = 960;
    Mist::setAudioChannelLayout(context_out, 2);
    codec_in = avcodec_find_decoder(AV_CODEC_ID_PCM_S16BE);
    context_in = avcodec_alloc_context3(codec_in);
    context_in->sample_rate = 48000;
    Mist::setAudioChannelLayout(context_in, 2);
    if (avcodec_open2(context_in, codec_in, 0) < 0) { return 2; }
    int16_t samples[2048] = {};
    if (!source.decodeFrame(reinterpret_cast<char *>(samples), sizeof(samples))) { return 2; }
    const bool converted = source.transformAudioFrame();
    passed = !converted && reportedError == AVERROR(ENOSPC);
    if (!passed) {
      fprintf(stderr, "FIFO write failure must report its actual error: converted=%d reported=%d expected=%d\n",
              converted, reportedError, AVERROR(ENOSPC));
    }
  }
  av_audio_fifo_free(audioBuffer);
  audioBuffer = 0;
  avcodec_free_context(&context_out);
  avcodec_free_context(&context_in);
  return passed ? 0 : 1;
}
