#include "hw_context_manager.h"

#include "../defines.h"
#include "utils.h"

#include <algorithm>
#include <array>
#include <mutex>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}
namespace FFmpeg {

  HWContextManager & HWContextManager::getInstance() {
    static HWContextManager instance;
    return instance;
  }

  HWContextManager::HWContextManager() {
    initFormatCompatibility();

#if defined(__APPLE__)
    deviceConstraints.allowNvidia = false;
    deviceConstraints.allowQsv = false;
    deviceConstraints.allowVaapi = false;
    deviceConstraints.allowMediaToolbox = true;
#elif defined(_WIN32)
    deviceConstraints.allowVaapi = false;
    deviceConstraints.allowMediaToolbox = false;
#else
    deviceConstraints.allowVaapi = false;
    deviceConstraints.allowMediaToolbox = false;
#endif
  }

  HWContextManager::~HWContextManager() {
    // Clear format cache
    supportedFormats.clear();
    for (auto & entry : framesContextCache) {
      if (entry.second) { av_buffer_unref(&entry.second); }
    }
    framesContextCache.clear();
  }

  void HWContextManager::setDeviceConstraints(const HWDeviceConstraints & constraints) {
    std::lock_guard<std::mutex> lock(managerMutex);

    if (deviceConstraints.allowNvidia == constraints.allowNvidia && deviceConstraints.allowQsv == constraints.allowQsv &&
        deviceConstraints.allowVaapi == constraints.allowVaapi && deviceConstraints.allowMediaToolbox == constraints.allowMediaToolbox &&
        deviceConstraints.allowSW == constraints.allowSW && deviceConstraints.devicePath == constraints.devicePath) {
      if (availableHardware.empty()) {
        // Ensure we probe hardware at least once even when constraints are unchanged.
        rebuildAvailableDevicesLocked();
      }
      return;
    }

    deviceConstraints = constraints;
    rebuildAvailableDevicesLocked();
  }

  void HWContextManager::initAvailableHardware() {
    std::lock_guard<std::mutex> lock(managerMutex);
    rebuildAvailableDevicesLocked();
  }

  void HWContextManager::rebuildAvailableDevicesLocked() {
    availableHardware.clear();

    bool hasNvidia = false;
    bool hasIntel = false;
    bool hasVaapi = false;
    bool hasMacGPU = false;

#ifdef __APPLE__
    hasMacGPU = true;
#endif

    static const std::array<AVHWDeviceType, 4> preferredTypes = {AV_HWDEVICE_TYPE_CUDA, AV_HWDEVICE_TYPE_QSV,
                                                                 AV_HWDEVICE_TYPE_VIDEOTOOLBOX, AV_HWDEVICE_TYPE_VAAPI};

    FFmpegUtils::setLogLevel(AV_LOG_PANIC);
    for (AVHWDeviceType type : preferredTypes) {
      if (!isDeviceAllowed(type)) { continue; }
      if (!isTypeSupportedOnPlatform(type)) {
        INFO_MSG("HWContextManager: Skipping %s hardware probe on unsupported platform", av_hwdevice_get_type_name(type));
        continue;
      }

      std::string devicePath = resolveDevicePath(type);
      const char *pathPtr = devicePath.empty() ? nullptr : devicePath.c_str();


      if (!probeDevice(type, pathPtr)) { continue; }

      HWDeviceInfo device;
      device.type = type;
      device.name = av_hwdevice_get_type_name(type);
      device.devicePath = devicePath;

      switch (type) {
        case AV_HWDEVICE_TYPE_CUDA:
          device.hwFormat = AV_PIX_FMT_CUDA;
          device.swFormat = AV_PIX_FMT_NV12;
          hasNvidia = true;
          break;
        case AV_HWDEVICE_TYPE_QSV:
          device.hwFormat = AV_PIX_FMT_QSV;
          device.swFormat = AV_PIX_FMT_NV12;
          hasIntel = true;
          break;
        case AV_HWDEVICE_TYPE_VIDEOTOOLBOX:
          device.hwFormat = AV_PIX_FMT_VIDEOTOOLBOX;
          device.swFormat = AV_PIX_FMT_NV12;
          break;
        case AV_HWDEVICE_TYPE_VAAPI:
          device.hwFormat = AV_PIX_FMT_VAAPI;
          device.swFormat = AV_PIX_FMT_NV12;
          hasVaapi = true;
          break;
        default: continue;
      }

      initDeviceCapabilities(device);

      availableHardware.push_back(device);
      INFO_MSG("HWContextManager: %s hardware acceleration available%s%s", device.name.c_str(),
               device.devicePath.empty() ? "" : " @ ", device.devicePath.empty() ? "" : device.devicePath.c_str());
    }
    FFmpegUtils::setLogLevel(AV_LOG_WARNING);

    if (availableHardware.empty()) {
      INFO_MSG("HWContextManager: No hardware acceleration available, using software only");
    } else {
      std::string caps;
      if (hasNvidia) caps += "NVIDIA ";
      if (hasIntel) caps += "Intel ";
      if (hasVaapi) caps += "VAAPI ";
      if (hasMacGPU) caps += "Apple ";
      if (!caps.empty() && caps.back() == ' ') { caps.pop_back(); }
      if (caps.empty()) { caps = "None"; }
      INFO_MSG("HWContextManager: Detected hardware capabilities: %s", caps.c_str());
    }
  }

  bool HWContextManager::isDeviceAllowed(AVHWDeviceType type) const {
    switch (type) {
      case AV_HWDEVICE_TYPE_CUDA: return deviceConstraints.allowNvidia;
      case AV_HWDEVICE_TYPE_QSV: return deviceConstraints.allowQsv;
      case AV_HWDEVICE_TYPE_VAAPI: return deviceConstraints.allowVaapi;
      case AV_HWDEVICE_TYPE_VIDEOTOOLBOX: return deviceConstraints.allowMediaToolbox;
      default: return deviceConstraints.allowSW;
    }
  }

  std::string HWContextManager::resolveDevicePath(AVHWDeviceType type) const {
    if (deviceConstraints.devicePath.empty()) { return std::string(); }

    switch (type) {
      case AV_HWDEVICE_TYPE_VAAPI:
      case AV_HWDEVICE_TYPE_QSV: return deviceConstraints.devicePath;
      default: return std::string();
    }
  }

  bool HWContextManager::probeDevice(AVHWDeviceType type, const char *device) const {
    AVBufferRef *ctx = nullptr;
    int ret = av_hwdevice_ctx_create(&ctx, type, device, nullptr, 0);
    if (ret < 0) {
      char err[128];
      av_strerror(ret, err, sizeof(err));
      INFO_MSG("HWContextManager: Failed to probe %s device%s%s: %s", av_hwdevice_get_type_name(type),
               (device && device[0]) ? " @ " : "", (device && device[0]) ? device : "", err);
      if (ctx) { av_buffer_unref(&ctx); }
      return false;
    }

    if (!ctx) {
      ERROR_MSG("HWContextManager: Probe succeeded but returned null context for %s", av_hwdevice_get_type_name(type));
      return false;
    }

    av_buffer_unref(&ctx);
    VERYHIGH_MSG("HWContextManager: Probe succeeded for %s%s%s", av_hwdevice_get_type_name(type),
                 (device && device[0]) ? " @ " : "", (device && device[0]) ? device : "");
    return true;
  }

  bool HWContextManager::isTypeSupportedOnPlatform(AVHWDeviceType type) const {
#if defined(__APPLE__)
    return type == AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
#elif defined(_WIN32)
    return type == AV_HWDEVICE_TYPE_CUDA || type == AV_HWDEVICE_TYPE_QSV;
#else
    return type == AV_HWDEVICE_TYPE_CUDA || type == AV_HWDEVICE_TYPE_QSV || type == AV_HWDEVICE_TYPE_VAAPI;
#endif
  }

  AVBufferRef *HWContextManager::initDevice(AVHWDeviceType type, const char *device) {
    // Create hardware device context - let libav handle device-specific paths
    AVBufferRef *ctx = nullptr;
    int ret = av_hwdevice_ctx_create(&ctx, type, device, nullptr, 0);

    if (ret < 0) {
      FFmpegUtils::printAvError(
        std::string("HWContextManager: Failed to create ") + av_hwdevice_get_type_name(type) + " device context", ret);
      return nullptr;
    }

    if (!ctx) {
      ERROR_MSG("HWContextManager: Device context creation returned success but null context for %s",
                av_hwdevice_get_type_name(type));
      return nullptr;
    }

    MEDIUM_MSG("HWContextManager: Successfully created %s device context", av_hwdevice_get_type_name(type));
    return ctx;
  }

  AVBufferRef *HWContextManager::getFramesContext(AVBufferRef *hwContext, int width, int height, AVPixelFormat hwFormat,
                                                  AVPixelFormat swFormat, int initialPoolSize) {
    if (!hwContext) return nullptr;

    std::lock_guard<std::mutex> lock(managerMutex);

    FramesKey key{hwContext->data, width, height, hwFormat, swFormat, initialPoolSize};
    auto cachedIt = framesContextCache.find(key);
    if (cachedIt != framesContextCache.end() && cachedIt->second) { return av_buffer_ref(cachedIt->second); }

    AVBufferRef *framesRef = av_hwframe_ctx_alloc(hwContext);
    if (!framesRef) {
      ERROR_MSG("HWContextManager: Failed to create hardware frames context");
      return nullptr;
    }

    AVHWFramesContext *framesCtx = (AVHWFramesContext *)framesRef->data;
    framesCtx->format = hwFormat;
    framesCtx->sw_format = swFormat;
    framesCtx->width = width;
    framesCtx->height = height;
    framesCtx->initial_pool_size = initialPoolSize;

    int ret = av_hwframe_ctx_init(framesRef);
    if (ret < 0) {
      FFmpegUtils::printAvError("HWContextManager: Failed to initialize hardware frames context", ret);
      av_buffer_unref(&framesRef);
      return nullptr;
    }

    framesContextCache[key] = framesRef;
    return av_buffer_ref(framesRef);
  }

  bool HWContextManager::isFormatConversionSupported(AVPixelFormat srcFormat, AVPixelFormat dstFormat, AVHWDeviceType hwType) {
    // Check if formats are identical
    if (srcFormat == dstFormat) return true;

    // Get supported formats for this hardware type
    auto it = supportedFormats.find(hwType);
    if (it == supportedFormats.end()) {
      MEDIUM_MSG("HWContextManager: No format support information for hardware type %d", hwType);
      return false;
    }

    // Check if both formats are in supported list
    const auto & formats = it->second;
    bool srcSupported = std::find(formats.begin(), formats.end(), srcFormat) != formats.end();
    bool dstSupported = std::find(formats.begin(), formats.end(), dstFormat) != formats.end();

    if (!srcSupported || !dstSupported) {
      MEDIUM_MSG("HWContextManager: Format conversion %d -> %d not supported by hardware type %d", srcFormat, dstFormat, hwType);
      return false;
    }

    // Special case: direct hardware-to-hardware conversion
    if (av_pix_fmt_desc_get(srcFormat)->flags & AV_PIX_FMT_FLAG_HWACCEL && av_pix_fmt_desc_get(dstFormat)->flags & AV_PIX_FMT_FLAG_HWACCEL) {
      // Only allow conversion between same hardware type
      return srcFormat == dstFormat;
    }

    // Check specific conversion restrictions
    switch (hwType) {
      case AV_HWDEVICE_TYPE_CUDA:
        // CUDA supports most conversions between supported formats
        return true;

      case AV_HWDEVICE_TYPE_QSV:
        // QSV has some format restrictions
        if (srcFormat == AV_PIX_FMT_QSV || dstFormat == AV_PIX_FMT_QSV) {
          return true; // QSV can convert to/from its hardware format
        }
        break;

      case AV_HWDEVICE_TYPE_VAAPI:
        // VAAPI supports conversion between most supported formats
        if (srcFormat == AV_PIX_FMT_VAAPI || dstFormat == AV_PIX_FMT_VAAPI) { return true; }
        break;

      case AV_HWDEVICE_TYPE_VIDEOTOOLBOX:
        // VideoToolbox has limited format conversion support
        if (srcFormat == AV_PIX_FMT_VIDEOTOOLBOX || dstFormat == AV_PIX_FMT_VIDEOTOOLBOX) {
          // Only allow conversion to/from NV12 and P010LE
          AVPixelFormat otherFormat = (srcFormat == AV_PIX_FMT_VIDEOTOOLBOX) ? dstFormat : srcFormat;
          return otherFormat == AV_PIX_FMT_NV12 || otherFormat == AV_PIX_FMT_P010LE;
        }
        break;

      default: MEDIUM_MSG("HWContextManager: Unknown hardware type %d", hwType); return false;
    }

    return false;
  }

  std::vector<AVPixelFormat> HWContextManager::getSupportedPixelFormats(AVHWDeviceType hwType) {
    auto it = supportedFormats.find(hwType);
    if (it != supportedFormats.end()) { return it->second; }
    return std::vector<AVPixelFormat>();
  }

  void HWContextManager::initFormatCompatibility() {
    // Initialize format compatibility matrix for each hardware type
    std::unordered_map<AVHWDeviceType, std::vector<AVPixelFormat>> formatMatrix;

    // CUDA format compatibility
    formatMatrix[AV_HWDEVICE_TYPE_CUDA] = {AV_PIX_FMT_CUDA,    AV_PIX_FMT_NV12,   AV_PIX_FMT_YUV420P,
                                           AV_PIX_FMT_YUV444P, AV_PIX_FMT_P010LE, AV_PIX_FMT_P016LE};

    // QuickSync format compatibility
    formatMatrix[AV_HWDEVICE_TYPE_QSV] = {AV_PIX_FMT_QSV, AV_PIX_FMT_NV12, AV_PIX_FMT_P010LE, AV_PIX_FMT_YUV420P};

    // VAAPI format compatibility
    formatMatrix[AV_HWDEVICE_TYPE_VAAPI] = {AV_PIX_FMT_VAAPI, AV_PIX_FMT_NV12, AV_PIX_FMT_P010LE, AV_PIX_FMT_YUV420P};

    // VideoToolbox format compatibility
    formatMatrix[AV_HWDEVICE_TYPE_VIDEOTOOLBOX] = {AV_PIX_FMT_VIDEOTOOLBOX, AV_PIX_FMT_NV12, AV_PIX_FMT_P010LE, AV_PIX_FMT_YUV420P};

    // Initialize supported formats for each hardware type
    for (const auto & entry : formatMatrix) {
      AVHWDeviceType hwType = entry.first;
      supportedFormats[hwType] = entry.second;
    }
  }

  size_t HWContextManager::calculateFrameSize(int width, int height, AVPixelFormat format) const {
    // Get format descriptor
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(format);
    if (!desc) return 0;

    // Calculate size based on format
    size_t size = width * height;
    for (int i = 0; i < desc->nb_components; i++) {
      int shift_w = (i == 1 || i == 2) ? desc->log2_chroma_w : 0;
      int shift_h = (i == 1 || i == 2) ? desc->log2_chroma_h : 0;
      size += (width >> shift_w) * (height >> shift_h);
    }

    // Add alignment padding
    size = (size + 15) & ~15;

    return size;
  }

  std::vector<HWDeviceInfo> HWContextManager::getAvailableDevices() const {
    std::lock_guard<std::mutex> lock(managerMutex);
    return availableHardware;
  }

  bool HWContextManager::supportsFormat(AVHWDeviceType type, AVPixelFormat format) const {
    std::lock_guard<std::mutex> lock(managerMutex);

    // Find any device of the specified type
    auto it = std::find_if(availableHardware.begin(), availableHardware.end(),
                           [type](const HWDeviceInfo & device) { return device.type == type; });

    if (it == availableHardware.end()) { return false; }

    return std::find(it->supportedPixelFormats.begin(), it->supportedPixelFormats.end(), format) !=
      it->supportedPixelFormats.end();
  }

  bool HWContextManager::supportsResolution(AVHWDeviceType type, int width, int height) const {
    std::lock_guard<std::mutex> lock(managerMutex);

    // Find any device of the specified type
    auto it = std::find_if(availableHardware.begin(), availableHardware.end(),
                           [type](const HWDeviceInfo & device) { return device.type == type; });

    if (it == availableHardware.end()) { return false; }

    if (width > static_cast<int>(it->maxWidth) || height > static_cast<int>(it->maxHeight)) { return false; }

    size_t totalPixels = static_cast<size_t>(width) * static_cast<size_t>(height);
    return totalPixels <= it->maxPixels;
  }

  void HWContextManager::initDeviceCapabilities(HWDeviceInfo & device) {
    // Get supported formats directly from the supportedFormats map
    device.supportedPixelFormats = getSupportedPixelFormats(device.type);
    device.maxWidth = 8192;
    device.maxHeight = 8192;
    device.maxPixels = 8192 * 8192;
    device.maxBitrate = 400000000;
  }
} // namespace FFmpeg
