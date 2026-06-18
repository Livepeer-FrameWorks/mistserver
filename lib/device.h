#pragma once

#include "json.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace Device {

  struct StreamEndpoint {
      std::string name;
      std::string protocol;
      std::string transport;
      std::string uri;
      std::string format;
      std::string resolution;
      std::string framerate;
      uint32_t width = 0;
      uint32_t height = 0;
      uint32_t fps = 0;
      uint32_t bitrate = 0;
      std::string profile;
      std::string address;
      uint16_t port = 0;
      std::string path;
      std::map<std::string, std::string> options;
  };

  struct ProtocolCapabilities {
      // Core capabilities
      bool hasPTZ = false;
      bool hasAudio = false;
      bool hasMetadata = false;
      bool hasVideo = false;
      bool hasRecording = false;
      bool hasWebControl = false;
      bool hasTally = false;

      // Transport capabilities
      bool hasRTPMulticast = false;
      bool hasRTPTCP = false;
      bool hasRTPRTSPTCP = false;
      std::vector<std::string> supportedTransports;

      // Media capabilities
      std::vector<std::string> supportedFormats;
      std::vector<std::string> supportedResolutions;
      std::vector<std::string> supportedFramerates;
      std::vector<std::string> supportedAudioFormats;

      // Control capabilities
      std::vector<std::string> supportedCommands;
  };

  struct ProtocolConfig {
      std::string type;
      std::string address;
      uint16_t port;
      std::string username;
      std::string password;
      ProtocolCapabilities capabilities;
  };

  struct AnalyticsModule {
      std::string name;
      std::string type;
      bool active = false;
      std::vector<std::string> parameters;
  };

  struct AnalyticsRule {
      std::string name;
      std::string type;
      bool active = false;
      std::vector<std::string> parameters;
  };

  struct AnalyticsCapabilities {
      bool hasAnalytics = false;
      std::string analyticsServiceUrl;
      std::vector<AnalyticsModule> supportedModules;
      std::vector<AnalyticsModule> activeModules;
      std::vector<AnalyticsRule> supportedRules;
      std::vector<AnalyticsRule> activeRules;
      std::vector<std::string> objectClassifications;
  };

  struct DeviceInfo {
      // Base device info
      std::string id;
      std::string host; // Raw host/IP for matching
      std::string name; // Display name
      std::string manufacturer;
      std::string model;
      std::string firmwareVersion;
      std::string serialNumber;
      std::string status;
      bool hasPTZ = false;
      bool hasAudio = false;
      bool hasMetadata = false;
      std::vector<std::string> features; // General device features
      std::string webControlUrl; // URL for web control interface
      std::string snapshotUri; // ONVIF snapshot URI for preview
      std::vector<std::string> ptzFeatures;
      std::string ptzProtocol; // User-chosen PTZ protocol: "" (auto), "onvif", "visca", "ndi"
      int defaultStream = -1; // Index into streams[] for preferred playback source (-1 = auto)
      std::vector<StreamEndpoint> streams;
      std::map<std::string, ProtocolConfig> protocols;
      AnalyticsCapabilities analytics;

      // Helper methods
      void addProtocol(const std::string & type, const std::string & address, uint16_t port = 0,
                       const std::string & username = "", const std::string & password = "") {
        ProtocolConfig config;
        config.type = type;
        config.address = address;
        config.port = port;
        config.username = username;
        config.password = password;
        protocols[type] = config;
      }

      bool supportsProtocol(const std::string & type) const { return protocols.find(type) != protocols.end(); }

      JSON::Value toJSON() const;
      static DeviceInfo fromJSON(const JSON::Value & json);
  };

  /**
   * @brief PTZ control actions
   */
  enum class PTZAction {
    PanTilt, ///< Pan/tilt movement
    Zoom, ///< Zoom control
    Stop, ///< Stop all movement
    Home, ///< Move to home position
    Preset, ///< Store or recall preset
    Focus, ///< Focus control
    Iris, ///< Iris control
    WhiteBalance ///< White balance control
  };

  /**
   * @brief Command structure for PTZ operations
   */
  struct PTZCommand {
      PTZAction action;
      std::map<std::string, float> args; // Command parameters
  };

  /**
   * @brief Base class for all device types
   */
  class Base {
    public:
      virtual ~Base() = default;

      /**
       * @brief Establish connection to the device
       * @return true if connection successful, false otherwise
       */
      virtual bool connect() = 0;

      /**
       * @brief Check if device is currently connected
       * @return true if connected, false otherwise
       */
      virtual bool isConnected() const = 0;

      /**
       * @brief Disconnect from the device and cleanup resources
       */
      virtual void disconnect() = 0;

      /**
       * @brief Convenience operator for connection status
       * @return true if connected, false otherwise
       */
      operator bool() const { return isConnected(); }

      // PTZ control interface
      /**
       * @brief Send a PTZ command to the device
       * @param cmd PTZ command to execute
       * @return true if command was executed successfully, false otherwise
       */
      virtual bool sendPTZ(const PTZCommand & cmd) = 0;

      // Capability querying
      virtual DeviceInfo queryCapabilities() const = 0;
  };

  /**
   * @brief Callback type for device discovery events
   * @param devices Vector of newly discovered devices
   */
  using DiscoveryCallback = std::function<void(const std::vector<DeviceInfo> &)>;

  /**
   * @brief Base class for device discovery implementations
   *
   * This class defines the interface for discovering devices of a specific type.
   * Each protocol (NDI, ONVIF, VISCA) implements its own discovery mechanism
   * while providing a consistent interface for the discovery controller.
   *
   * The class now supports both blocking and event-based async discovery patterns.
   */
  class Discovery {
    public:
      virtual ~Discovery() = default;

      /**
       * @brief Start async discovery with callback
       * @param callback Function to call when devices are found
       * @param timeoutMs Maximum time to wait for responses (0 = no timeout)
       * @return true if discovery started successfully, false otherwise
       */
      virtual bool startAsyncDiscovery(DiscoveryCallback callback, uint32_t timeoutMs = 0) = 0;

      /**
       * @brief Stop async discovery
       */
      virtual void stopAsyncDiscovery() {
        // Default: no-op for protocols that don't support async
      }

      /**
       * @brief Check if async discovery is currently running
       * @return true if running, false otherwise
       */
      virtual bool isAsyncDiscoveryRunning() const {
        return false; // Default: assume not running
      }

      /**
       * @brief Get the protocol name for this discovery implementation
       * @return Protocol name string
       */
      virtual std::string getProtocolName() const = 0;

      /**
       * @brief Send a command to a device
       * @param deviceId Unique device identifier
       * @param cmd Command to send
       * @return true if command was sent successfully, false otherwise
       */
      virtual bool sendCommand(const std::string & deviceId, const PTZCommand & cmd) = 0;

      /**
       * @brief Create a connected device instance from discovery info
       * @param info Device info with protocol config and address details
       * @return Connected device, or nullptr if creation failed
       */
      virtual std::unique_ptr<Base> createDevice(const DeviceInfo & info) const { return nullptr; }
  };

} // namespace Device
