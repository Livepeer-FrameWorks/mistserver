#define av_hwdevice_ctx_create testCreateDevice
#define avcodec_open2 testOpenCodec
#define av_hwframe_ctx_alloc testAllocPool
#define av_hwframe_ctx_init testInitPool
#define av_hwframe_get_buffer testGetHardwareFrame
#define av_hwframe_transfer_data testTransferFrame
#define avcodec_send_frame testSendFrame
#define avcodec_receive_packet testReceivePacket
#define main thumbsProcessorMain
#include "../src/process/process_thumbs.cpp"
#undef main
#include <cstdio>

static int liveDevices = 0, largeHardware = 0, largeSoftware = 0;
static void freeDevice(void *, uint8_t *data) {
  --liveDevices;
  av_free(data);
}
extern "C" int testCreateDevice(AVBufferRef **device, AVHWDeviceType type, const char *, AVDictionary *, int) {
  uint8_t *bytes = (uint8_t *)av_mallocz(sizeof(AVHWDeviceContext));
  if (!bytes) { return AVERROR(ENOMEM); }
  ((AVHWDeviceContext *)bytes)->type = type;
  *device = av_buffer_create(bytes, sizeof(AVHWDeviceContext), freeDevice, 0, 0);
  if (!*device) {
    av_free(bytes);
    return AVERROR(ENOMEM);
  }
  ++liveDevices;
  return 0;
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
  return 0;
}
extern "C" int testTransferFrame(AVFrame *, const AVFrame *, int) {
  return 0;
}
extern "C" int testOpenCodec(AVCodecContext *context, const AVCodec *, AVDictionary **) {
  if (context->hw_device_ctx) {
    if (context->width == 64) { return AVERROR(EINVAL); }
    ++largeHardware;
  } else if (context->width == 128) {
    ++largeSoftware;
  }
  return 0;
}
extern "C" int testSendFrame(AVCodecContext *, const AVFrame *) {
  return 0;
}
extern "C" int testReceivePacket(AVCodecContext *, AVPacket *packet) {
  const int result = av_new_packet(packet, 4);
  if (result >= 0) { memcpy(packet->data, "jpeg", 4); }
  return result;
}
int main() {
  const auto candidates = Mist::videoCodecCandidates(AV_CODEC_ID_MJPEG, true, true, true);
  std::vector<Mist::HardwareCodecCandidate> selected;
  for (const auto & candidate : candidates) {
    if (candidate.hardware && candidate.device != AV_HWDEVICE_TYPE_NONE) {
      selected.push_back(candidate);
      break;
    }
  }
  const bool offeredHardware = !selected.empty();
  for (const auto & candidate : candidates) {
    if (!candidate.hardware) {
      selected.push_back(candidate);
      break;
    }
  }
  if (selected.empty()) { return 2; }
  Util::Config config("thumbnail-hardware-selection-test");
  Mist::opt["source"] = "thumbnail-selection";
  Mist::opt["accel"] = "auto";
  {
    Mist::ProcessSink sink(&config);
    sink.jpegCandidates = selected;
    std::string encoded;
    if (!sink.encodeJpeg(std::vector<uint8_t>(64 * 64 * 3), 64, 64, encoded) ||
        !sink.encodeJpeg(std::vector<uint8_t>(128 * 64 * 3), 128, 64, encoded)) {
      return 1;
    }
  }
  if (liveDevices || (offeredHardware ? largeHardware != 1 || largeSoftware : largeSoftware != 1)) {
    fprintf(stderr, "thumbnail fallback for one geometry disabled hardware for another: hardware=%d software=%d live-devices=%d\n",
            largeHardware, largeSoftware, liveDevices);
    return 1;
  }
  puts("PASS actual thumbnail encode releases failed contexts and selects hardware independently per image geometry");
}
