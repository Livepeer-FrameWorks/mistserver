#define main avProcessorMain
#include "../src/process/process_av.cpp"
#undef main
extern "C" {
#include <libavformat/avformat.h>
}
#include <cstdio>
#include <dirent.h>
#include <fstream>

class DecoderProbe : public Mist::ProcessSource {
  public:
    using Mist::ProcessSource::ProcessSource;
    void track(const AVCodecParameters *parameters) {
      meta.reInit("", true);
      thisIdx = meta.addTrack();
      meta.setID(thisIdx, 1);
      meta.setType(thisIdx, "video");
      codecIn = AVCodecMap::mistCodec(parameters->codec_id);
      meta.setCodec(thisIdx, codecIn);
      meta.setWidth(thisIdx, parameters->width);
      meta.setHeight(thisIdx, parameters->height);
      meta.setFpks(thisIdx, 10000);
      if (parameters->extradata_size) {
        meta.setInit(thisIdx, (char *)parameters->extradata, parameters->extradata_size);
      }
      meta.update(0, 0, thisIdx, 4, 0, true, 100);
      userSelect[thisIdx];
    }
    bool feed(AVPacket *packet, AVRational timebase) {
      const int64_t dts = av_rescale_q(packet->dts, timebase, AVRational{1, 1000}) + 1000;
      const int64_t pts = av_rescale_q(packet->pts, timebase, AVRational{1, 1000}) + 1000;
      thisTime = dts;
      thisPacket.genericFill(dts, pts - dts, 1, (char *)packet->data, packet->size, 0, packet->flags & AV_PKT_FLAG_KEY);
      return decodeVideoFrame((char *)packet->data, packet->size);
    }
};
static long residentAnonymous() {
  std::ifstream status("/proc/self/status");
  std::string key;
  while (status >> key) {
    if (key == "RssAnon:") {
      long value;
      status >> value;
      return value;
    }
    std::getline(status, key);
  }
  return -1;
}
static long gpuMemory(bool cuda) {
  if (cuda) {
    FILE *query = popen("nvidia-smi --query-compute-apps=pid,used_gpu_memory --format=csv,noheader,nounits 2>/dev/null", "r");
    if (!query) { return -1; }
    long total = 0, pid = 0, memory = 0;
    char line[512];
    while (fgets(line, sizeof(line), query)) {
      if (sscanf(line, "%ld, %ld", &pid, &memory) == 2 && pid == getpid()) { total += memory * 1024; }
    }
    return pclose(query) ? -1 : total;
  }
  DIR *directory = opendir("/proc/self/fdinfo");
  if (!directory) { return -1; }
  std::map<std::string, long> clients;
  while (dirent *entry = readdir(directory)) {
    std::ifstream info(std::string("/proc/self/fdinfo/") + entry->d_name);
    std::string key, client;
    long resident = 0;
    while (info >> key) {
      if (key == "drm-client-id:") {
        info >> client;
      } else if (key.find("drm-resident-") == 0) {
        long value;
        info >> value;
        resident += value;
      }
      std::getline(info, key);
    }
    if (!client.empty()) { clients[client] = resident; }
  }
  closedir(directory);
  long total = 0;
  for (const auto & client : clients) { total += client.second; }
  return total;
}
int main(int argc, char **argv) {
  if (argc != 3) { return 2; }
  const AVHWDeviceType device = av_hwdevice_find_type_by_name(argv[1]);
  AVFormatContext *input = 0;
  if (avformat_open_input(&input, argv[2], 0, 0) < 0 || avformat_find_stream_info(input, 0) < 0) { return 2; }
  const int track = av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1, 0, 0);
  if (track < 0) { return 2; }
  AVCodecParameters *parameters = input->streams[track]->codecpar;
  const AVCodecID id = parameters->codec_id;
  Util::Config config("hardware-decoder-lifecycle-test");
  JSON::Value capa;
  isVideo = true;
  codecOut = "NV12";
  Mist::ProcessSource::init(&config, capa);
  Socket::Connection connection;
  DecoderProbe source(connection, config, capa);
  source.track(parameters);
  avformat_close_input(&input);
  AVPacket *packet = av_packet_alloc();
  bool tested = false;
  for (const auto & candidate : Mist::videoCodecCandidates(id, false, true, false)) {
    if (candidate.device != device || !candidate.selector.empty()) { continue; }
    tested = true;
    std::vector<long> resident, activeGPU, releasedGPU;
    for (int round = 0; round < 200; ++round) {
      config.is_active = true;
      if (!source.tryDecoder(candidate.codec->name, candidate.device, candidate.format, candidate.software)) {
        return 1;
      }
      if (avformat_open_input(&input, argv[2], 0, 0) < 0 || avformat_find_stream_info(input, 0) < 0) { return 2; }
      int packets = 0, frames = 0;
      const auto frame = [&]() {
        ++frames;
        if (!context_in || context_in->codec != candidate.codec || !context_in->hw_device_ctx) {
          fprintf(stderr, "decoder restart silently changed the requested hardware backend\n");
          return false;
        }
        if (!source.transformVideoFrame() || !frameConverted || frameConverted->format != AV_PIX_FMT_NV12) {
          fprintf(stderr, "hardware frame did not scale/download after restart\n");
          return false;
        }
        return true;
      };
      while (av_read_frame(input, packet) >= 0) {
        if (packet->stream_index == track) {
          ++packets;
          if (source.feed(packet, input->streams[track]->time_base) && !frame()) { return 1; }
          while (source.receiveDecodedFrame()) {
            if (!frame()) { return 1; }
          }
        }
        av_packet_unref(packet);
      }
      if (avcodec_send_packet(context_in, 0) < 0) { return 1; }
      while (source.receiveDecodedFrame()) {
        if (!frame()) { return 1; }
      }
      avformat_close_input(&input);
      const long gpuActive = gpuMemory(device == AV_HWDEVICE_TYPE_CUDA);
      source.releaseVideoDecoder();
      const long gpuReleased = gpuMemory(device == AV_HWDEVICE_TYPE_CUDA);
      if (gpuActive <= 0 || gpuReleased < 0) {
        fprintf(stderr, "GPU memory query did not observe the actual hardware decoder\n");
        return 1;
      }
      activeGPU.push_back(gpuActive);
      releasedGPU.push_back(gpuReleased);
      resident.push_back(residentAnonymous());
      if (context_in || frames != packets || !config.is_active) {
        fprintf(stderr, "%s restart %d lost frames: %d/%d\n", candidate.codec->name, round, frames, packets);
        return 1;
      }
      printf("MEMORY codec=%s round=%d rssanon_kib=%ld frames=%d gpu_active_kib=%ld gpu_released_kib=%ld\n",
             candidate.codec->name, round, resident.back(), frames, gpuActive, gpuReleased);
      fflush(stdout);
    }
    for (const auto & series : {resident, activeGPU, releasedGPU}) {
      // Warm up 100 real lifetimes before measuring another 100: driver caches
      // and allocator arenas can grow late in startup. Compare both edges of
      // the settled band so bounded allocator variation is not counted as growth.
      const auto middle = std::minmax_element(series.begin() + 100, series.begin() + 120);
      const auto final = std::minmax_element(series.end() - 20, series.end());
      if (*final.first > *middle.first + 1024 || *final.second > *middle.second + 1024) {
        fprintf(stderr, "%s memory grew after settling: middle %ld..%ld, final %ld..%ld KiB\n", candidate.codec->name,
                *middle.first, *middle.second, *final.first, *final.second);
        return 1;
      }
    }
  }
  av_packet_free(&packet);
  return tested ? 0 : 1;
}
