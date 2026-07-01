#pragma once
#include <mist/defines.h>
#include <mist/device.h>
#ifdef WITH_ONVIF
#include <mist/device_onvif.h>
#endif
#include <mist/device_visca.h>
#include <mist/json.h>
#include <mist/timing.h>

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#ifdef WITH_NDI
#include <mist/device_ndi.h>
#endif
#include "util.h"

#include <atomic>
#include <thread>

namespace Controller {

  struct DiscoveryState {
      bool ndiAttempted = false; // whether NDI runtime init has been tried (once)
      bool ndiInitialized = false; // whether NDI runtime init succeeded (gates NDI discovery)
      bool asyncStarted = false;
  };

  // Global control flags
  extern std::atomic<bool> stopDiscovery;
  extern std::atomic<bool> mutateShm;

  // Managed probe threads (replaces detached threads for safe shutdown)
  struct ProbeThread {
      std::thread thread;
      std::shared_ptr<std::atomic<bool>> done;
  };
  extern std::mutex probeThreadsMutex;
  extern std::vector<ProbeThread> probeThreads;
  void reapFinishedProbeThreads();

  // Protocol registry: maps protocol name -> Discovery instance (non-owning)
  extern std::map<std::string, ::Device::Discovery *> protocolRegistry;

  // Persistent discovery instances
#ifdef WITH_ONVIF
  extern std::unique_ptr<ONVIF::Discovery> onvifDiscovery;
#endif
  extern std::unique_ptr<VISCA::Discovery> viscaDiscovery;
#ifdef WITH_NDI
  extern std::unique_ptr<NDI::Discovery> ndiDiscovery;
#endif

  // Per-device entry holding DeviceInfo and a command serialization mutex
  struct CameraEntry {
      ::Device::DeviceInfo info;
      mutable std::mutex commandMutex;
      CameraEntry() = default;
      CameraEntry(const CameraEntry &) = delete;
      CameraEntry & operator=(const CameraEntry &) = delete;
  };

  // Thread-safe camera registry with shared_mutex for map-level protection
  // and per-device mutexes for PTZ command serialization.
  class CameraRegistry {
      mutable std::mutex mapMutex;
      std::map<std::string, std::shared_ptr<CameraEntry>> cameras;

      std::string canonicalKey(const ::Device::DeviceInfo & dev) const;

    public:
      // Returns JSON array; redacts protocol passwords when redactCredentials is true
      JSON::Value list(bool redactCredentials = true) const;

      // Finds entry by canonical key or cleaned ID. Caller gets shared_ptr;
      // entry stays alive even if later removed from the map.
      std::shared_ptr<CameraEntry> find(const std::string & id) const;

      size_t size() const;

      // Merge discovered/enriched devices (exclusive lock, additive update)
      void merge(const std::vector<::Device::DeviceInfo> & devices);

      // Remove device by ID or canonical key
      void remove(const std::string & id);

      // Update status field for a device
      void updateStatus(const std::string & id, const std::string & status);

      // Populate registry from persisted Storage["cameras"] JSON
      void loadFromStorage(const JSON::Value & camerasJson);

      // Serialize registry back to Storage["cameras"] for disk persistence
      void syncToStorage(JSON::Value & storage) const;

      // Write all camera data to shared memory (syncs to Storage first)
      void writeToShm();

      // Thread-safe iteration over all devices
      void forEachDevice(std::function<void(const std::string & key, const CameraEntry & entry)> fn) const;
  };

  extern CameraRegistry cameraRegistry;

  // Camera field data structure - contains only essential fields for shared memory
  struct CameraFieldData {
      Util::RelAccXFieldData idField;
      Util::RelAccXFieldData nameField;
      Util::RelAccXFieldData statusField;
      Util::RelAccXFieldData hostField;
      Util::RelAccXFieldData portField;
      Util::RelAccXFieldData protocolField;
      Util::RelAccXFieldData hasPTZField;
  };

  // Helper functions for device discovery
  std::string extractCleanIP(const std::string & address);
  std::string canonicalDeviceKey(const ::Device::DeviceInfo & dev);
  void updateDeviceInfo(::Device::DeviceInfo & device, const ::Device::DeviceInfo & devInfo);

  // Helper functions for PTZ command handling
  bool createPTZCommand(const std::string & commandName, const JSON::Value & args, ::Device::PTZCommand & cmd);
  bool sendCommandViaProtocol(const std::string & deviceId, const std::string & protocol,
                              const ::Device::PTZCommand & cmd, std::string & errorMsg);

  // Discovery process control
  void initDiscovery();
  size_t discoveryRun();
  void discoveryDeinit();
  extern size_t discoveryTimer;

  // Async discovery management
  void initAsyncDiscovery();
  void startAsyncDiscovery();
  void stopAsyncDiscovery();
  size_t checkAsyncDiscoveryProgress();

  // Event loop integration
  void onDeviceDiscovered(const std::vector<::Device::DeviceInfo> & devices);
  void onDeviceCapabilities(const std::vector<::Device::DeviceInfo> & devices);
  void setupDiscoveryEventCallbacks();
  void cleanupDiscoveryEventCallbacks();

  // Timer callback IDs for async discovery
  extern size_t ndiCheckTimer;

  // Shared memory management
  void setupProtocolsAccX(Util::RelAccX & rax);
  void setupStreamsAccX(Util::RelAccX & rax);
  void setupCameraAccX(Util::RelAccX & rax);
  size_t calculateRequiredPageSize(size_t maxCameras, size_t maxProtocols, size_t maxStreams);
  size_t calculateRequiredPageSize();
  void writeProtocolsToShm(const JSON::Value & protocols, Util::RelAccX & protoAccX);
  void writeStreamsToShm(const JSON::Value & streams, Util::RelAccX & streamAccX);
  void prettyPrintCamerasPage(const std::string & title);
  void writeToShmCameras();

  // Camera management API
  void cameraConfigure(const JSON::Value & request, JSON::Value & output);
  void listCameras(JSON::Value & output);
  void removeCamera(const JSON::Value & request, JSON::Value & output);
  void sendCommand(const JSON::Value & request, JSON::Value & output);
  void updateCamera(const JSON::Value & request, JSON::Value & output);
  void createCameraStream(const JSON::Value & request, JSON::Value & output);
  void listPresets(const JSON::Value & request, JSON::Value & output);

  // Internal functions for camera management
  void removeCameraByName(const std::string & name);
  void updateCameraStatus(const std::string & name, const std::string & status);

} // namespace Controller
