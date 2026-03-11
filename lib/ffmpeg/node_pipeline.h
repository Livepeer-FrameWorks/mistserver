#pragma once

#include "audio_decoder_node.h"
#include "audio_encoder_node.h"
#include "packet_context.h"
#include "utils.h"
#include "video_decoder_node.h"
#include "video_encoder_node.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <libavutil/hwcontext.h>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace FFmpeg {

  static const size_t INVALID_NODE_ID = (size_t)-1;

  // Forward declarations
  struct PipelineConfig;

  // Pipeline configuration struct
  struct PipelineConfig {
      bool isVideo{true};
      std::string codecOut;
      int quality{20};
      int bitrate{2000000};
      std::string resolution;
      std::string rateControlMode{"bitrate"};
      int crf{0};
      int qp{0};
      int cq{0};
      uint64_t maxBitrate{0};
      uint64_t vbvBufferSize{0};
      std::string rateControlRcOption;

      // Hardware acceleration config
      bool allowNvidia{true};
      bool allowQsv{true};
      bool allowVaapi{false};
      bool allowMediaToolbox{true};
      bool allowSW{true};
      std::string hwDevicePath; // Device path (e.g. /dev/dri/renderD128 for VAAPI)

      // Encoding parameters
      int gopSize{0};
      std::string tune{"zerolatency"};
      std::string preset{"faster"};

      // Video target parameters
      uint32_t targetWidth{0};
      uint32_t targetHeight{0};

      // Audio target parameters
      uint32_t targetSampleRate{0};
      uint64_t targetBitDepth{0};

      // Audio parameters
      std::vector<int> splitChannels;
  };

  // Forward declarations
  class VideoDecoderNode;
  class VideoEncoderNode;
  class VideoTransformerNode;
  class AudioDecoderNode;
  class AudioEncoderNode;
  class AudioTransformerNode;
  class PacketContext;
  class VideoFrameContext;
  class AudioFrameContext;

  struct NodeConnection {
      size_t sourceNodeId;
      size_t sourceOutputIdx;
      size_t destNodeId;
      size_t destInputIdx;
  };

  struct ProcessingPath {
      size_t pathId{0};
      std::vector<size_t> nodeIds;
      std::string format;
      uint64_t width{0};
      uint64_t height{0};
      uint64_t bitrate{0};
      std::string preset;
      int quality{0};
      bool hwPreferred{false};
  };

  /**
   * @brief FFmpeg pipeline for video/audio processing
   *
   * Thread Safety:
   * - All public methods are thread-safe unless explicitly noted
   * - Focused locking strategy:
   *   - queueMutex for queue operations
   *   - configMutex for configuration changes (read/write)
   *   - stateMutex for pipeline state
   * - Condition variables for synchronization:
   *   - queueCV for queue operations
   *   - stateCV for state changes
   * - Memory management is thread-safe with proper cleanup
   * - Queue operations are protected against overflow
   * - Metrics collection is atomic and thread-safe
   */
  class NodePipeline {
    public:
      // Configuration methods
      bool configure(const PipelineConfig & config);

      /// @brief Create a new pipeline
      /// @param isVideo Whether this is a video pipeline
      explicit NodePipeline(bool isVideo = true) : nextNodeId(0), isActive(false), isVideo(isVideo) {
        config.isVideo = isVideo;
        av_log_set_callback(FFmpegUtils::logCallback);
        FFmpegUtils::setLogLevel(FFmpegUtils::getLogLevel());
      }

      /// @brief Destructor
      virtual ~NodePipeline();

      // Template declarations only
      template<typename NodeType> size_t addNode(std::shared_ptr<NodeType> node);

      template<typename NodeType> std::vector<std::shared_ptr<NodeType>> getNodesOfType() const;

      /// @brief Get the current input codec name
      /// @return Input codec name or nullptr if not set
      const char *getCodecIn() const;

      /// @brief Set the input codec name
      /// @param codec Input codec name
      void setCodecIn(const char *codec);

      /// @brief Wait for pipeline to become inactive
      /// @param timeout Maximum time to wait in milliseconds
      /// @return True if pipeline became inactive, false if timeout occurred
      bool waitForInactive(std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

      /// @brief Wait for packets to become available for processing
      /// @param timeout Maximum time to wait
      /// @return True if packets are available, false on timeout
      bool waitForPackets(std::chrono::milliseconds timeout = std::chrono::milliseconds(1000));

      bool linkNodes(size_t inputNode, size_t outputNode);
      ProcessingNode & getNode(size_t nodeId);

      // Performance metrics
      uint64_t getDroppedFrameCount() const;
      uint64_t getDecodeTime() const;
      uint64_t getEncodeTime() const;
      uint64_t getTransformTime() const;

      /// @brief Set whether the pipeline is active
      /// @param active Whether the pipeline is active
      virtual void setIsActive(bool active);

      /// @brief Get whether this is a video pipeline
      /// @return True if this is a video pipeline
      bool getIsVideo() const { return isVideo.load(); }

      /// @brief Set whether this is a video pipeline
      /// @param isVid Whether this is a video pipeline
      void setIsVideo(bool isVid) { isVideo.store(isVid); }

      /// @brief Get sleep times
      /// @param sinkSleepMicros Total sink sleep time in microseconds
      /// @param sourceSleepMicros Total source sleep time in microseconds
      void getSleepTimes(uint64_t & sinkSleepMicros, uint64_t & sourceSleepMicros) const;

      /// @brief Update sleep times
      /// @param sinkSleepMicros Sink sleep time to add in microseconds
      /// @param sourceSleepMicros Source sleep time to add in microseconds
      void updateSleepTimes(uint64_t sinkSleepMicros, uint64_t sourceSleepMicros);

      /// @brief Receive video data
      /// @param sendTime Send time
      /// @param time Presentation time
      /// @param data Video data
      /// @param size Data size
      /// @param width Frame width
      /// @param height Frame height
      /// @param init Initialization data
      /// @param initSize Initialization data size
      /// @param fpks Frames per kilosecond
      /// @param codec Codec name
      /// @param isKeyframe Whether the frame is a keyframe
      void receiveVideo(uint64_t sendTime, uint64_t time, const char *data, size_t size, uint64_t width,
                        uint64_t height, const std::string & init, uint64_t fpks, const char *codec, bool isKeyframe);

      /// @brief Receive audio data
      /// @param sendTime Send time
      /// @param time Presentation time
      /// @param data Audio data
      /// @param size Data size
      /// @param bitDepth Sample bit depth
      /// @param channels Channel count
      /// @param sampleRate Sample rate
      /// @param init Initialization data
      /// @param codec Codec name
      void receiveAudio(uint64_t sendTime, uint64_t time, const char *data, size_t size, uint64_t bitDepth,
                        uint64_t channels, uint64_t sampleRate, const std::string & init, const std::string & codec);

      /// @brief Signal that we're done processing an output packet
      /// @param packet PacketContext that was processed
      void signalPacketProcessed(const std::shared_ptr<PacketContext> & packet);

      /// @brief Check if a codec is a raw format (no encoding needed)
      /// @param codec Codec name to check
      /// @return True if the codec is a raw format
      static bool isRawFormat(const std::string & codec);

      // Graph cycle detection
      size_t getNextNodeId() const { return nextNodeId; }

      // NEW: Backward construction methods
      /// @brief Create complete node graph using backward construction
      /// @param packet First packet containing input format information
      /// @return True if node graph was created successfully
      bool createNodeGraphBackwards(PacketContext *packet);

      /// @brief Recursively create input nodes for a target node
      /// @param targetNode The node that needs input
      /// @param availableFormat The format available from the source
      /// @return True if input nodes were created and connected successfully
      template<typename NodeType>
      bool createInputNodes(std::shared_ptr<NodeType> targetNode, const PacketContext::FormatInfo & availableFormat);

      /// @brief Create video input nodes (transformer + decoder) for an encoder
      /// @param encoder The video encoder that needs input nodes
      /// @param formatInfo Format information from input packet
      /// @param isRaw Whether the output format is raw (no encoder needed)
      /// @return True if input nodes were created and connected successfully
      bool createVideoInputNodes(std::shared_ptr<VideoEncoderNode> encoder, const PacketContext::FormatInfo & formatInfo, bool isRaw);

      /// @brief Create audio input nodes (transformer + decoder) for an encoder
      /// @param encoder The audio encoder that needs input nodes
      /// @param formatInfo Format information from input packet
      /// @param config Pipeline configuration
      /// @return True if input nodes were created and connected successfully
      bool createAudioInputNodes(std::shared_ptr<AudioEncoderNode> encoder,
                                 const PacketContext::FormatInfo & formatInfo, const PipelineConfig & config);

      /// @brief Create video decoder and connect to target node
      /// @param formatInfo Format information from input packet
      /// @param targetNodeId ID of the node to connect decoder to
      /// @return True if decoder was created and connected successfully
      std::shared_ptr<VideoDecoderNode> createVideoDecoder(const PacketContext::FormatInfo & formatInfo, size_t targetNodeId);

      /// @brief Create audio decoder and connect to target node
      /// @param formatInfo Format information from input packet
      /// @param targetNodeId ID of the node to connect decoder to
      /// @return True if decoder was created and connected successfully
      bool createAudioDecoder(const PacketContext::FormatInfo & formatInfo, size_t targetNodeId);

      /// @brief Create audio transformer for encoder with proper encoder restrictions
      /// @param encoder The audio encoder that needs the transformer
      /// @param formatInfo Format information from input packet
      /// @param config Pipeline configuration
      /// @param channelMap Channel mapping for split audio (empty for normal audio)
      /// @param transformerId Output parameter for the created transformer ID
      /// @return True if transformer was created and configured successfully
      bool createAudioTransformerForEncoder(std::shared_ptr<AudioEncoderNode> encoder,
                                            const PacketContext::FormatInfo & formatInfo, const PipelineConfig & config,
                                            const std::vector<int> & channelMap, size_t & transformerId);

      // Node name getters
      const std::string & getDecoderName();
      const std::string & getEncoderName();
      const std::string & getTransformerName();

      void addCallback(std::function<void(void *)>);

    protected:
      // Node management helpers
      bool needsTransformer(const PipelineConfig & config) const;

    private:
      std::function<void(void *)> outCb;

      AVPacket *inPacket{0};

      // Thread safety - focused mutexes for different concerns
      mutable std::mutex queueMutex; // Protects frame queues
      std::condition_variable queueCV; // Signals queue state changes

      mutable std::mutex configMutex; // Protects pipeline configuration

      mutable std::mutex stateMutex; // Protects pipeline state
      std::condition_variable stateCV; // Signals state changes

      // Node storage with correct template parameters
      std::unordered_map<size_t, std::shared_ptr<VideoTransformerNode>> videoNodes;
      std::unordered_map<size_t, std::shared_ptr<AudioTransformerNode>> audioNodes;
      std::unordered_map<size_t, std::shared_ptr<VideoDecoderNode>> videoDecoders;
      std::unordered_map<size_t, std::shared_ptr<AudioDecoderNode>> audioDecoders;
      std::unordered_map<size_t, std::shared_ptr<VideoEncoderNode>> videoEncoders;
      std::unordered_map<size_t, std::shared_ptr<AudioEncoderNode>> audioEncoders;
      size_t nextNodeId{0};
      size_t nextPathId{0};

      // Backward construction state
      bool nodeGraphConstructed{false};

      // Pipeline state
      std::atomic<bool> isActive{false};
      std::atomic<bool> isVideo{true};
      std::string codecIn; // Input codec name

      // Metrics (atomic)
      std::atomic<uint64_t> droppedFrameCount;
      std::atomic<uint64_t> totalSinkSleep{0};
      std::atomic<uint64_t> totalSourceSleep{0};

      // Configuration
      PipelineConfig config;

      FFmpeg::PipelineConfig getConfig() const;

  }; // End of class NodePipeline
} // namespace FFmpeg
