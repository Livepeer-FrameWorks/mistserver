#include "audio_decoder_node.h"

#include "../defines.h"
#include "../timing.h"
#include "packet_context.h"
#include "audio_frame_context.h"
#include "utils.h"
#include <cstring>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
}

namespace FFmpeg {

  AudioDecoderNode::AudioDecoderNode(AVCodecID _codecId, const std::string & extradata, uint64_t rate, uint64_t ch, uint64_t depth) {
    // Initialize audio parameters
    codecId = _codecId;
    sampleRate = rate;
    channels = ch;
    bitDepth = depth;
    initData = extradata;
    init();
  }

  AudioDecoderNode::~AudioDecoderNode() {
    if (codecCtx) {
      if (codecCtx->extradata) { av_freep(&codecCtx->extradata); }
      avcodec_free_context(&codecCtx);
    }
    if (frame) { av_frame_free(&frame); }
    INFO_MSG("Audio Decoder: Destroyed node");
  }

  bool AudioDecoderNode::init() {
    // Find decoder
    codec = avcodec_find_decoder(codecId);
    if (!codec) {
      ERROR_MSG("Audio Decoder: Could not find decoder for codec ID %d", codecId);
      return false;
    }

    // (Re)create decoder context
    if (codecCtx) {
      if (codecCtx->extradata) { av_freep(&codecCtx->extradata); }
      avcodec_free_context(&codecCtx);
    }
    codecCtx = avcodec_alloc_context3(codec);
    if (!codecCtx) {
      ERROR_MSG("Audio Decoder: Could not allocate decoder context");
      return false;
    }

    // Set basic parameters
    codecCtx->sample_rate = sampleRate;
    av_channel_layout_default(&codecCtx->ch_layout, static_cast<int>(channels));
    codecCtx->ch_layout.nb_channels = static_cast<unsigned int>(channels);
    codecCtx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    codecCtx->bits_per_raw_sample = bitDepth;
    // Ensure decoder interprets input packet PTS in milliseconds
    codecCtx->pkt_timebase = (AVRational){1, 1000};

    if (!initData.empty()) {
      codecCtx->extradata = (unsigned char *)initData.data();
      codecCtx->extradata_size = initData.size();
    }

    // Open codec
    int ret = avcodec_open2(codecCtx, codec, nullptr);
    if (ret < 0) {
      FFmpegUtils::printAvError("Audio Decoder: Could not open codec", ret);
      return false;
    }

    updateLongName();
    INFO_MSG("Successfully (re)initialized %s", getLongName().c_str());

    return true;
  }

  void AudioDecoderNode::updateLongName() {
    if (codec && codec->name && strstr(codec->name, "pcm") == codec->name) {
      longName = "AudioIngestRaw(";
    } else {
      longName = "AudioDecoder(";
    }
    longName += (codec && codec->name) ? codec->name : "Unknown";
    if (codecCtx->ch_layout.nb_channels == 1){
      longName += ", mono";
    }else if (codecCtx->ch_layout.nb_channels == 2){
      longName += ", stereo";
    }else{
      longName += ", " + std::to_string(codecCtx->ch_layout.nb_channels) + "ch";
    }
    if (!(codecCtx->sample_rate % 1000)){
      longName += ", " + std::to_string(codecCtx->sample_rate/1000) + "kHz";
    }else if (!(codecCtx->sample_rate % 100)){
      longName += ", " + std::to_string(codecCtx->sample_rate / 1000) + "." +
        std::to_string((codecCtx->sample_rate / 100) % 10) + "kHz";
    }else{
      longName += ", " + std::to_string(codecCtx->sample_rate) + "Hz";
    }
    longName += ")";
  }

  bool AudioDecoderNode::setInput(void * inData, size_t idx) {
    PacketContext * packet = (PacketContext*)inData;
    if (!packet || !packet->getPacket()) {
      ERROR_MSG("Audio Decoder: Invalid packet");
      return false;
    }

    // Get codec parameters from packet
    const AVCodecParameters *params = packet->getCodecParameters();
    if (!params) {
      ERROR_MSG("Audio Decoder: No codec parameters in packet");
      return false;
    }

    // Configure if needed - do this outside of any locks
    if (!codecCtx || sampleRate != params->sample_rate || channels != params->ch_layout.nb_channels ||
        bitDepth != params->bits_per_raw_sample || codecId != params->codec_id || initData != packet->getCodecData()) {
      INFO_MSG("Audio Decoder: Reconfiguring decoder...");

      // Set audio parameters
      sampleRate = params->sample_rate;
      channels = params->ch_layout.nb_channels;
      bitDepth = params->bits_per_raw_sample;
      codecId = params->codec_id;
      initData = packet->getCodecData();

      // Initialize decoder
      if (!init()) {
        ERROR_MSG("Audio Decoder: Failed to initialize decoder");
        return false;
      }
      // Reset timeline
      lastPtsMs = AV_NOPTS_VALUE;
      anchorPtsMs = AV_NOPTS_VALUE;
      totalDecodedSamples = 0;
    }

    // Set source node ID
    packet->setSourceNodeId(nodeId);

    // Validate decoder state
    if (!codecCtx || !codec) {
      ERROR_MSG("Audio Decoder: Decoder not initialized (ctx=%p, codec=%p)", codecCtx, codec);
      return false;
    }

    // Start processing time tracking
    uint64_t startTime = Util::getMicros();

    // Get packet for processing
    AVPacket *pkt = packet->getPacket();
    if (!pkt || !pkt->size){
      FAIL_MSG("Cannot process empty audio packet");
      return false;
    }
    VERYHIGH_MSG("Audio Decoder: Processing packet: size=%d, pts=%" PRIu64 ", flags=%d", pkt->size, pkt->pts, pkt->flags);

    // Send packet to decoder
    int ret = avcodec_send_packet(codecCtx, pkt);
    if (ret < 0) {
      if (ret == AVERROR_EOF){
        FAIL_MSG("Audio decoder is flushed and cannot accept new packet ");
        return false;
      }
      FFmpegUtils::printAvError("Audio Decoder: Error sending packet to decoder", ret);
      return false;
    }

    // Receive frames
    while (ret >= 0) {
      AVFrame *frame = av_frame_alloc();
      if (!frame) {
        ERROR_MSG("Audio Decoder: Failed to allocate frame");
        return false;
      }

      ret = avcodec_receive_frame(codecCtx, frame);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        av_frame_free(&frame);
        break;
      } else if (ret < 0) {
        FFmpegUtils::printAvError("Audio Decoder: Error receiving frame", ret);
        av_frame_free(&frame);
        return false;
      }

      // Establish robust PTS in milliseconds using best-effort with synthesis fallback
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 0, 0)
      int64_t be = frame->best_effort_timestamp;
#else
      int64_t be = av_frame_get_best_effort_timestamp(frame);
#endif
      if (be != AV_NOPTS_VALUE) {
        frame->pts = be; // already ms due to pkt_timebase
        anchorPtsMs = (anchorPtsMs == AV_NOPTS_VALUE) ? be : anchorPtsMs;
      } else {
        // Synthesize from last PTS and sample count
        int sr = codecCtx && codecCtx->sample_rate > 0 ? codecCtx->sample_rate : (int)sampleRate;
        if (sr <= 0) sr = 48000; // safe default
        int64_t deltaMs = (int64_t)((frame->nb_samples * 1000LL) / sr);
        if (lastPtsMs == AV_NOPTS_VALUE) {
          frame->pts = anchorPtsMs != AV_NOPTS_VALUE ? anchorPtsMs : 0;
        } else {
          frame->pts = lastPtsMs + deltaMs;
        }
      }
      lastPtsMs = frame->pts;
      totalDecodedSamples += frame->nb_samples;

      // Create frame context
      AudioFrameContext frameCtx(frame, (AVSampleFormat)frame->format, frame->ch_layout.nb_channels, frame->sample_rate);
      frameCtx.setSourceNodeId(getNodeId());

      totalProcessingTime.fetch_add(Util::getMicros(startTime));

      VERYHIGH_MSG("Audio Decoder: Decoded frame: pts=%" PRIu64 ", source=%zu, samples=%d", frame->pts, getNodeId(),
                   frame->nb_samples);

      // Call outputs
      for (auto & cb : callbacks) { cb(&frameCtx); }

      startTime = Util::getMicros();
    }

    return true;
  }

} // namespace FFmpeg
