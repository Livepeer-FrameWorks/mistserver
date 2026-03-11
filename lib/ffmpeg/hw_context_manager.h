#pragma once
#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

namespace FFmpeg {

  /// @brief Comprehensive information about a hardware device
  struct HWDeviceInfo {
      AVHWDeviceType type; ///< FFmpeg hardware device type
      AVPixelFormat hwFormat; ///< Hardware pixel format
      AVPixelFormat swFormat; ///< Software pixel format
      std::string name; ///< Human readable name
      std::string devicePath; ///< Specific device path (for multiple devices of same type)

      // Device capabilities (merged from DeviceCapabilities)
      std::vector<AVPixelFormat> supportedPixelFormats; ///< Supported pixel formats
      uint32_t maxWidth{0}; ///< Maximum supported width
      uint32_t maxHeight{0}; ///< Maximum supported height
      uint64_t maxPixels{0}; ///< Maximum total pixels
      uint64_t maxBitrate{0}; ///< Maximum bitrate in bits/sec

      HWDeviceInfo() = default;
      HWDeviceInfo(const HWDeviceInfo &) = default;
      HWDeviceInfo & operator=(const HWDeviceInfo &) = default;
      HWDeviceInfo(HWDeviceInfo && other) noexcept = default;
      HWDeviceInfo & operator=(HWDeviceInfo && other) noexcept = default;
  };

  /// @brief Hardware device constraints
  struct HWDeviceConstraints {
      bool allowNvidia{true}; ///< Allow NVIDIA CUDA devices
      bool allowQsv{true}; ///< Allow Intel QuickSync devices
      bool allowVaapi{true}; ///< Allow VAAPI devices
      bool allowMediaToolbox{true}; ///< Allow Apple MediaToolbox
      bool allowSW{true}; ///< Allow software fallback
      std::string devicePath; ///< Optional specific device path
  };

  /// @brief Manages hardware acceleration contexts and device selection
  class HWContextManager {
    public:
      /// @brief Get singleton instance
      static HWContextManager & getInstance();

      /// @brief Initialize hardware device
      /// @param type Hardware device type
      /// @param device Optional device name
      /// @return Hardware device context or nullptr if failed
      AVBufferRef *initDevice(AVHWDeviceType type, const char *device = nullptr);

      /// @brief Get frames context for hardware device
      /// @param hwContext Hardware device context
      /// @param width Frame width
      /// @param height Frame height
      /// @param hwFormat Hardware pixel format
      /// @param swFormat Software pixel format
      /// @param initialPoolSize Initial pool size
      /// @return Frames context or nullptr if failed
      AVBufferRef *getFramesContext(AVBufferRef *hwContext, int width, int height, AVPixelFormat hwFormat,
                                    AVPixelFormat swFormat, int initialPoolSize = 32);

      /// @brief Check if format conversion is supported
      /// @param srcFormat Source format
      /// @param dstFormat Destination format
      /// @param hwType Hardware device type
      /// @return True if conversion is supported
      bool isFormatConversionSupported(AVPixelFormat srcFormat, AVPixelFormat dstFormat, AVHWDeviceType hwType);

      /// @brief Get supported pixel formats for hardware device
      /// @param hwType Hardware device type
      /// @return Vector of supported formats
      std::vector<AVPixelFormat> getSupportedPixelFormats(AVHWDeviceType hwType);

      /// @brief Check if device supports format
      /// @param type Hardware device type
      /// @param format Pixel format
      /// @return True if format supported
      bool supportsFormat(AVHWDeviceType type, AVPixelFormat format) const;

      /// @brief Check if device supports resolution
      /// @param type Hardware device type
      /// @param width Frame width
      /// @param height Frame height
      /// @return True if resolution supported
      bool supportsResolution(AVHWDeviceType type, int width, int height) const;

      /// @brief Set device constraints
      /// @param constraints Device constraints to apply
      void setDeviceConstraints(const HWDeviceConstraints & constraints);

      std::vector<HWDeviceInfo> getAvailableDevices() const;

    private:
      HWContextManager();
      ~HWContextManager();
      HWContextManager(const HWContextManager &) = delete;
      HWContextManager & operator=(const HWContextManager &) = delete;

      struct FramesKey {
          const void *hwContextData;
          int width;
          int height;
          AVPixelFormat hwFormat;
          AVPixelFormat swFormat;
          int initialPool;
          bool operator==(const FramesKey & other) const {
            return hwContextData == other.hwContextData && width == other.width && height == other.height &&
              hwFormat == other.hwFormat && swFormat == other.swFormat && initialPool == other.initialPool;
          }
      };

      struct FramesKeyHash {
          size_t operator()(const FramesKey & key) const noexcept {
            size_t h = std::hash<const void *>{}(key.hwContextData);
            h ^= std::hash<int>{}(key.width) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<int>{}(key.height) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<int>{}(static_cast<int>(key.hwFormat)) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<int>{}(static_cast<int>(key.swFormat)) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<int>{}(key.initialPool) + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
          }
      };

      void initFormatCompatibility();
      void initAvailableHardware();
      void rebuildAvailableDevicesLocked();
      bool isDeviceAllowed(AVHWDeviceType type) const;
      bool isTypeSupportedOnPlatform(AVHWDeviceType type) const;
      bool probeDevice(AVHWDeviceType type, const char *device) const;
      std::string resolveDevicePath(AVHWDeviceType type) const;
      size_t calculateFrameSize(int width, int height, AVPixelFormat format) const;
      void initDeviceCapabilities(HWDeviceInfo & device);

      std::vector<HWDeviceInfo> availableHardware;
      std::unordered_map<AVHWDeviceType, std::vector<AVPixelFormat>> supportedFormats;
      std::unordered_map<FramesKey, AVBufferRef *, FramesKeyHash> framesContextCache;
      mutable std::mutex managerMutex;
      HWDeviceConstraints deviceConstraints;
  };

} // namespace FFmpeg
