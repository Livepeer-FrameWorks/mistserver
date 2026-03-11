#pragma once

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
#include <libavutil/time.h>
}

namespace FFmpeg {

  constexpr size_t DEFAULT_QUEUE_SIZE = 30;

  // Helper template for safe context casting
  template<typename SrcType, typename DstType>
  std::shared_ptr<DstType> safeCastContext(const std::shared_ptr<SrcType> & src) {
    if (!src) return nullptr;
    try {
      return std::dynamic_pointer_cast<DstType>(src);
    } catch (...) { return nullptr; }
  }

  /// @brief Node configuration parameters
  struct NodeConfig {
      // Hardware acceleration settings
      bool allowNvidia{true};
      bool allowQsv{true};
      bool allowVaapi{true};
      bool allowMediaToolbox{true};
      bool allowSW{true};
  };

  /// @brief Base class for all processing nodes in the pipeline
  class ProcessingNode {
    public:
      // Constructor
      explicit ProcessingNode() {}
      explicit ProcessingNode(const NodeConfig & cfg) : config(cfg) {}

      // Callbacks
      std::deque<std::function<void(void *)>> callbacks;

    protected:
      // Thread safety
      mutable std::mutex mutex;

      // Performance tracking
      std::atomic<uint64_t> totalProcessingTime{0};
      std::atomic<uint64_t> droppedFrames{0};

      // Move hardware flags to config
      NodeConfig config;

      // Node ID for identification in the pipeline
      size_t nodeId{0};

      std::string longName;

      // Input parameters
      uint64_t width{0};
      uint64_t height{0};
      uint64_t bitrate{0};
      uint64_t sampleRate{0};
      uint64_t channels{0};
      uint64_t bitDepth{0};
      uint64_t fpks{0}; // Frames per kilosecond
      std::string initData;

      // Hardware acceleration info
      struct HWInfo {
          AVBufferRef *context{nullptr};
          AVHWDeviceType type{AV_HWDEVICE_TYPE_NONE};
          AVPixelFormat format{AV_PIX_FMT_NONE};
          AVPixelFormat swFormat{AV_PIX_FMT_NONE};
          std::string deviceName;
          bool isEnabled{false};

          void cleanup() {
            if (context) {
              av_buffer_unref(&context);
              context = nullptr;
            }
            format = AV_PIX_FMT_NONE;
            swFormat = AV_PIX_FMT_NONE;
            deviceName.clear();
            isEnabled = false;
          }
      } hwInfo;

      // Hardware acceleration methods
      virtual bool configureHardwareAcceleration() { return true; }

      void trackFrameDrop() { droppedFrames.fetch_add(1, std::memory_order_relaxed); }

      // Hardware acceleration
      virtual bool isHWAccelerated() const { return hwInfo.isEnabled; }

    public:
      /// @brief Virtual destructor
      virtual ~ProcessingNode() {
        hwInfo.cleanup();
      }

      /// @brief Remove this node from the pipeline
      /// @return True if removal succeeded
      virtual bool remove() {
        std::lock_guard<std::mutex> lock(mutex);
        callbacks.clear();
        return true;
      }

      /// @brief Initialize the node with current configuration
      /// @return True if initialization succeeded
      virtual bool init() = 0;

      /// @brief Initialize hardware acceleration
      /// @param hwContext Hardware context to use
      /// @return True if initialization succeeded
      virtual bool initHW(AVBufferRef *hwContext) = 0;

      /// @brief Set hardware acceleration flags
      /// @param nvidia Allow NVIDIA acceleration
      /// @param qsv Allow QuickSync acceleration
      /// @param vaapi Allow VAAPI acceleration
      /// @param sw Allow software processing
      /// @param mediaToolbox Allow MediaToolbox acceleration
      void setHWFlags(bool nvidia, bool qsv, bool vaapi, bool sw, bool mediaToolbox) {
        std::lock_guard<std::mutex> lock(mutex);
        config.allowNvidia = nvidia;
        config.allowQsv = qsv;
        config.allowVaapi = vaapi;
        config.allowSW = sw;
        config.allowMediaToolbox = mediaToolbox;
      }

      /// @brief Check if this is an encoder node
      /// @return True if this is an encoder node
      virtual bool isEncoderNode() const { return false; }

      /// @brief Get total processing time in microseconds
      /// @return Total processing time
      uint64_t getProcessingTime() const { return totalProcessingTime.load(std::memory_order_relaxed); }

      /// @brief Get number of frames dropped
      /// @return Dropped frame count
      uint64_t getDroppedFrameCount() const { return droppedFrames.load(std::memory_order_relaxed); }

      /// @brief Get node name
      /// @return Node name
      const std::string & getLongName() const { return longName; }

      /// @brief Set an input frame
      /// @param frame Input frame
      /// @param idx Input index
      /// @return True if input was accepted
      virtual bool setInput(void * inputData, size_t idx = 0) = 0;

      // Hardware acceleration methods
      virtual AVHWDeviceType getHWDeviceType() const { return AV_HWDEVICE_TYPE_NONE; }

      /// @brief Set node ID
      /// @param id Node ID
      void setNodeId(size_t id) {
        nodeId = id;
      }

      /// @brief Get node ID
      /// @return Node ID
      size_t getNodeId() const {
        return nodeId;
      }

      // Clear outputs (thread-safe)
      void clearOutputs() {
        std::lock_guard<std::mutex> lock(mutex);
        callbacks.clear();
      }

      /// @brief Get current node configuration
      /// @return Node configuration
      const NodeConfig & getConfig() const { return config; }

      // Common getters
      uint64_t getWidth() const { return width; }
      uint64_t getHeight() const { return height; }
      uint64_t getSampleRate() const { return sampleRate; }
      uint64_t getChannels() const { return channels; }
      uint64_t getBitDepth() const { return bitDepth; }
      uint64_t getFrameRate() const { return fpks; }
      uint64_t getBitrate() const { return bitrate; }

      void setSampleRate(uint64_t rate) { sampleRate = rate; }

      void setChannels(uint64_t ch) { channels = ch; }

      void setBitDepth(uint64_t depth) { bitDepth = depth; }

      void setFrameRate(uint64_t rate) { fpks = rate; }

      void setInitData(const std::string & data) { initData = data; }

      void setWidth(uint64_t w) { width = w; }

      void setHeight(uint64_t h) { height = h; }

      /// @brief Set target bitrate
      /// @param bitrate Bitrate in bits/s
      void setBitrate(int64_t _bitrate) { bitrate = _bitrate; }
  };

} // namespace FFmpeg
