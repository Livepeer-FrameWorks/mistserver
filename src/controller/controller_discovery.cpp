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
#include <cctype>
#include <deque>
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
  struct AsyncProtocolRuntime {
      int fd = -1;
      unsigned int retryCount = 0;
      uint64_t nextRetryMs = 0;
      uint64_t generation = 0;
  };
#ifdef WITH_ONVIF
  static AsyncProtocolRuntime onvifRuntime;
#endif
  static AsyncProtocolRuntime viscaRuntime;
#ifdef WITH_ONVIF
  static bool onvifEpochComplete = false;
#endif
  static bool viscaEpochComplete = false;

#ifdef WITH_ONVIF
  // Cached ONVIF devices for PTZ - avoids re-running GetCapabilities per command
  struct CachedOnvifDevice {
    std::unique_ptr<ONVIF::Device> device;
    std::mutex mutex; // serialize PTZ calls per device
    uint64_t lastUsedMs = 0;
  };
  static std::mutex onvifCacheMutex;
  static std::map<std::string, std::shared_ptr<CachedOnvifDevice>> onvifDeviceCache;

  static std::shared_ptr<CachedOnvifDevice>
      getOrCreateOnvifDevice(const std::string & key, const ::Device::ProtocolConfig & config) {
    std::lock_guard<std::mutex> lk(onvifCacheMutex);
    auto it = onvifDeviceCache.find(key);
    if (it != onvifDeviceCache.end()) {
      it->second->lastUsedMs = Util::bootMS();
      it->second->device->setTLSConfig(config.tlsPolicy, config.tlsCaFile);
      if (!config.username.empty()) { it->second->device->setCredentials(config.username, config.password); }
      return it->second;
    }
    auto entry = std::make_shared<CachedOnvifDevice>();
    const std::string path = config.path.empty() ? "/onvif/device_service" : config.path;
    const std::string scheme = config.scheme.empty() ? "http" : config.scheme;
    entry->device = std::unique_ptr<ONVIF::Device>(new ONVIF::Device(config.address, config.port, path, scheme));
    entry->device->setTLSConfig(config.tlsPolicy, config.tlsCaFile);
    entry->device->setRequestTimeout(3);
    if (!config.username.empty()) { entry->device->setCredentials(config.username, config.password); }
    entry->lastUsedMs = Util::bootMS();
    onvifDeviceCache[key] = entry;
    return entry;
  }

  static void invalidateOnvifCache(const std::string & endpointId) {
    if (endpointId.empty()) return;
    std::lock_guard<std::mutex> lk(onvifCacheMutex);
    onvifDeviceCache.erase(endpointId);
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
  static std::mutex capabilityResultsMutex;
  static std::deque<std::vector<::Device::DeviceInfo>> capabilityResults;

  // Protocol registry for polymorphic dispatch
  std::map<std::string, ::Device::Discovery*> protocolRegistry;

  // Initialize persistent discovery instances
#ifdef WITH_NDI
  std::unique_ptr<NDI::Discovery> ndiDiscovery;
#endif
#ifdef WITH_ONVIF
  std::unique_ptr<ONVIF::Discovery> onvifDiscovery;
#endif
  std::unique_ptr<VISCA::Discovery> viscaDiscovery;

  // join strings with a delimiter - useful for logging
  std::string joinStrings(const std::vector<std::string> & strings, const std::string & delim) {
    std::string result;
    for (size_t i = 0; i < strings.size(); ++i) {
      if (i > 0) result += delim;
      result += strings[i];
    }
    return result;
  }

  // CameraRegistry implementation
  std::string CameraRegistry::canonicalKey(const ::Device::DeviceInfo & dev) const {
    return canonicalDeviceKey(dev);
  }

  std::string CameraRegistry::resolveIdLocked(const std::string & id) const {
    if (cameras.count(id)) return id;
    auto endpoint = endpointIndex.find(id);
    return endpoint == endpointIndex.end() ? "" : endpoint->second;
  }

  void CameraRegistry::indexEntryLocked(const std::string & cameraId, const ::Device::DeviceInfo & info) {
    for (const auto & protocol : info.protocols) {
      const std::string endpointId = protocolEndpointId(protocol.first, protocol.second, info);
      if (endpointId.empty()) continue;
      auto existing = endpointIndex.find(endpointId);
      if (existing != endpointIndex.end() && existing->second != cameraId) {
        WARN_MSG("Refusing duplicate protocol endpoint identity '%s'", endpointId.c_str());
        continue;
      }
      endpointIndex[endpointId] = cameraId;
    }
  }

  void CameraRegistry::unindexEntryLocked(const ::Device::DeviceInfo & info) {
    for (const auto & protocol : info.protocols) {
      const std::string endpointId = protocolEndpointId(protocol.first, protocol.second, info);
      if (!endpointId.empty()) endpointIndex.erase(endpointId);
    }
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
          if (proto.isMember("password") && !proto["password"].asString().empty()) {
            proto["password"] = "***";
          }
        }
      }
      result.append(camJson);
    }
    return result;
  }

  std::shared_ptr<CameraEntry> CameraRegistry::find(const std::string & id) const {
    std::lock_guard<std::mutex> lock(mapMutex);
    const std::string cameraId = resolveIdLocked(id);
    auto it = cameras.find(cameraId);
    return it == cameras.end() ? nullptr : it->second;
  }

  bool CameraRegistry::snapshot(const std::string & id, ::Device::DeviceInfo & out) const {
    std::lock_guard<std::mutex> lock(mapMutex);
    const std::string cameraId = resolveIdLocked(id);
    auto it = cameras.find(cameraId);
    if (it == cameras.end()) return false;
    out = it->second->info;
    return true;
  }

  std::vector<std::pair<std::string, ::Device::DeviceInfo>> CameraRegistry::snapshots() const {
    std::vector<std::pair<std::string, ::Device::DeviceInfo>> result;
    std::lock_guard<std::mutex> lock(mapMutex);
    result.reserve(cameras.size());
    for (const auto & camera : cameras) result.push_back({camera.first, camera.second->info});
    return result;
  }

  bool CameraRegistry::update(const std::string & id, std::function<void(::Device::DeviceInfo &)> fn) {
    std::lock_guard<std::mutex> lock(mapMutex);
    const std::string cameraId = resolveIdLocked(id);
    auto it = cameras.find(cameraId);
    if (it == cameras.end()) return false;
    const ::Device::DeviceInfo previous = it->second->info;
    unindexEntryLocked(it->second->info);
    fn(it->second->info);
    it->second->info.id = cameraId;
    for (const auto &protocol : it->second->info.protocols) {
      const std::string endpointId = protocolEndpointId(protocol.first, protocol.second, it->second->info);
      auto collision = endpointIndex.find(endpointId);
      if (!endpointId.empty() && collision != endpointIndex.end() && collision->second != cameraId) {
        it->second->info = previous;
        indexEntryLocked(cameraId, previous);
        return false;
      }
    }
    indexEntryLocked(cameraId, it->second->info);
    return true;
  }

  size_t CameraRegistry::size() const {
    std::lock_guard<std::mutex> lock(mapMutex);
    return cameras.size();
  }

  void CameraRegistry::forEachDevice(
      std::function<void(const std::string & key, const ::Device::DeviceInfo & info)> fn) const {
    const auto copies = snapshots();
    for (const auto & camera : copies) fn(camera.first, camera.second);
  }

  void CameraRegistry::merge(const std::vector<::Device::DeviceInfo> & devices) {
    for (const auto & devInfo : devices) {
      if (stopDiscovery.load()) break;
      mergeOne(devInfo);
    }
  }

  std::string CameraRegistry::mergeOne(const ::Device::DeviceInfo & device) {
    ::Device::DeviceInfo incoming = device;
    if (incoming.protocols.empty()) return "";
    std::unordered_set<std::string> incomingEndpoints;
    for (auto & protocol : incoming.protocols) {
      if (protocol.first != "onvif" && protocol.first != "visca" && protocol.first != "ndi") return "";
      if (protocol.second.type.empty()) protocol.second.type = protocol.first;
      if (protocol.second.type != protocol.first) return "";
      if (protocol.second.endpointId.empty()) {
        protocol.second.endpointId = protocolEndpointId(protocol.first, protocol.second, incoming);
      }
      if (protocol.second.endpointId.empty() ||
          (protocol.first == "onvif" && protocol.second.endpointId.find("onvif:") != 0) ||
          (protocol.first == "visca" && protocol.second.endpointId.compare(0, 6, "visca:") != 0) ||
          (protocol.first == "ndi" &&
           (protocol.second.endpointId.size() <= 4 || protocol.second.endpointId.compare(0, 4, "ndi:") != 0)) ||
          !incomingEndpoints.insert(protocol.second.endpointId).second) {
        return "";
      }
    }

    std::lock_guard<std::mutex> lock(mapMutex);
    std::string cameraId;
    for (const auto & protocol : incoming.protocols) {
      auto found = endpointIndex.find(protocol.second.endpointId);
      if (found == endpointIndex.end()) continue;
      if (!cameraId.empty() && cameraId != found->second) {
        WARN_MSG("Refusing to merge discovery result spanning multiple logical cameras");
        return "";
      }
      cameraId = found->second;
    }

    if (cameraId.empty()) {
      cameraId = "camera:" + Util::generateUUID();
      auto entry = std::make_shared<CameraEntry>();
      incoming.id = cameraId;
      entry->info = incoming;
      cameras[cameraId] = entry;
      indexEntryLocked(cameraId, entry->info);
      return cameraId;
    }

    auto it = cameras.find(cameraId);
    if (it == cameras.end()) return "";
    unindexEntryLocked(it->second->info);
    updateDeviceInfo(it->second->info, incoming);
    it->second->info.id = cameraId;
    indexEntryLocked(cameraId, it->second->info);
    return cameraId;
  }

  std::string CameraRegistry::associate(const std::string & primaryId, const std::string & secondaryId,
                                        std::string & error) {
    std::lock_guard<std::mutex> lock(mapMutex);
    const std::string primaryKey = resolveIdLocked(primaryId);
    const std::string secondaryKey = resolveIdLocked(secondaryId);
    if (primaryKey.empty() || secondaryKey.empty()) {
      error = "Camera not found";
      return "";
    }
    if (primaryKey == secondaryKey) {
      error = "Cameras are already associated";
      return "";
    }
    auto primary = cameras.find(primaryKey);
    auto secondary = cameras.find(secondaryKey);
    if (primary == cameras.end() || secondary == cameras.end()) {
      error = "Camera not found";
      return "";
    }
    for (const auto & protocol : secondary->second->info.protocols) {
      auto existing = primary->second->info.protocols.find(protocol.first);
      if (existing != primary->second->info.protocols.end() &&
          existing->second.endpointId != protocol.second.endpointId) {
        error = "Both cameras contain different " + protocol.first + " endpoints";
        return "";
      }
    }

    unindexEntryLocked(primary->second->info);
    unindexEntryLocked(secondary->second->info);
    for (auto &protocol : primary->second->info.protocols) protocol.second.associationSource = "manual";
    ::Device::DeviceInfo merged = secondary->second->info;
    for (auto & protocol : merged.protocols) protocol.second.associationSource = "manual";
    updateDeviceInfo(primary->second->info, merged);
    primary->second->info.id = primaryKey;
    cameras.erase(secondary);
    indexEntryLocked(primaryKey, primary->second->info);
    return primaryKey;
  }

  std::string CameraRegistry::disassociate(const std::string & id, const std::string & protocol,
                                           std::string & error) {
    std::lock_guard<std::mutex> lock(mapMutex);
    const std::string cameraId = resolveIdLocked(id);
    auto camera = cameras.find(cameraId);
    if (camera == cameras.end()) {
      error = "Camera not found";
      return "";
    }
    auto endpoint = camera->second->info.protocols.find(protocol);
    if (endpoint == camera->second->info.protocols.end()) {
      error = "Protocol is not associated with this camera";
      return "";
    }
    if (camera->second->info.protocols.size() == 1) {
      error = "Cannot disassociate the camera's only protocol";
      return "";
    }

    unindexEntryLocked(camera->second->info);
    ::Device::DeviceInfo detached;
    detached.id = "camera:" + Util::generateUUID();
    detached.name = camera->second->info.name;
    detached.host = endpoint->second.address;
    detached.status = camera->second->info.status;
    detached.protocols[protocol] = endpoint->second;
    detached.protocols[protocol].associationSource = "manual";
    detached.hasPTZ = endpoint->second.capabilities.hasPTZ;
    detached.hasAudio = endpoint->second.capabilities.hasAudio;
    detached.hasMetadata = endpoint->second.capabilities.hasMetadata;

    // Move streams that unambiguously belong to the detached endpoint. NDI streams identify
    // themselves directly; ONVIF-owned RTSP/snapshot endpoints belong with ONVIF.
    std::vector<::Device::StreamEndpoint> retainedStreams;
    for (const auto &stream : camera->second->info.streams) {
      std::string streamProtocol = stream.protocol;
      std::transform(streamProtocol.begin(), streamProtocol.end(), streamProtocol.begin(),
                     [](unsigned char c) { return std::tolower(c); });
      const bool belongs = streamProtocol == protocol ||
                           (protocol == "onvif" && (streamProtocol == "rtsp" || streamProtocol == "http" ||
                                                    streamProtocol == "https"));
      if (belongs) detached.streams.push_back(stream);
      else retainedStreams.push_back(stream);
    }
    camera->second->info.streams.swap(retainedStreams);
    if (camera->second->info.defaultStream >= static_cast<int>(camera->second->info.streams.size())) {
      camera->second->info.defaultStream = camera->second->info.streams.empty() ? -1 : 0;
    }
    detached.defaultStream = detached.streams.empty() ? -1 : 0;

    camera->second->info.protocols.erase(endpoint);
    camera->second->info.hasPTZ = false;
    camera->second->info.hasAudio = false;
    camera->second->info.hasMetadata = false;
    for (const auto & remaining : camera->second->info.protocols) {
      camera->second->info.hasPTZ |= remaining.second.capabilities.hasPTZ;
      camera->second->info.hasAudio |= remaining.second.capabilities.hasAudio;
      camera->second->info.hasMetadata |= remaining.second.capabilities.hasMetadata;
    }
    indexEntryLocked(cameraId, camera->second->info);

    auto newEntry = std::make_shared<CameraEntry>();
    newEntry->info = detached;
    cameras[detached.id] = newEntry;
    indexEntryLocked(detached.id, detached);
    return detached.id;
  }

  size_t CameraRegistry::inferUniqueIpAssociations() {
    std::lock_guard<std::mutex> lock(mapMutex);
    std::map<std::string, std::vector<std::string>> onvifByIp;
    std::map<std::string, std::vector<std::string>> viscaByIp;
    for (const auto &camera : cameras) {
      const auto &info = camera.second->info;
      // NDI is a source identity, not a host identity. Never drag it into an inferred grouping.
      if (info.protocols.count("ndi")) continue;
      auto onvif = info.protocols.find("onvif");
      if (onvif != info.protocols.end()) {
        const std::string ip = extractCleanIP(onvif->second.address);
        if (!ip.empty()) onvifByIp[ip].push_back(camera.first);
      }
      auto visca = info.protocols.find("visca");
      if (visca != info.protocols.end()) {
        const std::string ip = extractCleanIP(visca->second.address);
        if (!ip.empty()) viscaByIp[ip].push_back(camera.first);
      }
    }

    size_t associated = 0;
    for (const auto &onvifGroup : onvifByIp) {
      auto viscaGroup = viscaByIp.find(onvifGroup.first);
      if (onvifGroup.second.size() != 1 || viscaGroup == viscaByIp.end() || viscaGroup->second.size() != 1) continue;
      const std::string primaryId = onvifGroup.second.front();
      const std::string secondaryId = viscaGroup->second.front();
      if (primaryId == secondaryId) continue;
      auto primary = cameras.find(primaryId);
      auto secondary = cameras.find(secondaryId);
      if (primary == cameras.end() || secondary == cameras.end()) continue;
      if (primary->second->info.protocols.count("visca") || secondary->second->info.protocols.count("onvif")) continue;

      unindexEntryLocked(primary->second->info);
      unindexEntryLocked(secondary->second->info);
      for (auto &protocol : primary->second->info.protocols) protocol.second.associationSource = "inferred";
      ::Device::DeviceInfo merged = secondary->second->info;
      for (auto &protocol : merged.protocols) protocol.second.associationSource = "inferred";
      updateDeviceInfo(primary->second->info, merged);
      primary->second->info.id = primaryId;
      cameras.erase(secondary);
      indexEntryLocked(primaryId, primary->second->info);
      ++associated;
    }
    return associated;
  }

  void CameraRegistry::remove(const std::string & id) {
    std::lock_guard<std::mutex> lock(mapMutex);
    const std::string cameraId = resolveIdLocked(id);
    auto it = cameras.find(cameraId);
    if (it == cameras.end()) return;
    unindexEntryLocked(it->second->info);
    cameras.erase(it);
  }

  bool CameraRegistry::removeIfProtocolOnly(const std::string & id, const std::string & proto) {
    std::lock_guard<std::mutex> lock(mapMutex);
    const std::string cameraId = resolveIdLocked(id);
    auto it = cameras.find(cameraId);
    if (it == cameras.end()) return false;
    if (it->second->info.protocols.size() != 1 || !it->second->info.protocols.count(proto)) return false;
    if (it->second->info.protocols.at(proto).associationSource == "manual") return false;
    unindexEntryLocked(it->second->info);
    cameras.erase(it);
    return true;
  }

  void CameraRegistry::updateStatus(const std::string & id, const std::string & status) {
    std::lock_guard<std::mutex> lock(mapMutex);
    const std::string cameraId = resolveIdLocked(id);
    auto it = cameras.find(cameraId);
    if (it != cameras.end()) it->second->info.status = status;
  }

  void CameraRegistry::loadFromStorage(const JSON::Value & camerasJson) {
    std::lock_guard<std::mutex> lock(mapMutex);
    cameras.clear();
    endpointIndex.clear();
    const auto isCameraId = [](const std::string & id) {
      if (id.size() != 43 || id.substr(0, 7) != "camera:") return false;
      const std::string uuid = id.substr(7);
      for (size_t i = 0; i < uuid.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
          if (uuid[i] != '-') return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(uuid[i]))) {
          return false;
        }
      }
      return true;
    };
    jsonForEachConst(camerasJson, it) {
      if (!it->isObject()) continue;
      ::Device::DeviceInfo info = ::Device::DeviceInfo::fromJSON(*it);
      bool valid = isCameraId(info.id) && !cameras.count(info.id) && !info.protocols.empty();
      std::unordered_set<std::string> localEndpoints;
      for (auto & protocol : info.protocols) {
        const std::string &type = protocol.first;
        const ::Device::ProtocolConfig &config = protocol.second;
        ::Device::ProtocolConfig derivedConfig = config;
        derivedConfig.endpointId.clear();
        if ((type != "onvif" && type != "visca" && type != "ndi") || config.type != type ||
            config.endpointId.empty() || endpointIndex.count(config.endpointId) ||
            !localEndpoints.insert(config.endpointId).second ||
            (config.associationSource != "" && config.associationSource != "manual" &&
             config.associationSource != "inferred") ||
            (type == "onvif" &&
             (config.endpointId.find("onvif:") != 0 ||
              (config.scheme != "http" && config.scheme != "https") ||
              (config.tlsPolicy != "required" && config.tlsPolicy != "opportunistic" &&
               config.tlsPolicy != "insecure") || config.path.empty() || config.path[0] != '/')) ||
            (type == "visca" &&
             (config.endpointId != protocolEndpointId(type, derivedConfig, info) ||
              (config.transport != "udp" && config.transport != "tcp") ||
              (config.framing != "raw" && config.framing != "visca-ip"))) ||
            (type == "ndi" && (config.endpointId.size() <= 4 || config.endpointId.compare(0, 4, "ndi:") != 0))) {
          valid = false;
        }
      }
      if (!valid) continue;
      auto entry = std::make_shared<CameraEntry>();
      entry->info = std::move(info);
      cameras[entry->info.id] = entry;
      indexEntryLocked(entry->info.id, entry->info);
    }
  }

  void CameraRegistry::syncToStorage(JSON::Value & storage) const {
    std::lock_guard<std::mutex> lock(mapMutex);
    storage["cameras"] = JSON::Value();
    for (const auto & kv : cameras) {
      storage["cameras"].append(kv.second->info.toJSON());
    }
  }

  static size_t calcProtocolsNestedSize(size_t maxProtocols);
  static size_t calcStreamsNestedSize(size_t maxStreams);

  // Return a deep copy of the given JSON with any "password" fields masked, for safe logging.
  static JSON::Value redactSecrets(const JSON::Value & in) {
    JSON::Value out = in;
    if (out.isObject()) {
      jsonForEach(out, it) {
        if (it.key() == "password" && (*it).isString()) {
          *it = "***";
        } else {
          *it = redactSecrets(*it);
        }
      }
    } else if (out.isArray()) {
      jsonForEach(out, it) { *it = redactSecrets(*it); }
    }
    return out;
  }

  void CameraRegistry::writeToShm() {
    // Sync to Storage for persistence and SHM publication.
    syncToStorage(Storage);

    INFO_MSG("Writing camera information to shared memory");
    jsonForEach(Storage["cameras"], it) { INFO_MSG("%s\n", redactSecrets(*it).toString().c_str()); }

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
    jsonForEach(Storage["cameras"], it) {
      if ((*it)["protocols"].size() > maxProtocols) maxProtocols = (*it)["protocols"].size();
      if ((*it)["streams"].size() > maxStreams) maxStreams = (*it)["streams"].size();
    }
    camAccX.addField("protocols", RAX_NESTED, (uint32_t)calcProtocolsNestedSize(maxProtocols));
    camAccX.addField("streams", RAX_NESTED, (uint32_t)calcStreamsNestedSize(maxStreams));

    size_t reqCount = (pageSize - camAccX.getOffset()) / camAccX.getRSize();
    camAccX.setRCount(reqCount);
    size_t cameraCount = Storage["cameras"].size();
    camAccX.setPresent(cameraCount);
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
    jsonForEach(Storage["cameras"], it) {
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
      camAccX.setString("ptzProtocol", camera["ptzProtocol"].asString(), index);
      camAccX.setInt("defaultStream", camera.isMember("defaultStream") ? camera["defaultStream"].asInt() : -1,
                     index);
      camAccX.setString("snapshotUri", camera["snapshotUri"].asString(), index);
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

  // End CameraRegistry implementation
  static inline bool missingInitialSetupConfig() {
    if (!Storage.isMember("account") || Storage["account"].size() < 1) { return true; }
    if (!Storage.isMember("config") || !Storage["config"].isMember("protocols") ||
        Storage["config"]["protocols"].size() < 1) {
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
    JSON::Value &cfg = Controller::Storage["config"];
    bool discoveryEnabled = !cfg.isMember("device_discovery") || cfg["device_discovery"].asBool();
    if (!discoveryEnabled) {
      if (discoveryState.asyncStarted) {
        stopAsyncDiscovery();
        discoveryState.asyncStarted = false;
      }
      return 10000;
    }

    // Attempt to bring up the NDI runtime once. NDI::initialize() dlopens the runtime,
    // verifies CPU support and inits the library; failure (runtime missing or unsupported
    // CPU) disables ONLY NDI discovery - ONVIF and VISCA discovery continue regardless.
    if (!discoveryState.ndiAttempted) {
      discoveryState.ndiAttempted = true;
#ifdef WITH_NDI
      if (NDI::initialize()) {
        discoveryState.ndiInitialized = true;
        INFO_MSG("NDI initialized %s", NDI::version());
      } else {
        WARN_MSG("NDI runtime unavailable - NDI discovery disabled (ONVIF/VISCA unaffected)");
      }
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

    // Capability workers never touch controller-owned state. Apply their immutable results here.
    std::deque<std::vector<::Device::DeviceInfo>> readyResults;
    {
      std::lock_guard<std::mutex> lock(capabilityResultsMutex);
      readyResults.swap(capabilityResults);
    }
    while (!readyResults.empty()) {
      onDeviceCapabilities(readyResults.front());
      readyResults.pop_front();
    }

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
    {
      std::lock_guard<std::mutex> lock(capabilityMutex);
      pendingCaps.clear();
      lastProbedAtMs.clear();
    }
    {
      std::lock_guard<std::mutex> lock(capabilityResultsMutex);
      capabilityResults.clear();
    }

    protocolRegistry.clear();
#ifdef WITH_ONVIF
    {
      std::lock_guard<std::mutex> lock(onvifCacheMutex);
      onvifDeviceCache.clear();
    }
#endif

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
#ifdef WITH_ONVIF
    onvifDiscovery.reset();
#endif
    viscaDiscovery.reset();
    discoveryState = DiscoveryState{};

    INFO_MSG("Discovery system shutdown complete");
  }

  // setup protocol fields in a RelAccX structure
  void setupProtocolsAccX(Util::RelAccX & rax) {
    rax.addField("type", RAX_32STRING);
    rax.addField("endpointId", RAX_512STRING);
    rax.addField("address", RAX_256STRING);
    rax.addField("port", RAX_32UINT);
    rax.addField("scheme", RAX_32STRING);
    rax.addField("transport", RAX_32STRING);
    rax.addField("framing", RAX_32STRING);
    rax.addField("path", RAX_256STRING);
    rax.addField("alternateEndpoints", RAX_512STRING);
    rax.addField("tlsPolicy", RAX_32STRING);
    rax.addField("tlsStatus", RAX_64STRING);
    rax.addField("lastError", RAX_256STRING);
    rax.addField("associationSource", RAX_32STRING);
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
    rax.addField("supportedResolutions", RAX_256STRING);
    rax.addField("supportedFramerates", RAX_256STRING);
    rax.addField("supportedAudioFormats", RAX_256STRING);
  }

  // setup stream fields in a RelAccX structure
  void setupStreamsAccX(Util::RelAccX & rax) {
    rax.addField("protocol", RAX_32STRING);
    rax.addField("format", RAX_32STRING);
    rax.addField("transport", RAX_32STRING);
    rax.addField("uri", RAX_512STRING);
    rax.addField("name", RAX_128STRING);
    rax.addField("resolution", RAX_32STRING);
    rax.addField("framerate", RAX_32STRING);
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
    rax.addField("id", RAX_64STRING);
    rax.addField("name", RAX_128STRING);
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
    rax.addField("ptzProtocol", RAX_32STRING);
    rax.addField("defaultStream", RAX_32INT);
    rax.addField("snapshotUri", RAX_512STRING);
  }

  // Exact nested sizes using in-memory RelAccX (no SHM side effects)
  static size_t calcProtocolsNestedSize(size_t maxProtocols) {
    std::vector<char> buf(16384);
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

  // calculate required page size exactly using in-memory RelAccX layouts
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
    auto endpointIdField = protoAccX.getFieldData("endpointId");
    auto addressField = protoAccX.getFieldData("address");
    auto portField = protoAccX.getFieldData("port");
    auto schemeField = protoAccX.getFieldData("scheme");
    auto transportField = protoAccX.getFieldData("transport");
    auto framingField = protoAccX.getFieldData("framing");
    auto pathField = protoAccX.getFieldData("path");
    auto alternateEndpointsField = protoAccX.getFieldData("alternateEndpoints");
    auto tlsPolicyField = protoAccX.getFieldData("tlsPolicy");
    auto tlsStatusField = protoAccX.getFieldData("tlsStatus");
    auto lastErrorField = protoAccX.getFieldData("lastError");
    auto associationSourceField = protoAccX.getFieldData("associationSource");
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
    auto supportedResolutionsField = protoAccX.getFieldData("supportedResolutions");
    auto supportedFrameratesField = protoAccX.getFieldData("supportedFramerates");
    auto supportedAudioFormatsField = protoAccX.getFieldData("supportedAudioFormats");

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
      protoAccX.setString(endpointIdField, proto["endpointId"].asString(), protoIndex);
      protoAccX.setString(addressField, proto["address"].asString(), protoIndex);
      protoAccX.setInt(portField, proto["port"].asInt(), protoIndex);
      protoAccX.setString(schemeField, proto["scheme"].asString(), protoIndex);
      protoAccX.setString(transportField, proto["transport"].asString(), protoIndex);
      protoAccX.setString(framingField, proto["framing"].asString(), protoIndex);
      protoAccX.setString(pathField, proto["path"].asString(), protoIndex);
      protoAccX.setString(alternateEndpointsField, toCsv(proto["alternateEndpoints"]), protoIndex);
      protoAccX.setString(tlsPolicyField, proto["tlsPolicy"].asString(), protoIndex);
      protoAccX.setString(tlsStatusField, proto["tlsStatus"].asString(), protoIndex);
      protoAccX.setString(lastErrorField, proto["lastError"].asString(), protoIndex);
      protoAccX.setString(associationSourceField, proto["associationSource"].asString(), protoIndex);
      protoAccX.setString(usernameField, "", protoIndex);
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
      protoAccX.setString(supportedResolutionsField, toCsv(caps["supportedResolutions"]), protoIndex);
      protoAccX.setString(supportedFrameratesField, toCsv(caps["supportedFramerates"]), protoIndex);
      protoAccX.setString(supportedAudioFormatsField, toCsv(caps["supportedAudioFormats"]), protoIndex);
      protoIndex++;
    }
    protoAccX.setPresent(protoIndex);
    protoAccX.setEndPos(protoIndex);
  }

  void writeStreamsToShm(const JSON::Value & streams, Util::RelAccX & streamAccX) {
    // Get field references first
    auto protocolField = streamAccX.getFieldData("protocol");
    auto formatField = streamAccX.getFieldData("format");
    auto transportField = streamAccX.getFieldData("transport");
    auto uriField = streamAccX.getFieldData("uri");
    auto nameField = streamAccX.getFieldData("name");
    auto resolutionField = streamAccX.getFieldData("resolution");
    auto framerateField = streamAccX.getFieldData("framerate");
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
      streamAccX.setString(resolutionField, stream["resolution"].asString(), streamIndex);
      streamAccX.setString(framerateField, stream["framerate"].asString(), streamIndex);
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
    streamAccX.setPresent(streamIndex);
    streamAccX.setEndPos(streamIndex);
  }

  // pretty print JSON values recursively
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

  void associateCameras(const JSON::Value & request, JSON::Value & output) {
    const std::string primaryId = request["primaryId"].asString();
    const std::string secondaryId = request["secondaryId"].asString();
    if (primaryId.empty() || secondaryId.empty()) {
      output["success"] = false;
      output["error"] = "primaryId and secondaryId are required";
      return;
    }
    std::string error;
    const std::string cameraId = cameraRegistry.associate(primaryId, secondaryId, error);
    output["success"] = !cameraId.empty();
    if (cameraId.empty()) {
      output["error"] = error;
      return;
    }
    mutateShm.store(true);
    output["id"] = cameraId;
    output["cameras"] = cameraRegistry.list(true);
  }

  void disassociateCamera(const JSON::Value & request, JSON::Value & output) {
    const std::string cameraId = request["id"].asString();
    const std::string protocol = request["protocol"].asString();
    if (cameraId.empty() || protocol.empty()) {
      output["success"] = false;
      output["error"] = "id and protocol are required";
      return;
    }
    std::string error;
    const std::string detachedId = cameraRegistry.disassociate(cameraId, protocol, error);
    output["success"] = !detachedId.empty();
    if (detachedId.empty()) {
      output["error"] = error;
      return;
    }
    mutateShm.store(true);
    output["id"] = detachedId;
    output["cameras"] = cameraRegistry.list(true);
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

    ::Device::DeviceInfo cameraInfo;
    if (!cameraRegistry.snapshot(id, cameraInfo)) {
      output["success"] = false;
      output["error"] = "Device not found";
      return;
    }

    bool hasPTZ = cameraInfo.hasPTZ;
    std::vector<std::string> supportedProtocols;
    if (preferredProtocol.empty()) { preferredProtocol = cameraInfo.ptzProtocol; }
    for (const auto & proto : cameraInfo.protocols) {
      if (proto.second.capabilities.hasPTZ) { supportedProtocols.push_back(proto.first); }
    }

    INFO_MSG("PTZ command '%s' for device '%s'%s", commandName.c_str(), id.c_str(),
             preferredProtocol.empty() ? " (auto)" : (" via " + preferredProtocol).c_str());

    // In automatic mode, prefer fire-and-forget protocols (ONVIF/VISCA) over NDI
    if (preferredProtocol.empty()) {
      std::sort(supportedProtocols.begin(), supportedProtocols.end(),
                [](const std::string & a, const std::string & b) {
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

    std::string protocolUsed;
    if (!preferredProtocol.empty()) {
      auto it = std::find(supportedProtocols.begin(), supportedProtocols.end(), preferredProtocol);
      if (it == supportedProtocols.end()) {
        errorMsg = "Requested protocol does not support PTZ for this camera";
      } else {
        commandSent = sendCommandViaProtocol(id, preferredProtocol, cmd, errorMsg);
        if (commandSent) protocolUsed = preferredProtocol;
      }
    } else {
      for (const auto & protocol : supportedProtocols) {
        if (sendCommandViaProtocol(id, protocol, cmd, errorMsg)) {
          commandSent = true;
          protocolUsed = protocol;
          break;
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
    if (!protocolUsed.empty()) output["protocol"] = protocolUsed;
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
      ::Device::DeviceInfo info;
      if (!entry || !cameraRegistry.snapshot(deviceId, info)) {
        errorMsg = "Camera not found in registry";
        WARN_MSG("PTZ via ONVIF: %s", errorMsg.c_str());
        return false;
      }
      auto pit = info.protocols.find("onvif");
      if (pit == info.protocols.end()) {
        errorMsg = "Camera has no ONVIF endpoint";
        return false;
      }
      ::Device::ProtocolConfig config = pit->second;
      if (config.address.empty()) config.address = info.host;
      if (config.scheme.empty()) config.scheme = "http";
      if (!config.port) config.port = config.scheme == "https" ? 443 : 80;
      if (config.endpointId.empty()) config.endpointId = protocolEndpointId("onvif", config, info);
      if (config.address.empty()) {
        errorMsg = "No ONVIF host for device";
        WARN_MSG("PTZ via ONVIF: %s", errorMsg.c_str());
        return false;
      }
      std::lock_guard<std::mutex> commandLock(entry->commandMutex);
      std::shared_ptr<CachedOnvifDevice> cached = getOrCreateOnvifDevice(config.endpointId, config);
      std::lock_guard<std::mutex> devLock(cached->mutex);
      if (!cached->device->isConnected()) {
        HIGH_MSG("PTZ via ONVIF: Connecting to %s:%d (auth: %s)", config.address.c_str(), config.port,
                 config.username.empty() ? "none" : "yes");
        if (!cached->device->connect()) {
          errorMsg = "Failed to connect to ONVIF device at " + config.address + ":" + std::to_string(config.port);
          WARN_MSG("PTZ via ONVIF: %s", errorMsg.c_str());
          invalidateOnvifCache(config.endpointId);
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
      ::Device::DeviceInfo info;
      if (!entry || !cameraRegistry.snapshot(deviceId, info)) {
        errorMsg = "Camera not found in registry";
        WARN_MSG("PTZ via VISCA: %s", errorMsg.c_str());
        return false;
      }
      auto pit = info.protocols.find("visca");
      if (pit == info.protocols.end()) {
        errorMsg = "Camera has no VISCA endpoint";
        return false;
      }
      std::string host = pit->second.address.empty() ? info.host : pit->second.address;
      if (host.empty()) {
        errorMsg = "No VISCA host for device";
        WARN_MSG("PTZ via VISCA: %s", errorMsg.c_str());
        return false;
      }
      const std::string viscaId = pit->second.endpointId.empty() ? protocolEndpointId("visca", pit->second, info)
                                                                 : pit->second.endpointId;
      HIGH_MSG("PTZ via VISCA: Sending to %s", viscaId.c_str());
      std::lock_guard<std::mutex> commandLock(entry->commandMutex);
      bool result = viscaDiscovery && viscaDiscovery->sendCommand(info, cmd);
      if (!result) { errorMsg = "VISCA command failed for " + viscaId; }
      return result;
    }
    // NDI PTZ sends control metadata to the source. Do this synchronously so the API reports the
    // actual outcome instead of a premature success; these are infrequent, user-initiated commands,
    // so the brief send latency is acceptable and preferable to lying to the caller.
    if (protocol == "ndi") {
      auto entry = cameraRegistry.find(deviceId);
      ::Device::DeviceInfo info;
      if (!entry || !cameraRegistry.snapshot(deviceId, info) || !info.protocols.count("ndi")) {
        errorMsg = "Camera has no NDI endpoint";
        return false;
      }
      const std::string endpointId = info.protocols.at("ndi").endpointId;
      HIGH_MSG("PTZ via NDI: sending to '%s'", endpointId.c_str());
      std::lock_guard<std::mutex> commandLock(entry->commandMutex);
      bool result = it->second->sendCommand(endpointId, cmd);
      if (!result) { errorMsg = "NDI command failed for " + endpointId; }
      return result;
    }
    HIGH_MSG("PTZ via %s: Sending to '%s'", protocol.c_str(), deviceId.c_str());
    bool result = it->second->sendCommand(deviceId, cmd);
    if (!result) { errorMsg = protocol + " command failed for " + deviceId; }
    return result;
  }

  // Invoked when NDI sources disappear. NDI sources are ephemeral, so drop entries that are
  // exclusively NDI; cameras that also expose ONVIF/VISCA (or were manually configured) are kept.
  void onDeviceRemoved(const std::vector<::Device::DeviceInfo> & devices) {
    bool changed = false;
    for (const auto & dev : devices) {
      auto protocol = dev.protocols.find("ndi");
      if (protocol == dev.protocols.end() || protocol->second.endpointId.empty()) continue;
      const std::string endpointId = protocol->second.endpointId;
      ::Device::DeviceInfo current;
      if (cameraRegistry.snapshot(endpointId, current)) {
        auto currentNdi = current.protocols.find("ndi");
        if (currentNdi != current.protocols.end() && currentNdi->second.associationSource == "manual") {
          cameraRegistry.update(endpointId, [](::Device::DeviceInfo &info) {
            info.protocols["ndi"].lastError = "NDI source unavailable";
            if (info.protocols.size() == 1) info.status = "disconnected";
          });
          INFO_MSG("NDI source '%s' is unavailable; retaining its explicit camera association", dev.name.c_str());
          changed = true;
          continue;
        }
      }
      if (cameraRegistry.removeIfProtocolOnly(endpointId, "ndi")) {
        INFO_MSG("Removed stale NDI-only camera '%s'", dev.name.c_str());
        changed = true;
      }
    }
    if (changed) { mutateShm.store(true); }
  }

  // Async Discovery Implementation
  void onDeviceDiscovered(const std::vector<::Device::DeviceInfo> & devices) {
    HIGH_MSG("Processing %zu discovered devices via callback", devices.size());

    // Merge additions into registry (struct operations, no JSON round-trip)
    cameraRegistry.merge(devices);
    mutateShm.store(true);

    // Queue capability enrichment jobs with dedupe and throttling
    struct ProbeJob {
        ::Device::DeviceInfo dev;
        std::string proto;
        ::Device::Discovery *discovery = nullptr;
        std::string pendingKey;
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
        auto registryIt = protocolRegistry.find(proto.first);
        if (enqueue && registryIt != protocolRegistry.end() && registryIt->second) {
          ProbeJob job;
          job.dev = devInfo;
          job.proto = proto.first;
          job.discovery = registryIt->second;
          job.pendingKey = key;
          jobs.push_back(job);
        } else if (enqueue) {
          std::lock_guard<std::mutex> lock(capabilityMutex);
          pendingCaps.erase(key);
        }
      }
    }

    if (jobs.empty()) return;

    // Offload capability enrichment to a managed background thread
    auto doneFlag = std::make_shared<std::atomic<bool>>(false);
    std::thread probeThread([jobs, doneFlag]() {
      std::vector<::Device::DeviceInfo> enriched;
      enriched.reserve(jobs.size());
      try {
        for (const auto & job : jobs) {
        if (stopDiscovery.load() || !Controller::conf.is_active) break;

        const auto & devInfo = job.dev;
        const std::string & protoType = job.proto;
        ::Device::DeviceInfo capsInfo;
        capsInfo.host = devInfo.host;
        capsInfo.id = devInfo.id;
        capsInfo.name = devInfo.name;

        if (job.discovery) {
          // Enrich device info with stored credentials from registry
          ::Device::DeviceInfo enrichedInfo = devInfo;
          std::string devKey = canonicalDeviceKey(devInfo);
          ::Device::DeviceInfo storedInfo;
          if (cameraRegistry.snapshot(devKey, storedInfo)) {
            auto protoIt = storedInfo.protocols.find(protoType);
            if (protoIt != storedInfo.protocols.end()) {
              enrichedInfo.protocols[protoType].username = protoIt->second.username;
              enrichedInfo.protocols[protoType].password = protoIt->second.password;
              enrichedInfo.protocols[protoType].tlsPolicy = protoIt->second.tlsPolicy;
              enrichedInfo.protocols[protoType].tlsCaFile = protoIt->second.tlsCaFile;
            }
          }
          capsInfo.protocols[protoType] = enrichedInfo.protocols[protoType];
          auto dev = job.discovery->createDevice(enrichedInfo);
          if (dev && !stopDiscovery.load() && Controller::conf.is_active && dev->connect()) {
            updateDeviceInfo(capsInfo, dev->queryCapabilities());
          }
        }

          enriched.push_back(capsInfo);

          {
            std::lock_guard<std::mutex> lock(capabilityMutex);
            lastProbedAtMs[job.pendingKey] = Util::bootMS();
            pendingCaps.erase(job.pendingKey);
          }
        }
      } catch (const std::exception &e) {
        WARN_MSG("Capability probe worker failed: %s", e.what());
      } catch (...) {
        WARN_MSG("Capability probe worker failed with an unknown exception");
      }

      // Ensure jobs skipped because of shutdown cannot remain permanently pending.
      {
        std::lock_guard<std::mutex> lock(capabilityMutex);
        for (const auto & job : jobs) pendingCaps.erase(job.pendingKey);
      }
      if (!enriched.empty() && !stopDiscovery.load() && Controller::conf.is_active) {
        std::lock_guard<std::mutex> lock(capabilityResultsMutex);
        capabilityResults.push_back(std::move(enriched));
      }
      doneFlag->store(true);
    });
    {
      std::lock_guard<std::mutex> lk(probeThreadsMutex);
      probeThreads.push_back({std::move(probeThread), doneFlag});
    }
  }

  void autoCreateCameraStreams(){
    JSON::Value &existingStreams = Controller::Storage["streams"];
    bool thumbnailing = Controller::Storage["config"]["auto_camera_thumbnailing"].asBool();
    INFO_MSG("autoCreateCameraStreams: thumbnailing=%s", thumbnailing ? "true" : "false");

    cameraRegistry.forEachDevice([&](const std::string &key, const ::Device::DeviceInfo &info){
      if (info.streams.empty()){
        HIGH_MSG("autoCreateCameraStreams: device '%s' has no streams, skipping", key.c_str());
        return;
      }

      // Pick the default stream URI - auto-select prefers RTSP/ONVIF over NDI
      int defIdx = info.defaultStream;
      if (defIdx < 0 || defIdx >= (int)info.streams.size()){
        defIdx = 0;
        for (size_t si = 0; si < info.streams.size(); ++si){
          std::string proto = info.streams[si].protocol;
          for (auto &ch : proto) ch = tolower(ch);
          if (proto == "rtsp" || proto == "onvif"){
            defIdx = (int)si;
            break;
          }
        }
      }
      const std::string &uri = info.streams[defIdx].uri;
      if (uri.empty()){
        HIGH_MSG("autoCreateCameraStreams: device '%s' default stream has no URI", key.c_str());
        return;
      }

      // One stream per device: cam_SANITIZEDKEY
      std::string baseName = key;
      for (auto &c : baseName){
        if (!isalnum(c) && c != '_' && c != '-' && c != '.') c = '_';
        else c = tolower(c);
      }
      std::string streamName = "cam_" + baseName;
      INFO_MSG("autoCreateCameraStreams: device='%s' streamName='%s' defIdx=%d uri='%s' exists=%s",
               key.c_str(), streamName.c_str(), defIdx, uri.c_str(),
               existingStreams.isMember(streamName) ? "yes" : "no");

      if (existingStreams.isMember(streamName)){
        JSON::Value updated = existingStreams[streamName];

        // Update source if default stream changed
        std::string currentSource = updated["source"].asString();
        bool sourceChanged = (currentSource != uri);
        if (sourceChanged){
          updated["source"] = uri;
          INFO_MSG("Updating stream '%s' source: %s -> %s", streamName.c_str(), currentSource.c_str(), uri.c_str());
        }

        // Sync thumbnailing process
        bool hasAV0 = updated.isMember("processes") && updated["processes"].isMember("AV0");
        if (thumbnailing && !hasAV0){
          JSON::Value mjpeg;
          mjpeg["process"] = "AV";
          mjpeg["codec"] = "JPEG";
          mjpeg["x-LSP-kind"] = "video";
          mjpeg["quality"] = 15;
          mjpeg["gopsize"] = 30;
          updated["processes"]["AV0"] = mjpeg;
          INFO_MSG("Adding MJPEG thumbnailing process to stream '%s'", streamName.c_str());
        }else if (!thumbnailing && hasAV0){
          updated["processes"].removeMember("AV0");
          INFO_MSG("Removing MJPEG thumbnailing process from stream '%s'", streamName.c_str());
        }

        if (sourceChanged || (thumbnailing && !hasAV0) || (!thumbnailing && hasAV0)){
          JSON::Value addReq;
          addReq[streamName] = updated;
          Controller::AddStreams(addReq, existingStreams);
        }
        if (sourceChanged){
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
      if (thumbnailing){
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

      if (existingStreams.isMember(streamName)){
        INFO_MSG("Auto-created stream '%s' for device '%s' (source: %s)", streamName.c_str(),
                 info.id.c_str(), uri.c_str());
      }else{
        WARN_MSG("Failed to auto-create stream '%s' for device '%s'", streamName.c_str(),
                 info.id.c_str());
      }
    });
  }

  void onDeviceCapabilities(const std::vector<::Device::DeviceInfo> & devices){
    HIGH_MSG("Merging %zu capability results", devices.size());
    cameraRegistry.merge(devices);
    mutateShm.store(true);

    if (Controller::Storage["config"]["auto_camera_streams"].asBool()){
      autoCreateCameraStreams();
    }
  }

  void initAsyncDiscovery(){
    INFO_MSG("Initializing async discovery system");

    // Initialize discovery instances if not already done
#ifdef WITH_ONVIF
    if (!onvifDiscovery) { onvifDiscovery = std::unique_ptr<ONVIF::Discovery>(new ONVIF::Discovery); }
#endif
    if (!viscaDiscovery) { viscaDiscovery = std::unique_ptr<VISCA::Discovery>(new VISCA::Discovery); }
#ifdef WITH_NDI
    // Only stand up NDI discovery when the runtime actually loaded; otherwise NDI is skipped
    if (discoveryState.ndiInitialized && !ndiDiscovery) {
      ndiDiscovery = std::unique_ptr<NDI::Discovery>(new NDI::Discovery);
    }
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

  static void scheduleDiscoveryRetry(AsyncProtocolRuntime & runtime, const char *protocol) {
    static const uint32_t delays[] = {1000, 2000, 5000, 10000, 30000};
    const size_t index = std::min<size_t>(runtime.retryCount, sizeof(delays) / sizeof(delays[0]) - 1);
    runtime.nextRetryMs = Util::bootMS() + delays[index];
    if (runtime.retryCount < sizeof(delays) / sizeof(delays[0]) - 1) ++runtime.retryCount;
    WARN_MSG("%s discovery will retry in %u ms", protocol, delays[index]);
  }

#ifdef WITH_ONVIF
  static bool startOnvifDiscovery() {
    if (!onvifDiscovery || onvifDiscovery->isAsyncDiscoveryRunning()) return false;
    if (Util::bootMS() < onvifRuntime.nextRetryMs) return false;
    ++onvifRuntime.generation;
    if (!onvifDiscovery->startAsyncDiscovery(onDeviceDiscovered, 10000)) {
      scheduleDiscoveryRetry(onvifRuntime, "ONVIF");
      return false;
    }
    const int fd = onvifDiscovery->getSocket();
    if (fd < 0) {
      onvifDiscovery->stopAsyncDiscovery();
      scheduleDiscoveryRetry(onvifRuntime, "ONVIF");
      return false;
    }
    Controller::E.remove(fd);
    onvifRuntime.fd = fd;
    Controller::E.addSocket(fd, [](void *) {
      if (onvifDiscovery && onvifRuntime.fd >= 0 && onvifDiscovery->getSocket() == onvifRuntime.fd) {
        onvifDiscovery->processSocketData();
      }
    }, 0);
    onvifRuntime.retryCount = 0;
    onvifRuntime.nextRetryMs = 0;
    HIGH_MSG("ONVIF async discovery started, socket %d registered", fd);
    return true;
  }
#endif

  static bool startViscaDiscovery() {
    if (!viscaDiscovery || viscaDiscovery->isAsyncDiscoveryRunning()) return false;
    if (Util::bootMS() < viscaRuntime.nextRetryMs) return false;
    ++viscaRuntime.generation;
    if (!viscaDiscovery->startAsyncDiscovery(onDeviceDiscovered, 10000)) {
      scheduleDiscoveryRetry(viscaRuntime, "VISCA");
      return false;
    }
    const int fd = viscaDiscovery->getSocket();
    if (fd < 0) {
      viscaDiscovery->stopAsyncDiscovery();
      scheduleDiscoveryRetry(viscaRuntime, "VISCA");
      return false;
    }
    Controller::E.remove(fd);
    viscaRuntime.fd = fd;
    Controller::E.addSocket(fd, [](void *) {
      if (viscaDiscovery && viscaRuntime.fd >= 0 && viscaDiscovery->getSocket() == viscaRuntime.fd) {
        viscaDiscovery->processSocketData();
      }
    }, 0);
    viscaRuntime.retryCount = 0;
    viscaRuntime.nextRetryMs = 0;
    HIGH_MSG("VISCA async discovery started, socket %d registered", fd);
    return true;
  }

  void startAsyncDiscovery() {
    if (!Controller::conf.is_active || stopDiscovery.load()) return;

#ifdef WITH_ONVIF
    startOnvifDiscovery();
#endif
    startViscaDiscovery();

#ifdef WITH_NDI
    if (ndiDiscovery) {
      ndiDiscovery->setRemovalCallback(onDeviceRemoved);
      if (!ndiDiscovery->isAsyncDiscoveryRunning()) ndiDiscovery->startAsyncDiscovery(onDeviceDiscovered, 0);
      if (ndiDiscovery->isAsyncDiscoveryRunning() && ndiCheckTimer == std::string::npos) {
        ndiCheckTimer = Controller::E.addInterval([]() -> size_t {
          if (!Controller::conf.is_active || stopDiscovery.load()) return 0;
          if (ndiDiscovery) ndiDiscovery->checkForNewSources();
          return 1000;
        }, 1000);
      }
    }
#endif

    // Start tally check timer - monitors viewer counts and sets tally lights
    if (tallyTimer == std::string::npos) {
      tallyTimer = Controller::E.addInterval([]() -> size_t {
        if (!Controller::conf.is_active || stopDiscovery.load()) return 0;

        // Build IP -> hasViewers map from configured streams
        std::map<std::string, bool> viewersByIP;
        jsonForEach(Controller::Storage["streams"], jit) {
          std::string streamName = jit.key();
          std::string source = (*jit).isMember("source") ? (*jit)["source"].asString() : "";
          if (source.empty()) continue;
          std::string ip = extractCleanIP(source);
          if (ip.empty()) continue;
          if (Controller::hasViewers(streamName)) { viewersByIP[ip] = true; }
        }

        // Update tally state for each camera
        cameraRegistry.forEachDevice([&](const std::string & key, const ::Device::DeviceInfo & info) {
          std::string cameraIP = extractCleanIP(info.host);
          if (cameraIP.empty()) return;

          bool desired = viewersByIP.count(cameraIP) > 0;
          auto stateIt = currentTallyState.find(cameraIP);
          if (stateIt != currentTallyState.end() && stateIt->second == desired) return;
          currentTallyState[cameraIP] = desired;

#ifdef WITH_NDI
          auto ndiIt = info.protocols.find("ndi");
          if (ndiIt != info.protocols.end() && ndiIt->second.capabilities.hasTally && ndiDiscovery) {
            ndiDiscovery->setTally(ndiIt->second.endpointId, desired, false);
          }
#endif
          auto viscaIt = info.protocols.find("visca");
          if (viscaIt != info.protocols.end() && viscaDiscovery) {
            viscaDiscovery->setTally(info, desired);
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
      if (onvifRuntime.fd >= 0) {
        Controller::E.remove(onvifRuntime.fd);
        onvifRuntime.fd = -1;
      }
      onvifDiscovery->stopAsyncDiscovery();
      onvifRuntime = AsyncProtocolRuntime{};
      onvifEpochComplete = false;
    }
#endif

    // Stop VISCA discovery and remove socket
    if (viscaDiscovery) {
      if (viscaRuntime.fd >= 0) {
        Controller::E.remove(viscaRuntime.fd);
        viscaRuntime.fd = -1;
      }
      viscaDiscovery->stopAsyncDiscovery();
      viscaRuntime = AsyncProtocolRuntime{};
      viscaEpochComplete = false;
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
      if (onvifRuntime.fd >= 0) {
        Controller::E.remove(onvifRuntime.fd);
        onvifRuntime.fd = -1;
      }
      scheduleDiscoveryRetry(onvifRuntime, "ONVIF");
      onvifEpochComplete = true;
    }
    startOnvifDiscovery();
    if (onvifDiscovery && onvifDiscovery->isAsyncDiscoveryRunning()) { anyRunning = true; }
#endif

    if (viscaDiscovery && viscaDiscovery->checkAsyncTimeout()) {
      if (viscaRuntime.fd >= 0) {
        Controller::E.remove(viscaRuntime.fd);
        viscaRuntime.fd = -1;
      }
      scheduleDiscoveryRetry(viscaRuntime, "VISCA");
      viscaEpochComplete = true;
    }
    startViscaDiscovery();
    if (viscaDiscovery && viscaDiscovery->isAsyncDiscoveryRunning()) { anyRunning = true; }

#ifdef WITH_ONVIF
    if (onvifEpochComplete && viscaEpochComplete) {
      const size_t inferred = cameraRegistry.inferUniqueIpAssociations();
      if (inferred) mutateShm.store(true);
      onvifEpochComplete = false;
      viscaEpochComplete = false;
    }
#endif

#ifdef WITH_NDI
    if (ndiDiscovery && ndiDiscovery->isAsyncDiscoveryRunning()) { anyRunning = true; }
#endif

    // Check often enough to honor the 10-second discovery deadline and short retry backoffs.
    return anyRunning ? 1000 : 1000;
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
      std::string proto = request["ptzProtocol"].asString();
      ::Device::DeviceInfo current;
      if (!cameraRegistry.snapshot(camId, current)) {
        output["error"] = "Camera not found";
        return;
      }
      if (!proto.empty()) {
        auto configured = current.protocols.find(proto);
        if (configured == current.protocols.end() || !configured->second.capabilities.hasPTZ) {
          output["error"] = "PTZ protocol is not available for this camera";
          return;
        }
      }
      if (!cameraRegistry.update(camId, [&](::Device::DeviceInfo & info) { info.ptzProtocol = proto; })) {
        output["error"] = "Camera not found";
        return;
      }
      mutateShm.store(true);
      INFO_MSG("PTZ protocol preference set to '%s' for '%s'", proto.empty() ? "automatic" : proto.c_str(), camId.c_str());
      output = cameraRegistry.list(true);
      return;
    }

    // Default playback stream index: {id, defaultStream: 0|1|2|...}
    if (request.isMember("id") && request.isMember("defaultStream")) {
      std::string camId = request["id"].asString();
      ::Device::DeviceInfo info;
      if (!cameraRegistry.snapshot(camId, info)) {
        output["error"] = "Camera not found";
        return;
      }
      int idx = request["defaultStream"].asInt();
      if (idx < 0 || idx >= (int)info.streams.size()) {
        output["error"] = "defaultStream index out of range";
        return;
      }
      cameraRegistry.update(camId, [&](::Device::DeviceInfo & mutableInfo) { mutableInfo.defaultStream = idx; });
      mutateShm.store(true);

      // Update the auto-created stream source to match the new default
      bool autoStreams = Controller::Storage["config"]["auto_camera_streams"].asBool();
      INFO_MSG("auto_camera_streams=%s, triggering update", autoStreams ? "true" : "false");
      if (autoStreams) {
        autoCreateCameraStreams();
      }

      output = cameraRegistry.list(true);
      return;
    }

    // Credential-update path: {id, credentials: {protocol, username, password}}
    if (request.isMember("id") && request.isMember("credentials")) {
      std::string camId = request["id"].asString();
      ::Device::DeviceInfo probeInfo;
      if (!cameraRegistry.snapshot(camId, probeInfo)) {
        output["error"] = "Camera not found";
        return;
      }
      const JSON::Value & creds = request["credentials"];
      std::string proto = creds.isMember("protocol") ? creds["protocol"].asString() : "onvif";
      std::string user = creds.isMember("username") ? creds["username"].asString() : "";
      std::string pass = creds.isMember("password") ? creds["password"].asString() : "";
      if (!probeInfo.protocols.count(proto)) {
        output["error"] = "Protocol endpoint not found";
        return;
      }
      if (creds.isMember("tlsPolicy")) {
        const std::string policy = creds["tlsPolicy"].asString();
        if (policy != "required" && policy != "opportunistic" && policy != "insecure") {
          output["error"] = "tlsPolicy must be required, opportunistic, or insecure";
          return;
        }
      }
      if (creds.isMember("scheme")) {
        const std::string requestedScheme = creds["scheme"].asString();
        if (proto != "onvif" || (requestedScheme != "http" && requestedScheme != "https")) {
          output["error"] = "ONVIF scheme must be http or https";
          return;
        }
      }
      const std::string oldOnvifEndpoint = proto == "onvif" ? probeInfo.protocols[proto].endpointId : "";
      if (!cameraRegistry.update(camId, [&](::Device::DeviceInfo & info) {
        auto pit = info.protocols.find(proto);
        if (pit != info.protocols.end()) {
          if (creds.isMember("username")) pit->second.username = user;
          if (creds.isMember("password")) pit->second.password = pass;
          if (creds.isMember("scheme")) {
            const std::string oldScheme = pit->second.scheme.empty() ? "http" : pit->second.scheme;
            const bool usedDefaultPort = pit->second.port == (oldScheme == "https" ? 443 : 80);
            pit->second.scheme = creds["scheme"].asString();
            if (usedDefaultPort) pit->second.port = pit->second.scheme == "https" ? 443 : 80;
            if (pit->second.endpointId.find("onvif:http://") == 0 ||
                pit->second.endpointId.find("onvif:https://") == 0) {
              ::Device::ProtocolConfig identityConfig = pit->second;
              identityConfig.endpointId.clear();
              pit->second.endpointId = protocolEndpointId("onvif", identityConfig, info);
            }
          }
          if (creds.isMember("tlsPolicy")) pit->second.tlsPolicy = creds["tlsPolicy"].asString();
          if (creds.isMember("tlsCaFile")) pit->second.tlsCaFile = creds["tlsCaFile"].asString();
        }
      })) {
        output["error"] = "Camera update would duplicate an existing protocol endpoint";
        return;
      }
      mutateShm.store(true);

      // Re-probe device with new credentials
      cameraRegistry.snapshot(camId, probeInfo);
      // Clear throttle so probe runs immediately
      {
        std::lock_guard<std::mutex> lock(capabilityMutex);
        std::string devKey = canonicalDeviceKey(probeInfo);
        for (auto it = lastProbedAtMs.begin(); it != lastProbedAtMs.end(); ) {
          if (it->first.find(devKey) == 0) it = lastProbedAtMs.erase(it);
          else ++it;
        }
      }
#ifdef WITH_ONVIF
      if (proto == "onvif") {
        invalidateOnvifCache(oldOnvifEndpoint);
        invalidateOnvifCache(probeInfo.protocols["onvif"].endpointId);
      }
#endif
      INFO_MSG("Credentials saved for '%s', triggering capability re-probe", camId.c_str());
      onDeviceDiscovered({probeInfo});

      output = cameraRegistry.list(true);
      return;
    }

    // Add an explicit protocol endpoint.
    std::string proto = request.isMember("protocol") ? request["protocol"].asString() : "onvif";
    std::transform(proto.begin(), proto.end(), proto.begin(), [](unsigned char c) { return std::tolower(c); });
    if (proto != "onvif" && proto != "visca" && proto != "ndi") {
      output["error"] = "protocol must be onvif, visca, or ndi";
      return;
    }
    std::string host = request["host"].asString();
    if (host.empty() && proto != "ndi") {
      output["error"] = "Missing 'host' field";
      return;
    }
    ::Device::DeviceInfo info;
    info.host = host;
    if (request.isMember("name")) info.name = request["name"].asString();
    uint16_t port = request.isMember("port") ? request["port"].asInt() : 0;
    std::string user = request.isMember("username") ? request["username"].asString() : "";
    std::string pass = request.isMember("password") ? request["password"].asString() : "";
    ::Device::ProtocolConfig config;
    config.type = proto;
    config.address = host;
    config.port = port;
    config.username = user;
    config.password = pass;
    config.scheme = request.isMember("scheme") ? request["scheme"].asString() : (proto == "onvif" ? "http" : "");
    config.transport = request.isMember("transport") ? request["transport"].asString()
                                                        : (proto == "visca" ? "udp" : (proto == "onvif" ? "tcp" : ""));
    config.framing = request.isMember("framing") ? request["framing"].asString()
                                                   : (proto == "visca" ? "visca-ip" : (proto == "onvif" ? "soap" : ""));
    config.path = request.isMember("path") ? request["path"].asString()
                                             : (proto == "onvif" ? "/onvif/device_service" : "");
    config.tlsPolicy = request.isMember("tlsPolicy") ? request["tlsPolicy"].asString() : "opportunistic";
    config.tlsCaFile = request.isMember("tlsCaFile") ? request["tlsCaFile"].asString() : "";
    config.associationSource = "manual";
    if (config.tlsPolicy != "required" && config.tlsPolicy != "opportunistic" && config.tlsPolicy != "insecure") {
      output["error"] = "tlsPolicy must be required, opportunistic, or insecure";
      return;
    }
    if (proto == "onvif") {
      if (config.scheme != "http" && config.scheme != "https") {
        output["error"] = "ONVIF scheme must be http or https";
        return;
      }
      if (!config.port) config.port = config.scheme == "https" ? 443 : 80;
      if (config.path.empty() || config.path[0] != '/') {
        output["error"] = "ONVIF path must start with /";
        return;
      }
    } else if (proto == "visca") {
      if (config.transport != "udp" && config.transport != "tcp") {
        output["error"] = "VISCA transport must be udp or tcp";
        return;
      }
      if (config.framing != "raw" && config.framing != "visca-ip") {
        output["error"] = "VISCA framing must be raw or visca-ip";
        return;
      }
      if (!config.port) config.port = config.framing == "raw" ? 1259 : 52381;
    }
    if (proto == "ndi") {
      config.endpointId = request.isMember("endpointId") ? request["endpointId"].asString() : "";
      if (config.endpointId.size() <= 4 || config.endpointId.substr(0, 4) != "ndi:") {
        output["error"] = "NDI endpointId must be ndi:<exact source name>";
        return;
      }
    } else {
      const std::string derivedEndpointId = protocolEndpointId(proto, config, info);
      config.endpointId = request.isMember("endpointId") ? request["endpointId"].asString() : derivedEndpointId;
      if (config.endpointId.empty()) {
        output["error"] = "Could not derive protocol endpointId";
        return;
      }
      if (proto == "visca" && config.endpointId != derivedEndpointId) {
        output["error"] = "VISCA endpointId must match the normalized configured host and port";
        return;
      }
    }
    info.protocols[proto] = config;

    const std::string cameraId = cameraRegistry.mergeOne(info);
    if (cameraId.empty()) {
      output["error"] = "Could not create camera endpoint";
      return;
    }
    info.id = cameraId;
    mutateShm.store(true);
    onDeviceDiscovered({info});
    output = cameraRegistry.list(true);
  }

  void cameraConfigure(const JSON::Value & request, JSON::Value & output) {
    JSON::Value &cfg = Controller::Storage["config"];
    INFO_MSG("cameraConfigure called: %s", redactSecrets(request).toString().c_str());

    if (request.isMember("auto_camera_streams")){
      bool newVal = request["auto_camera_streams"].asBool();
      bool oldVal = cfg["auto_camera_streams"].asBool();
      cfg["auto_camera_streams"] = newVal;
      INFO_MSG("auto_camera_streams: %s -> %s", oldVal ? "true" : "false", newVal ? "true" : "false");

      if (newVal){
        autoCreateCameraStreams();
      }else if (oldVal){
        // Disable: remove all auto-camera tagged streams
        JSON::Value &streams = Controller::Storage["streams"];
        std::vector<std::string> toRemove;
        jsonForEach(streams, it){
          if (it->isMember("tags")){
            jsonForEach((*it)["tags"], tit){
              if (tit->asStringRef() == "auto-camera"){
                toRemove.push_back(it.key());
                break;
              }
            }
          }
        }
        for (const auto &name : toRemove){
          Controller::deleteStream(name, streams);
        }
      }
    }

    if (request.isMember("device_discovery")){
      bool newVal = request["device_discovery"].asBool();
      INFO_MSG("device_discovery: %s -> %s",
               cfg.isMember("device_discovery") && cfg["device_discovery"].asBool() ? "true" : "false",
               newVal ? "true" : "false");
      cfg["device_discovery"] = newVal;
    }

    if (request.isMember("auto_camera_thumbnailing")){
      bool newThumb = request["auto_camera_thumbnailing"].asBool();
      INFO_MSG("auto_camera_thumbnailing: %s -> %s", cfg["auto_camera_thumbnailing"].asBool() ? "true" : "false", newThumb ? "true" : "false");
      cfg["auto_camera_thumbnailing"] = newThumb;
      if (cfg["auto_camera_streams"].asBool()){
        autoCreateCameraStreams();
      }else{
        INFO_MSG("Skipping autoCreateCameraStreams: auto_camera_streams is disabled");
      }
    }

    output["device_discovery"] = cfg.isMember("device_discovery") ? cfg["device_discovery"] : JSON::Value(true);
    output["auto_camera_streams"] = cfg["auto_camera_streams"];
    output["auto_camera_thumbnailing"] = cfg["auto_camera_thumbnailing"];
  }

  void createCameraStream(const JSON::Value & request, JSON::Value & output) {
    std::string cameraId = request["id"].asString();
    ::Device::DeviceInfo info;
    if (!cameraRegistry.snapshot(cameraId, info)) {
      output["error"] = "Camera not found";
      return;
    }

    int streamIdx = request.isMember("stream_index") ? request["stream_index"].asInt() : 0;
    if (info.streams.empty()) {
      output["error"] = "Camera has no discovered streams";
      return;
    }
    size_t idx = std::min((size_t)std::max(streamIdx, 0), info.streams.size() - 1);
    const auto & stream = info.streams[idx];
    if (stream.uri.empty()) {
      output["error"] = "No valid stream URI";
      return;
    }

    std::string streamName = request.isMember("stream_name") ? request["stream_name"].asString() : "";
    if (streamName.empty()) {
      streamName = info.name.empty() ? cameraId : info.name;
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
    ::Device::DeviceInfo devInfo;
    if (!entry || !cameraRegistry.snapshot(id, devInfo)) {
      output["success"] = false;
      output["error"] = "Camera not found";
      return;
    }

    bool hasOnvif = devInfo.protocols.count("onvif") > 0;

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
    std::lock_guard<std::mutex> commandLock(entry->commandMutex);
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
