// Exercise ProcAV's actual configuration path; device/open boundaries are deterministic here.
#define av_hwdevice_ctx_create testCreateDevice
#define avcodec_open2 testOpenCodec
#define av_hwframe_ctx_alloc testAllocPool
#define av_hwframe_ctx_init testInitPool
#define av_hwframe_get_buffer testGetHardwareFrame
#define av_hwframe_transfer_data testTransferFrame
#define avcodec_send_frame testSendFrame
#define main avProcessorMain
#include "../src/process/process_av.cpp"
#undef main
#undef av_hwdevice_ctx_create
#undef avcodec_open2
#undef av_hwframe_ctx_alloc
#undef av_hwframe_ctx_init
#undef av_hwframe_get_buffer
#undef av_hwframe_transfer_data
#undef avcodec_send_frame

extern "C" int avcodec_send_frame(AVCodecContext *, const AVFrame *);
extern "C" int avcodec_open2(AVCodecContext *, const AVCodec *, AVDictionary **);
#include <cstdio>
#include <vector>

namespace {
  std::vector<AVHWDeviceType> attempts;
  bool acceptDevice = false, rejectOpen = false, softwareOpened = false, invalidPool = false;
  int devices = 0, released = 0, hardwareSends = 0, softwareSends = 0;
  void freeDevice(void *, uint8_t *data) {
    ++released;
    av_free(data);
  }
  class SelectionProbe : public Mist::ProcessSource {
    public:
      using Mist::ProcessSource::ProcessSource;
      void track(const std::string & codec) {
        meta.reInit("", true);
        thisIdx = meta.addTrack();
        meta.setID(thisIdx, 1);
        meta.setType(thisIdx, "video");
        meta.setCodec(thisIdx, codec);
        meta.setWidth(thisIdx, 64);
        meta.setHeight(thisIdx, 64);
        meta.setFpks(thisIdx, 30000);
        meta.update(0, 0, thisIdx, 4, 0, true, 33);
        userSelect[thisIdx];
      }
  };
} // namespace
extern "C" int testCreateDevice(AVBufferRef **device, AVHWDeviceType type, const char *, AVDictionary *, int) {
  attempts.push_back(type);
  if (softwareOpened) { return AVERROR(EINVAL); }
  if (!acceptDevice) { return AVERROR(ENODEV); }
  uint8_t *bytes = (uint8_t *)av_mallocz(sizeof(AVHWDeviceContext));
  if (!bytes) { return AVERROR(ENOMEM); }
  ((AVHWDeviceContext *)bytes)->type = type;
  *device = av_buffer_create(bytes, sizeof(AVHWDeviceContext), freeDevice, 0, 0);
  if (!*device) {
    av_free(bytes);
    return AVERROR(ENOMEM);
  }
  ++devices;
  return 0;
}
extern "C" int testOpenCodec(AVCodecContext *ctx, const AVCodec *codec, AVDictionary **options) {
  if (ctx->hw_device_ctx) {
    invalidPool = invalidPool || (av_codec_is_decoder(codec) && ctx->hw_frames_ctx);
    return rejectOpen ? AVERROR(EIO) : 0;
  }
  softwareOpened = true;
  return avcodec_open2(ctx, codec, options);
}

static void freePool(void *, uint8_t *data) {
  av_buffer_unref(&((AVHWFramesContext *)data)->device_ref);
  av_free(data);
}
extern "C" AVBufferRef *testAllocPool(AVBufferRef *device) {
  uint8_t *bytes = (uint8_t *)av_mallocz(sizeof(AVHWFramesContext));
  if (!bytes) { return 0; }
  ((AVHWFramesContext *)bytes)->device_ref = av_buffer_ref(device);
  return av_buffer_create(bytes, sizeof(AVHWFramesContext), freePool, 0, 0);
}
extern "C" int testInitPool(AVBufferRef *) {
  return 0;
}
extern "C" int testGetHardwareFrame(AVBufferRef *pool, AVFrame *frame, int) {
  AVHWFramesContext *context = (AVHWFramesContext *)pool->data;
  frame->format = context->format;
  frame->width = context->width;
  frame->height = context->height;
  frame->hw_frames_ctx = av_buffer_ref(pool);
  return frame->hw_frames_ctx ? 0 : AVERROR(ENOMEM);
}
extern "C" int testTransferFrame(AVFrame *, const AVFrame *, int) {
  return 0;
}
extern "C" int testSendFrame(AVCodecContext *context, const AVFrame *frame) {
  if (context->hw_device_ctx) {
    if (!frame) { return AVERROR_EOF; }
    ++hardwareSends;
    acceptDevice = false;
    return AVERROR(EIO);
  }
  ++softwareSends;
  return 0;
}

int checkEncoderSelection(bool runtime = false) {
  packet_out = av_packet_alloc();
  allowHW = allowSW = true;
  isVideo = true;
  acceptDevice = false;
  Mist::opt["bitrate"] = 200000;
  Mist::opt["gopsize"] = 30;
  Mist::opt["preset"] = "veryfast";
  Mist::opt["tune"] = "zerolatency";
  Mist::opt["quality"] = 20;
  Mist::opt["encode_threads"] = 1;
  for (const char *name : {"H264", "AV1", "JPEG"}) {
    bool offered = false;
    void *iterator = 0;
    while (const AVCodec *codec = av_codec_iterate(&iterator)) {
      if (!av_codec_is_encoder(codec) || codec->id != AVCodecMap::codecID(name)) { continue; }
      offered = offered || avcodec_get_hw_config(codec, 0);
    }
    if (!offered) { continue; }
    attempts.clear();
    softwareOpened = false;
    acceptDevice = runtime;
    hardwareSends = softwareSends = 0;
    Util::Config config("hardware-encoder-selection-test");
    JSON::Value capa;
    Mist::ProcessSource::init(&config, capa);
    Socket::Connection connection;
    bool active = true;
    AVFrame *input = 0;
    {
      SelectionProbe source(connection, config, capa);
      source.track("H264");
      config.is_active = true;
      codecOut = name;
      source.allocateVideoEncoder();
      if (runtime && context_out) {
        input = av_frame_alloc();
        input->format = AV_PIX_FMT_NV12;
        input->width = input->height = 64;
        if (av_frame_get_buffer(input, 32) < 0) { return 2; }
        frameConverted = input;
        conf.is_active = co.is_active = true;
        source.encodeVideo();
        active = config.is_active;
      }
    }
    av_frame_free(&input);
    frameConverted = 0;
    frameTimes.clear();
    frameReady = false;
    avcodec_free_context(&context_out);
    av_frame_free(&frameInHW);
    codec_out = 0;
    if (attempts.empty() || !softwareOpened || !active || (runtime && (hardwareSends != 1 || softwareSends != 1)) ||
        devices != released) {
      fprintf(stderr, "encoder %s did not try offered hardware before software: attempts=%zu software=%d hw-send=%d sw-send=%d active=%d refs=%d\n",
              name, attempts.size(), softwareOpened, hardwareSends, softwareSends, active, devices - released);
      return 1;
    }
  }
  av_packet_free(&packet_out);
  puts("PASS actual ProcAV tries offered encoders, including JPEG, before software fallback");
  return 0;
}

int main(int argc, char **argv) {
  const std::string mode = argc > 1 ? argv[1] : "auto";
  if (mode == "encoder" || mode == "encoder-failure") { return checkEncoderSelection(mode == "encoder-failure"); }
  acceptDevice = mode != "auto";
  rejectOpen = mode == "failed-open";
  allowHW = true;
  allowSW = mode != "hw";
  isVideo = true;
  size_t tested = 0;
  for (const char *codec : {"H264", "HEVC", "AV1", "VP9", "MPEG2", "JPEG"}) {
    bool offered = false;
    void *cursor = 0;
    while (const AVCodec *candidate = av_codec_iterate(&cursor)) {
      if (!av_codec_is_decoder(candidate) || candidate->id != AVCodecMap::codecID(codec)) { continue; }
      for (int i = 0; const AVCodecHWConfig *hw = avcodec_get_hw_config(candidate, i); ++i) {
        offered = offered || (hw->device_type != AV_HWDEVICE_TYPE_NONE && (hw->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX));
      }
    }
    if (!offered) { continue; }
    ++tested;
    attempts.clear();
    softwareOpened = false;
    rejectOpen = mode == "failed-open";
    invalidPool = false;
    Util::Config config("hardware-selection-test");
    JSON::Value capa;
    Mist::ProcessSource::init(&config, capa);
    Socket::Connection connection;
    bool opened;
    {
      SelectionProbe source(connection, config, capa);
      source.track(codec);
      config.is_active = true;
      opened = source.configVideoDecoder();
      if (mode == "runtime-failure") {
        rejectOpen = true;
        source.recoverVideoDecoder();
        opened = opened && config.is_active && context_in;
      }
    }
    avcodec_free_context(&context_in);
    if (!opened || attempts.empty() || invalidPool || (mode == "hw" && softwareOpened) ||
        (mode != "hw" && !softwareOpened) || devices != released) {
      fprintf(stderr, "hardware selection mode=%s codec=%s: opened=%d attempts=%zu software=%d precreated-pool=%d live-device-refs=%d\n",
              mode.c_str(), codec, opened, attempts.size(), softwareOpened, invalidPool, devices - released);
      return 1;
    }
  }
  if (!tested) { return 77; }
  puts("PASS actual ProcAV tries offered hardware before software; hw mode opens; failed candidates release devices "
       "and leave decoder pools owned by libav");
}
