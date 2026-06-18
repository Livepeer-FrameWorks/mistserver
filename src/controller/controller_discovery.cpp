#include "controller_discovery.h"

#include "controller_statistics.h"
#include "controller_storage.h"
#include "controller_streams.h"

#include <mist/defines.h>
#include <mist/procs.h>
#include <mist/shared_memory.h>
#include <mist/stream.h>
#include <mist/util.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Controller {

  CameraRegistry cameraRegistry;
  static uint64_t pageSize = CAMERAS_INITSIZE;
  std::atomic<bool> mutateShm{true};
  std::atomic<bool> stopDiscovery{false};
  static DiscoveryState discoveryState;
  size_t discoveryTimer = std::string::npos;
  size_t ndiCheckTimer = std::string::npos;
  size_t tallyTimer = std::string::npos;
  std::map<std::string, bool> currentTallyState;
  // Track registered FDs in event loop to avoid stale handlers on library-managed closes
#ifdef WITH_ONVIF
  static int lastOnvifSocket = -1;
#endif
  static int lastViscaSocket = -1;

#ifdef WITH_ONVIF
  // Cached ONVIF devices for PTZ — avoids re-running GetCapabilities per command
  struct CachedOnvifDevice {
      std::unique_ptr<ONVIF::Device> device;
      std::mutex mutex; // serialize PTZ calls per device
      uint64_t lastUsedMs = 0;
  };
  static std::mutex onvifCacheMutex;
  static std::map<std::string, std::unique_ptr<CachedOnvifDevice>> onvifDeviceCache;

  static CachedOnvifDevice *getOrCreateOnvifDevice(const std::string & key, const std::string & host, uint16_t port,
                                                   const std::string & user, const std::string & pass) {
    std::lock_guard<std::mutex> lk(onvifCacheMutex);
    auto it = onvifDeviceCache.find(key);
    if (it != onvifDeviceCache.end()) {
      it->second->lastUsedMs = Util::bootMS();
      return it->second.get();
    }
    auto entry = std::unique_ptr<CachedOnvifDevice>(new CachedOnvifDevice());
    entry->device = std::unique_ptr<ONVIF::Device>(new ONVIF::Device(host, port));
    entry->device->setRequestTimeout(3);
    if (!user.empty()) { entry->device->setCredentials(user, pass); }
    entry->lastUsedMs = Util::bootMS();
    auto *ptr = entry.get();
    onvifDeviceCache[key] = std::move(entry);
    return ptr;
  }

  static void invalidateOnvifCache(const std::string & key) {
    std::lock_guard<std::mutex> lk(onvifCacheMutex);
    onvifDeviceCache.erase(key);
  }
#endif

  // Managed probe threads for safe shutdown
  std::mutex probeThreadsMutex;
  std::vector<ProbeThread> probeThreads;

  void reapFinishedProbeThreads() {
    std::lock_guard<std::mutex> lk(probeThreadsMutex);
    auto it = probeThreads.begin();
    while (it != probeThreads.end()) {
      if (it->done->load()) {
        if (it->thread.joinable()) it->thread.join();
        it = probeThreads.erase(it);
      } else {
        ++it;
      }
    }
  }

  // Capability probe dedupe + throttling
  static std::mutex capabilityMutex;
  static std::unordered_set<std::string> pendingCaps; // key: deviceId|proto
  static std::unordered_map<std::string, uint64_t> lastProbedAtMs; // key -> ms since boot
  static const uint64_t kProbeTtlMs = 30000; // 30s between probes per device/protocol

  // Protocol registry for polymorphic dispatch
  std::map<std::string, ::Device::Discovery *> protocolRegistry;

  // Initialize persistent discovery instances
#ifdef WITH_NDI
  std::unique_ptr<NDI::Discovery> ndiDiscovery;
#endif
#ifdef WITH_ONVIF
  std::unique_ptr<ONVIF::Discovery> onvifDiscovery;
#endif
  std::unique_ptr<VISCA::Discovery> viscaDiscovery;

  // Helper function to join strings with a delimiter - useful for logging
  std::string joinStrings(const std::vector<std::string> & strings, const std::string & delim) {
    std::string result;
    for (size_t i = 0; i < strings.size(); ++i) {
      if (i > 0) result += delim;
      result += strings[i];
    }
    return result;
  }

  // ── CameraRegistry implementation ──

  std::string CameraRegistry::canonicalKey(const ::Device::DeviceInfo & dev) const {
    return canonicalDeviceKey(dev);
  }

  JSON::Value CameraRegistry::list(bool redactCredentials) const {
    JSON::Value result;
    std::lock_guard<std::mutex> lock(mapMutex);
    for (const auto & kv : cameras) {
      JSON::Value camJson = kv.second->info.toJSON();
      camJson["id"] = kv.first;
      if (redactCredentials && camJson.isMember("protocols")) {
        for (size_t i = 0; i < camJson["protocols"].size(); ++i) {
          JSON::Value & proto = camJson["protocols"][i];
          if (proto.isMember("password") && !proto["password"].asString().empty()) { proto["password"] = "***"; }
        }
      }
      result.append(camJson);
    }
    return result;
  }

  std::shared_ptr<CameraEntry> CameraRegistry::find(const std::string & id) const {
    std::lock_guard<std::mutex> lock(mapMutex);
    auto it = cameras.find(id);
    if (it != cameras.end()) return it->second;
    // Try cleaned form of the input
    std::string cleanId = extractCleanIP(id);
    if (!cleanId.empty() && cleanId != id) {
      it = cameras.find(cleanId);
      if (it != cameras.end()) return it->second;
    }
    // Scan by cleaned host as last resort
    if (!cleanId.empty()) {
      for (const auto & kv : cameras) {
        if (extractCleanIP(kv.second->info.host) == cleanId) return kv.second;
      }
    }
    return nullptr;
  }

  size_t CameraRegistry::size() const {
    std::lock_guard<std::mutex> lock(mapMutex);
    return cameras.size();
  }

  void CameraRegistry::forEachDevice(std::function<void(const std::string & key, const CameraEntry & entry)> fn) const {
    std::lock_guard<std::mutex> lock(mapMutex);
    for (const auto & kv : cameras) { fn(kv.first, *kv.second); }
  }

  void CameraRegistry::merge(const std::vector<::Device::DeviceInfo> & devices) {
    std::lock_guard<std::mutex> lock(mapMutex);
    for (const auto & devInfo : devices) {
      if (stopDiscovery.load()) break;
      std::string key = canonicalKey(devInfo);
      if (key.empty()) continue;

      auto it = cameras.find(key);
      if (it != cameras.end()) {
        std::lock_guard<std::mutex> cmdLock(it->second->commandMutex);
        updateDeviceInfo(it->second->info, devInfo);
        continue;
      }
      // Scan existing entries by cleaned host
      bool found = false;
      for (auto & kv : cameras) {
        if (extractCleanIP(kv.second->info.host) == key) {
          std::lock_guard<std::mutex> cmdLock(kv.second->commandMutex);
          updateDeviceInfo(kv.second->info, devInfo);
          found = true;
          break;
        }
      }
      if (!found) {
        auto entry = std::make_shared<CameraEntry>();
        entry->info = devInfo;
        if (entry->info.id.empty()) entry->info.id = key;
        cameras[key] = entry;
      }
    }
  }

  void CameraRegistry::remove(const std::string & id) {
    std::lock_guard<std::mutex> lock(mapMutex);
    if (cameras.erase(id)) return;
    std::string cleanId = extractCleanIP(id);
    if (!cleanId.empty() && cleanId != id) {
      if (cameras.erase(cleanId)) return;
    }
    // Scan by cleaned host
    if (!cleanId.empty()) {
      for (auto it = cameras.begin(); it != cameras.end(); ++it) {
        if (extractCleanIP(it->second->info.host) == cleanId) {
          cameras.erase(it);
          return;
        }
      }
    }
  }

  void CameraRegistry::updateStatus(const std::string & id, const std::string & status) {
    std::lock_guard<std::mutex> lock(mapMutex);
    auto it = cameras.find(id);
    if (it != cameras.end()) {
      it->second->info.status = status;
      return;
    }
    std::string cleanId = extractCleanIP(id);
    if (!cleanId.empty()) {
      for (auto & kv : cameras) {
        if (extractCleanIP(kv.second->info.host) == cleanId) {
          kv.second->info.status = status;
          return;
        }
      }
    }
  }

  void CameraRegistry::loadFromStorage(const JSON::Value & camerasJson) {
    std::lock_guard<std::mutex> lock(mapMutex);
    cameras.clear();
    jsonForEachConst (camerasJson, it) {
      if (!it->isObject()) continue;
      ::Device::DeviceInfo info = ::Device::DeviceInfo::fromJSON(*it);
      std::string key = canonicalKey(info);
      if (key.empty()) continue;
      auto entry = std::make_shared<CameraEntry>();
      if (info.id.empty()) info.id = key;
      entry->info = std::move(info);
      cameras[key] = entry;
    }
  }

  void CameraRegistry::syncToStorage(JSON::Value & storage) const {
    std::lock_guard<std::mutex> lock(mapMutex);
    storage["cameras"] = JSON::Value();
    for (const auto & kv : cameras) { storage["cameras"].append(kv.second->info.toJSON()); }
  }

  static size_t calcProtocolsNestedSize(size_t maxProtocols);
  static size_t calcStreamsNestedSize(size_t maxStreams);

  void CameraRegistry::writeToShm() {
    // Sync to Storage for persistence and SHM write compatibility
    syncToStorage(Storage);

    INFO_MSG("Writing camera information to shared memory");
    jsonForEach (Storage["cameras"], it) { INFO_MSG("%s\n", (*it).toString().c_str()); }

    pageSize = calculateRequiredPageSize();

    IPC::sharedPage camerasPage(SHM_CAMERAS, pageSize, false, false);
    if (camerasPage.mapped) {
      camerasPage.master = true;
      Util::RelAccX oldAccX(camerasPage.mapped, false);
      size_t sizeRequired = oldAccX.getOffset() + oldAccX.getRSize() * Storage["cameras"].size();
      if (pageSize < sizeRequired) pageSize = sizeRequired;
      oldAccX.setReload();
    }
    camerasPage.close();
    camerasPage.init(SHM_CAMERAS, pageSize, true, false);

    Util::RelAccX camAccX(camerasPage.mapped, false);
    setupCameraAccX(camAccX);

    size_t maxProtocols = 0;
    size_t maxStreams = 0;
    jsonForEach (Storage["cameras"], it) {
      if ((*it)["protocols"].size() > maxProtocols) maxProtocols = (*it)["protocols"].size();
      if ((*it)["streams"].size() > maxStreams) maxStreams = (*it)["streams"].size();
    }
    camAccX.addField("protocols", RAX_NESTED, (uint32_t)calcProtocolsNestedSize(maxProtocols));
    camAccX.addField("streams", RAX_NESTED, (uint32_t)calcStreamsNestedSize(maxStreams));

    size_t reqCount = (pageSize - camAccX.getOffset()) / camAccX.getRSize();
    camAccX.setRCount(reqCount);
    size_t cameraCount = Storage["cameras"].size();
    camAccX.setPresent(reqCount);
    camAccX.setEndPos(cameraCount);

    auto toCsv = [](const JSON::Value & arr) -> std::string {
      std::string out;
      for (size_t i = 0; i < arr.size(); ++i) {
        if (i) out += ",";
        out += arr[i].asString();
      }
      return out;
    };

    size_t index = 0;
    jsonForEach (Storage["cameras"], it) {
      const JSON::Value & camera = *it;
      camAccX.setString("id", camera["id"].asString(), index);
      camAccX.setString("name", camera["name"].asString(), index);
      camAccX.setString("status", camera["status"].asString(), index);
      camAccX.setString("host", camera["host"].asString(), index);
      if (camera["protocols"].size() > 0) {
        const JSON::Value & p0 = camera["protocols"][0u];
        camAccX.setInt("port", p0["port"].asInt(), index);
        camAccX.setString("protocol", p0["type"].asString(), index);
      } else {
        camAccX.setInt("port", 0, index);
        camAccX.setString("protocol", "", index);
      }
      camAccX.setInt("hasPTZ", camera["hasPTZ"].asBool() ? 1 : 0, index);
      camAccX.setInt("hasAudio", camera["hasAudio"].asBool() ? 1 : 0, index);
      camAccX.setInt("hasMetadata", camera["hasMetadata"].asBool() ? 1 : 0, index);
      camAccX.setString("manufacturer", camera["manufacturer"].asString(), index);
      camAccX.setString("model", camera["model"].asString(), index);
      camAccX.setString("firmwareVersion", camera["firmwareVersion"].asString(), index);
      camAccX.setString("serialNumber", camera["serialNumber"].asString(), index);
      camAccX.setString("webControlUrl", camera["webControlUrl"].asString(), index);
      camAccX.setString("features", toCsv(camera["features"]), index);
      camAccX.setString("ptzFeatures", toCsv(camera["ptzFeatures"]), index);
      {
        Util::RelAccX protoAccX(camAccX.getPointer("protocols", index), false);
        if (!protoAccX.isReady()) {
          setupProtocolsAccX(protoAccX);
          protoAccX.setRCount((uint32_t)maxProtocols);
          protoAccX.setReady();
        }
        writeProtocolsToShm(camera["protocols"], protoAccX);
      }
      {
        Util::RelAccX streamAccX(camAccX.getPointer("streams", index), false);
        if (!streamAccX.isReady()) {
          setupStreamsAccX(streamAccX);
          streamAccX.setRCount((uint32_t)maxStreams);
          streamAccX.setReady();
        }
        writeStreamsToShm(camera["streams"], streamAccX);
      }
      index++;
    }
    camAccX.setReady();
    camerasPage.master = false;
    mutateShm.store(false);
  }

  // ── End CameraRegistry implementation ──

  static inline bool missingInitialSetupConfig() {
    if (!Storage.isMember("account") || Storage["account"].size() < 1) { return true; }
    if (!Storage.isMember("config") || !Storage["config"].isMember("protocols") || Storage["config"]["protocols"].size() < 1) {
      return true;
    }
    return false;
  }

  void initDiscovery() {
    INFO_MSG("Initializing discovery system");

    // Close any existing shared memory page
    IPC::sharedPage camerasPage(SHM_CAMERAS, pageSize, false, false);
    if (camerasPage.mapped) {
      camerasPage.master = true;
      Util::RelAccX oldAccX(camerasPage.mapped, false);
      oldAccX.setReload();
      camerasPage.close();
    }

    // Reset discovery state
    discoveryState = DiscoveryState{};
    stopDiscovery.store(false);
    mutateShm.store(true);

    // Populate registry from persisted camera data
    if (Storage.isMember("cameras") && Storage["cameras"].size()) {
      cameraRegistry.loadFromStorage(Storage["cameras"]);
      INFO_MSG("Loaded %zu cameras from storage", cameraRegistry.size());
    }

    INFO_MSG("Discovery system initialized");
  }

  size_t discoveryRun() {
    // Check if controller is still active
    if (!Controller::conf.is_active || stopDiscovery.load()) {
      return 10000; // Still return interval for event loop
    }

    if (missingInitialSetupConfig()) {
      if (!Controller::conf.is_active || stopDiscovery.load()) { return 0; }
      // Keep discovery paused during first-time setup without polluting logs.
      return 10000;
    }

    // Discovery can be disabled at runtime; defaults to enabled when the key is absent
    // so existing deployments keep their always-on behavior.
    JSON::Value & cfg = Controller::Storage["config"];
    bool discoveryEnabled = !cfg.isMember("device_discovery") || cfg["device_discovery"].asBool();
    if (!discoveryEnabled) {
      if (discoveryState.asyncStarted) {
        stopAsyncDiscovery();
        discoveryState.asyncStarted = false;
      }
      return 10000;
    }

    // Initialize NDI on first run if needed
    if (!discoveryState.ndiInitialized) {
#ifdef WITH_NDI
      if (!NDIlib_is_supported_CPU()) {
        FAIL_MSG("NDI requires SSE4.2 CPU");
        return 10000;
      }
      if (!NDI::initialize()) {
        FAIL_MSG("Failed to initialize NDI");
        return 10000;
      }
      INFO_MSG("NDI initialized %s", NDIlib_version());
      discoveryState.ndiInitialized = true;
#endif
    }

    // Initialize async discovery on first run (or after re-enabling discovery)
    if (!discoveryState.asyncStarted) {
      initAsyncDiscovery();
      startAsyncDiscovery();
      discoveryState.asyncStarted = true;
    }

    // Reap any finished probe threads
    reapFinishedProbeThreads();

    // Check async discovery progress and handle timeouts
    size_t nextCheck = checkAsyncDiscoveryProgress();

    // Write to shared memory if needed
    if (mutateShm.load()) {
      HIGH_MSG("Writing cameras to shared memory");
      writeToShmCameras();
      mutateShm.store(false);
    }

    return nextCheck;
  }

  void discoveryDeinit() {
    INFO_MSG("Discovery system shutting down");

    // Signal discovery to stop
    stopDiscovery.store(true);

    // Stop async discovery and clean up sockets before joining threads
    stopAsyncDiscovery();

    // Join all outstanding probe threads before destroying discovery instances
    {
      std::lock_guard<std::mutex> lk(probeThreadsMutex);
      for (auto & pt : probeThreads) {
        if (pt.thread.joinable()) pt.thread.join();
      }
      probeThreads.clear();
    }

    // Clean up shared memory page
    IPC::sharedPage camerasPage(SHM_CAMERAS, pageSize, false, false);
    if (camerasPage.mapped) {
      camerasPage.master = true;
      Util::RelAccX oldAccX(camerasPage.mapped, false);
      oldAccX.setReload();
      camerasPage.close();
    }

#ifdef WITH_NDI
    // Clean up NDI resources
    if (discoveryState.ndiInitialized) {
      ndiDiscovery.reset();
      NDI::deinitialize();
      INFO_MSG("NDI resources cleaned up");
    }
#endif

    INFO_MSG("Discovery system shutdown complete");
  }

  // Helper function to setup protocol fields in a RelAccX structure
  void setupProtocolsAccX(Util::RelAccX & rax) {
    rax.addField("type", RAX_32STRING);
    rax.addField("address", RAX_256STRING);
    rax.addField("port", RAX_32UINT);
    rax.addField("username", RAX_64STRING);
    rax.addField("password", RAX_64STRING);
    rax.addField("hasPTZ", RAX_32UINT);
    rax.addField("hasAudio", RAX_32UINT);
    rax.addField("hasMetadata", RAX_32UINT);
    rax.addField("hasVideo", RAX_32UINT);
    rax.addField("hasRecording", RAX_32UINT);
    rax.addField("hasWebControl", RAX_32UINT);
    rax.addField("hasTally", RAX_32UINT);
    rax.addField("hasRTPMulticast", RAX_32UINT);
    rax.addField("hasRTPTCP", RAX_32UINT);
    rax.addField("hasRTPRTSPTCP", RAX_32UINT);
    rax.addField("supportedTransports", RAX_256STRING);
    rax.addField("supportedCommands", RAX_512STRING);
    rax.addField("supportedFormats", RAX_256STRING);
  }

  // Helper function to setup stream fields in a RelAccX structure
  void setupStreamsAccX(Util::RelAccX & rax) {
    rax.addField("protocol", RAX_32STRING);
    rax.addField("format", RAX_32STRING);
    rax.addField("transport", RAX_32STRING);
    rax.addField("uri", RAX_512STRING);
    rax.addField("name", RAX_128STRING);
    rax.addField("address", RAX_256STRING);
    rax.addField("port", RAX_32UINT);
    rax.addField("path", RAX_256STRING);
    rax.addField("width", RAX_32UINT);
    rax.addField("height", RAX_32UINT);
    rax.addField("fps", RAX_32UINT);
    rax.addField("bitrate", RAX_32UINT);
    rax.addField("profile", RAX_64STRING);
  }

  // Camera base schema (non-nested)
  void setupCameraAccX(Util::RelAccX & rax) {
    rax.addField("id", RAX_32STRING);
    rax.addField("name", RAX_32STRING);
    rax.addField("status", RAX_32STRING);
    rax.addField("host", RAX_256STRING);
    rax.addField("port", RAX_32UINT);
    rax.addField("protocol", RAX_32STRING);
    rax.addField("hasPTZ", RAX_32UINT);
    rax.addField("hasAudio", RAX_32UINT);
    rax.addField("hasMetadata", RAX_32UINT);
    rax.addField("manufacturer", RAX_64STRING);
    rax.addField("model", RAX_64STRING);
    rax.addField("firmwareVersion", RAX_64STRING);
    rax.addField("serialNumber", RAX_64STRING);
    rax.addField("webControlUrl", RAX_256STRING);
    rax.addField("features", RAX_512STRING);
    rax.addField("ptzFeatures", RAX_512STRING);
  }

  // Exact nested sizes using in-memory RelAccX (no SHM side effects)
  static size_t calcProtocolsNestedSize(size_t maxProtocols) {
    std::vector<char> buf(4096);
    Util::RelAccX rax(buf.data(), false);
    setupProtocolsAccX(rax);
    rax.setRCount((uint32_t)maxProtocols);
    rax.setReady();
    return rax.getOffset() + (uint64_t)rax.getRSize() * (uint64_t)maxProtocols;
  }

  static size_t calcStreamsNestedSize(size_t maxStreams) {
    std::vector<char> buf(4096);
    Util::RelAccX rax(buf.data(), false);
    setupStreamsAccX(rax);
    rax.setRCount((uint32_t)maxStreams);
    rax.setReady();
    return rax.getOffset() + (uint64_t)rax.getRSize() * (uint64_t)maxStreams;
  }

  // Helper function to calculate required page size exactly using in-memory RelAccX layouts
  size_t calculateRequiredPageSize(size_t maxCameras, size_t maxProtocols, size_t maxStreams) {
    const size_t protoNested = calcProtocolsNestedSize(maxProtocols);
    const size_t streamNested = calcStreamsNestedSize(maxStreams);

    std::vector<char> buf(8192);
    Util::RelAccX cam(buf.data(), false);
    setupCameraAccX(cam);
    cam.addField("protocols", RAX_NESTED, (uint32_t)protoNested);
    cam.addField("streams", RAX_NESTED, (uint32_t)streamNested);
    cam.setRCount((uint32_t)maxCameras);
    cam.setReady();
    // Return the exact size RelAccX will use for this schema and record count
    return cam.getOffset() + (uint64_t)cam.getRSize() * (uint64_t)maxCameras;
  }
  size_t calculateRequiredPageSize() {
    size_t maxProtocols = 0;
    size_t maxStreams = 0;
    jsonForEach (Storage["cameras"], it) {
      if ((*it)["protocols"].size() > maxProtocols) { maxProtocols = (*it)["protocols"].size(); }
      if ((*it)["streams"].size() > maxStreams) { maxStreams = (*it)["streams"].size(); }
    }
    size_t calculated = calculateRequiredPageSize(Storage["cameras"].size(), maxProtocols, maxStreams);
    return std::max(calculated, (size_t)(64 * 1024));
  }

  void writeProtocolsToShm(const JSON::Value & protocols, Util::RelAccX & protoAccX) {
    // Get field references first
    auto typeField = protoAccX.getFieldData("type");
    auto addressField = protoAccX.getFieldData("address");
    auto portField = protoAccX.getFieldData("port");
    auto usernameField = protoAccX.getFieldData("username");
    auto passwordField = protoAccX.getFieldData("password");
    auto hasPTZField = protoAccX.getFieldData("hasPTZ");
    auto hasAudioField = protoAccX.getFieldData("hasAudio");
    auto hasMetadataField = protoAccX.getFieldData("hasMetadata");
    auto hasVideoField = protoAccX.getFieldData("hasVideo");
    auto hasRecordingField = protoAccX.getFieldData("hasRecording");
    auto hasWebControlField = protoAccX.getFieldData("hasWebControl");
    auto hasTallyField = protoAccX.getFieldData("hasTally");
    auto hasRTPMulticastField = protoAccX.getFieldData("hasRTPMulticast");
    auto hasRTPTCPField = protoAccX.getFieldData("hasRTPTCP");
    auto hasRTPRTSPTCPField = protoAccX.getFieldData("hasRTPRTSPTCP");
    auto supportedTransportsField = protoAccX.getFieldData("supportedTransports");
    auto supportedCommandsField = protoAccX.getFieldData("supportedCommands");
    auto supportedFormatsField = protoAccX.getFieldData("supportedFormats");

    auto toCsv = [](const JSON::Value & arr) -> std::string {
      std::string out;
      for (size_t i = 0; i < arr.size(); ++i) {
        if (i) out += ",";
        out += arr[i].asString();
      }
      return out;
    };
    size_t protoIndex = 0;
    jsonForEachConst (protocols, it) {
      const JSON::Value & proto = *it;
      protoAccX.setString(typeField, proto["type"].asString(), protoIndex);
      protoAccX.setString(addressField, proto["address"].asString(), protoIndex);
      protoAccX.setInt(portField, proto["port"].asInt(), protoIndex);
      protoAccX.setString(usernameField, proto["username"].asString(), protoIndex);
      protoAccX.setString(passwordField, "", protoIndex);
      const JSON::Value & caps = proto["capabilities"];
      protoAccX.setInt(hasPTZField, caps["hasPTZ"].asBool() ? 1 : 0, protoIndex);
      protoAccX.setInt(hasAudioField, caps["hasAudio"].asBool() ? 1 : 0, protoIndex);
      protoAccX.setInt(hasMetadataField, caps["hasMetadata"].asBool() ? 1 : 0, protoIndex);
      protoAccX.setInt(hasVideoField, caps["hasVideo"].asBool() ? 1 : 0, protoIndex);
      protoAccX.setInt(hasRecordingField, caps["hasRecording"].asBool() ? 1 : 0, protoIndex);
      protoAccX.setInt(hasWebControlField, caps["hasWebControl"].asBool() ? 1 : 0, protoIndex);
      protoAccX.setInt(hasTallyField, caps["hasTally"].asBool() ? 1 : 0, protoIndex);
      protoAccX.setInt(hasRTPMulticastField, caps["hasRTPMulticast"].asBool() ? 1 : 0, protoIndex);
      protoAccX.setInt(hasRTPTCPField, caps["hasRTPTCP"].asBool() ? 1 : 0, protoIndex);
      protoAccX.setInt(hasRTPRTSPTCPField, caps["hasRTPRTSPTCP"].asBool() ? 1 : 0, protoIndex);
      protoAccX.setString(supportedTransportsField, toCsv(caps["supportedTransports"]), protoIndex);
      protoAccX.setString(supportedCommandsField, toCsv(caps["supportedCommands"]), protoIndex);
      protoAccX.setString(supportedFormatsField, toCsv(caps["supportedFormats"]), protoIndex);
      protoIndex++;
    }
    protoAccX.setEndPos(protoIndex);
  }

  void writeStreamsToShm(const JSON::Value & streams, Util::RelAccX & streamAccX) {
    // Get field references first
    auto protocolField = streamAccX.getFieldData("protocol");
    auto formatField = streamAccX.getFieldData("format");
    auto transportField = streamAccX.getFieldData("transport");
    auto uriField = streamAccX.getFieldData("uri");
    auto nameField = streamAccX.getFieldData("name");
    auto addressField = streamAccX.getFieldData("address");
    auto portField = streamAccX.getFieldData("port");
    auto pathField = streamAccX.getFieldData("path");
    auto widthField = streamAccX.getFieldData("width");
    auto heightField = streamAccX.getFieldData("height");
    auto fpsField = streamAccX.getFieldData("fps");
    auto bitrateField = streamAccX.getFieldData("bitrate");
    auto profileField = streamAccX.getFieldData("profile");

    size_t streamIndex = 0;
    for (size_t i = 0; i < streams.size(); i++) {
      const JSON::Value & stream = streams[i];
      streamAccX.setString(protocolField, stream["protocol"].asString(), streamIndex);
      streamAccX.setString(formatField, stream["format"].asString(), streamIndex);
      streamAccX.setString(transportField, stream["transport"].asString(), streamIndex);
      streamAccX.setString(uriField, stream["uri"].asString(), streamIndex);
      streamAccX.setString(nameField, stream["name"].asString(), streamIndex);
      streamAccX.setString(addressField, stream["address"].asString(), streamIndex);
      streamAccX.setInt(portField, stream["port"].asInt(), streamIndex);
      streamAccX.setString(pathField, stream["path"].asString(), streamIndex);
      streamAccX.setInt(widthField, stream["width"].asInt(), streamIndex);
      streamAccX.setInt(heightField, stream["height"].asInt(), streamIndex);
      streamAccX.setInt(fpsField, stream["fps"].asInt(), streamIndex);
      streamAccX.setInt(bitrateField, stream["bitrate"].asInt(), streamIndex);
      streamAccX.setString(profileField, stream["profile"].asString(), streamIndex);
      streamIndex++;
    }
    streamAccX.setEndPos(streamIndex);
  }

  // Helper function to pretty print JSON values recursively
  void prettyPrintValue(const JSON::Value & value, const std::string & key = "", int indent = 0) {
    std::string indentStr(indent * 2, ' ');
    std::string prefix = key.empty() ? "" : key + ": ";

    if (value.isBool()) {
      INFO_MSG("%s%s%s", indentStr.c_str(), prefix.c_str(), value.asBool() ? "yes" : "no");
    } else if (value.isInt() || value.isDouble() || value.isString()) {
      if (!value.asString().empty()) {
        INFO_MSG("%s%s%s", indentStr.c_str(), prefix.c_str(), value.asString().c_str());
      }
    } else if (value.isArray()) {
      if (value.size() > 0) {
        INFO_MSG("%s%s[", indentStr.c_str(), prefix.c_str());
        for (size_t i = 0; i < value.size(); i++) {
          if (value[i].isObject()) {
            INFO_MSG("%s{", std::string(indent + 2, ' ').c_str());
            jsonForEachConst (value[i], field) { prettyPrintValue(*field, field.key(), indent + 4); }
            INFO_MSG("%s}%s", std::string(indent + 2, ' ').c_str(), (i < value.size() - 1) ? "," : "");
          } else {
            prettyPrintValue(value[i], "", indent + 2);
          }
        }
        INFO_MSG("%s]", indentStr.c_str());
      }
    } else if (value.isObject()) {
      if (!key.empty()) { INFO_MSG("%s%s{", indentStr.c_str(), prefix.c_str()); }
      jsonForEachConst (value, field) { prettyPrintValue(*field, field.key(), indent + 2); }
      if (!key.empty()) { INFO_MSG("%s}", indentStr.c_str()); }
    }
  }

  void writeToShmCameras() {
    if (!mutateShm.load()) return;
    cameraRegistry.writeToShm();
  }

  void listCameras(JSON::Value & output) {
    output = cameraRegistry.list(true);
  }

  void removeCamera(const JSON::Value & request, JSON::Value & output) {
    cameraRegistry.remove(request["id"].asString());
    mutateShm.store(true);
    output = cameraRegistry.list(true);
  }

  void sendCommand(const JSON::Value & request, JSON::Value & output) {
    std::string id = request["id"].asString();
    std::string commandName = request["command"].asString();
    // Request-level protocol overrides device-level preference
    std::string preferredProtocol = request.isMember("protocol") ? request["protocol"].asString() : "";

    std::shared_ptr<CameraEntry> entry = cameraRegistry.find(id);
    if (!entry) {
      WARN_MSG("PTZ: Device '%s' not found in registry", id.c_str());
      output["success"] = false;
      output["error"] = "Device not found";
      return;
    }

    ::Device::PTZCommand cmd;
    if (!createPTZCommand(commandName, request["args"], cmd)) {
      WARN_MSG("PTZ: Invalid command '%s' or arguments for device '%s'", commandName.c_str(), id.c_str());
      output["success"] = false;
      output["error"] = "Invalid command or arguments";
      return;
    }

    // Read device info under lock, then release before calling sendCommandViaProtocol
    bool hasPTZ;
    std::vector<std::string> supportedProtocols;
    {
      std::lock_guard<std::mutex> devLock(entry->commandMutex);
      hasPTZ = entry->info.hasPTZ;
      // Device-level ptzProtocol preference (overridden by request-level)
      if (preferredProtocol.empty()) { preferredProtocol = entry->info.ptzProtocol; }
      for (const auto & proto : entry->info.protocols) {
        if (proto.second.capabilities.hasPTZ) { supportedProtocols.push_back(proto.first); }
      }
    }

    INFO_MSG("PTZ command '%s' for device '%s'%s", commandName.c_str(), id.c_str(),
             preferredProtocol.empty() ? " (auto)" : (" via " + preferredProtocol).c_str());

    // In automatic mode, prefer fire-and-forget protocols (ONVIF/VISCA) over NDI
    if (preferredProtocol.empty()) {
      std::sort(supportedProtocols.begin(), supportedProtocols.end(), [](const std::string & a, const std::string & b) {
        auto priority = [](const std::string & p) -> int {
          if (p == "onvif") return 0;
          if (p == "visca") return 1;
          return 2;
        };
        return priority(a) < priority(b);
      });
    }

    if (!hasPTZ) {
      WARN_MSG("PTZ: Device '%s' does not support PTZ", id.c_str());
      output["success"] = false;
      output["error"] = "Device does not support PTZ";
      return;
    }

    if (supportedProtocols.empty()) {
      WARN_MSG("PTZ: Device '%s' has hasPTZ=true but no protocols with PTZ capability", id.c_str());
      output["success"] = false;
      output["error"] = "No protocols support PTZ control";
      return;
    }

    HIGH_MSG("PTZ: %zu protocol(s) support PTZ for '%s'", supportedProtocols.size(), id.c_str());

    bool commandSent = false;
    std::string errorMsg;

    if (!preferredProtocol.empty()) {
      auto it = std::find(supportedProtocols.begin(), supportedProtocols.end(), preferredProtocol);
      if (it != supportedProtocols.end()) {
        commandSent = sendCommandViaProtocol(id, preferredProtocol, cmd, errorMsg);
      }
    }

    if (!commandSent) {
      for (const auto & protocol : supportedProtocols) {
        if (protocol != preferredProtocol) {
          if (sendCommandViaProtocol(id, protocol, cmd, errorMsg)) {
            commandSent = true;
            break;
          }
        }
      }
    }

    if (commandSent) {
      HIGH_MSG("PTZ: Command '%s' sent successfully to '%s'", commandName.c_str(), id.c_str());
    } else {
      WARN_MSG("PTZ: Command '%s' failed for '%s': %s", commandName.c_str(), id.c_str(),
               errorMsg.empty() ? "no protocol succeeded" : errorMsg.c_str());
    }

    output["success"] = commandSent;
    if (!commandSent) {
      output["error"] = errorMsg.empty() ? "Failed to send command via any available protocol" : errorMsg;
    }
  }

  bool sendCommandViaProtocol(const std::string & deviceId, const std::string & protocol,
                              const ::Device::PTZCommand & cmd, std::string & errorMsg) {
    HIGH_MSG("PTZ: Trying protocol '%s' for device '%s'", protocol.c_str(), deviceId.c_str());
    auto it = protocolRegistry.find(protocol);
    if (it == protocolRegistry.end() || !it->second) {
      errorMsg = "Protocol " + protocol + " not available";
      WARN_MSG("PTZ: %s", errorMsg.c_str());
      return false;
    }
#ifdef WITH_ONVIF
    if (protocol == "onvif") {
      auto entry = cameraRegistry.find(deviceId);
      if (!entry) {
        errorMsg = "Camera not found in registry";
        WARN_MSG("PTZ via ONVIF: %s", errorMsg.c_str());
        return false;
      }
      std::string host, user, pass;
      uint16_t port = 80;
      {
        std::lock_guard<std::mutex> lock(entry->commandMutex);
        auto pit = entry->info.protocols.find("onvif");
        if (pit != entry->info.protocols.end()) {
          host = pit->second.address;
          port = pit->second.port;
          user = pit->second.username;
          pass = pit->second.password;
        } else {
          host = entry->info.host;
        }
      }
      if (host.empty()) {
        errorMsg = "No ONVIF host for device";
        WARN_MSG("PTZ via ONVIF: %s", errorMsg.c_str());
        return false;
      }
      std::string cacheKey = host + ":" + std::to_string(port);
      CachedOnvifDevice *cached = getOrCreateOnvifDevice(cacheKey, host, port, user, pass);
      std::lock_guard<std::mutex> devLock(cached->mutex);
      if (!cached->device->isConnected()) {
        HIGH_MSG("PTZ via ONVIF: Connecting to %s:%d (auth: %s)", host.c_str(), port, user.empty() ? "none" : "yes");
        if (!cached->device->connect()) {
          errorMsg = "Failed to connect to ONVIF device at " + host + ":" + std::to_string(port);
          WARN_MSG("PTZ via ONVIF: %s", errorMsg.c_str());
          invalidateOnvifCache(cacheKey);
          return false;
        }
      }
      bool result = cached->device->sendPTZ(cmd);
      HIGH_MSG("PTZ via ONVIF: sendPTZ returned %s", result ? "true" : "false");
      if (!result) { errorMsg = "ONVIF device rejected PTZ command"; }
      return result;
    }
#endif
    if (protocol == "visca") {
      auto entry = cameraRegistry.find(deviceId);
      if (!entry) {
        errorMsg = "Camera not found in registry";
        WARN_MSG("PTZ via VISCA: %s", errorMsg.c_str());
        return false;
      }
      std::string host;
      uint16_t port = 52381;
      {
        std::lock_guard<std::mutex> lock(entry->commandMutex);
        auto pit = entry->info.protocols.find("visca");
        if (pit != entry->info.protocols.end()) {
          if (!pit->second.address.empty()) host = pit->second.address;
          if (pit->second.port != 0) port = pit->second.port;
        }
        if (host.empty()) host = entry->info.host;
      }
      if (host.empty()) {
        errorMsg = "No VISCA host for device";
        WARN_MSG("PTZ via VISCA: %s", errorMsg.c_str());
        return false;
      }
      std::string viscaId = host + ":" + std::to_string(port);
      HIGH_MSG("PTZ via VISCA: Sending to %s", viscaId.c_str());
      bool result = it->second->sendCommand(viscaId, cmd);
      if (!result) { errorMsg = "VISCA command failed for " + viscaId; }
      return result;
    }
    // NDI PTZ can block (receiver connect/disconnect) — dispatch async
    if (protocol == "ndi") {
      HIGH_MSG("PTZ via NDI: Dispatching async to '%s'", deviceId.c_str());
      ::Device::Discovery *disc = it->second;
      std::string devId = deviceId;
      auto done = std::make_shared<std::atomic<bool>>(false);
      std::thread t([disc, devId, cmd, done]() {
        bool result = disc->sendCommand(devId, cmd);
        if (result) {
          HIGH_MSG("PTZ via NDI (async): command succeeded for '%s'", devId.c_str());
        } else {
          WARN_MSG("PTZ via NDI (async): command failed for '%s'", devId.c_str());
        }
        done->store(true);
      });
      {
        std::lock_guard<std::mutex> lk(probeThreadsMutex);
        probeThreads.push_back({std::move(t), done});
      }
      return true;
    }
    HIGH_MSG("PTZ via %s: Sending to '%s'", protocol.c_str(), deviceId.c_str());
    bool result = it->second->sendCommand(deviceId, cmd);
    if (!result) { errorMsg = protocol + " command failed for " + deviceId; }
    return result;
  }

  // Async Discovery Implementation
  void onDeviceDiscovered(const std::vector<::Device::DeviceInfo> & devices) {
    HIGH_MSG("Processing %zu discovered devices via callback", devices.size());

    // Phase 1: merge into registry (struct operations, no JSON round-trip)
    cameraRegistry.merge(devices);
    mutateShm.store(true);

    // Phase 2: queue capability enrichment jobs with dedupe & throttling
    struct ProbeJob {
        ::Device::DeviceInfo dev;
        std::string proto;
    };

    std::vector<ProbeJob> jobs;
    jobs.reserve(devices.size());

    uint64_t tnow = Util::bootMS();
    for (const auto & devInfo : devices) {
      std::string devKey = canonicalDeviceKey(devInfo);
      for (const auto & proto : devInfo.protocols) {
        std::string key = devKey + "|" + proto.first;
        bool enqueue = false;
        {
          std::lock_guard<std::mutex> lock(capabilityMutex);
          auto itLast = lastProbedAtMs.find(key);
          bool throttled = (itLast != lastProbedAtMs.end() && (tnow - itLast->second) < kProbeTtlMs);
          if (!throttled && !pendingCaps.count(key)) {
            pendingCaps.insert(key);
            enqueue = true;
          }
        }
        if (enqueue) { jobs.push_back({devInfo, proto.first}); }
      }
    }

    if (jobs.empty()) return;

    // Phase 3: offload capability enrichment to a managed background thread
    auto doneFlag = std::make_shared<std::atomic<bool>>(false);
    std::thread probeThread([jobs, doneFlag]() {
      std::vector<::Device::DeviceInfo> enriched;
      enriched.reserve(jobs.size());

      for (const auto & job : jobs) {
        if (stopDiscovery.load() || !Controller::conf.is_active) break;

        const auto & devInfo = job.dev;
        const std::string & protoType = job.proto;
        ::Device::DeviceInfo capsInfo;
        capsInfo.host = devInfo.host;
        capsInfo.id = devInfo.id;
        capsInfo.name = devInfo.name;

        auto regIt = protocolRegistry.find(protoType);
        if (regIt != protocolRegistry.end() && regIt->second) {
          // Enrich device info with stored credentials from registry
          ::Device::DeviceInfo enrichedInfo = devInfo;
          std::string devKey = canonicalDeviceKey(devInfo);
          auto entry = cameraRegistry.find(devKey);
          if (entry) {
            std::lock_guard<std::mutex> lk(entry->commandMutex);
            auto protoIt = entry->info.protocols.find(protoType);
            if (protoIt != entry->info.protocols.end()) {
              enrichedInfo.protocols[protoType].username = protoIt->second.username;
              enrichedInfo.protocols[protoType].password = protoIt->second.password;
            }
          }
          auto dev = regIt->second->createDevice(enrichedInfo);
          if (dev && !stopDiscovery.load() && Controller::conf.is_active && dev->connect()) {
            updateDeviceInfo(capsInfo, dev->queryCapabilities());
          }
        }

        enriched.push_back(capsInfo);

        std::string devKey = canonicalDeviceKey(devInfo);
        std::string key = devKey + "|" + protoType;
        {
          std::lock_guard<std::mutex> lock(capabilityMutex);
          lastProbedAtMs[key] = Util::bootMS();
          pendingCaps.erase(key);
        }
      }

      if (!enriched.empty() && !stopDiscovery.load() && Controller::conf.is_active) { onDeviceCapabilities(enriched); }
      doneFlag->store(true);
    });
    {
      std::lock_guard<std::mutex> lk(probeThreadsMutex);
      probeThreads.push_back({std::move(probeThread), doneFlag});
    }
  }

  void autoCreateCameraStreams() {
    JSON::Value & existingStreams = Controller::Storage["streams"];
    bool thumbnailing = Controller::Storage["config"]["auto_camera_thumbnailing"].asBool();
    INFO_MSG("autoCreateCameraStreams: thumbnailing=%s", thumbnailing ? "true" : "false");

    cameraRegistry.forEachDevice([&](const std::string & key, const CameraEntry & entry) {
      const auto & info = entry.info;
      if (info.streams.empty()) {
        HIGH_MSG("autoCreateCameraStreams: device '%s' has no streams, skipping", key.c_str());
        return;
      }

      // Pick the default stream URI — auto-select prefers RTSP/ONVIF over NDI
      int defIdx = info.defaultStream;
      if (defIdx < 0 || defIdx >= (int)info.streams.size()) {
        defIdx = 0;
        for (size_t si = 0; si < info.streams.size(); ++si) {
          std::string proto = info.streams[si].protocol;
          for (auto & ch : proto) ch = tolower(ch);
          if (proto == "rtsp" || proto == "onvif") {
            defIdx = (int)si;
            break;
          }
        }
      }
      const std::string & uri = info.streams[defIdx].uri;
      if (uri.empty()) {
        HIGH_MSG("autoCreateCameraStreams: device '%s' default stream has no URI", key.c_str());
        return;
      }

      // One stream per device: cam_SANITIZEDKEY
      std::string baseName = key;
      for (auto & c : baseName) {
        if (!isalnum(c) && c != '_' && c != '-' && c != '.')
          c = '_';
        else
          c = tolower(c);
      }
      std::string streamName = "cam_" + baseName;
      INFO_MSG("autoCreateCameraStreams: device='%s' streamName='%s' defIdx=%d uri='%s' exists=%s", key.c_str(),
               streamName.c_str(), defIdx, uri.c_str(), existingStreams.isMember(streamName) ? "yes" : "no");

      if (existingStreams.isMember(streamName)) {
        JSON::Value updated = existingStreams[streamName];

        // Update source if default stream changed
        std::string currentSource = updated["source"].asString();
        bool sourceChanged = (currentSource != uri);
        if (sourceChanged) {
          updated["source"] = uri;
          INFO_MSG("Updating stream '%s' source: %s -> %s", streamName.c_str(), currentSource.c_str(), uri.c_str());
        }

        // Sync thumbnailing process
        bool hasAV0 = updated.isMember("processes") && updated["processes"].isMember("AV0");
        if (thumbnailing && !hasAV0) {
          JSON::Value mjpeg;
          mjpeg["process"] = "AV";
          mjpeg["codec"] = "JPEG";
          mjpeg["x-LSP-kind"] = "video";
          mjpeg["quality"] = 15;
          mjpeg["gopsize"] = 30;
          updated["processes"]["AV0"] = mjpeg;
          INFO_MSG("Adding MJPEG thumbnailing process to stream '%s'", streamName.c_str());
        } else if (!thumbnailing && hasAV0) {
          updated["processes"].removeMember("AV0");
          INFO_MSG("Removing MJPEG thumbnailing process from stream '%s'", streamName.c_str());
        }

        if (sourceChanged || (thumbnailing && !hasAV0) || (!thumbnailing && hasAV0)) {
          JSON::Value addReq;
          addReq[streamName] = updated;
          Controller::AddStreams(addReq, existingStreams);
        }
        if (sourceChanged) {
          INFO_MSG("Nuking stream '%s' to restart with new source", streamName.c_str());
          std::deque<std::string> cmd;
          cmd.push_back(Util::getMyPath() + "MistUtilNuke");
          cmd.push_back(streamName);
          int si = 0, so = 1, se = 2;
          Util::Procs::StartPiped(cmd, &si, &so, &se);
        }
        return;
      }

      // Create new stream
      JSON::Value streamConfig;
      streamConfig["source"] = uri;
      streamConfig["tags"].append("auto-camera");
      if (thumbnailing) {
        JSON::Value mjpeg;
        mjpeg["process"] = "AV";
        mjpeg["codec"] = "JPEG";
        mjpeg["x-LSP-kind"] = "video";
        mjpeg["quality"] = 15;
        mjpeg["gopsize"] = 30;
        streamConfig["processes"]["AV0"] = mjpeg;
      }

      JSON::Value addReq;
      addReq[streamName] = streamConfig;
      Controller::AddStreams(addReq, existingStreams);

      if (existingStreams.isMember(streamName)) {
        INFO_MSG("Auto-created stream '%s' for device '%s' (source: %s)", streamName.c_str(), info.id.c_str(), uri.c_str());
      } else {
        WARN_MSG("Failed to auto-create stream '%s' for device '%s'", streamName.c_str(), info.id.c_str());
      }
    });
  }

  void onDeviceCapabilities(const std::vector<::Device::DeviceInfo> & devices) {
    HIGH_MSG("Merging %zu capability results", devices.size());
    cameraRegistry.merge(devices);
    mutateShm.store(true);

    if (Controller::Storage["config"]["auto_camera_streams"].asBool()) { autoCreateCameraStreams(); }
  }

  void initAsyncDiscovery() {
    INFO_MSG("Initializing async discovery system");

    // Initialize discovery instances if not already done
#ifdef WITH_ONVIF
    if (!onvifDiscovery) { onvifDiscovery = std::unique_ptr<ONVIF::Discovery>(new ONVIF::Discovery); }
#endif
    if (!viscaDiscovery) { viscaDiscovery = std::unique_ptr<VISCA::Discovery>(new VISCA::Discovery); }
#ifdef WITH_NDI
    if (!ndiDiscovery) { ndiDiscovery = std::unique_ptr<NDI::Discovery>(new NDI::Discovery); }
#endif

    // Populate protocol registry for polymorphic dispatch
    protocolRegistry.clear();
    if (viscaDiscovery) protocolRegistry["visca"] = viscaDiscovery.get();
#ifdef WITH_ONVIF
    if (onvifDiscovery) protocolRegistry["onvif"] = onvifDiscovery.get();
#endif
#ifdef WITH_NDI
    if (ndiDiscovery) protocolRegistry["ndi"] = ndiDiscovery.get();
#endif
  }

  void startAsyncDiscovery() {
    if (!Controller::conf.is_active || stopDiscovery.load()) return;

    HIGH_MSG("Starting async discovery for all protocols");

#ifdef WITH_ONVIF
    // Start ONVIF async discovery
    if (onvifDiscovery) {
      // Ensure any previously registered handler is removed (library may have already closed old FD)
      if (lastOnvifSocket >= 0) {
        Controller::E.remove(lastOnvifSocket);
        lastOnvifSocket = -1;
      }
      if (onvifDiscovery->startAsyncDiscovery(onDeviceDiscovered, 10000)) { // 10 second timeout
        int socket = onvifDiscovery->getSocket();
        if (socket >= 0) {
          // Remove any stale mapping for this numeric FD, just in case
          Controller::E.remove(socket);
          Controller::E.addSocket(socket, [](void *) {
            if (onvifDiscovery) { onvifDiscovery->processSocketData(); }
          }, 0);
          lastOnvifSocket = socket;
          HIGH_MSG("ONVIF async discovery started, socket %d registered", socket);
        }
      }
    }
#endif

    // Start VISCA async discovery
    if (viscaDiscovery) {
      if (lastViscaSocket >= 0) {
        Controller::E.remove(lastViscaSocket);
        lastViscaSocket = -1;
      }
      if (viscaDiscovery->startAsyncDiscovery(onDeviceDiscovered, 10000)) { // 10 second timeout
        int socket = viscaDiscovery->getSocket();
        if (socket >= 0) {
          Controller::E.remove(socket);
          Controller::E.addSocket(socket, [](void *) {
            if (viscaDiscovery) { viscaDiscovery->processSocketData(); }
          }, 0);
          lastViscaSocket = socket;
          HIGH_MSG("VISCA async discovery started, socket %d registered", socket);
        }
      }
    }

#ifdef WITH_NDI
    // Start NDI async discovery
    if (ndiDiscovery) {
      if (ndiDiscovery->startAsyncDiscovery(onDeviceDiscovered, 0)) { // No timeout for NDI
        HIGH_MSG("NDI async discovery started");

        // Add timer to check for NDI source changes
        ndiCheckTimer = Controller::E.addInterval([]() -> size_t {
          if (!Controller::conf.is_active || stopDiscovery.load()) { return 0; }
          if (ndiDiscovery && ndiDiscovery->checkForNewSources()) {
            // If timeout reached, restart discovery
            ndiDiscovery->startAsyncDiscovery(onDeviceDiscovered, 0);
          }
          return 1000; // Check every second
        }, 1000);
      }
    }
#endif

    // Start tally check timer — monitors viewer counts and sets tally lights
    if (tallyTimer == std::string::npos) {
      tallyTimer = Controller::E.addInterval([]() -> size_t {
        if (!Controller::conf.is_active || stopDiscovery.load()) return 0;

        // Build IP → hasViewers map from configured streams
        std::map<std::string, bool> viewersByIP;
        jsonForEach (Controller::Storage["streams"], jit) {
          std::string streamName = jit.key();
          std::string source = (*jit).isMember("source") ? (*jit)["source"].asString() : "";
          if (source.empty()) continue;
          std::string ip = extractCleanIP(source);
          if (ip.empty()) continue;
          if (Controller::hasViewers(streamName)) { viewersByIP[ip] = true; }
        }

        // Update tally state for each camera
        cameraRegistry.forEachDevice([&](const std::string & key, const CameraEntry & entry) {
          std::string cameraIP = extractCleanIP(entry.info.host);
          if (cameraIP.empty()) return;

          bool desired = viewersByIP.count(cameraIP) > 0;
          auto stateIt = currentTallyState.find(cameraIP);
          if (stateIt != currentTallyState.end() && stateIt->second == desired) return;
          currentTallyState[cameraIP] = desired;

#ifdef WITH_NDI
          auto ndiIt = entry.info.protocols.find("ndi");
          if (ndiIt != entry.info.protocols.end() && ndiIt->second.capabilities.hasTally && ndiDiscovery) {
            ndiDiscovery->setTally(entry.info.id, desired, false);
          }
#endif
          auto viscaIt = entry.info.protocols.find("visca");
          if (viscaIt != entry.info.protocols.end() && viscaDiscovery) {
            std::string host = viscaIt->second.address;
            uint16_t port = viscaIt->second.port ? viscaIt->second.port : 52381;
            if (host.empty()) host = cameraIP;
            viscaDiscovery->setTally(host + ":" + std::to_string(port), desired);
          }
        });

        return 2000;
      }, 2000);
    }
  }

  void stopAsyncDiscovery() {
    INFO_MSG("Stopping async discovery");

#ifdef WITH_ONVIF
    // Stop ONVIF discovery and remove socket
    if (onvifDiscovery) {
      if (lastOnvifSocket >= 0) {
        Controller::E.remove(lastOnvifSocket);
        lastOnvifSocket = -1;
      }
      int socket = onvifDiscovery->getSocket();
      if (socket >= 0) { Controller::E.remove(socket); }
      onvifDiscovery->stopAsyncDiscovery();
    }
#endif

    // Stop VISCA discovery and remove socket
    if (viscaDiscovery) {
      if (lastViscaSocket >= 0) {
        Controller::E.remove(lastViscaSocket);
        lastViscaSocket = -1;
      }
      int socket = viscaDiscovery->getSocket();
      if (socket >= 0) { Controller::E.remove(socket); }
      viscaDiscovery->stopAsyncDiscovery();
    }

#ifdef WITH_NDI
    // Stop NDI discovery and remove timer
    if (ndiDiscovery) {
      ndiDiscovery->stopAsyncDiscovery();
      if (ndiCheckTimer != std::string::npos) {
        Controller::E.removeInterval(ndiCheckTimer);
        ndiCheckTimer = std::string::npos;
      }
    }
#endif

    // Stop tally timer and clean up persistent receivers
    if (tallyTimer != std::string::npos) {
      Controller::E.removeInterval(tallyTimer);
      tallyTimer = std::string::npos;
    }
    currentTallyState.clear();
#ifdef WITH_NDI
    if (ndiDiscovery) { ndiDiscovery->cleanupTallyReceivers(); }
#endif
  }

  size_t checkAsyncDiscoveryProgress() {
    if (!Controller::conf.is_active || stopDiscovery.load()) { return 10000; }
    // Check for timeouts and restart discovery if needed
    bool anyRunning = false;

#ifdef WITH_ONVIF
    if (onvifDiscovery && onvifDiscovery->checkAsyncTimeout()) {
      // ONVIF timeout - remove socket and restart
      if (lastOnvifSocket >= 0) {
        Controller::E.remove(lastOnvifSocket);
        lastOnvifSocket = -1;
      }
      int socket = onvifDiscovery->getSocket();
      if (socket >= 0) { Controller::E.remove(socket); }
      // Restart discovery
      startAsyncDiscovery();
    }
    if (onvifDiscovery && onvifDiscovery->isAsyncDiscoveryRunning()) { anyRunning = true; }
#endif

    if (viscaDiscovery && viscaDiscovery->checkAsyncTimeout()) {
      // VISCA timeout - remove socket and restart
      if (lastViscaSocket >= 0) {
        Controller::E.remove(lastViscaSocket);
        lastViscaSocket = -1;
      }
      int socket = viscaDiscovery->getSocket();
      if (socket >= 0) { Controller::E.remove(socket); }
      // Restart discovery
      startAsyncDiscovery();
    }
    if (viscaDiscovery && viscaDiscovery->isAsyncDiscoveryRunning()) { anyRunning = true; }

#ifdef WITH_NDI
    if (ndiDiscovery && ndiDiscovery->isAsyncDiscoveryRunning()) { anyRunning = true; }
#endif

    // Return next check interval (10 seconds if nothing running, 30 seconds if active)
    return anyRunning ? 30000 : 10000;
  }

  void removeCameraByName(const std::string & name) {
    cameraRegistry.remove(name);
    mutateShm.store(true);
  }

  void updateCameraStatus(const std::string & name, const std::string & status) {
    cameraRegistry.updateStatus(name, status);
    mutateShm.store(true);
  }

  void updateCamera(const JSON::Value & request, JSON::Value & output) {
    // PTZ protocol preference path: {id, ptzProtocol: "onvif"|"visca"|"ndi"|""}
    if (request.isMember("id") && request.isMember("ptzProtocol")) {
      std::string camId = request["id"].asString();
      auto entry = cameraRegistry.find(camId);
      if (!entry) {
        output["error"] = "Camera not found";
        return;
      }
      std::string proto = request["ptzProtocol"].asString();
      {
        std::lock_guard<std::mutex> lock(entry->commandMutex);
        entry->info.ptzProtocol = proto;
      }
      mutateShm.store(true);
      INFO_MSG("PTZ protocol preference set to '%s' for '%s'", proto.empty() ? "automatic" : proto.c_str(), camId.c_str());
      output = cameraRegistry.list(true);
      return;
    }

    // Default playback stream index: {id, defaultStream: 0|1|2|...}
    if (request.isMember("id") && request.isMember("defaultStream")) {
      std::string camId = request["id"].asString();
      auto entry = cameraRegistry.find(camId);
      if (!entry) {
        output["error"] = "Camera not found";
        return;
      }
      int idx = request["defaultStream"].asInt();
      {
        std::lock_guard<std::mutex> lock(entry->commandMutex);
        INFO_MSG("defaultStream update: cam='%s' idx=%d streams.size=%zu", camId.c_str(), idx, entry->info.streams.size());
        if (idx >= 0 && idx < (int)entry->info.streams.size()) {
          entry->info.defaultStream = idx;
          INFO_MSG("defaultStream set to %d (uri=%s)", idx, entry->info.streams[idx].uri.c_str());
        } else {
          WARN_MSG("defaultStream index %d out of range (0..%zu)", idx, entry->info.streams.size());
        }
      }
      mutateShm.store(true);

      // Update the auto-created stream source to match the new default
      bool autoStreams = Controller::Storage["config"]["auto_camera_streams"].asBool();
      INFO_MSG("auto_camera_streams=%s, triggering update", autoStreams ? "true" : "false");
      if (autoStreams) { autoCreateCameraStreams(); }

      output = cameraRegistry.list(true);
      return;
    }

    // Credential-update path: {id, credentials: {protocol, username, password}}
    if (request.isMember("id") && request.isMember("credentials")) {
      std::string camId = request["id"].asString();
      auto entry = cameraRegistry.find(camId);
      if (!entry) {
        output["error"] = "Camera not found";
        return;
      }
      const JSON::Value & creds = request["credentials"];
      std::string proto = creds.isMember("protocol") ? creds["protocol"].asString() : "onvif";
      std::string user = creds.isMember("username") ? creds["username"].asString() : "";
      std::string pass = creds.isMember("password") ? creds["password"].asString() : "";
      {
        std::lock_guard<std::mutex> lock(entry->commandMutex);
        auto pit = entry->info.protocols.find(proto);
        if (pit != entry->info.protocols.end()) {
          if (!user.empty()) pit->second.username = user;
          if (!pass.empty()) pit->second.password = pass;
        } else {
          entry->info.addProtocol(proto, entry->info.host, 0, user, pass);
        }
      }
      mutateShm.store(true);

      // Re-probe device with new credentials
      ::Device::DeviceInfo probeInfo;
      {
        std::lock_guard<std::mutex> lk(entry->commandMutex);
        probeInfo.host = entry->info.host;
        probeInfo.id = entry->info.id;
        probeInfo.name = entry->info.name;
        probeInfo.protocols = entry->info.protocols;
      }
      // Clear throttle so probe runs immediately
      {
        std::lock_guard<std::mutex> lock(capabilityMutex);
        std::string devKey = canonicalDeviceKey(probeInfo);
        for (auto it = lastProbedAtMs.begin(); it != lastProbedAtMs.end();) {
          if (it->first.find(devKey) == 0)
            it = lastProbedAtMs.erase(it);
          else
            ++it;
        }
      }
#ifdef WITH_ONVIF
      // Invalidate cached ONVIF device so new credentials take effect
      if (proto == "onvif") {
        std::string cacheHost;
        uint16_t cachePort = 80;
        {
          std::lock_guard<std::mutex> lk(entry->commandMutex);
          auto pit = entry->info.protocols.find("onvif");
          if (pit != entry->info.protocols.end()) {
            cacheHost = pit->second.address;
            cachePort = pit->second.port;
          }
          if (cacheHost.empty()) cacheHost = entry->info.host;
        }
        invalidateOnvifCache(cacheHost + ":" + std::to_string(cachePort));
      }
#endif
      INFO_MSG("Credentials saved for '%s', triggering capability re-probe", camId.c_str());
      onDeviceDiscovered({probeInfo});

      output = cameraRegistry.list(true);
      return;
    }

    // Add/update camera by host path
    std::string host = request["host"].asString();
    if (host.empty()) {
      output["error"] = "Missing 'host' field";
      return;
    }
    ::Device::DeviceInfo info;
    info.host = host;
    info.id = canonicalDeviceKey(info);
    if (request.isMember("name")) info.name = request["name"].asString();

    std::string proto = request.isMember("protocol") ? request["protocol"].asString() : "onvif";
    uint16_t port = request.isMember("port") ? request["port"].asInt() : 0;
    std::string user = request.isMember("username") ? request["username"].asString() : "";
    std::string pass = request.isMember("password") ? request["password"].asString() : "";
    info.addProtocol(proto, host, port, user, pass);

    cameraRegistry.merge({info});
    mutateShm.store(true);
    onDeviceDiscovered({info});
    output = cameraRegistry.list(true);
  }

  void cameraConfigure(const JSON::Value & request, JSON::Value & output) {
    JSON::Value & cfg = Controller::Storage["config"];
    INFO_MSG("cameraConfigure called: %s", request.toString().c_str());

    if (request.isMember("auto_camera_streams")) {
      bool newVal = request["auto_camera_streams"].asBool();
      bool oldVal = cfg["auto_camera_streams"].asBool();
      cfg["auto_camera_streams"] = newVal;
      INFO_MSG("auto_camera_streams: %s -> %s", oldVal ? "true" : "false", newVal ? "true" : "false");

      if (newVal) {
        autoCreateCameraStreams();
      } else if (oldVal) {
        // Disable: remove all auto-camera tagged streams
        JSON::Value & streams = Controller::Storage["streams"];
        std::vector<std::string> toRemove;
        jsonForEach (streams, it) {
          if (it->isMember("tags")) {
            jsonForEach ((*it)["tags"], tit) {
              if (tit->asStringRef() == "auto-camera") {
                toRemove.push_back(it.key());
                break;
              }
            }
          }
        }
        for (const auto & name : toRemove) { Controller::deleteStream(name, streams); }
      }
    }

    if (request.isMember("device_discovery")) {
      bool newVal = request["device_discovery"].asBool();
      INFO_MSG("device_discovery: %s -> %s",
               cfg.isMember("device_discovery") && cfg["device_discovery"].asBool() ? "true" : "false", newVal ? "true" : "false");
      cfg["device_discovery"] = newVal;
    }

    if (request.isMember("auto_camera_thumbnailing")) {
      bool newThumb = request["auto_camera_thumbnailing"].asBool();
      INFO_MSG("auto_camera_thumbnailing: %s -> %s", cfg["auto_camera_thumbnailing"].asBool() ? "true" : "false",
               newThumb ? "true" : "false");
      cfg["auto_camera_thumbnailing"] = newThumb;
      if (cfg["auto_camera_streams"].asBool()) {
        autoCreateCameraStreams();
      } else {
        INFO_MSG("Skipping autoCreateCameraStreams: auto_camera_streams is disabled");
      }
    }

    output["device_discovery"] = cfg.isMember("device_discovery") ? cfg["device_discovery"] : JSON::Value(true);
    output["auto_camera_streams"] = cfg["auto_camera_streams"];
    output["auto_camera_thumbnailing"] = cfg["auto_camera_thumbnailing"];
  }

  void createCameraStream(const JSON::Value & request, JSON::Value & output) {
    std::string cameraId = request["id"].asString();
    auto entry = cameraRegistry.find(cameraId);
    if (!entry) {
      output["error"] = "Camera not found";
      return;
    }

    int streamIdx = request.isMember("stream_index") ? request["stream_index"].asInt() : 0;
    if (entry->info.streams.empty()) {
      output["error"] = "Camera has no discovered streams";
      return;
    }
    size_t idx = std::min((size_t)std::max(streamIdx, 0), entry->info.streams.size() - 1);
    const auto & stream = entry->info.streams[idx];
    if (stream.uri.empty()) {
      output["error"] = "No valid stream URI";
      return;
    }

    std::string streamName = request.isMember("stream_name") ? request["stream_name"].asString() : "";
    if (streamName.empty()) {
      streamName = entry->info.name.empty() ? cameraId : entry->info.name;
      // AddStreams rejects names that differ from their sanitized form, which
      // lowercases and strips invalid characters. Match that here (including the
      // lowercasing) so the generated name is accepted. Done inline rather than
      // via Util::sanitizeName because that treats spaces as wildcard separators.
      for (auto & c : streamName) {
        if (!isalnum((unsigned char)c) && c != '_' && c != '.' && c != '-') {
          c = '_';
        } else {
          c = tolower((unsigned char)c);
        }
      }
    }

    JSON::Value streamConfig;
    streamConfig["source"] = stream.uri;
    JSON::Value addReq;
    addReq[streamName] = streamConfig;
    Controller::AddStreams(addReq, Controller::Storage["streams"]);

    output["stream_name"] = streamName;
    output["source"] = stream.uri;
    if (Controller::Storage["streams"].isMember(streamName)) {
      output["success"] = true;
    } else {
      output["success"] = false;
      output["error"] = "Stream creation failed - name may be invalid";
    }
  }

  void listPresets(const JSON::Value & request, JSON::Value & output) {
    std::string id = request["id"].asString();
    if (id.empty()) {
      output["success"] = false;
      output["error"] = "Missing 'id' field";
      return;
    }

    auto entry = cameraRegistry.find(id);
    if (!entry) {
      output["success"] = false;
      output["error"] = "Camera not found";
      return;
    }

    // Copy device info under lock, then release before network I/O
    ::Device::DeviceInfo devInfo;
    bool hasOnvif;
    {
      std::lock_guard<std::mutex> devLock(entry->commandMutex);
      devInfo = entry->info;
      hasOnvif = entry->info.protocols.count("onvif") > 0;
    }

    if (!hasOnvif) {
      output["success"] = false;
      output["error"] = "Camera has no ONVIF protocol";
      return;
    }

#ifdef WITH_ONVIF
    auto regIt = protocolRegistry.find("onvif");
    if (regIt == protocolRegistry.end() || !regIt->second) {
      output["success"] = false;
      output["error"] = "ONVIF protocol not available";
      return;
    }
    auto dev = regIt->second->createDevice(devInfo);
    if (!dev || !dev->connect()) {
      output["success"] = false;
      output["error"] = "Failed to connect to camera";
      return;
    }

    auto onvifDev = dynamic_cast<ONVIF::Device *>(dev.get());
    if (!onvifDev) {
      output["success"] = false;
      output["error"] = "Not an ONVIF device";
      return;
    }

    auto profiles = onvifDev->getMediaProfiles();
    if (!profiles || profiles.value.empty()) {
      output["success"] = false;
      output["error"] = "No media profiles available";
      return;
    }

    auto presets = onvifDev->getPresets(profiles.value[0].token);
    if (!presets) {
      output["success"] = false;
      output["error"] = presets.error.message;
      return;
    }

    JSON::Value presetsJson;
    for (const auto & p : presets.value) {
      JSON::Value preset;
      preset["token"] = p.token;
      preset["name"] = p.name;
      preset["pan"] = (double)p.pan;
      preset["tilt"] = (double)p.tilt;
      preset["zoom"] = (double)p.zoom;
      presetsJson.append(preset);
    }
    output["presets"] = presetsJson;
    output["success"] = true;
#else
    output["success"] = false;
    output["error"] = "ONVIF support not compiled";
#endif
  }

} // namespace Controller
