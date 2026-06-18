#pragma once

#include "device.h"
#include "http_parser.h"
#include "soap.h"
#include "socket.h"
#include "xml.h"

#include <chrono>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ONVIF {

  // Forward declarations
  class Device;

  /**
   * @brief Error information for ONVIF operations
   */
  struct Error {
      enum class Type {
        None,
        Transport, ///< Network/HTTP error
        SOAPFault, ///< SOAP fault returned
        Validation, ///< Schema validation error
        Authentication, ///< Auth failed
        Timeout ///< Operation timed out
      };

      Type type = Type::None;
      std::string message;
      SOAP::Fault soapFault; ///< If type == SOAPFault

      Error() = default;
      Error(Type t, const std::string & msg) : type(t), message(msg) {}
      Error(Type t, const std::string & msg, const SOAP::Fault & f) : type(t), message(msg), soapFault(f) {}
  };

  /**
   * @brief Result type for ONVIF operations that can fail
   */
  template<typename T> struct Result {
      bool success = false;
      Error error;
      T value; ///< Only valid if success == true

      Result() = default;
      Result(bool s) : success(s) {}
      Result(bool s, const Error & e) : success(s), error(e) {}
      Result(bool s, const T & v) : success(s), value(v) {}
      Result(bool s, const Error & e, const T & v) : success(s), error(e), value(v) {}

      operator bool() const { return success; }
  };

  /**
   * @brief Specialization for void return type
   */
  template<> struct Result<void> {
      bool success = false;
      Error error;

      Result() = default;
      Result(bool s) : success(s) {}
      Result(bool s, const Error & e) : success(s), error(e) {}

      operator bool() const { return success; }
  };

  /**
   * @brief WS-Discovery configuration
   */
  struct DiscoveryConfig {
      std::vector<std::string> types; ///< Device types to probe for
      std::vector<std::string> scopes; ///< Scopes to filter by
      std::string matchBy; ///< Optional scope matching algorithm
      std::vector<std::string> interfaces; ///< Network interfaces to use (empty = all)
      bool useMulticast = true; ///< Use multicast vs unicast discovery
      bool useTLS = false; ///< Use HTTPS for probe responses
      uint16_t port = 3702; ///< Discovery port
      std::chrono::milliseconds timeout{500}; ///< Timeout per interface
  };

  struct VideoConfig {
      int width;
      int height;
      int fps;
      std::string encoding;
      int quality;
      int bitrate;
  };

  struct AudioConfig {
      std::string encoding;
      int bitrate;
      int sampleRate;
  };

  struct MediaProfile {
      std::string name;
      std::string token;
      std::string videoSourceToken;
      VideoConfig video;
      AudioConfig audio;
  };

  struct DeviceInfo {
      std::string manufacturer;
      std::string model;
      std::string firmwareVersion;
      std::string serialNumber;
      std::string hardwareId;
      std::string address;
      uint16_t port;
  };

  struct SystemDateTime {
      bool isDST;
      bool dateTimeType; // true for NTP, false for Manual
      std::string timezone;
      std::string utcDateTime;
      std::string localDateTime;
  };

  struct NetworkInterface {
      std::string name;
      bool enabled;
      std::string ipv4Address;
      std::string ipv4Mask;
      std::string ipv4Gateway;
      std::string macAddress;
  };

  struct DeviceCapabilities {
      std::string mediaXAddr; // Media service endpoint
      bool ptz; // PTZ capabilities
      bool events; // Event capabilities
      bool imaging; // Imaging capabilities
      bool analytics; // Analytics capabilities
      bool multicast; // Multicast capabilities
      bool rtpMulticast; // RTP multicast capabilities
      bool metadata; // Metadata capabilities
      bool backchannel; // Audio backchannel capabilities
      bool media; // Media capabilities
      bool deviceIO; // Device IO capabilities
      bool recording; // Recording capabilities
      bool search; // Search capabilities
      bool replay; // Replay capabilities
      bool receiver; // Receiver capabilities
      bool display; // Display capabilities

      DeviceCapabilities()
        : ptz(false), events(false), imaging(false), analytics(false), multicast(false), rtpMulticast(false),
          metadata(false), backchannel(false), media(false), deviceIO(false), recording(false), search(false),
          replay(false), receiver(false), display(false) {}
  };

  struct User {
      std::string username;
      std::string password;
      std::string userLevel; // Administrator, Operator, User, etc.
  };

  struct PTZPreset {
      std::string token;
      std::string name;
      float pan;
      float tilt;
      float zoom;
  };

  struct PTZStatus {
      float pan;
      float tilt;
      float zoom;
      std::string moveStatus; // IDLE, MOVING, UNKNOWN
      std::string error;
      std::time_t utcTime;
  };

  struct PTZSpeed {
      float x;
      float y;
      float zoom;
  };

  struct PTZConfiguration {
      std::string token;
      std::string name;
      int useCount;
      std::string nodeToken;
      PTZSpeed defaultSpeed;
  };

  struct PresetTour {
      std::string token;
      std::string name;
      std::vector<PTZPreset> presets;
      bool autoStart;
      int stayTime; // seconds
      float speed;
  };

  struct StreamSetup {
      std::string protocol; // RTP-Unicast, RTP-Multicast, RTSP, etc.
      std::string transport; // UDP, TCP, RTSP, HTTP, etc.
      std::string encoding; // H.264, H.265, MJPEG, etc.
  };

  struct StreamingCapabilities {
      bool RTPMulticast;
      bool RTP_TCP;
      bool RTP_RTSP_TCP;
      bool NonAggregateControl;
      std::vector<std::string> streamingProfiles; // e.g. "RTP-Unicast", "RTP-Multicast"
      std::vector<std::string> transportProtocols; // e.g. "UDP", "TCP", "RTSP", "HTTP"
  };

  struct TransportDetails {
      std::string protocol; // UDP, TCP, RTSP, HTTP
      int port; // Port number
      std::string path; // URL path
      std::map<std::string, std::string> options; // Protocol specific options
  };

  struct ImagingSettings {
      float brightness = -1;
      float contrast = -1;
      float colorSaturation = -1;
      float sharpness = -1;
      struct {
          std::string mode;
          float defaultSpeed = 0;
          float nearLimit = 0;
          float farLimit = 0;
      } focus;
      struct {
          std::string mode;
          float iris = 0;
      } exposure;
      struct {
          std::string mode;
          float crGain = 0;
          float cbGain = 0;
      } whiteBalance;
  };

  struct ONVIFEvent {
      std::string topic;
      std::string source;
      std::string sourceValue;
      std::string dataName;
      std::string dataValue;
      std::string timestamp;
  };

  // ONVIF Device
  class Device : public ::Device::Base {
    public:
      Device() = default;
      Device(const std::string & host, uint16_t port = 80, const std::string & path = "/onvif/device_service");
      Device(const std::string & uri);
      virtual ~Device() override;

      // Base class virtual methods
      virtual bool connect() override;
      virtual bool isConnected() const override;
      virtual void disconnect() override;
      virtual bool sendPTZ(const ::Device::PTZCommand & cmd) override;
      virtual ::Device::DeviceInfo queryCapabilities() const override;

      // Credentials management
      void setCredentials(const std::string & user, const std::string & pass);

      // Timeout configuration
      void setRequestTimeout(int seconds) { requestTimeoutSeconds = seconds; }

      // PTZ methods
      bool hasPTZService() const;
      Result<bool> absoluteMove(const std::string & profileToken, float pan, float tilt, float zoom);
      Result<bool> relativeMove(const std::string & profileToken, float pan, float tilt, float zoom);
      Result<bool> continuousMove(const std::string & profileToken, float pan, float tilt, float zoom);
      Result<bool> stop(const std::string & profileToken);
      Result<bool> setHomePosition(const std::string & profileToken);
      Result<bool> gotoHomePosition(const std::string & profileToken);
      Result<std::vector<PTZPreset>> getPresets(const std::string & profileToken);
      Result<bool> gotoPreset(const std::string & profileToken, const std::string & presetToken, const std::string & presetName = "");
      Result<bool> setPreset(const std::string & profileToken, const std::string & presetToken, const std::string & presetName);
      Result<bool> removePreset(const std::string & profileToken, const std::string & presetToken);
      Result<PTZStatus> getPTZStatus(const std::string & profileToken);
      Result<bool> setPTZConfiguration(const std::string & profileToken, const PTZConfiguration & config);

      // Helper methods for SOAP message creation and sending
      SOAP::Message createMessage(const std::string & action, SOAP::onvif::ServiceType service) const;
      Result<void> sendRequestVoid(const std::string & service, const std::string & action, const std::string & request,
                                   const std::string & endpointOverride = "");
      Result<std::string> sendRequestAndGetResponse(const std::string & service, const std::string & action,
                                                    const std::string & request, std::string & response,
                                                    const std::string & endpointOverride = "") const;
      bool sendRequest(const std::string & service, const std::string & action, const std::string & request, std::string & response);

      // New error handling methods
      Result<bool> tryConnect();
      Result<std::vector<MediaProfile>> getMediaProfiles() const;
      Result<std::string> getDefaultStreamUri(const std::string & profileToken) const;
      Result<std::string> getStreamUriWithProtocol(const std::string & profileToken, const std::string & protocol,
                                                   const std::string & transport) const;
      Result<std::string> getStreamUriWithTransport(const std::string & profileToken, const TransportDetails & transport) const;

      // User management
      Result<std::vector<User>> getUsers() const;
      Result<bool> createUser(const User & user);
      Result<bool> deleteUser(const std::string & username);
      Result<bool> setUser(const User & user);

      // Media methods
      Result<MediaProfile> getMediaProfile(const std::string & token) const;
      Result<bool> createMediaProfile(const MediaProfile & profile);
      Result<bool> deleteMediaProfile(const std::string & token);
      Result<bool> modifyMediaProfile(const MediaProfile & profile);
      Result<std::string> getStreamUri(const std::string & profileToken, const std::string & protocol,
                                       const std::string & transport) const;
      Result<std::vector<StreamSetup>> getStreamSetup(const std::string & profileToken) const;
      Result<std::string> getSnapshotUri(const std::string & profileToken) const;
      Result<bool> setVideoEncoderConfiguration(const std::string & token, const VideoConfig & config);
      Result<bool> setAudioEncoderConfiguration(const std::string & token, const AudioConfig & config);

      // Preset Tour methods
      bool startPresetTour(const std::string & profileToken, const std::string & tourToken);
      bool stopPresetTour(const std::string & profileToken, const std::string & tourToken);
      bool createPresetTour(const std::string & profileToken);
      bool modifyPresetTour(const std::string & profileToken, const std::string & tourToken, const PresetTour & tour);
      bool removePresetTour(const std::string & profileToken, const std::string & tourToken);

      // System date/time methods
      Result<SystemDateTime> getSystemDateAndTime() const;
      Result<void> setSystemDateAndTime(const SystemDateTime & dateTime);

      // Device Management Methods
      Result<std::vector<NetworkInterface>> getNetworkInterfaces() const;
      Result<DeviceCapabilities> getDeviceCapabilities() const;
      Result<void> reboot();
      Result<void> factoryReset();

      // Device information methods
      Result<DeviceInfo> getDeviceInformation() const;
      Result<StreamingCapabilities> getStreamingCapabilities() const;
      Result<std::vector<TransportDetails>> getTransportDetails(const std::string & profileToken) const;

      // Analytics methods
      ::Device::AnalyticsCapabilities queryAnalytics(const std::string & profileToken) const;

      // Imaging service methods
      Result<ImagingSettings> getImagingSettings(const std::string & videoSourceToken) const;
      Result<bool> setImagingSettings(const std::string & videoSourceToken, const ImagingSettings & settings);
      Result<bool> focusMove(const std::string & videoSourceToken, float speed);
      Result<bool> focusStop(const std::string & videoSourceToken);
      std::string getVideoSourceToken() const;

      // Events methods
      Result<std::string> createPullPointSubscription(int initialTerminationTimeSec = 60);
      Result<std::vector<ONVIFEvent>>
        pullMessages(const std::string & subscriptionEndpoint, int timeoutSec = 5, int messageLimit = 10);
      Result<bool> renewSubscription(const std::string & subscriptionEndpoint, int terminationTimeSec = 60);
      Result<bool> unsubscribe(const std::string & subscriptionEndpoint);

      // WS-Security authentication helper
      void addAuthToMessage(SOAP::Message & msg) const;

    protected:
      std::string host;
      std::string path;
      uint16_t port;
      std::string username;
      std::string password;
      std::string name;
      std::string uri;
      mutable bool useDigestAuth = true;
      std::string address;
      bool connected;
      mutable std::map<std::string, std::string> services;
      DeviceInfo deviceInfo;
      std::string defaultProfileToken;
      mutable std::string videoSourceToken_;
      int requestTimeoutSeconds = 5;
      SOAP::onvif::ONVIFMessageFactory messageFactory;

      Result<HTTP::Parser> readResponseWithTimeout(Socket::Connection & conn, int timeoutSeconds = 2) const;

      // XML Helper methods
      Result<XML::Document> parseResponse(const HTTP::Parser & resp, bool allowEmptyBody = false) const;
      Result<XML::Node> findNode(const XML::Document & doc, const std::string & localName) const;
      Result<std::string> getNodeText(const XML::Node & node, const std::string & localName) const;
      Result<SOAP::Fault> extractSOAPFault(const XML::Document & doc) const;
      std::string getDateTimeString(const XML::Document & doc, const XML::Node & dtNode) const;

      // Helper to handle common HTTP status codes and SOAP faults
      Result<XML::Document> handleResponse(const HTTP::Parser & resp, bool allowEmptyBody = false) const;

      // Helper to extract values ignoring namespace
      template<typename T>
      Result<T> extractResponse(const std::string & response, const std::string & responseName,
                                std::function<Result<T>(const XML::Document &)> extractor) const {
        try {
          HTTP::Parser parser;
          parser.body = response;
          auto doc = parseResponse(parser, false);
          if (!doc) return Result<T>(false, doc.error);

          auto body = findSOAPBody(doc.value);
          if (!body) return Result<T>(false, body.error);

          auto responseNode = findNode(doc.value, responseName);
          if (!responseNode) return Result<T>(false, responseNode.error);

          return extractor(doc.value);
        } catch (const std::exception & e) {
          return Result<T>(false, Error(Error::Type::Validation, std::string("Failed to extract response: ") + e.what()));
        }
      }

      // Helper to find SOAP body in response
      Result<XML::Node> findSOAPBody(const XML::Document & doc) const {
        try {
          std::string xpath = "//*[local-name()='Envelope']/*[local-name()='Body']";
          auto nodes = doc.evaluateXPathAll(xpath);
          if (nodes.empty()) {
            return Result<XML::Node>(false, Error(Error::Type::Validation, "Failed to find SOAP body"));
          }
          return Result<XML::Node>(true, nodes[0]);
        } catch (const std::exception & e) {
          return Result<XML::Node>(false, Error(Error::Type::Validation, std::string("Failed to find SOAP body: ") + e.what()));
        }
      }

      // Helper to extract common response patterns
      template<typename T>
      Result<T> extractResponse(const std::string & response, const std::string & responseName,
                                std::function<Result<T>(const XML::Node &)> extractor) const {
        try {
          HTTP::Parser parser;
          parser.body = response;
          auto doc = parseResponse(parser, false);
          if (!doc) return Result<T>(false, doc.error);

          auto body = findSOAPBody(doc.value);
          if (!body) return Result<T>(false, body.error);

          auto responseNode = findNode(doc.value, responseName);
          if (!responseNode) return Result<T>(false, responseNode.error);

          return extractor(responseNode.value);
        } catch (const std::exception & e) {
          return Result<T>(false, Error(Error::Type::Validation, std::string("Failed to extract response: ") + e.what()));
        }
      }

      // Helper to build common request patterns
      Result<std::string> buildRequest(const std::string & action, std::function<void(XML::Document &, XML::Node &)> builder,
                                       SOAP::onvif::ServiceType service) const {
        try {
          SOAP::Message msg = createMessage(action, service);
          XML::Document doc;
          XML::Node root = doc.createNode(action);
          builder(doc, root);
          msg.setBodyNode(root);
          return Result<std::string>(true, msg.toString());
        } catch (const std::exception & e) {
          return Result<std::string>(false, Error(Error::Type::Validation, std::string("Failed to build request: ") + e.what()));
        }
      }
  };

  // WS-Discovery helper
  class Discovery : public ::Device::Discovery {
    private:
      std::unique_ptr<Socket::UDPConnection> socket;
      bool parseProbeMatch(const std::string & response, ::Device::DeviceInfo & info);
      void logProbeMatch(const std::string & response);

      // Async discovery state
      ::Device::DiscoveryCallback asyncCallback;
      bool asyncRunning;
      std::vector<::Device::DeviceInfo> asyncDevices;
      uint64_t asyncStartTime;
      uint32_t asyncTimeoutMs;
      // Per-sender response accumulation buffer (instance-owned)
      std::map<std::string, std::string> responseBuffers;

      // Socket management for event loop integration
      void setupAsyncSocket();
      void handleSocketData();

    public:
      Discovery();
      ~Discovery() override;

      // Base class overrides
      std::string getProtocolName() const override { return "onvif"; }
      bool sendCommand(const std::string & deviceId, const ::Device::PTZCommand & cmd) override;
      std::unique_ptr<::Device::Base> createDevice(const ::Device::DeviceInfo & info) const override;
      void sendProbeResponse(const std::string & messageId, const std::string & relatesTo,
                             const std::string & endpointRef, const std::string & types, const std::string & scopes,
                             const std::string & xaddrs, uint32_t metadataVersion);

      // Async discovery overrides
      bool startAsyncDiscovery(::Device::DiscoveryCallback callback, uint32_t timeoutMs = 0) override;
      void stopAsyncDiscovery() override;
      bool isAsyncDiscoveryRunning() const override { return asyncRunning; }

      // Public method to get socket for event loop registration
      int getSocket() const { return socket ? socket->getSock() : -1; }

      // Method to be called by event loop when socket has data
      void processSocketData();

      // Method to check if async discovery should timeout
      bool checkAsyncTimeout();
  };

} // namespace ONVIF
