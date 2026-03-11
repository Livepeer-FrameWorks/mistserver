#include "node_pipeline.h"

#include "../defines.h"
#include "audio_decoder_node.h"
#include "audio_encoder_node.h"
#include "audio_transformer_node.h"
#include "hw_context_manager.h"
#include "packet_context.h"
#include "video_decoder_node.h"
#include "video_encoder_node.h"
#include "video_transformer_node.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/pixdesc.h>
}

namespace FFmpeg {

  NodePipeline::~NodePipeline() {
    if (inPacket) { av_packet_free(&inPacket); }
  }

  const char *NodePipeline::getCodecIn() const {
    std::lock_guard<std::mutex> lock(configMutex);
    return codecIn.empty() ? nullptr : codecIn.c_str();
  }

  bool NodePipeline::waitForInactive(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(stateMutex);
    return stateCV.wait_for(lock, timeout, [this]() { return !isActive.load(); });
  }

  void NodePipeline::signalPacketProcessed(const std::shared_ptr<PacketContext> & packet) {
    if (!packet) return;

    // Log completion for debugging
    VERYHIGH_MSG("Pipeline output: Packet processing complete: PTS=%" PRIu64 ", size=%d", packet->getPts(), packet->getSize());

    // Notify waiters that processing is complete
    stateCV.notify_all();
  }

  bool NodePipeline::isRawFormat(const std::string & codec) {
    return codec == "YUYV" || codec == "UYVY" || codec == "NV12";
  }

  bool NodePipeline::configure(const PipelineConfig & newConfig) {
    // Store configuration
    {
      std::lock_guard<std::mutex> lock(configMutex);
      config = newConfig;
      isVideo.store(config.isVideo);
    }

    HWDeviceConstraints constraints;
    constraints.allowNvidia = newConfig.allowNvidia;
    constraints.allowQsv = newConfig.allowQsv;
    constraints.allowVaapi = newConfig.allowVaapi;
    constraints.allowMediaToolbox = newConfig.allowMediaToolbox;
    constraints.allowSW = newConfig.allowSW;
    constraints.devicePath = newConfig.hwDevicePath;
    HWContextManager::getInstance().setDeviceConstraints(constraints);

    return true;
  }

  bool NodePipeline::needsTransformer(const PipelineConfig & config) const {
    if (config.isVideo) {
      // Always need transformer if resolution change is requested
      if (config.targetWidth > 0 || config.targetHeight > 0 || !config.resolution.empty()) { return true; }

      // Always need transformer for JPEG encoding (requires format conversion)
      if (config.codecOut == "JPEG") { return true; }

      // Always need transformer for raw formats
      if (config.codecOut == "YUYV" || config.codecOut == "UYVY" || config.codecOut == "NV12") { return true; }

      return false;
    } else {
      return true;
    }
  }

  FFmpeg::PipelineConfig NodePipeline::getConfig() const {
    std::lock_guard<std::mutex> lock(stateMutex);
    return config;
  }

  void NodePipeline::setCodecIn(const char *codec) {
    std::lock_guard<std::mutex> lock(configMutex);
    codecIn = codec ? codec : "";
  }

  void NodePipeline::receiveVideo(uint64_t sendTime, uint64_t time, const char *data, size_t size, uint64_t width,
                                  uint64_t height, const std::string & init, uint64_t fpks, const char *codec, bool isKeyframe) {
    if (!data || size == 0) {
      ERROR_MSG("Pipeline ingestion: Invalid video data");
      droppedFrameCount++;
      return;
    }

    // Check active state without holding mutex
    if (!isActive.load()) {
      WARN_MSG("Pipeline ingestion: Pipeline not active, dropping packet");
      droppedFrameCount++;
      return;
    }

    VERYHIGH_MSG("Pipeline ingestion: Receiving video packet: codec=%s, %" PRIu64 "x%" PRIu64 ", time=%" PRIu64 ", "
                 "size=%zu, init=%s",
                 codec ? codec : "unknown", width, height, time, size, init.size() ? "present" : "none");

    if (!inPacket) { inPacket = av_packet_alloc(); }
    if (!inPacket) {
      FAIL_MSG("Could not allocate packet!");
      droppedFrameCount++;
      return;
    }
    inPacket->data = (unsigned char *)data;
    inPacket->size = size;

    PacketContext packet(inPacket);

    // Set packet properties
    packet.setPts(time);
    packet.setDts(AV_NOPTS_VALUE);
    packet.setKeyframe(isKeyframe);

    // Set codec data if provided
    if (init.size()) { packet.setCodecData((const uint8_t *)init.data(), init.size()); }

    // Set FormatInfo
    PacketContext::FormatInfo formatInfo;
    formatInfo.codecName = codec?codec:"";
    formatInfo.width = width;
    formatInfo.height = height;
    formatInfo.fpks = fpks;
    formatInfo.isKeyframe = isKeyframe;

    // Determine codecId from codecName
    if (formatInfo.codecName == "H264") {
      formatInfo.codecId = AV_CODEC_ID_H264;
    } else if (formatInfo.codecName == "AV1") {
      formatInfo.codecId = AV_CODEC_ID_AV1;
    } else if (formatInfo.codecName == "HEVC" || formatInfo.codecName == "H265") {
      formatInfo.codecId = AV_CODEC_ID_HEVC;
    } else if (formatInfo.codecName == "VP8") {
      formatInfo.codecId = AV_CODEC_ID_VP8;
    } else if (formatInfo.codecName == "VP9") {
      formatInfo.codecId = AV_CODEC_ID_VP9;
    } else if (formatInfo.codecName == "JPEG") {
      formatInfo.codecId = AV_CODEC_ID_MJPEG;
    } else {
      formatInfo.codecId = AV_CODEC_ID_NONE;
    }

    packet.setFormatInfo(formatInfo);

    // Also set AVCodecParameters for compatibility with nodes that use getCodecParameters()
    // Video packet
    packet.setCodecParameters(width, height, formatInfo.codecId, init);

    VERYHIGH_MSG("Pipeline processing: Processing packet: time=%" PRIu64 ", size=%u", packet.getPts(), packet.getSize());

    // Use backward construction on first packet to create entire node graph upfront
    if (!nodeGraphConstructed) {
      INFO_MSG("Pipeline processing: First packet received, starting backward construction");
      if (!createNodeGraphBackwards(&packet)) {
        ERROR_MSG("Pipeline processing: Failed to construct node graph backwards");
        droppedFrameCount++;
        return;
      }
    }

    // Route packet to appropriate decoder
    {
      std::lock_guard<std::mutex> lock(stateMutex);

      // Route packet to first video decoder
      if (videoDecoders.empty()) {
        ERROR_MSG("Pipeline processing: No video decoders found after backward construction");
        droppedFrameCount++;
        return;
      }

      auto videoDecoderIt = videoDecoders.begin();
      if (videoDecoderIt->second) {
        VERYHIGH_MSG("Pipeline processing: Setting input packet on video decoder %zu", videoDecoderIt->first);
        if (!videoDecoderIt->second->setInput(&packet, 0)) {
          ERROR_MSG("Pipeline processing: Failed to set input on video decoder %zu", videoDecoderIt->first);
          droppedFrameCount++;
          return;
        }
      } else {
        ERROR_MSG("Pipeline processing: video decoder %zu invalid!", videoDecoderIt->first);
        droppedFrameCount++;
        return;
      }
    } // Release stateMutex before processing nodes
    if (codec && codecIn.empty()) { setCodecIn(codec); }
  }

  void NodePipeline::receiveAudio(uint64_t sendTime, uint64_t time, const char *data, size_t size, uint64_t bitDepth,
                                  uint64_t channels, uint64_t sampleRate, const std::string & init, const std::string & codec) {
    if (!data || !size) {
      ERROR_MSG("Pipeline ingestion: Invalid audio data");
      droppedFrameCount++;
      return;
    }

    // Check active state without holding mutex
    if (!isActive.load()) {
      WARN_MSG("Pipeline ingestion: Pipeline not active, dropping packet");
      droppedFrameCount++;
      return;
    }

    VERYHIGH_MSG("Pipeline ingestion: Receiving audio packet: codec=%s, %" PRIu64 " channels, %" PRIu64
                 "Hz, time=%" PRIu64 ", size=%zu",
                 codec.c_str(), channels, sampleRate, time, size);

    if (!inPacket) { inPacket = av_packet_alloc(); }
    if (!inPacket) {
      FAIL_MSG("Could not allocate packet!");
      droppedFrameCount++;
      return;
    }
    inPacket->data = (unsigned char *)data;
    inPacket->size = size;

    // Call ingestPacket directly with all parameters
    PacketContext packet(inPacket);

    // Set packet properties
    packet.setPts(time);
    packet.setDts(AV_NOPTS_VALUE);
    packet.setKeyframe(false);

    // Set codec data if provided
    if (init.size()) { packet.setCodecData((const uint8_t *)init.data(), init.size()); }

    // Set FormatInfo
    PacketContext::FormatInfo formatInfo;
    formatInfo.codecName = codec;
    formatInfo.channels = channels;
    formatInfo.sampleRate = sampleRate;
    formatInfo.bitDepth = bitDepth;

    // Determine codecId from codecName
    if (codec == "AAC") {
      formatInfo.codecId = AV_CODEC_ID_AAC;
    } else if (codec == "PCM") {
      if (bitDepth == 32){
        formatInfo.codecId = AV_CODEC_ID_PCM_S32BE;
      }else if (bitDepth == 16){
        formatInfo.codecId = AV_CODEC_ID_PCM_S16BE;
      }
    } else if (codec == "opus") {
      formatInfo.codecId = AV_CODEC_ID_OPUS;
    } else if (codec == "MP3") {
      formatInfo.codecId = AV_CODEC_ID_MP3;
    } else if (codec == "FLAC") {
      formatInfo.codecId = AV_CODEC_ID_FLAC;
    } else if (codec == "vorbis") {
      formatInfo.codecId = AV_CODEC_ID_VORBIS;
    } else {
      formatInfo.codecId = AV_CODEC_ID_NONE;
    }

    packet.setFormatInfo(formatInfo);

    // Audio packet - create temporary AVCodecParameters
    AVCodecParameters *params = avcodec_parameters_alloc();
    if (params) {
      params->codec_type = AVMEDIA_TYPE_AUDIO;
      params->codec_id = formatInfo.codecId;
      params->sample_rate = sampleRate;
      av_channel_layout_default(&params->ch_layout, static_cast<int>(channels));
      params->ch_layout.nb_channels = static_cast<unsigned int>(channels);
      params->bits_per_raw_sample = bitDepth;

      // Set extradata if provided
      if (init.size()) {
        params->extradata = (uint8_t *)av_mallocz(init.size() + AV_INPUT_BUFFER_PADDING_SIZE);
        if (params->extradata) {
          memcpy(params->extradata, init.data(), init.size());
          params->extradata_size = init.size();
        }
      }

      packet.setCodecParameters(params);
      avcodec_parameters_free(&params);
    }

    VERYHIGH_MSG("Pipeline processing: Processing packet: time=%" PRIu64 ", size=%u", packet.getPts(), packet.getSize());

    // Use backward construction on first packet to create entire node graph upfront
    if (!nodeGraphConstructed) {
      INFO_MSG("Pipeline processing: First packet received, starting backward construction");
      if (!createNodeGraphBackwards(&packet)) {
        ERROR_MSG("Pipeline processing: Failed to construct node graph backwards");
        droppedFrameCount++;
        return;
      }
    }

    // Route packet to appropriate decoder
    {
      std::lock_guard<std::mutex> lock(stateMutex);

      // Route packet to first audio decoder
      if (audioDecoders.empty()) {
        ERROR_MSG("Pipeline processing: No audio decoders found after backward construction");
        droppedFrameCount++;
        return;
      }

      auto audioDecoderIt = audioDecoders.begin();
      if (audioDecoderIt->second) {
        if (!audioDecoderIt->second->setInput(&packet, 0)) {
          ERROR_MSG("Pipeline processing: Failed to set input on %s", audioDecoderIt->second->getLongName().c_str());
          droppedFrameCount++;
          return;
        }
      } else {
        ERROR_MSG("Pipeline processing: audio decoder %zu invalid!", audioDecoderIt->first);
        droppedFrameCount++;
        return;
      }
    } // Release stateMutex before processing nodes

    if (codec.size() && codecIn.empty()) { codecIn = codec; }
  }

  template<typename NodeType> size_t NodePipeline::addNode(std::shared_ptr<NodeType> node) {
    if (!node) { return INVALID_NODE_ID; }
    size_t id = nextNodeId++;

    if (std::is_same<NodeType, FFmpeg::VideoDecoderNode>::value) {
      videoDecoders[id] = std::dynamic_pointer_cast<FFmpeg::VideoDecoderNode>(node);
    } else if (std::is_same<NodeType, FFmpeg::VideoEncoderNode>::value) {
      if (outCb) { node->callbacks.push_back(outCb); }
      videoEncoders[id] = std::dynamic_pointer_cast<FFmpeg::VideoEncoderNode>(node);
    } else if (std::is_same<NodeType, FFmpeg::VideoTransformerNode>::value) {
      videoNodes[id] = std::dynamic_pointer_cast<FFmpeg::VideoTransformerNode>(node);
    } else if (std::is_same<NodeType, FFmpeg::AudioDecoderNode>::value) {
      audioDecoders[id] = std::dynamic_pointer_cast<FFmpeg::AudioDecoderNode>(node);
    } else if (std::is_same<NodeType, FFmpeg::AudioEncoderNode>::value) {
      if (outCb) { node->callbacks.push_back(outCb); }
      audioEncoders[id] = std::dynamic_pointer_cast<FFmpeg::AudioEncoderNode>(node);
    } else if (std::is_same<NodeType, FFmpeg::AudioTransformerNode>::value) {
      audioNodes[id] = std::dynamic_pointer_cast<FFmpeg::AudioTransformerNode>(node);
    } else {
      --nextNodeId; // Node ID was not used, rewind.
      ERROR_MSG("Pipeline processing: Unknown node type: %s", node->getLongName().c_str());
      return INVALID_NODE_ID;
    }
    node->setNodeId(id);
    INFO_MSG("Added %s (node %zu)", node->getLongName().c_str(), id);
    return id;
  }

  template<typename NodeType> std::vector<std::shared_ptr<NodeType>> NodePipeline::getNodesOfType() const {
    std::vector<std::shared_ptr<NodeType>> result;
    if (std::is_same<NodeType, FFmpeg::VideoTransformerNode>::value) {
      for (const auto & pair : videoNodes) { result.push_back(pair.second); }
    } else if (std::is_same<NodeType, FFmpeg::AudioTransformerNode>::value) {
      for (const auto & pair : audioNodes) { result.push_back(pair.second); }
    }
    return result;
  }

  const std::string noneText = "None";

  const std::string & NodePipeline::getDecoderName() {
    std::lock_guard<std::mutex> lock(stateMutex);
    if (isVideo) {
      switch (videoDecoders.size()) {
        case 0: return noneText;
        case 1: return videoDecoders.begin()->second->getLongName();
        default: {
          // Build a comprehensive decoder name string
          static std::string videoDecoderNames;
          videoDecoderNames.clear();
          for (const auto & pair : videoDecoders) {
            if (pair.second && pair.second->getLongName().size()) {
              if (!videoDecoderNames.empty()) videoDecoderNames += " + ";
              videoDecoderNames += pair.second->getLongName();
            }
          }
          return videoDecoderNames;
        }
      }
    } else {
      switch (audioDecoders.size()) {
        case 0: return noneText;
        case 1: return audioDecoders.begin()->second->getLongName();
        default: {
          // Build a comprehensive decoder name string
          static std::string audioDecoderNames;
          audioDecoderNames.clear();
          for (const auto & pair : audioDecoders) {
            if (pair.second && pair.second->getLongName().size()) {
              if (!audioDecoderNames.empty()) audioDecoderNames += " + ";
              audioDecoderNames += pair.second->getLongName();
            }
          }
          return audioDecoderNames;
        }
      }
    }
  }

  const std::string & NodePipeline::getEncoderName() {
    std::lock_guard<std::mutex> lock(stateMutex);
    if (isVideo) {
      switch (videoEncoders.size()) {
        case 0: return noneText;
        case 1: return videoEncoders.begin()->second->getLongName();
        default: {
          // Build a comprehensive encoder name string
          static std::string videoEncoderNames;
          videoEncoderNames.clear();
          for (const auto & pair : videoEncoders) {
            if (pair.second && pair.second->getLongName().size()) {
              if (!videoEncoderNames.empty()) videoEncoderNames += " + ";
              videoEncoderNames += pair.second->getLongName();
            }
          }
          return videoEncoderNames;
        }
      }
    } else {
      switch (audioEncoders.size()) {
        case 0: return noneText;
        case 1: return audioEncoders.begin()->second->getLongName();
        default: {
          // Build a comprehensive decoder name string
          static std::string audioEncoderNames;
          audioEncoderNames.clear();
          for (const auto & pair : audioEncoders) {
            if (pair.second && pair.second->getLongName().size()) {
              if (!audioEncoderNames.empty()) audioEncoderNames += " + ";
              audioEncoderNames += pair.second->getLongName();
            }
          }
          return audioEncoderNames;
        }
      }
    }
  }

  const std::string & NodePipeline::getTransformerName() {
    std::lock_guard<std::mutex> lock(stateMutex);
    if (isVideo) {
      switch (videoNodes.size()) {
        case 0: return noneText;
        case 1: return videoNodes.begin()->second->getLongName();
        default: {
          // Build a comprehensive encoder name string
          static std::string videoTransformerNames;
          videoTransformerNames.clear();
          for (const auto & pair : videoEncoders) {
            if (pair.second && pair.second->getLongName().size()) {
              if (!videoTransformerNames.empty()) videoTransformerNames += " + ";
              videoTransformerNames += pair.second->getLongName();
            }
          }
          return videoTransformerNames;
        }
      }
    } else {
      switch (audioNodes.size()) {
        case 0: return noneText;
        case 1: return audioNodes.begin()->second->getLongName();
        default: {
          // Build a comprehensive decoder name string
          static std::string audioTransformerNames;
          audioTransformerNames.clear();
          for (const auto & pair : audioEncoders) {
            if (pair.second && pair.second->getLongName().size()) {
              if (!audioTransformerNames.empty()) audioTransformerNames += " + ";
              audioTransformerNames += pair.second->getLongName();
            }
          }
          return audioTransformerNames;
        }
      }
    }
  }

  void NodePipeline::setIsActive(bool active) {
    isActive.store(active, std::memory_order_release);
    if (!active) { stateCV.notify_all(); }
  }

  // Backward construction implementation
  bool NodePipeline::createNodeGraphBackwards(PacketContext * packet) {
    if (!packet) {
      ERROR_MSG("Pipeline construction: Invalid packet for backward construction");
      return false;
    }

    if (nodeGraphConstructed) {
      VERYHIGH_MSG("Pipeline construction: Node graph already constructed");
      return true;
    }

    MEDIUM_MSG("Pipeline construction: Starting backward construction for %s pipeline", config.isVideo ? "video" : "audio");

    const PacketContext::FormatInfo & formatInfo = packet->getFormatInfo();

    try {
      // Create encoders based on PipelineConfig output requirements
      if (config.isVideo) {
        bool isRaw = isRawFormat(config.codecOut);
        if (isRaw) {
          MEDIUM_MSG("Pipeline construction: Raw format detected (%s), encoder will pass through to egress", config.codecOut.c_str());
        }

        // Create video encoder
        std::shared_ptr<VideoEncoderNode> encoder = std::make_shared<FFmpeg::VideoEncoderNode>(
          config.codecOut.c_str(), config.bitrate > 0 ? config.bitrate : 2000000, config.gopSize, config.tune.c_str(),
          config.preset.c_str(), config.quality, config.rateControlMode, config.crf, config.qp, config.cq,
          config.maxBitrate, config.vbvBufferSize, config.rateControlRcOption);

        if (!encoder) {
          ERROR_MSG("Pipeline construction: Failed to create video encoder");
          return false;
        }

        encoder->setHardwareAcceleration(config.allowNvidia, config.allowQsv, config.allowVaapi,
                                         config.allowMediaToolbox, config.allowSW, config.hwDevicePath);

        if (!encoder->init()) {
          ERROR_MSG("Pipeline construction: Failed to initialize video encoder");
          return false;
        }

        size_t encoderId = addNode(encoder);
        if (encoderId == INVALID_NODE_ID) {
          ERROR_MSG("Pipeline construction: Failed to add video encoder");
          return false;
        }

        // Attach input nodes to the encoder
        if (!createVideoInputNodes(encoder, formatInfo, isRaw)) {
          ERROR_MSG("Pipeline construction: Failed to create video input nodes");
          return false;
        }

      } else {
        // Create audio encoder(s)
        uint64_t outSampleRate = config.targetSampleRate > 0 ? config.targetSampleRate : formatInfo.sampleRate;
        uint64_t outBitrate = config.bitrate > 0 ? config.bitrate : 128000;
        uint64_t outBitDepth = config.targetBitDepth > 0 ? config.targetBitDepth : formatInfo.bitDepth;

        // Check if we need multiple encoders for track splitting
        if (!config.splitChannels.empty()) {
          // Create multiple transformer-encoder pairs - one for each track
          MEDIUM_MSG("Pipeline construction: Creating %zu audio tracks for splitting", config.splitChannels.size());

          // Store transformer IDs for decoder connection
          std::vector<size_t> transformerIds;
          uint64_t channelOffset = 0;

          for (size_t trackIdx = 0; trackIdx < config.splitChannels.size(); trackIdx++) {
            uint64_t trackChannels = config.splitChannels[trackIdx];

            // Create encoder for this track
            std::shared_ptr<AudioEncoderNode> encoder = std::make_shared<FFmpeg::AudioEncoderNode>(
              config.codecOut.c_str(), outSampleRate, trackChannels, outBitrate, outBitDepth);

            if (!encoder) {
              ERROR_MSG("Pipeline construction: Failed to create audio encoder for track %zu", trackIdx);
              return false;
            }

            size_t encoderId = addNode(encoder);
            if (encoderId == INVALID_NODE_ID) {
              ERROR_MSG("Pipeline construction: Failed to add audio encoder for track %zu", trackIdx);
              return false;
            }

            // Create transformer for this track
            std::vector<int> channelMap;
            channelMap.assign(formatInfo.channels, -1);
            for (size_t i = channelOffset; i < channelOffset + trackChannels; i++) {
              if (i >= 0 && i < formatInfo.channels) {
                // Set mapping: output channel outputIdx gets input channel inputIdx
                channelMap[i - channelOffset] = i;
              }
            }
            channelOffset += trackChannels;

            size_t transformerId;
            if (!createAudioTransformerForEncoder(encoder, formatInfo, config, channelMap, transformerId)) {
              ERROR_MSG("Pipeline construction: Failed to create audio transformer for track %zu", trackIdx);
              return false;
            }

            transformerIds.push_back(transformerId);

            // Connect transformer to encoder
            if (!linkNodes(transformerId, encoderId)) {
              ERROR_MSG("Pipeline construction: Failed to connect transformer to encoder for track %zu", trackIdx);
              return false;
            }
          }

          // Create single decoder and connect to all transformers
          std::shared_ptr<AudioDecoderNode> decoder = std::make_shared<AudioDecoderNode>(
            static_cast<AVCodecID>(formatInfo.codecId), "", formatInfo.sampleRate, formatInfo.channels, formatInfo.bitDepth);

          if (!decoder) {
            ERROR_MSG("Pipeline construction: Failed to create audio decoder for track splitting");
            return false;
          }

          size_t decoderId = addNode(decoder);
          if (decoderId == INVALID_NODE_ID) {
            ERROR_MSG("Pipeline construction: Failed to add audio decoder for track splitting");
            return false;
          }

          // Connect decoder to all transformers
          for (size_t i = 0; i < transformerIds.size(); i++) {
            if (!linkNodes(decoderId, transformerIds[i])) {
              ERROR_MSG("Pipeline construction: Failed to connect decoder to transformer for track %zu", i);
              return false;
            }
          }

          MEDIUM_MSG("Pipeline construction: Connected decoder %zu to %zu transformers for track splitting", decoderId,
                     transformerIds.size());

        } else {
          // Single encoder (normal case)
          std::shared_ptr<AudioEncoderNode> encoder = std::make_shared<FFmpeg::AudioEncoderNode>(
            config.codecOut.c_str(), outSampleRate, formatInfo.channels, outBitrate, outBitDepth);

          if (!encoder) {
            ERROR_MSG("Pipeline construction: Failed to create audio encoder");
            return false;
          }

          size_t encoderId = addNode(encoder);
          if (encoderId == INVALID_NODE_ID) {
            ERROR_MSG("Pipeline construction: Failed to add audio encoder");
            return false;
          }

          // Attach input nodes to the encoder
          if (!createAudioInputNodes(encoder, formatInfo, config)) {
            ERROR_MSG("Pipeline construction: Failed to create audio input nodes");
            return false;
          }
        }
      }

      nodeGraphConstructed = true;
      return true;

    } catch (const std::exception & e) {
      ERROR_MSG("Pipeline construction: Exception during backward construction: %s", e.what());
      return false;
    }
  }

  // Helper methods for creating input nodes
  bool NodePipeline::createAudioTransformerForEncoder(std::shared_ptr<AudioEncoderNode> encoder,
                                                      const PacketContext::FormatInfo & formatInfo, const PipelineConfig & config,
                                                      const std::vector<int> & channelMap, size_t & transformerId) {
    if (!encoder) {
      ERROR_MSG("Pipeline construction: Invalid audio encoder for transformer creation");
      return false;
    }

    // Use encoder's required sample rate and channels, not input or config values
    uint64_t encoderSampleRate = encoder->getSampleRate();

    // Override with config if explicitly set
    uint64_t outSampleRate = config.targetSampleRate > 0 ? config.targetSampleRate : encoderSampleRate;

    // For split audio, use the channel map size; for normal audio, use input channels
    uint64_t outChannels = encoder->getChannels();

    // Get the encoder's expected sample format - matching legacy code
    AVSampleFormat encoderFormat = encoder->getSampleFormat();

    std::shared_ptr<AudioTransformerNode> transformer =
      std::make_shared<FFmpeg::AudioTransformerNode>(static_cast<int>(outSampleRate), static_cast<int>(outChannels), encoderFormat);

    if (!transformer) {
      ERROR_MSG("Pipeline construction: Failed to create audio transformer");
      return false;
    }

    // Apply channel mapping if provided (for split audio)
    if (!channelMap.empty()) {
      if (!transformer->updateChannelMap(channelMap)) {
        ERROR_MSG("Pipeline construction: Failed to set channel map for transformer");
        return false;
      }
    }

    if (!transformer->init()) {
      ERROR_MSG("Pipeline construction: Failed to initialize audio transformer");
      return false;
    }

    transformerId = addNode(transformer);
    if (transformerId == INVALID_NODE_ID) {
      ERROR_MSG("Pipeline construction: Failed to add audio transformer to pipeline");
      return false;
    }

    return true;
  }

  bool NodePipeline::createVideoInputNodes(std::shared_ptr<VideoEncoderNode> encoder,
                                           const PacketContext::FormatInfo & formatInfo, bool isRaw) {
    // Encoder is always provided (even for raw formats)
    if (!encoder) {
      ERROR_MSG("Pipeline construction: Invalid video encoder");
      return false;
    }

    // Determine if we need a transformer
    bool needsTransformation = needsTransformer(config);

    // Raw inputs must be transformed
    if (formatInfo.codecName == "YUYV" || formatInfo.codecName == "UYVY" || formatInfo.codecName == "NV12") {
      needsTransformation = true;
    }

    // If the encoder is software but hardware decoding may be used, force a transformer to
    // handle HW->SW transfer and SW pixel format conversion. This avoids passing HW pixel
    // formats (e.g. videotoolbox_vld) to software encoders like libaom/libx264.
    bool hwDecodePossible = config.allowNvidia || config.allowQsv || config.allowVaapi || config.allowMediaToolbox;
    if (!encoder->isHardwareCodec() && hwDecodePossible) { needsTransformation = true; }

    if (needsTransformation) {
      // Create transformer
      std::shared_ptr<VideoTransformerNode> transformer =
        std::make_shared<FFmpeg::VideoTransformerNode>(config.resolution, config.quality);

      if (!transformer) {
        ERROR_MSG("Pipeline construction: Failed to create video transformer");
        return false;
      }

      transformer->setHardwareAcceleration(config.allowNvidia, config.allowQsv, config.allowVaapi,
                                           config.allowMediaToolbox, config.allowSW, config.hwDevicePath);
      transformer->setTargetFormat(encoder->getPreferredSwPixelFormat());

      if (!transformer->init()) {
        ERROR_MSG("Pipeline construction: Failed to initialize video transformer");
        return false;
      }

      size_t transformerId = addNode(transformer);
      if (transformerId == INVALID_NODE_ID) {
        ERROR_MSG("Pipeline construction: Failed to add video transformer");
        return false;
      }

      // Connect transformer to encoder
      size_t encoderId = INVALID_NODE_ID;
      for (const auto & pair : videoEncoders) {
        if (pair.second == encoder) {
          encoderId = pair.first;
          break;
        }
      }

      if (encoderId == INVALID_NODE_ID) {
        ERROR_MSG("Pipeline construction: Could not find encoder ID");
        return false;
      }

      if (!linkNodes(transformerId, encoderId)) {
        ERROR_MSG("Pipeline construction: Failed to connect transformer to encoder");
        return false;
      }

      // Create decoder and connect to transformer
      std::shared_ptr<VideoDecoderNode> decoder = createVideoDecoder(formatInfo, transformerId);
      if (!decoder) {
        ERROR_MSG("Pipeline construction: Failed to create video decoder");
        return false;
      }

      if (decoder->isHWAccelerated()) {
        AVBufferRef *deviceCtx = decoder->getHWDeviceContext();
        if (deviceCtx) {
          transformer->setSharedHardwareContext(deviceCtx, decoder->getHWPixelFormat(), decoder->getSWPixelFormat());
          if (encoder->isHardwareCodec()) {
            encoder->setSharedHardwareContext(deviceCtx, decoder->getHWPixelFormat(), decoder->getSWPixelFormat());
          }
          av_buffer_unref(&deviceCtx);
        }
      }

    } else {
      // No transformer needed - connect decoder directly to encoder
      size_t encoderId = INVALID_NODE_ID;
      for (const auto & pair : videoEncoders) {
        if (pair.second == encoder) {
          encoderId = pair.first;
          break;
        }
      }

      if (encoderId == INVALID_NODE_ID) {
        ERROR_MSG("Pipeline construction: Could not find encoder ID");
        return false;
      }

      // Create decoder and connect directly to encoder
      std::shared_ptr<VideoDecoderNode> decoder = createVideoDecoder(formatInfo, encoderId);
      if (!decoder) {
        ERROR_MSG("Pipeline construction: Failed to create video decoder");
        return false;
      }

      if (decoder->isHWAccelerated()) {
        AVBufferRef *deviceCtx = decoder->getHWDeviceContext();
        if (deviceCtx) {
          if (encoder->isHardwareCodec()) {
            encoder->setSharedHardwareContext(deviceCtx, decoder->getHWPixelFormat(), decoder->getSWPixelFormat());
          }
          av_buffer_unref(&deviceCtx);
        }
      }
    }

    MEDIUM_MSG("Pipeline construction: Created and connected video decoder (codec: %s)", formatInfo.codecName.c_str());
    return true;
  }

  bool NodePipeline::createAudioInputNodes(std::shared_ptr<AudioEncoderNode> encoder,
                                           const PacketContext::FormatInfo & formatInfo, const PipelineConfig & config) {
    if (!encoder) {
      ERROR_MSG("Pipeline construction: Invalid audio encoder");
      return false;
    }

    // Determine if we need a transformer
    bool needsTransformation = needsTransformer(config);

    if (needsTransformation) {
      // Create transformer using helper function to ensure encoder restrictions are applied
      std::vector<int> emptyChannelMap; // No channel mapping for normal audio
      size_t transformerId;

      if (!createAudioTransformerForEncoder(encoder, formatInfo, config, emptyChannelMap, transformerId)) {
        ERROR_MSG("Pipeline construction: Failed to create audio transformer");
        return false;
      }

      // Connect transformer to encoder
      size_t encoderId = INVALID_NODE_ID;
      for (const auto & pair : audioEncoders) {
        if (pair.second == encoder) {
          encoderId = pair.first;
          break;
        }
      }

      if (encoderId == INVALID_NODE_ID) {
        ERROR_MSG("Pipeline construction: Could not find encoder ID");
        return false;
      }

      if (!linkNodes(transformerId, encoderId)) {
        ERROR_MSG("Pipeline construction: Failed to connect transformer to encoder");
        return false;
      }

      // Create decoder and connect to transformer
      return createAudioDecoder(formatInfo, transformerId);
    } else {
      // Create decoder and connect directly to encoder
      size_t encoderId = INVALID_NODE_ID;
      for (const auto & pair : audioEncoders) {
        if (pair.second == encoder) {
          encoderId = pair.first;
          break;
        }
      }

      if (encoderId == INVALID_NODE_ID) {
        ERROR_MSG("Pipeline construction: Could not find encoder ID");
        return false;
      }

      return createAudioDecoder(formatInfo, encoderId);
    }
  }

  std::shared_ptr<VideoDecoderNode> NodePipeline::createVideoDecoder(const PacketContext::FormatInfo & formatInfo, size_t targetNodeId) {
    // Convert codec name from Mist format to FFmpeg format
    std::string ffmpegCodecName;
    if (formatInfo.codecName == "H264") {
      ffmpegCodecName = "h264";
    } else if (formatInfo.codecName == "HEVC" || formatInfo.codecName == "H265") {
      ffmpegCodecName = "hevc";
    } else if (formatInfo.codecName == "AV1") {
      ffmpegCodecName = "av1";
    } else if (formatInfo.codecName == "VP9") {
      ffmpegCodecName = "vp9";
    } else if (formatInfo.codecName == "JPEG") {
      ffmpegCodecName = "mjpeg";
    } else {
      // Use lowercase version as fallback
      ffmpegCodecName = formatInfo.codecName;
      std::transform(ffmpegCodecName.begin(), ffmpegCodecName.end(), ffmpegCodecName.begin(), ::tolower);
    }

    // Create decoder
    std::shared_ptr<VideoDecoderNode> decoder = std::make_shared<VideoDecoderNode>(ffmpegCodecName);
    if (!decoder) {
      ERROR_MSG("Pipeline construction: Failed to create video decoder for codec %s", ffmpegCodecName.c_str());
      return nullptr;
    }

    decoder->setHardwareAcceleration(config.allowNvidia, config.allowQsv, config.allowVaapi, config.allowMediaToolbox,
                                     config.allowSW, config.hwDevicePath);

    if (!decoder->init()) {
      ERROR_MSG("Pipeline construction: Failed to initialize video decoder");
      return nullptr;
    }

    size_t decoderId = addNode(decoder);
    if (decoderId == INVALID_NODE_ID) {
      ERROR_MSG("Pipeline construction: Failed to add video decoder");
      return nullptr;
    }

    // Connect decoder to target node
    if (!linkNodes(decoderId, targetNodeId)) {
      ERROR_MSG("Pipeline construction: Failed to connect decoder to target node");
      return nullptr;
    }
    return decoder;
  }

  bool NodePipeline::createAudioDecoder(const PacketContext::FormatInfo & formatInfo, size_t targetNodeId) {
    std::shared_ptr<AudioDecoderNode> decoder = std::make_shared<AudioDecoderNode>(
      static_cast<AVCodecID>(formatInfo.codecId), "", formatInfo.sampleRate, formatInfo.channels, formatInfo.bitDepth);

    if (!decoder) {
      ERROR_MSG("Pipeline construction: Failed to create audio decoder for codec ID %d", formatInfo.codecId);
      return false;
    }

    size_t decoderId = addNode(decoder);
    if (decoderId == INVALID_NODE_ID) {
      ERROR_MSG("Pipeline construction: Failed to add audio decoder");
      return false;
    }

    linkNodes(decoderId, targetNodeId);

    return true;
  }

  AudioTransformerNode noNode;

  ProcessingNode & NodePipeline::getNode(size_t nodeId) {
    if (videoNodes.count(nodeId)) { return *videoNodes.at(nodeId); }
    if (audioNodes.count(nodeId)) { return *audioNodes.at(nodeId); }
    if (videoDecoders.count(nodeId)) { return *videoDecoders.at(nodeId); }
    if (videoEncoders.count(nodeId)) { return *videoEncoders.at(nodeId); }
    if (audioDecoders.count(nodeId)) { return *audioDecoders.at(nodeId); }
    if (audioEncoders.count(nodeId)) { return *audioEncoders.at(nodeId); }
    return noNode;
  }

  bool NodePipeline::linkNodes(size_t in, size_t out) {
    auto & sourceNode = getNode(in);
    auto & targetNode = getNode(out);
    if (&sourceNode == &noNode || &targetNode == &noNode) { return false; }
    sourceNode.callbacks.push_back([&targetNode](void *d) { targetNode.setInput(d); });
    return true;
  }

  void NodePipeline::addCallback(std::function<void(void*)> cb){
    outCb = cb;
    for (auto & N : videoEncoders) { N.second->callbacks.push_back(cb); }
    for (auto & N : audioEncoders) { N.second->callbacks.push_back(cb); }
  }

  uint64_t NodePipeline::getDroppedFrameCount() const {
    uint64_t r = droppedFrameCount.load();
    for (auto & N : videoEncoders) { r += N.second->getDroppedFrameCount(); }
    for (auto & N : audioEncoders) { r += N.second->getDroppedFrameCount(); }
    for (auto & N : videoDecoders) { r += N.second->getDroppedFrameCount(); }
    for (auto & N : audioDecoders) { r += N.second->getDroppedFrameCount(); }
    for (auto & N : videoNodes) { r += N.second->getDroppedFrameCount(); }
    for (auto & N : audioNodes) { r += N.second->getDroppedFrameCount(); }
    return r;
  }

  uint64_t NodePipeline::getDecodeTime() const {
    uint64_t r = 0;
    for (auto & N : videoDecoders) { r += N.second->getProcessingTime(); }
    for (auto & N : audioDecoders) { r += N.second->getProcessingTime(); }
    return r;
  }

  uint64_t NodePipeline::getEncodeTime() const {
    uint64_t r = 0;
    for (auto & N : videoEncoders) { r += N.second->getProcessingTime(); }
    for (auto & N : audioEncoders) { r += N.second->getProcessingTime(); }
    return r;
  }

  uint64_t NodePipeline::getTransformTime() const {
    uint64_t r = 0;
    for (auto & N : videoNodes) { r += N.second->getProcessingTime(); }
    for (auto & N : audioNodes) { r += N.second->getProcessingTime(); }
    return r;
  }

  void NodePipeline::getSleepTimes(uint64_t &sinkSleepMicros, uint64_t &sourceSleepMicros) const {
    sinkSleepMicros = totalSinkSleep.load(std::memory_order_relaxed);
    sourceSleepMicros = totalSourceSleep.load(std::memory_order_relaxed);
  }

  void NodePipeline::updateSleepTimes(uint64_t sinkSleepMicros, uint64_t sourceSleepMicros) {
    totalSinkSleep.fetch_add(sinkSleepMicros, std::memory_order_relaxed);
    totalSourceSleep.fetch_add(sourceSleepMicros, std::memory_order_relaxed);
  }

} // namespace FFmpeg
