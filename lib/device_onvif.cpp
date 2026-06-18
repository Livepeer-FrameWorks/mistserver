#include "device_onvif.h"

#include "defines.h"
#include "encode.h"
#include "socket.h"
#include "url.h"
#include "util.h"
#include "xml.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <chrono>
#include <cstring>
#include <ctime>
#include <ifaddrs.h>
#include <map>
#include <memory>
#include <net/if.h>
#include <netdb.h>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace ONVIF {

  // Helper function for URL decoding
  std::string urlDecode(const std::string & encoded) {
    std::string result;
    result.reserve(encoded.length());

    for (size_t i = 0; i < encoded.length(); ++i) {
      if (encoded[i] == '%' && i + 2 < encoded.length()) {
        int value;
        std::istringstream is(encoded.substr(i + 1, 2));
        if (is >> std::hex >> value) {
          result += static_cast<char>(value);
          i += 2;
        } else {
          result += encoded[i];
        }
      } else if (encoded[i] == '+') {
        result += ' ';
      } else {
        result += encoded[i];
      }
    }

    return result;
  }

  // Helper function for getting UTC time in ISO8601 format
  std::string getUTCTime(int offsetSeconds) {
    time_t now;
    time(&now);
    now += offsetSeconds;
    struct tm *tm = gmtime(&now);
    char timestamp[30];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", tm);
    return std::string(timestamp);
  }

  // Utility function to trim whitespace from both ends of a string
  static std::string trim(const std::string & str) {
    auto start = std::find_if_not(str.begin(), str.end(), [](unsigned char ch) { return std::isspace(ch); });
    auto end = std::find_if_not(str.rbegin(), str.rend(), [](unsigned char ch) { return std::isspace(ch); }).base();
    return start < end ? std::string(start, end) : std::string();
  }

  // ONVIF Device Implementation
  Device::Device(const std::string & host, uint16_t port, const std::string & path)
    : ::Device::Base(), host(host), path(path.empty() ? "/onvif/device_service" : path), port(port),
      uri("http://" + host + ":" + std::to_string(port) + path), connected(false), messageFactory(SOAP::Version::SOAP_1_2) {
    HIGH_MSG("Creating ONVIF device at %s:%d%s", host.c_str(), port, path.c_str());
  }

  Device::~Device() {
    VERYHIGH_MSG("ONVIF: Destroying device at %s:%d", host.c_str(), port);
    disconnect();
  }

  bool Device::connect() {
    if (isConnected()) {
      HIGH_MSG("ONVIF: Successfully connected to device at %s", host.c_str());
      return true;
    }

    VERYHIGH_MSG("ONVIF: Attempting to connect to %s:%d%s", host.c_str(), port, path.c_str());

    auto result = tryConnect();
    return result.success;
  }

  Result<bool> Device::tryConnect() {
    if (!host.empty()) {
      HIGH_MSG("ONVIF: Attempting to connect to %s:%d%s", host.c_str(), port, path.c_str());

      // Create GetCapabilities message with WS-Security auth
      auto msg = createMessage("GetCapabilities", SOAP::onvif::ServiceType::Device);

      // Use XML helpers to build the body
      XML::Document doc;
      XML::Node root = doc.createNode("GetCapabilities");
      root.setAttribute("xmlns", "http://www.onvif.org/ver10/device/wsdl");
      XML::Node category = doc.createNode("Category");
      category.setTextContent("All");
      root.addChild(category);
      msg.setBodyNode(root);

      std::string response;
      auto result = sendRequestAndGetResponse("device", "GetCapabilities", msg.toString(), response);

      if (!result) {
        if (result.error.type == Error::Type::Transport && result.error.message.find("HTTP error 401") != std::string::npos) {
          // Device requires authentication
          return Result<bool>{false, Error{Error::Type::Authentication, "Device requires authentication. Please set credentials using setCredentials()"}};
        }
        ERROR_MSG("ONVIF: Connection failed: %s", result.error.message.c_str());
        return Result<bool>{false, result.error};
      }

      try {
        XML::Document responseDoc(response);
        auto body = findNode(responseDoc, "GetCapabilitiesResponse");
        if (!body) { return Result<bool>{false, Error{Error::Type::Validation, "Failed to find SOAP body"}}; }

        auto capsResponse = findNode(responseDoc, "GetCapabilitiesResponse");
        if (!capsResponse) {
          return Result<bool>{false, Error{Error::Type::Validation, "Failed to find capabilities response"}};
        }

        auto capabilities = findNode(responseDoc, "Capabilities");
        if (!capabilities) {
          return Result<bool>{false, Error{Error::Type::Validation, "Failed to find capabilities"}};
        }

        // Extract service endpoints with proper namespace handling
        std::vector<std::pair<std::string, std::string>> serviceNodes = {
          {"device", "Device"}, {"media", "Media"},     {"ptz", "PTZ"},
          {"events", "Events"}, {"imaging", "Imaging"}, {"analytics", "Analytics"}};

        bool foundAnyService = false;
        for (const auto & service : serviceNodes) {
          // Try both with and without tt: namespace prefix
          auto serviceNode = findNode(responseDoc, service.second);
          if (!serviceNode) { serviceNode = findNode(responseDoc, "tt:" + service.second); }

          if (serviceNode.value.isValid()) {
            // Try both with and without tt: namespace prefix for XAddr
            auto xaddr = serviceNode.value.getChild("XAddr");
            if (!xaddr.isValid()) { xaddr = serviceNode.value.getChild("tt:XAddr"); }

            if (xaddr.isValid()) {
              std::string serviceUrl = xaddr.getTextContent();
              if (!serviceUrl.empty()) {
                services[service.first] = serviceUrl;
                INSANE_MSG("ONVIF: Found %s service at %s", service.first.c_str(), services[service.first].c_str());
                foundAnyService = true;
              }
            }
          }
        }

        if (!foundAnyService) {
          return Result<bool>{false, Error{Error::Type::Validation, "No services found in capabilities"}};
        }

        connected = true;
        HIGH_MSG("ONVIF: Successfully connected to device at %s", host.c_str());
        return Result<bool>{true};

      } catch (const std::exception & e) { return Result<bool>{false, Error{Error::Type::Validation, e.what()}}; }
    }
    return Result<bool>{false, Error{Error::Type::Transport, "No host specified"}};
  }

  bool Device::isConnected() const {
    return connected;
  }

  void Device::disconnect() {
    connected = false;
  }

  void Device::setCredentials(const std::string & user, const std::string & pass) {
    HIGH_MSG("ONVIF: Setting credentials for device at %s:%d", host.c_str(), port);
    username = user;
    password = pass;
  }

  Result<DeviceInfo> Device::getDeviceInformation() const {
    try {
      HIGH_MSG("ONVIF: Getting device information for %s:%d", host.c_str(), port);

      // Build request using helper
      auto request = buildRequest("GetDeviceInformation", [](XML::Document & doc, XML::Node & root) {
        root.setAttribute("xmlns", "http://www.onvif.org/ver10/device/wsdl");
      }, SOAP::onvif::ServiceType::Device);
      if (!request) {
        WARN_MSG("ONVIF: Failed to build device info request - %s", request.error.message.c_str());
        return Result<DeviceInfo>{false, request.error};
      }

      // Send request and get response
      std::string response;
      auto result = sendRequestAndGetResponse("device", "GetDeviceInformation", request.value, response);
      if (!result) {
        WARN_MSG("ONVIF: Failed to get device information - %s", result.error.message.c_str());
        return Result<DeviceInfo>{false, result.error};
      }

      // Log raw response for debugging
      INSANE_MSG("ONVIF: Parsing device information response:");
      std::istringstream responseStream(response);
      std::string line;
      while (std::getline(responseStream, line)) { INSANE_MSG("ONVIF: | %s", line.c_str()); }

      // Extract device info using response helper
      return extractResponse<DeviceInfo>(response, "GetDeviceInformationResponse", [](const XML::Document & doc) -> Result<DeviceInfo> {
        try {
          DeviceInfo info;
          auto infoNode = doc.evaluateXPath("//*[local-name()='GetDeviceInformationResponse']");
          if (!infoNode.isValid()) {
            WARN_MSG("ONVIF: Failed to find device info response node");
            return Result<DeviceInfo>{false, Error{Error::Type::Validation, "Failed to find device info"}};
          }
          INSANE_MSG("ONVIF: Found device info response node");

          // Helper function to safely get text content and log it
          auto getTextValue = [&infoNode](const std::string & childName) -> std::string {
            auto node = infoNode.getChild("tt:" + childName);
            if (!node.isValid()) { node = infoNode.getChild(childName); }
            if (node.isValid()) {
              std::string val = node.getTextContent();
              INSANE_MSG("ONVIF: Found %s = %s", childName.c_str(), val.c_str());
              return val;
            }
            INSANE_MSG("ONVIF: %s not found", childName.c_str());
            return "";
          };

          // Extract all fields with logging
          info.manufacturer = getTextValue("Manufacturer");
          info.model = getTextValue("Model");
          info.firmwareVersion = getTextValue("FirmwareVersion");
          info.serialNumber = getTextValue("SerialNumber");
          info.hardwareId = getTextValue("HardwareId");

          // Log device info summary
          HIGH_MSG("ONVIF: Device info - %s %s (SN:%s FW:%s)", info.manufacturer.c_str(), info.model.c_str(),
                   info.serialNumber.c_str(), info.firmwareVersion.c_str());

          return Result<DeviceInfo>{true, info};
        } catch (const std::exception & e) {
          WARN_MSG("ONVIF: Exception while parsing device info: %s", e.what());
          return Result<DeviceInfo>{false, Error{Error::Type::Validation, e.what()}};
        }
      });
    } catch (const std::runtime_error & e) {
      WARN_MSG("ONVIF: Runtime error getting device info: %s", e.what());
      return Result<DeviceInfo>{false, Error{Error::Type::Validation, e.what()}};
    }
  }

  Result<std::vector<MediaProfile>> Device::getMediaProfiles() const {
    try {
      // Build request using helper
      auto request = buildRequest("GetProfiles", [](XML::Document & doc, XML::Node & root) {
        root.setAttribute("xmlns", "http://www.onvif.org/ver10/media/wsdl");
      }, SOAP::onvif::ServiceType::Media);
      if (!request) return Result<std::vector<MediaProfile>>{false, request.error};

      // Send request and get response
      std::string response;
      auto result = sendRequestAndGetResponse("media", "GetProfiles", request.value, response);
      if (!result) return Result<std::vector<MediaProfile>>{false, result.error};

      // Extract profiles using response helper
      return extractResponse<std::vector<MediaProfile>>(response, "GetProfilesResponse",
                                                        [](const XML::Document & doc) -> Result<std::vector<MediaProfile>> {
        try {
          std::vector<MediaProfile> profiles;
          auto profileNodes = doc.evaluateXPathAll("//*[local-name()='Profiles' or local-name()='Profile']");

          VERYHIGH_MSG("ONVIF: Processing %zu media profiles", profileNodes.size());

          for (const auto & profileNode : profileNodes) {
            MediaProfile profile;
            profile.token = profileNode.getAttribute("token");
            profile.name = profileNode.getChild("Name").getTextContent();

            // Get video source token from VideoSourceConfiguration
            auto vscNode = profileNode.getChild("VideoSourceConfiguration");
            if (vscNode.isValid()) {
              auto srcToken = vscNode.getChild("SourceToken");
              if (srcToken.isValid()) { profile.videoSourceToken = srcToken.getTextContent(); }
            }

            // Get video configuration
            auto videoNode = profileNode.getChild("VideoEncoderConfiguration");
            if (videoNode.isValid()) {
              profile.video.width = std::stoi(videoNode.getChild("Resolution").getChild("Width").getTextContent());
              profile.video.height = std::stoi(videoNode.getChild("Resolution").getChild("Height").getTextContent());
              profile.video.fps = std::stoi(videoNode.getChild("RateControl").getChild("FrameRateLimit").getTextContent());
              profile.video.bitrate = std::stoi(videoNode.getChild("RateControl").getChild("BitrateLimit").getTextContent());
              profile.video.encoding = videoNode.getChild("Encoding").getTextContent();
              profile.video.quality = std::stoi(videoNode.getChild("Quality").getTextContent());
            }

            // Get audio configuration
            auto audioNode = profileNode.getChild("AudioEncoderConfiguration");
            if (audioNode.isValid()) {
              profile.audio.encoding = audioNode.getChild("Encoding").getTextContent();
              profile.audio.bitrate = std::stoi(audioNode.getChild("Bitrate").getTextContent());
              profile.audio.sampleRate = std::stoi(audioNode.getChild("SampleRate").getTextContent());
            }

            profiles.push_back(profile);
          }

          return Result<std::vector<MediaProfile>>{true, profiles};
        } catch (const std::exception & e) {
          return Result<std::vector<MediaProfile>>{false, Error{Error::Type::Validation, e.what()}};
        }
      });
    } catch (const std::runtime_error & e) {
      return Result<std::vector<MediaProfile>>{false, Error{Error::Type::Validation, e.what()}};
    }
  }

  Result<std::string> Device::getDefaultStreamUri(const std::string & profileToken) const {
    if (!connected) { return Result<std::string>{false, Error{Error::Type::Transport, "Not connected"}}; }

    try {
      // Build request using helper
      auto request = buildRequest("GetStreamUri", [&](XML::Document & doc, XML::Node & root) {
        auto setup = doc.createNode("StreamSetup");
        auto stream = doc.createNode("tt:Stream");
        stream.setTextContent("RTP-Unicast");
        setup.addChild(stream);

        auto transport = doc.createNode("tt:Transport");
        auto protocol = doc.createNode("tt:Protocol");
        protocol.setTextContent("RTSP");
        transport.addChild(protocol);
        setup.addChild(transport);
        root.addChild(setup);

        auto tokenNode = doc.createNode("ProfileToken");
        tokenNode.setTextContent(profileToken);
        root.addChild(tokenNode);
      }, SOAP::onvif::ServiceType::Media);
      if (!request) return Result<std::string>{false, request.error};

      // Send request and get response
      std::string response;
      auto result = sendRequestAndGetResponse("media", "GetStreamUri", request.value, response);
      if (!result) return Result<std::string>{false, result.error};

      // Extract URI using response helper
      return extractResponse<std::string>(response, "GetStreamUriResponse", [](const XML::Document & doc) -> Result<std::string> {
        auto uriNode = doc.evaluateXPath("//*[local-name()='Uri']");
        if (!uriNode.isValid()) {
          return Result<std::string>{false, Error{Error::Type::Validation, "Failed to find Uri node"}};
        }
        return Result<std::string>{true, uriNode.getTextContent()};
      });
    } catch (const std::exception & e) { return Result<std::string>{false, Error{Error::Type::Validation, e.what()}}; }
  }

  Result<std::string> Device::getStreamUriWithProtocol(const std::string & profileToken, const std::string & protocol,
                                                       const std::string & transport) const {
    if (!connected) { return Result<std::string>{false, Error{Error::Type::Transport, "Not connected"}}; }

    try {
      // Build request using helper
      auto request = buildRequest("GetStreamUri", [&](XML::Document & doc, XML::Node & root) {
        auto setup = doc.createNode("StreamSetup");
        auto stream = doc.createNode("tt:Stream");
        stream.setTextContent(protocol);
        setup.addChild(stream);

        auto transportNode = doc.createNode("tt:Transport");
        auto protocolNode = doc.createNode("tt:Protocol");
        protocolNode.setTextContent(transport);
        transportNode.addChild(protocolNode);
        setup.addChild(transportNode);
        root.addChild(setup);

        auto tokenNode = doc.createNode("ProfileToken");
        tokenNode.setTextContent(profileToken);
        root.addChild(tokenNode);
      }, SOAP::onvif::ServiceType::Media);
      if (!request) return Result<std::string>{false, request.error};

      // Send request and get response
      std::string response;
      auto result = sendRequestAndGetResponse("media", "GetStreamUri", request.value, response);
      if (!result) return Result<std::string>{false, result.error};

      // Extract URI using response helper
      return extractResponse<std::string>(response, "GetStreamUriResponse", [](const XML::Document & doc) -> Result<std::string> {
        auto uriNode = doc.evaluateXPath("//*[local-name()='Uri']");
        if (!uriNode.isValid()) {
          return Result<std::string>{false, Error{Error::Type::Validation, "Failed to find Uri node"}};
        }
        return Result<std::string>{true, uriNode.getTextContent()};
      });
    } catch (const std::exception & e) { return Result<std::string>{false, Error{Error::Type::Validation, e.what()}}; }
  }

  Result<std::string> Device::getStreamUriWithTransport(const std::string & profileToken, const TransportDetails & transport) const {
    if (!connected) { return Result<std::string>{false, Error{Error::Type::Transport, "Not connected"}}; }

    try {
      // Build request using helper
      auto request = buildRequest("GetStreamUri", [&](XML::Document & doc, XML::Node & root) {
        auto setup = doc.createNode("StreamSetup");
        auto stream = doc.createNode("tt:Stream");
        stream.setTextContent(transport.options.at("streamType"));
        setup.addChild(stream);

        auto transportNode = doc.createNode("tt:Transport");
        auto protocolNode = doc.createNode("tt:Protocol");
        protocolNode.setTextContent(transport.options.at("transport"));
        transportNode.addChild(protocolNode);
        setup.addChild(transportNode);
        root.addChild(setup);

        auto tokenNode = doc.createNode("ProfileToken");
        tokenNode.setTextContent(profileToken);
        root.addChild(tokenNode);
      }, SOAP::onvif::ServiceType::Media);
      if (!request) return Result<std::string>{false, request.error};

      // Send request and get response
      std::string response;
      auto result = sendRequestAndGetResponse("media", "GetStreamUri", request.value, response);
      if (!result) return Result<std::string>{false, result.error};

      // Extract URI using response helper
      return extractResponse<std::string>(response, "GetStreamUriResponse", [](const XML::Document & doc) -> Result<std::string> {
        auto uriNode = doc.evaluateXPath("//*[local-name()='Uri']");
        if (!uriNode.isValid()) {
          return Result<std::string>{false, Error{Error::Type::Validation, "Failed to find Uri node"}};
        }
        return Result<std::string>{true, uriNode.getTextContent()};
      });
    } catch (const std::exception & e) { return Result<std::string>{false, Error{Error::Type::Validation, e.what()}}; }
  }

  bool Device::hasPTZService() const {
    return services.find("ptz") != services.end();
  }

  Result<bool> Device::absoluteMove(const std::string & profileToken, float pan, float tilt, float zoom) {
    if (!connected) { return Result<bool>{false, Error{Error::Type::Transport, "Not connected"}}; }

    try {
      auto msg = createMessage("AbsoluteMove", SOAP::onvif::ServiceType::PTZ);

      // Create XML document for body
      XML::Document doc;
      XML::Node root = doc.createNode("tptz:AbsoluteMove");

      // Add profile token
      XML::Node tokenNode = doc.createNode("tptz:ProfileToken");
      tokenNode.setTextContent(profileToken);
      root.addChild(tokenNode);

      // Add position
      XML::Node position = doc.createNode("tptz:Position");

      // Pan/Tilt vector
      XML::Node panTilt = doc.createNode("tt:PanTilt");
      panTilt.setAttribute("x", std::to_string(pan));
      panTilt.setAttribute("y", std::to_string(tilt));
      position.addChild(panTilt);

      // Zoom vector
      XML::Node zoomNode = doc.createNode("tt:Zoom");
      zoomNode.setAttribute("x", std::to_string(zoom));
      position.addChild(zoomNode);

      root.addChild(position);
      msg.setBodyNode(root);

      // Send request
      auto result = sendRequestVoid("ptz", "AbsoluteMove", msg.toString());
      if (!result) return Result<bool>{false, result.error};

      return Result<bool>{true, true};
    } catch (const std::exception & e) { return Result<bool>{false, Error{Error::Type::Validation, e.what()}}; }
  }

  Result<bool> Device::relativeMove(const std::string & profileToken, float pan, float tilt, float zoom) {
    if (!connected) { return Result<bool>{false, Error{Error::Type::Transport, "Not connected"}}; }

    try {
      auto msg = createMessage("RelativeMove", SOAP::onvif::ServiceType::PTZ);

      // Create XML document for body
      XML::Document doc;
      XML::Node root = doc.createNode("tptz:RelativeMove");

      // Add profile token
      XML::Node tokenNode = doc.createNode("tptz:ProfileToken");
      tokenNode.setTextContent(profileToken);
      root.addChild(tokenNode);

      // Add translation
      XML::Node translation = doc.createNode("tptz:Translation");

      // Pan/Tilt vector
      XML::Node panTilt = doc.createNode("tt:PanTilt");
      panTilt.setAttribute("x", std::to_string(pan));
      panTilt.setAttribute("y", std::to_string(tilt));
      translation.addChild(panTilt);

      // Zoom vector
      XML::Node zoomNode = doc.createNode("tt:Zoom");
      zoomNode.setAttribute("x", std::to_string(zoom));
      translation.addChild(zoomNode);

      root.addChild(translation);
      msg.setBodyNode(root);

      // Send request
      auto result = sendRequestVoid("ptz", "RelativeMove", msg.toString());
      if (!result) return Result<bool>{false, result.error};

      return Result<bool>{true, true};
    } catch (const std::exception & e) { return Result<bool>{false, Error{Error::Type::Validation, e.what()}}; }
  }

  Result<bool> Device::continuousMove(const std::string & profileToken, float pan, float tilt, float zoom) {
    if (!connected) { return Result<bool>{false, Error{Error::Type::Transport, "Not connected"}}; }

    try {
      auto msg = createMessage("ContinuousMove", SOAP::onvif::ServiceType::PTZ);

      auto doc = msg.getDocument();
      auto root = doc.createNode("tptz:ContinuousMove");

      auto token = doc.createNode("tptz:ProfileToken");
      token.setTextContent(profileToken);
      root.addChild(token);

      auto velocity = doc.createNode("tptz:Velocity");

      auto panTilt = doc.createNode("tt:PanTilt");
      panTilt.setAttribute("x", std::to_string(pan));
      panTilt.setAttribute("y", std::to_string(tilt));
      velocity.addChild(panTilt);

      if (zoom != 0) {
        auto zoomVec = doc.createNode("tt:Zoom");
        zoomVec.setAttribute("x", std::to_string(zoom));
        velocity.addChild(zoomVec);
      }

      root.addChild(velocity);

      msg.setBodyNode(root);

      // Send request
      std::string response;
      if (!sendRequest("ptz", "ContinuousMove", msg.toString(), response)) {
        return Result<bool>(false, Error(Error::Type::Transport, "Failed to send continuous move request"));
      }

      return Result<bool>(true);
    } catch (const std::exception & e) {
      return Result<bool>(false, Error(Error::Type::Transport, std::string("Exception in continuousMove: ") + e.what()));
    }
  }

  Result<bool> Device::stop(const std::string & profileToken) {
    if (!connected) { return Result<bool>{false, Error{Error::Type::Transport, "Not connected"}}; }

    try {
      auto msg = createMessage("Stop", SOAP::onvif::ServiceType::PTZ);

      auto doc = msg.getDocument();
      auto root = doc.createNode("tptz:Stop");

      auto token = doc.createNode("tptz:ProfileToken");
      token.setTextContent(profileToken);
      root.addChild(token);

      auto pantilt = doc.createNode("tptz:PanTilt");
      pantilt.setTextContent("true");
      root.addChild(pantilt);

      auto zoom = doc.createNode("tptz:Zoom");
      zoom.setTextContent("true");
      root.addChild(zoom);

      msg.setBodyNode(root);

      // Send request
      std::string response;
      if (!sendRequest("ptz", "Stop", msg.toString(), response)) {
        return Result<bool>(false, Error(Error::Type::Transport, "Failed to send stop request"));
      }

      return Result<bool>(true);
    } catch (const std::exception & e) {
      return Result<bool>(false, Error(Error::Type::Transport, std::string("Exception in stop: ") + e.what()));
    }
  }

  Result<PTZStatus> Device::getPTZStatus(const std::string & profileToken) {
    if (!connected) { return Result<PTZStatus>{false, Error{Error::Type::Transport, "Not connected"}}; }

    try {
      auto msg = createMessage("GetStatus", SOAP::onvif::ServiceType::PTZ);

      // Use XML helpers to build the request
      XML::Document doc;
      XML::Node root = doc.createNode("tptz:GetStatus");

      XML::Node tokenNode = doc.createNode("tptz:ProfileToken");
      tokenNode.setTextContent(profileToken);
      root.addChild(tokenNode);

      msg.setBodyNode(root);

      // Send request and get response
      std::string response;
      auto result = sendRequestAndGetResponse("ptz", "GetStatus", msg.toString(), response);
      if (!result) { return Result<PTZStatus>{false, result.error}; }

      // Parse response
      HTTP::Parser parser;
      if (!parser.Read(result.value)) {
        return Result<PTZStatus>{false, Error{Error::Type::Transport, "Failed to parse HTTP response"}};
      }

      auto doc_result = parseResponse(parser, false);
      if (!doc_result) { return Result<PTZStatus>{false, doc_result.error}; }

      // Find PTZ status node
      auto statusNode = findNode(doc_result.value, "PTZStatus");
      if (!statusNode) {
        return Result<PTZStatus>{false, Error{Error::Type::Validation, "Failed to find PTZStatus node"}};
      }

      PTZStatus status;

      // Get position info
      auto positionNode = statusNode.value.getChild("Position");
      if (positionNode.isValid()) {
        // Get pan/tilt
        auto panTiltNode = positionNode.getChild("PanTilt");
        if (panTiltNode.isValid()) {
          status.pan = std::stof(panTiltNode.getAttribute("x"));
          status.tilt = std::stof(panTiltNode.getAttribute("y"));
        }

        // Get zoom
        auto zoomNode = positionNode.getChild("Zoom");
        if (zoomNode.isValid()) { status.zoom = std::stof(zoomNode.getAttribute("x")); }
      }

      // Get move status
      auto moveStatusNode = statusNode.value.getChild("MoveStatus");
      if (moveStatusNode.isValid()) { status.moveStatus = moveStatusNode.getTextContent(); }

      // Get error
      auto errorNode = statusNode.value.getChild("Error");
      if (errorNode.isValid()) { status.error = errorNode.getTextContent(); }

      // Get UTC time
      auto utcTimeNode = statusNode.value.getChild("UtcTime");
      if (utcTimeNode.isValid()) {
        std::string timeStr = utcTimeNode.getTextContent();
        struct tm tm = {};
        if (strptime(timeStr.c_str(), "%Y-%m-%dT%H:%M:%SZ", &tm)) { status.utcTime = timegm(&tm); }
      }

      return Result<PTZStatus>{true, status};
    } catch (const std::exception & e) { return Result<PTZStatus>{false, Error{Error::Type::Validation, e.what()}}; }
  }

  Result<bool> Device::gotoHomePosition(const std::string & profileToken) {
    if (!connected) { return Result<bool>{false, Error{Error::Type::Transport, "Not connected"}}; }

    try {
      auto msg = createMessage("GotoHomePosition", SOAP::onvif::ServiceType::PTZ);

      auto doc = msg.getDocument();
      auto root = doc.createNode("tptz:GotoHomePosition");

      auto token = doc.createNode("tptz:ProfileToken");
      token.setTextContent(profileToken);
      root.addChild(token);

      msg.setBodyNode(root);

      // Send request
      std::string response;
      if (!sendRequest("ptz", "GotoHomePosition", msg.toString(), response)) {
        return Result<bool>(false, Error(Error::Type::Transport, "Failed to send goto home position request"));
      }

      return Result<bool>(true);
    } catch (const std::exception & e) {
      return Result<bool>(false, Error(Error::Type::Transport, std::string("Exception in gotoHomePosition: ") + e.what()));
    }
  }

  Result<bool> Device::gotoPreset(const std::string & profileToken, const std::string & presetToken, const std::string & presetName) {
    if (!connected || !hasPTZService()) return Result<bool>(false);

    try {
      // Create PTZ SOAP message
      auto msg = createMessage("GotoPreset", SOAP::onvif::ServiceType::PTZ);

      auto doc = msg.getDocument();
      auto root = doc.createNode("tptz:GotoPreset");

      auto token = doc.createNode("tptz:ProfileToken");
      token.setTextContent(profileToken);
      root.addChild(token);

      if (!presetToken.empty()) {
        auto preset = doc.createNode("tptz:PresetToken");
        preset.setTextContent(presetToken);
        root.addChild(preset);
      } else if (!presetName.empty()) {
        auto preset = doc.createNode("tptz:PresetName");
        preset.setTextContent(presetName);
        root.addChild(preset);
      } else {
        return Result<bool>(false, Error(Error::Type::Validation, "Either preset token or name must be provided"));
      }

      msg.setBodyNode(root);

      // Send request
      std::string response;
      if (!sendRequest("ptz", "GotoPreset", msg.toString(), response)) {
        return Result<bool>(false, Error(Error::Type::Transport, "Failed to send goto preset request"));
      }

      return Result<bool>(true);
    } catch (const std::exception & e) {
      return Result<bool>(false, Error(Error::Type::Transport, std::string("Exception in gotoPreset: ") + e.what()));
    }
  }

  Result<bool> Device::setPreset(const std::string & profileToken, const std::string & presetToken, const std::string & presetName) {
    if (!connected || !hasPTZService()) return Result<bool>(false);

    try {
      // Create PTZ SOAP message
      auto msg = createMessage("SetPreset", SOAP::onvif::ServiceType::PTZ);

      auto doc = msg.getDocument();
      auto root = doc.createNode("tptz:SetPreset");

      auto token = doc.createNode("tptz:ProfileToken");
      token.setTextContent(profileToken);
      root.addChild(token);

      if (!presetToken.empty()) {
        auto preset = doc.createNode("tptz:PresetToken");
        preset.setTextContent(presetToken);
        root.addChild(preset);
      }

      if (!presetName.empty()) {
        auto name = doc.createNode("tptz:PresetName");
        name.setTextContent(presetName);
        root.addChild(name);
      }

      msg.setBodyNode(root);

      // Send request
      std::string response;
      if (!sendRequest("ptz", "SetPreset", msg.toString(), response)) {
        return Result<bool>(false, Error(Error::Type::Transport, "Failed to send set preset request"));
      }

      return Result<bool>(true);
    } catch (const std::exception & e) {
      return Result<bool>(false, Error(Error::Type::Transport, std::string("Exception in setPreset: ") + e.what()));
    }
  }

  bool Device::sendRequest(const std::string & service, const std::string & action, const std::string & request,
                           std::string & response) {
    auto result = sendRequestAndGetResponse(service, action, request, response);
    if (!result) { return false; }
    return true;
  }

  /// Reads a full HTTP/SOAP response from `conn` within `timeoutSeconds`,
  /// and returns either the raw body XML or a transport error.
  Result<HTTP::Parser> Device::readResponseWithTimeout(Socket::Connection & conn, int timeoutSeconds) const {
    // Make socket non-blocking so we can poll + timeout
    conn.setBlocking(false);

    HTTP::Parser resp;
    auto startTime = std::chrono::steady_clock::now();

    // keep trying Read() until it returns true (complete) or we hit timeout
    while (!resp.Read(conn)) {
      auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - startTime).count();

      if (elapsed >= timeoutSeconds) {
        INFO_MSG("ONVIF: Timeout after %d seconds", timeoutSeconds);
        return Result<HTTP::Parser>(false, Error(Error::Type::Transport, "Timeout waiting for response"));
      }

      if (!conn) {
        return Result<HTTP::Parser>(false, Error(Error::Type::Transport, "Connection lost while reading response"));
      }

      // pull in any bytes that are ready (non-blocking)
      conn.spool();

      // throttle your loop so you're not busy-spinning
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // if we get here, resp.Read(conn) returned true → full message
    return Result<HTTP::Parser>(true, std::move(resp));
  }

  Result<std::string> Device::sendRequestAndGetResponse(const std::string & service, const std::string & action,
                                                        const std::string & request, std::string & response,
                                                        const std::string & endpointOverride) const {
    try {
      // Get service endpoint from map (or use override)
      std::string serviceUrl;
      if (!endpointOverride.empty()) {
        serviceUrl = endpointOverride;
      } else {
        auto it = services.find(service);
        if (it != services.end()) {
          serviceUrl = it->second;
          INSANE_MSG("ONVIF: Using service URL from map: %s", serviceUrl.c_str());
        } else if (service == "device" && !connected) {
          serviceUrl = "http://" + host + ":" + std::to_string(port) + "/onvif/device_service";
          INSANE_MSG("ONVIF: Using default device service URL for initial connection: %s", serviceUrl.c_str());
        } else {
          std::string error = "Service '" + service + "' endpoint not found in service map. ";
          error += "Available services: ";
          for (const auto & svc : services) { error += svc.first + ", "; }
          WARN_MSG("ONVIF: %s", error.c_str());
          return Result<std::string>{false, Error{Error::Type::Validation, error}};
        }
      }

      INSANE_MSG("ONVIF: Sending request to %s - Action: %s", serviceUrl.c_str(), action.c_str());

      INSANE_MSG("ONVIF: Request body:");
      std::istringstream requestStream(request);
      std::string line;
      while (std::getline(requestStream, line)) { INSANE_MSG("ONVIF: | %s", line.c_str()); }

      // Parse URL and create connection
      HTTP::URL url(serviceUrl);
      if (url.host.empty()) {
        WARN_MSG("ONVIF: Invalid service URL: %s", serviceUrl.c_str());
        return Result<std::string>{false, Error{Error::Type::Validation, "Invalid service URL"}};
      }

      Socket::Connection conn;
      conn.open(url.host, url.getPort(), false);
      if (!conn) {
        WARN_MSG("ONVIF: Failed to connect to %s:%d", url.host.c_str(), url.getPort());
        return Result<std::string>{false, Error{Error::Type::Transport, "Failed to connect to service endpoint"}};
      }

      // Build HTTP request using Parser
      HTTP::Parser httpRequest;
      httpRequest.method = "POST";
      httpRequest.url = url.path;
      httpRequest.protocol = "HTTP/1.1";
      httpRequest.SetHeader("Host", url.host + ":" + std::to_string(url.getPort()));
      httpRequest.SetHeader("Content-Type", "application/soap+xml; charset=utf-8; action=\"" + action + "\"");
      httpRequest.SetHeader("Content-Length", std::to_string(request.length()));
      if (!username.empty() && !password.empty()) {
        std::string auth = username + ":" + password;
        std::string encoded = Encodings::Base64::encode(auth);
        httpRequest.SetHeader("Authorization", "Basic " + encoded);
      }
      httpRequest.SetHeader("Connection", "close");
      httpRequest.SetBody(request);

      // Send request
      httpRequest.SendRequest(conn);
      if (!conn) {
        WARN_MSG("ONVIF: Failed to send request");
        return Result<std::string>{false, Error{Error::Type::Transport, "Failed to send request"}};
      }

      // Read response with timeout
      auto resp = readResponseWithTimeout(conn, requestTimeoutSeconds);
      if (!resp) {
        WARN_MSG("ONVIF: Failed to read response - %s", resp.error.message.c_str());
        return Result<std::string>{false, resp.error};
      }

      auto handled = handleResponse(resp.value, true);
      if (!handled) {
        if (handled.error.type == Error::Type::Authentication && useDigestAuth) {
          WARN_MSG("ONVIF: Auth failed with WS-Security, retrying without digest");
          useDigestAuth = false;

          // Retry once with digest disabled
          Socket::Connection conn2;
          conn2.open(url.host, url.getPort(), false);
          if (conn2) {
            HTTP::Parser retry;
            retry.method = "POST";
            retry.url = url.path;
            retry.protocol = "HTTP/1.1";
            retry.SetHeader("Host", url.host + ":" + std::to_string(url.getPort()));
            retry.SetHeader("Content-Type", "application/soap+xml; charset=utf-8; action=\"" + action + "\"");
            retry.SetHeader("Content-Length", std::to_string(request.length()));
            if (!username.empty() && !password.empty()) {
              std::string auth = username + ":" + password;
              std::string encoded = Encodings::Base64::encode(auth);
              retry.SetHeader("Authorization", "Basic " + encoded);
            }
            retry.SetHeader("Connection", "close");
            retry.SetBody(request);
            retry.SendRequest(conn2);
            if (conn2) {
              auto resp2 = readResponseWithTimeout(conn2, requestTimeoutSeconds);
              if (resp2) {
                auto handled2 = handleResponse(resp2.value, true);
                if (handled2) {
                  response = resp2.value.body;
                  return Result<std::string>{true, response};
                }
              }
            }
          }
        }
        return Result<std::string>{false, handled.error};
      }

      response = resp.value.body;
      return Result<std::string>{true, response};
    } catch (const std::exception & e) {
      WARN_MSG("ONVIF: Exception in sendRequestAndGetResponse: %s", e.what());
      return Result<std::string>{false, Error{Error::Type::Transport, e.what()}};
    }
  }

  Result<void> Device::sendRequestVoid(const std::string & service, const std::string & action,
                                       const std::string & request, const std::string & endpointOverride) {
    std::string response;
    auto result = sendRequestAndGetResponse(service, action, request, response, endpointOverride);
    if (!result) { return Result<void>{false, result.error}; }
    return Result<void>{true};
  }

  // Device Management Methods
  Result<SystemDateTime> Device::getSystemDateAndTime() const {
    if (!connected) { return Result<SystemDateTime>{false, Error{Error::Type::Transport, "Not connected"}}; }

    // Build request using helper
    auto request = buildRequest("GetSystemDateAndTime", [](XML::Document & doc, XML::Node & root) {
      root.setAttribute("xmlns", "http://www.onvif.org/ver10/device/wsdl");
    }, SOAP::onvif::ServiceType::Device);
    if (!request) return Result<SystemDateTime>{false, request.error};

    // Send request and get response
    std::string response;
    auto result = sendRequestAndGetResponse("device", "GetSystemDateAndTime", request.value, response);
    if (!result) return Result<SystemDateTime>{false, result.error};

    // Extract system date/time using our response helper
    return extractResponse<SystemDateTime>(response, "GetSystemDateAndTimeResponse",
                                           [](const XML::Document & doc) -> Result<SystemDateTime> {
      SystemDateTime dateTime;
      XML::Node systemDateTime = doc.evaluateXPath("//*[local-name()='SystemDateAndTime']");
      if (systemDateTime.isValid()) {
        // Parse date/time fields using namespace-independent XPath
        XML::Node dateTimeType = doc.evaluateXPath("//*[local-name()='DateTimeType']");
        if (dateTimeType.isValid()) {
          INSANE_MSG("ONVIF: DateTime info - Type:%s DST:%s TZ:%s UTC:%s", dateTimeType.getValue().c_str(),
                     doc.evaluateXPath("//*[local-name()='DaylightSavings']").getValue().c_str(),
                     doc.evaluateXPath("//*[local-name()='TimeZone']/*[local-name()='TZ']").getValue().c_str(),
                     doc.evaluateXPath("//*[local-name()='UTCDateTime']").getValue().c_str());
          dateTime.dateTimeType = dateTimeType.getValue() == "NTP";
        }

        XML::Node daylightSavings = doc.evaluateXPath("//*[local-name()='DaylightSavings']");
        if (daylightSavings.isValid()) {
          INSANE_MSG("ONVIF: DaylightSavings: %s", daylightSavings.getValue().c_str());
          dateTime.isDST = daylightSavings.getValue() == "true";
        }

        XML::Node timezone = doc.evaluateXPath("//*[local-name()='TimeZone']/*[local-name()='TZ']");
        if (timezone.isValid()) {
          INSANE_MSG("ONVIF: TimeZone: %s", timezone.getValue().c_str());
          dateTime.timezone = timezone.getValue();
        }

        // Get UTC time
        XML::Node utcNode = doc.evaluateXPath("//*[local-name()='UTCDateTime']");
        if (utcNode.isValid()) {
          INSANE_MSG("ONVIF: UTCDateTime: %s", utcNode.getValue().c_str());
          dateTime.utcDateTime = utcNode.getValue();
        }

        // Get local time
        XML::Node localNode = doc.evaluateXPath("//*[local-name()='LocalDateTime']");
        if (localNode.isValid()) { dateTime.localDateTime = localNode.getValue(); }

        return Result<SystemDateTime>{true, std::move(dateTime)};
      }
      return Result<SystemDateTime>{false, Error{Error::Type::Validation, "Failed to find SystemDateAndTime node"}};
    });
  }

  Result<void> Device::setSystemDateAndTime(const SystemDateTime & dateTime) {
    try {
      auto msg = createMessage("SetSystemDateAndTime", SOAP::onvif::ServiceType::Device);

      XML::Document doc;
      XML::Node root = doc.createNode("tds:SetSystemDateAndTime");

      // Add date/time type
      XML::Node typeNode = doc.createNode("tds:DateTimeType");
      typeNode.setValue(dateTime.dateTimeType ? "NTP" : "Manual");
      root.addChild(typeNode);

      // Add DST
      XML::Node dstNode = doc.createNode("tds:DaylightSavings");
      dstNode.setValue(dateTime.isDST ? "true" : "false");
      root.addChild(dstNode);

      // Add timezone
      if (!dateTime.timezone.empty()) {
        XML::Node tzNode = doc.createNode("tds:TimeZone");
        XML::Node tzTokenNode = doc.createNode("tt:TZ");
        tzTokenNode.setValue(dateTime.timezone);
        tzNode.addChild(tzTokenNode);
        root.addChild(tzNode);
      }

      msg.setBodyNode(root);

      std::string response;
      auto result = sendRequestAndGetResponse("device", "SetSystemDateAndTime", msg.toString(), response);
      if (!result) { return Result<void>{false, result.error}; }

      return Result<void>{true};
    } catch (const std::runtime_error & e) { return Result<void>{false, Error{Error::Type::Validation, e.what()}}; }
  }

  Result<std::vector<NetworkInterface>> Device::getNetworkInterfaces() const {
    if (!connected) {
      return Result<std::vector<NetworkInterface>>{false, Error{Error::Type::Transport, "Not connected"}};
    }

    // Build request using helper
    auto request = buildRequest("GetNetworkInterfaces", [](XML::Document & doc, XML::Node & root) {
      root.setAttribute("xmlns", "http://www.onvif.org/ver10/device/wsdl");
    }, SOAP::onvif::ServiceType::Device);
    if (!request) return Result<std::vector<NetworkInterface>>{false, request.error};

    // Send request and get response
    std::string response;
    auto result = sendRequestAndGetResponse("device", "GetNetworkInterfaces", request.value, response);
    if (!result) return Result<std::vector<NetworkInterface>>{false, result.error};

    // Extract network interfaces using our response helper
    return extractResponse<std::vector<NetworkInterface>>(
      response, "GetNetworkInterfacesResponse", [](const XML::Document & doc) -> Result<std::vector<NetworkInterface>> {
      std::vector<NetworkInterface> interfaces;

      // Get all interface nodes using XPath to avoid namespace issues
      auto nodes = doc.evaluateXPathAll("//*[local-name()='NetworkInterface']");
      for (const auto & ifaceNode : nodes) {
        NetworkInterface iface;

        // Get interface info using the current ifaceNode
        auto nameNode = ifaceNode.getChild("Name");
        if (nameNode.isValid()) iface.name = nameNode.getTextContent();

        auto hwNode = ifaceNode.getChild("HwAddress");
        if (hwNode.isValid()) iface.macAddress = hwNode.getTextContent();

        // Get IPv4 info
        auto ipv4Node = ifaceNode.getChild("IPv4");
        if (ipv4Node.isValid()) {
          auto manualNode = ipv4Node.getChild("ManualAddress");
          if (manualNode.isValid()) {
            auto addrNode = manualNode.getChild("Address");
            if (addrNode.isValid()) iface.ipv4Address = addrNode.getTextContent();

            auto maskNode = manualNode.getChild("PrefixLength");
            if (maskNode.isValid()) iface.ipv4Mask = maskNode.getTextContent();
          }
        }

        // Get enabled state
        auto enabledNode = ifaceNode.getChild("Enabled");
        if (enabledNode.isValid()) { iface.enabled = (enabledNode.getTextContent() == "true"); }

        interfaces.push_back(std::move(iface));
      }

      return Result<std::vector<NetworkInterface>>{true, std::move(interfaces)};
    });
  }

  Result<DeviceCapabilities> Device::getDeviceCapabilities() const {
    try {
      HIGH_MSG("ONVIF: Getting device capabilities for %s:%d", host.c_str(), port);

      // Build request using helper
      auto request = buildRequest("GetCapabilities", [](XML::Document & doc, XML::Node & root) {
        root.setAttribute("xmlns", "http://www.onvif.org/ver10/device/wsdl");
      }, SOAP::onvif::ServiceType::Device);
      if (!request) {
        WARN_MSG("ONVIF: Failed to build capabilities request - %s", request.error.message.c_str());
        return Result<DeviceCapabilities>{false, request.error};
      }

      // Send request and get response
      std::string response;
      auto result = sendRequestAndGetResponse("device", "GetCapabilities", request.value, response);
      if (!result) {
        WARN_MSG("ONVIF: Failed to get capabilities - %s", result.error.message.c_str());
        return Result<DeviceCapabilities>{false, result.error};
      }

      // Log raw response for debugging
      INSANE_MSG("ONVIF: Parsing device capabilities response:");
      std::istringstream responseStream(response);
      std::string line;
      while (std::getline(responseStream, line)) { INSANE_MSG("ONVIF: | %s", line.c_str()); }

      // Extract capabilities using response helper
      return extractResponse<DeviceCapabilities>(response, "GetCapabilitiesResponse",
                                                 [](const XML::Document & doc) -> Result<DeviceCapabilities> {
        try {
          DeviceCapabilities caps;

          // Find capabilities node
          auto capsNode = doc.evaluateXPath("//*[local-name()='Capabilities']");
          if (!capsNode.isValid()) {
            WARN_MSG("ONVIF: Failed to find Capabilities node in response");
            return Result<DeviceCapabilities>{false, Error{Error::Type::Validation, "Failed to find capabilities"}};
          }
          INSANE_MSG("ONVIF: Found Capabilities node");

          // ONVIF GetCapabilities indicates service support via presence of child elements
          // (e.g. <PTZ><XAddr>http://...</XAddr></PTZ>), not boolean text values
          auto hasService = [](const XML::Node & parent, const std::string & childName) -> bool {
            auto node = parent.getChild(childName);
            if (!node.isValid()) { node = parent.getChild("tt:" + childName); }
            if (node.isValid()) {
              INSANE_MSG("ONVIF: Found service %s", childName.c_str());
              return true;
            }
            INSANE_MSG("ONVIF: Service %s not found", childName.c_str());
            return false;
          };

          // Find media node and extract XAddr
          auto mediaNode = capsNode.getChild("tt:Media");
          if (!mediaNode.isValid()) { mediaNode = capsNode.getChild("Media"); }
          if (mediaNode.isValid()) {
            INSANE_MSG("ONVIF: Found Media node");
            auto xaddrNode = mediaNode.getChild("tt:XAddr");
            if (!xaddrNode.isValid()) { xaddrNode = mediaNode.getChild("XAddr"); }
            if (xaddrNode.isValid()) {
              caps.mediaXAddr = xaddrNode.getTextContent();
              INSANE_MSG("ONVIF: Found Media XAddr: %s", caps.mediaXAddr.c_str());
            } else {
              WARN_MSG("ONVIF: No Media XAddr found");
            }
          } else {
            WARN_MSG("ONVIF: No Media node found in capabilities");
          }

          // Extract other capabilities
          caps.ptz = hasService(capsNode, "PTZ");
          caps.events = hasService(capsNode, "Events");
          caps.imaging = hasService(capsNode, "Imaging");
          caps.media = hasService(capsNode, "Media");
          caps.analytics = hasService(capsNode, "Analytics");
          caps.deviceIO = hasService(capsNode, "DeviceIO");
          caps.recording = hasService(capsNode, "Recording");
          caps.search = hasService(capsNode, "Search");
          caps.replay = hasService(capsNode, "Replay");
          caps.receiver = hasService(capsNode, "Receiver");
          caps.display = hasService(capsNode, "Display");

          // Log capabilities summary
          HIGH_MSG("ONVIF: Core capabilities - PTZ:%s Media:%s Events:%s Analytics:%s", caps.ptz ? "yes" : "no",
                   caps.media ? "yes" : "no", caps.events ? "yes" : "no", caps.analytics ? "yes" : "no");

          VERYHIGH_MSG("ONVIF: Extended capabilities - IO:%s Recording:%s Search:%s Display:%s", caps.deviceIO ? "yes" : "no",
                       caps.recording ? "yes" : "no", caps.search ? "yes" : "no", caps.display ? "yes" : "no");

          return Result<DeviceCapabilities>{true, std::move(caps)};
        } catch (const std::exception & e) {
          return Result<DeviceCapabilities>{
            false, Error{Error::Type::Validation, std::string("Failed to parse device capabilities: ") + e.what()}};
        }
      });
    } catch (const std::runtime_error & e) {
      return Result<DeviceCapabilities>{false, Error{Error::Type::Validation, e.what()}};
    }
  }

  Result<void> Device::reboot() {
    try {
      auto msg = createMessage("SystemReboot", SOAP::onvif::ServiceType::Device);

      std::string response;
      auto result = sendRequestAndGetResponse("device", "SystemReboot", msg.toString(), response);
      if (!result) { return Result<void>{false, result.error}; }

      return Result<void>{true};
    } catch (const std::runtime_error & e) { return Result<void>{false, Error{Error::Type::Validation, e.what()}}; }
  }

  Result<void> Device::factoryReset() {
    try {
      auto msg = createMessage("SystemFactoryDefault", SOAP::onvif::ServiceType::Device);

      std::string response;
      auto result = sendRequestAndGetResponse("device", "SystemFactoryDefault", msg.toString(), response);
      if (!result) { return Result<void>{false, result.error}; }

      return Result<void>{true};
    } catch (const std::runtime_error & e) { return Result<void>{false, Error{Error::Type::Validation, e.what()}}; }
  }

  // User Management Methods
  Result<std::vector<User>> Device::getUsers() const {
    std::vector<User> users;
    if (!connected) return Result<std::vector<User>>{false, Error{Error::Type::Transport, "Not connected"}};

    try {
      auto msg = createMessage("GetUsers", SOAP::onvif::ServiceType::Device);

      // Use XML helpers to build the request
      XML::Document doc;
      XML::Node root = doc.createNode("tds:GetUsers");
      msg.setBodyNode(root);

      std::string response;
      auto result = sendRequestAndGetResponse("device", "GetUsers", msg.toString(), response);
      if (!result) {
        WARN_MSG("ONVIF: Failed to get users - %s", result.error.message.c_str());
        return Result<std::vector<User>>{false, result.error};
      }

      // Parse response using our helper
      auto responseDoc = XML::Document(response);
      auto usersNode = responseDoc.evaluateXPath("//*[local-name()='GetUsersResponse']");
      if (!usersNode.isValid()) {
        WARN_MSG("ONVIF: Failed to find users response node");
        return Result<std::vector<User>>{false, Error{Error::Type::Validation, "Failed to find users response node"}};
      }

      // Use XPath to get all User nodes directly
      auto userNodes = responseDoc.evaluateXPathAll("//*[local-name()='User']");
      for (const auto & userNode : userNodes) {
        User user;
        auto username = userNode.getChild("Username");
        auto userLevel = userNode.getChild("UserLevel");

        if (username.isValid()) { user.username = username.getTextContent(); }

        if (userLevel.isValid()) { user.userLevel = userLevel.getTextContent(); }

        users.push_back(user);
      }

      return Result<std::vector<User>>{true, users};
    } catch (const std::exception & e) {
      return Result<std::vector<User>>{false, Error{Error::Type::Validation, e.what()}};
    }
  }

  Result<bool> Device::createUser(const User & user) {
    if (!connected) { return Result<bool>{false, Error{Error::Type::Transport, "Not connected"}}; }

    try {
      auto msg = createMessage("CreateUsers", SOAP::onvif::ServiceType::Device);

      // Use XML helpers to build the request
      XML::Document doc;
      XML::Node root = doc.createNode("tds:CreateUsers");

      // Create User node
      XML::Node userNode = doc.createNode("tds:User");

      XML::Node username = doc.createNode("tt:Username");
      username.setTextContent(user.username);
      userNode.addChild(username);

      XML::Node password = doc.createNode("tt:Password");
      password.setTextContent(user.password);
      userNode.addChild(password);

      XML::Node userLevel = doc.createNode("tt:UserLevel");
      userLevel.setTextContent(user.userLevel);
      userNode.addChild(userLevel);

      root.addChild(userNode);
      msg.setBodyNode(root);

      // Send request
      auto result = sendRequestVoid("device", "CreateUsers", msg.toString());
      if (!result) { return Result<bool>{false, result.error}; }

      return Result<bool>{true, true};
    } catch (const std::exception & e) { return Result<bool>{false, Error{Error::Type::Validation, e.what()}}; }
  }

  Result<bool> Device::deleteUser(const std::string & username) {
    if (!connected) { return Result<bool>{false, Error{Error::Type::Transport, "Not connected"}}; }

    try {
      auto msg = createMessage("DeleteUsers", SOAP::onvif::ServiceType::Device);

      // Use XML helpers to build the request
      XML::Document doc;
      XML::Node root = doc.createNode("tds:DeleteUsers");

      XML::Node usernameNode = doc.createNode("tds:Username");
      usernameNode.setTextContent(username);
      root.addChild(usernameNode);

      msg.setBodyNode(root);

      // Send request
      auto result = sendRequestVoid("device", "DeleteUsers", msg.toString());
      if (!result) { return Result<bool>{false, result.error}; }

      return Result<bool>{true, true};
    } catch (const std::exception & e) { return Result<bool>{false, Error{Error::Type::Validation, e.what()}}; }
  }

  Result<bool> Device::setUser(const User & user) {
    if (!connected) { return Result<bool>{false, Error{Error::Type::Transport, "Not connected"}}; }

    try {
      auto msg = createMessage("SetUser", SOAP::onvif::ServiceType::Device);

      // Use XML helpers to build the request
      XML::Document doc;
      XML::Node root = doc.createNode("tds:SetUser");

      // Create User node
      XML::Node userNode = doc.createNode("tds:User");

      XML::Node username = doc.createNode("tt:Username");
      username.setTextContent(user.username);
      userNode.addChild(username);

      if (!user.password.empty()) {
        XML::Node password = doc.createNode("tt:Password");
        password.setTextContent(user.password);
        userNode.addChild(password);
      }

      XML::Node userLevel = doc.createNode("tt:UserLevel");
      userLevel.setTextContent(user.userLevel);
      userNode.addChild(userLevel);

      root.addChild(userNode);
      msg.setBodyNode(root);

      // Send request
      auto result = sendRequestVoid("device", "SetUser", msg.toString());
      if (!result) { return Result<bool>{false, result.error}; }

      return Result<bool>{true, true};
    } catch (const std::exception & e) { return Result<bool>{false, Error{Error::Type::Validation, e.what()}}; }
  }

  // Enhanced Media Methods
  Result<bool> Device::setVideoEncoderConfiguration(const std::string & token, const VideoConfig & config) {
    if (!connected) { return Result<bool>{false, Error{Error::Type::Transport, "Not connected"}}; }

    try {
      auto msg = createMessage("SetVideoEncoderConfiguration", SOAP::onvif::ServiceType::Media);

      // Use XML helpers to build the request
      XML::Document doc;
      XML::Node root = doc.createNode("trt:SetVideoEncoderConfiguration");

      // Create Configuration node
      XML::Node configNode = doc.createNode("trt:Configuration");
      configNode.setAttribute("token", token);

      // Add encoding settings
      XML::Node encoding = doc.createNode("tt:Encoding");
      encoding.setTextContent(config.encoding);
      configNode.addChild(encoding);

      XML::Node resolution = doc.createNode("tt:Resolution");
      XML::Node width = doc.createNode("tt:Width");
      width.setTextContent(std::to_string(config.width));
      XML::Node height = doc.createNode("tt:Height");
      height.setTextContent(std::to_string(config.height));
      resolution.addChild(width);
      resolution.addChild(height);
      configNode.addChild(resolution);

      XML::Node quality = doc.createNode("tt:Quality");
      quality.setTextContent(std::to_string(config.quality));
      configNode.addChild(quality);

      XML::Node rateControl = doc.createNode("tt:RateControl");
      XML::Node frameRate = doc.createNode("tt:FrameRateLimit");
      frameRate.setTextContent(std::to_string(config.fps));
      XML::Node bitrate = doc.createNode("tt:BitrateLimit");
      bitrate.setTextContent(std::to_string(config.bitrate));
      rateControl.addChild(frameRate);
      rateControl.addChild(bitrate);
      configNode.addChild(rateControl);

      root.addChild(configNode);

      // Add ForcePersistence flag
      XML::Node force = doc.createNode("trt:ForcePersistence");
      force.setTextContent("true");
      root.addChild(force);

      msg.setBodyNode(root);

      // Send request
      auto result = sendRequestVoid("media", "SetVideoEncoderConfiguration", msg.toString());
      if (!result) { return Result<bool>{false, result.error}; }

      return Result<bool>{true, true};
    } catch (const std::exception & e) { return Result<bool>{false, Error{Error::Type::Validation, e.what()}}; }
  }

  Result<bool> Device::setAudioEncoderConfiguration(const std::string & token, const AudioConfig & config) {
    if (!connected) { return Result<bool>{false, Error{Error::Type::Transport, "Not connected"}}; }

    try {
      auto msg = createMessage("SetAudioEncoderConfiguration", SOAP::onvif::ServiceType::Media);

      // Use XML helpers to build the request
      XML::Document doc;
      XML::Node root = doc.createNode("trt:SetAudioEncoderConfiguration");

      // Create Configuration node
      XML::Node configNode = doc.createNode("trt:Configuration");
      configNode.setAttribute("token", token);

      // Add encoding settings
      XML::Node encoding = doc.createNode("tt:Encoding");
      encoding.setTextContent(config.encoding);
      configNode.addChild(encoding);

      XML::Node bitrate = doc.createNode("tt:Bitrate");
      bitrate.setTextContent(std::to_string(config.bitrate));
      configNode.addChild(bitrate);

      XML::Node sampleRate = doc.createNode("tt:SampleRate");
      sampleRate.setTextContent(std::to_string(config.sampleRate));
      configNode.addChild(sampleRate);

      root.addChild(configNode);

      // Add ForcePersistence flag
      XML::Node force = doc.createNode("trt:ForcePersistence");
      force.setTextContent("true");
      root.addChild(force);

      msg.setBodyNode(root);

      // Send request
      auto result = sendRequestVoid("media", "SetAudioEncoderConfiguration", msg.toString());
      if (!result) { return Result<bool>{false, result.error}; }

      return Result<bool>{true, true};
    } catch (const std::exception & e) { return Result<bool>{false, Error{Error::Type::Validation, e.what()}}; }
  }

  Result<std::string> Device::getSnapshotUri(const std::string & profileToken) const {
    if (!connected) { return Result<std::string>{false, Error{Error::Type::Transport, "Not connected"}}; }

    try {
      // Build request using helper
      auto request = buildRequest("GetSnapshotUri", [&](XML::Document & doc, XML::Node & root) {
        auto tokenNode = doc.createNode("ProfileToken");
        tokenNode.setTextContent(profileToken);
        root.addChild(tokenNode);
      }, SOAP::onvif::ServiceType::Media);
      if (!request) return Result<std::string>{false, request.error};

      // Send request and get response
      std::string response;
      auto result = sendRequestAndGetResponse("media", "GetSnapshotUri", request.value, response);
      if (!result) return Result<std::string>{false, result.error};

      // Extract URI using response helper
      return extractResponse<std::string>(response, "GetSnapshotUriResponse", [](const XML::Document & doc) -> Result<std::string> {
        auto uriNode = doc.evaluateXPath("//*[local-name()='Uri']");
        if (!uriNode.isValid()) {
          return Result<std::string>{false, Error{Error::Type::Validation, "Failed to find Uri node"}};
        }
        return Result<std::string>{true, uriNode.getTextContent()};
      });
    } catch (const std::exception & e) { return Result<std::string>{false, Error{Error::Type::Validation, e.what()}}; }
  }

  // Enhanced PTZ Methods
  Result<bool> Device::setHomePosition(const std::string & profileToken) {
    if (!connected || !hasPTZService()) return Result<bool>(false);

    try {
      auto msg = createMessage("SetHomePosition", SOAP::onvif::ServiceType::PTZ);

      auto doc = msg.getDocument();
      auto root = doc.createNode("tptz:SetHomePosition");

      auto token = doc.createNode("tptz:ProfileToken");
      token.setTextContent(profileToken);
      root.addChild(token);

      msg.setBodyNode(root);

      // Send request
      std::string response;
      if (!sendRequest("ptz", "SetHomePosition", msg.toString(), response)) {
        return Result<bool>(false, Error(Error::Type::Transport, "Failed to send set home position request"));
      }

      return Result<bool>(true);
    } catch (const std::exception & e) {
      return Result<bool>(false, Error(Error::Type::Transport, std::string("Exception in setHomePosition: ") + e.what()));
    }
  }

  Result<std::vector<PTZPreset>> Device::getPresets(const std::string & profileToken) {
    if (!connected || !hasPTZService()) {
      return Result<std::vector<PTZPreset>>{false, Error{Error::Type::Transport, "Device not connected or no PTZ service"}};
    }

    try {
      auto msg = createMessage("GetPresets", SOAP::onvif::ServiceType::PTZ);

      // Use XML helpers to build the request
      XML::Document doc;
      XML::Node root = doc.createNode("tptz:GetPresets");
      XML::Node tokenNode = doc.createNode("tptz:ProfileToken");
      tokenNode.setTextContent(profileToken);
      root.addChild(tokenNode);
      msg.setBodyNode(root);

      std::string response;
      auto result = sendRequestAndGetResponse("ptz", "GetPresets", msg.toString(), response);
      if (!result) { return Result<std::vector<PTZPreset>>{false, result.error}; }

      return extractResponse<std::vector<PTZPreset>>(response, "GetPresetsResponse",
                                                     [](const XML::Document & doc) -> Result<std::vector<PTZPreset>> {
        try {
          std::vector<PTZPreset> presets;
          auto presetNodes = doc.evaluateXPathAll("//*[local-name()='Preset']");

          for (const auto & presetNode : presetNodes) {
            PTZPreset preset;
            preset.token = presetNode.getAttribute("token");
            preset.name = presetNode.getChild("Name").getTextContent();

            auto position = presetNode.getChild("PTZPosition");
            if (position.isValid()) {
              auto panTilt = position.getChild("PanTilt");
              if (panTilt.isValid()) {
                preset.pan = std::stof(panTilt.getAttribute("x"));
                preset.tilt = std::stof(panTilt.getAttribute("y"));
              }

              auto zoom = position.getChild("Zoom");
              if (zoom.isValid()) { preset.zoom = std::stof(zoom.getAttribute("x")); }
            }

            presets.push_back(preset);
          }

          return Result<std::vector<PTZPreset>>{true, presets};
        } catch (const std::exception & e) {
          return Result<std::vector<PTZPreset>>{false, Error{Error::Type::Validation, e.what()}};
        }
      });
    } catch (const std::runtime_error & e) {
      return Result<std::vector<PTZPreset>>{false, Error{Error::Type::Validation, e.what()}};
    }
  }

  Result<bool> Device::removePreset(const std::string & profileToken, const std::string & presetToken) {
    if (!connected || !hasPTZService()) return Result<bool>(false);

    try {
      auto msg = createMessage("RemovePreset", SOAP::onvif::ServiceType::PTZ);

      auto doc = msg.getDocument();
      auto root = doc.createNode("tptz:RemovePreset");

      auto token = doc.createNode("tptz:ProfileToken");
      token.setTextContent(profileToken);
      root.addChild(token);

      auto preset = doc.createNode("tptz:PresetToken");
      preset.setTextContent(presetToken);
      root.addChild(preset);

      msg.setBodyNode(root);

      // Send request
      std::string response;
      if (!sendRequest("ptz", "RemovePreset", msg.toString(), response)) {
        return Result<bool>(false, Error(Error::Type::Transport, "Failed to send remove preset request"));
      }

      return Result<bool>(true);
    } catch (const std::exception & e) {
      return Result<bool>(false, Error(Error::Type::Transport, std::string("Exception in removePreset: ") + e.what()));
    }
  }

  Result<bool> Device::setPTZConfiguration(const std::string & profileToken, const PTZConfiguration & config) {
    if (!connected || !hasPTZService()) return Result<bool>(false);

    try {
      auto msg = createMessage("SetConfiguration", SOAP::onvif::ServiceType::PTZ);

      // Add PTZ configuration
      auto doc = msg.getDocument();
      auto root = doc.createNode("tptz:PTZConfiguration");

      // Add required fields
      auto token = doc.createNode("tt:Token");
      token.setTextContent(config.token);
      root.addChild(token);

      auto name = doc.createNode("tt:Name");
      name.setTextContent(config.name);
      root.addChild(name);

      auto useCount = doc.createNode("tt:UseCount");
      useCount.setTextContent(std::to_string(config.useCount));
      root.addChild(useCount);

      auto nodeToken = doc.createNode("tt:NodeToken");
      nodeToken.setTextContent(config.nodeToken);
      root.addChild(nodeToken);

      // Add default speed
      auto defaultSpeed = doc.createNode("tt:DefaultPTZSpeed");

      if (config.defaultSpeed.x != 0 || config.defaultSpeed.y != 0) {
        auto panTilt = doc.createNode("tt:PanTilt");
        panTilt.setAttribute("x", std::to_string(config.defaultSpeed.x));
        panTilt.setAttribute("y", std::to_string(config.defaultSpeed.y));
        defaultSpeed.addChild(panTilt);
      }

      if (config.defaultSpeed.zoom != 0) {
        auto zoom = doc.createNode("tt:Zoom");
        zoom.setAttribute("x", std::to_string(config.defaultSpeed.zoom));
        defaultSpeed.addChild(zoom);
      }

      root.addChild(defaultSpeed);

      msg.setBodyNode(root);

      // Send request
      std::string response;
      if (!sendRequest("ptz", "SetConfiguration", msg.toString(), response)) {
        return Result<bool>(false, Error(Error::Type::Transport, "Failed to send set PTZ configuration request"));
      }

      return Result<bool>(true);
    } catch (const std::exception & e) {
      return Result<bool>(false, Error(Error::Type::Transport, std::string("Exception in setPTZConfiguration: ") + e.what()));
    }
  }

  // Preset Tour methods
  bool Device::startPresetTour(const std::string & profileToken, const std::string & tourToken) {
    if (!connected || !hasPTZService()) return false;
    try {
      auto msg = createMessage("OperatePresetTour", SOAP::onvif::ServiceType::PTZ);
      auto doc = msg.getDocument();
      auto root = doc.createNode("tptz:OperatePresetTour");
      auto token = doc.createNode("tptz:ProfileToken");
      token.setTextContent(profileToken);
      root.addChild(token);
      auto tour = doc.createNode("tptz:PresetTourToken");
      tour.setTextContent(tourToken);
      root.addChild(tour);
      auto op = doc.createNode("tptz:Operation");
      op.setTextContent("Start");
      root.addChild(op);
      msg.setBodyNode(root);
      std::string response;
      return sendRequest("ptz", "OperatePresetTour", msg.toString(), response);
    } catch (...) { return false; }
  }

  bool Device::stopPresetTour(const std::string & profileToken, const std::string & tourToken) {
    if (!connected || !hasPTZService()) return false;
    try {
      auto msg = createMessage("OperatePresetTour", SOAP::onvif::ServiceType::PTZ);
      auto doc = msg.getDocument();
      auto root = doc.createNode("tptz:OperatePresetTour");
      auto token = doc.createNode("tptz:ProfileToken");
      token.setTextContent(profileToken);
      root.addChild(token);
      auto tour = doc.createNode("tptz:PresetTourToken");
      tour.setTextContent(tourToken);
      root.addChild(tour);
      auto op = doc.createNode("tptz:Operation");
      op.setTextContent("Stop");
      root.addChild(op);
      msg.setBodyNode(root);
      std::string response;
      return sendRequest("ptz", "OperatePresetTour", msg.toString(), response);
    } catch (...) { return false; }
  }

  bool Device::createPresetTour(const std::string & profileToken) {
    if (!connected || !hasPTZService()) return false;
    try {
      auto msg = createMessage("CreatePresetTour", SOAP::onvif::ServiceType::PTZ);
      auto doc = msg.getDocument();
      auto root = doc.createNode("tptz:CreatePresetTour");
      auto token = doc.createNode("tptz:ProfileToken");
      token.setTextContent(profileToken);
      root.addChild(token);
      msg.setBodyNode(root);
      std::string response;
      return sendRequest("ptz", "CreatePresetTour", msg.toString(), response);
    } catch (...) { return false; }
  }

  bool Device::modifyPresetTour(const std::string & profileToken, const std::string & tourToken, const PresetTour & tour) {
    if (!connected || !hasPTZService()) return false;
    try {
      auto msg = createMessage("ModifyPresetTour", SOAP::onvif::ServiceType::PTZ);
      auto doc = msg.getDocument();
      auto root = doc.createNode("tptz:ModifyPresetTour");
      auto token = doc.createNode("tptz:ProfileToken");
      token.setTextContent(profileToken);
      root.addChild(token);

      auto tourNode = doc.createNode("tptz:PresetTour");
      tourNode.setAttribute("token", tourToken);

      auto nameNode = doc.createNode("tt:Name");
      nameNode.setTextContent(tour.name);
      tourNode.addChild(nameNode);

      auto autoStart = doc.createNode("tt:AutoStart");
      autoStart.setTextContent(tour.autoStart ? "true" : "false");
      tourNode.addChild(autoStart);

      // Add tour spots from the preset list
      for (const auto & preset : tour.presets) {
        auto spot = doc.createNode("tt:TourSpot");
        auto presetDetail = doc.createNode("tt:PresetDetail");
        auto presetToken = doc.createNode("tt:PresetToken");
        presetToken.setTextContent(preset.token);
        presetDetail.addChild(presetToken);
        spot.addChild(presetDetail);

        if (tour.stayTime > 0) {
          auto stay = doc.createNode("tt:StayTime");
          stay.setTextContent("PT" + std::to_string(tour.stayTime) + "S");
          spot.addChild(stay);
        }
        if (tour.speed > 0) {
          auto speedNode = doc.createNode("tt:Speed");
          auto panTilt = doc.createNode("tt:PanTilt");
          panTilt.setAttribute("x", std::to_string(tour.speed));
          panTilt.setAttribute("y", std::to_string(tour.speed));
          speedNode.addChild(panTilt);
          spot.addChild(speedNode);
        }
        tourNode.addChild(spot);
      }

      root.addChild(tourNode);
      msg.setBodyNode(root);
      std::string response;
      return sendRequest("ptz", "ModifyPresetTour", msg.toString(), response);
    } catch (...) { return false; }
  }

  bool Device::removePresetTour(const std::string & profileToken, const std::string & tourToken) {
    if (!connected || !hasPTZService()) return false;
    try {
      auto msg = createMessage("RemovePresetTour", SOAP::onvif::ServiceType::PTZ);
      auto doc = msg.getDocument();
      auto root = doc.createNode("tptz:RemovePresetTour");
      auto token = doc.createNode("tptz:ProfileToken");
      token.setTextContent(profileToken);
      root.addChild(token);
      auto tour = doc.createNode("tptz:PresetTourToken");
      tour.setTextContent(tourToken);
      root.addChild(tour);
      msg.setBodyNode(root);
      std::string response;
      return sendRequest("ptz", "RemovePresetTour", msg.toString(), response);
    } catch (...) { return false; }
  }

  Result<MediaProfile> Device::getMediaProfile(const std::string & token) const {
    if (!connected) { return Result<MediaProfile>{false, Error{Error::Type::Transport, "Not connected"}}; }

    // Build request using helper
    auto request = buildRequest("GetProfile", [&](XML::Document & doc, XML::Node & root) {
      root.setAttribute("xmlns", "http://www.onvif.org/ver10/media/wsdl");
      auto tokenNode = doc.createNode("ProfileToken");
      tokenNode.setTextContent(token);
      root.addChild(tokenNode);
    }, SOAP::onvif::ServiceType::Media);
    if (!request) return Result<MediaProfile>{false, request.error};

    // Send request and get response
    std::string response;
    auto result = sendRequestAndGetResponse("media", "GetProfile", request.value, response);
    if (!result) return Result<MediaProfile>{false, result.error};

    // Extract profile using our response helper
    return extractResponse<MediaProfile>(response, "GetProfileResponse", [](const XML::Document & doc) -> Result<MediaProfile> {
      MediaProfile profile;

      // Get profile node
      auto profileNode = doc.evaluateXPath("//*[local-name()='Profile']");
      if (!profileNode.isValid()) {
        return Result<MediaProfile>{false, Error{Error::Type::Validation, "Failed to find profile"}};
      }

      // Get profile token and name
      auto token = profileNode.getAttribute("token");
      if (!token.empty()) profile.token = token;

      auto name = profileNode.getChild("Name");
      if (name.isValid()) profile.name = name.getTextContent();

      // Get video configuration
      auto videoNode = profileNode.getChild("VideoEncoderConfiguration");
      if (videoNode.isValid()) {
        auto width = videoNode.getChild("Resolution").getChild("Width");
        auto height = videoNode.getChild("Resolution").getChild("Height");
        auto fps = videoNode.getChild("RateControl").getChild("FrameRateLimit");
        auto bitrate = videoNode.getChild("RateControl").getChild("BitrateLimit");
        auto quality = videoNode.getChild("Quality");
        auto encoding = videoNode.getChild("Encoding");

        if (width.isValid()) profile.video.width = std::stoi(width.getTextContent());
        if (height.isValid()) profile.video.height = std::stoi(height.getTextContent());
        if (fps.isValid()) profile.video.fps = std::stoi(fps.getTextContent());
        if (bitrate.isValid()) profile.video.bitrate = std::stoi(bitrate.getTextContent());
        if (quality.isValid()) profile.video.quality = std::stoi(quality.getTextContent());
        if (encoding.isValid()) profile.video.encoding = encoding.getTextContent();
      }

      // Get audio configuration
      auto audioNode = profileNode.getChild("AudioEncoderConfiguration");
      if (audioNode.isValid()) {
        auto bitrate = audioNode.getChild("Bitrate");
        auto sampleRate = audioNode.getChild("SampleRate");
        auto encoding = audioNode.getChild("Encoding");

        if (bitrate.isValid()) profile.audio.bitrate = std::stoi(bitrate.getTextContent());
        if (sampleRate.isValid()) profile.audio.sampleRate = std::stoi(sampleRate.getTextContent());
        if (encoding.isValid()) profile.audio.encoding = encoding.getTextContent();
      }

      return Result<MediaProfile>{true, std::move(profile)};
    });
  }

  Result<bool> Device::createMediaProfile(const MediaProfile & profile) {
    if (!connected) { return Result<bool>{false, Error{Error::Type::Transport, "Not connected"}}; }

    try {
      auto msg = createMessage("CreateProfile", SOAP::onvif::ServiceType::Media);

      XML::Document doc;
      XML::Node root = doc.createNode("trt:CreateProfile");

      XML::Node name = doc.createNode("trt:Name");
      name.setTextContent(profile.name);
      root.addChild(name);

      XML::Node token = doc.createNode("trt:Token");
      token.setTextContent(profile.token);
      root.addChild(token);

      msg.setBodyNode(root);

      auto result = sendRequestVoid("media", "CreateProfile", msg.toString());
      if (!result) { return Result<bool>{false, result.error}; }

      return Result<bool>{true, true};
    } catch (const std::exception & e) { return Result<bool>{false, Error{Error::Type::Validation, e.what()}}; }
  }

  Result<bool> Device::deleteMediaProfile(const std::string & token) {
    if (!connected) { return Result<bool>{false, Error{Error::Type::Transport, "Not connected"}}; }

    try {
      auto msg = createMessage("DeleteProfile", SOAP::onvif::ServiceType::Media);

      XML::Document doc;
      XML::Node root = doc.createNode("trt:DeleteProfile");

      XML::Node profileToken = doc.createNode("ProfileToken");
      profileToken.setTextContent(token);
      root.addChild(profileToken);

      msg.setBodyNode(root);

      auto result = sendRequestVoid("media", "DeleteProfile", msg.toString());
      if (!result) { return Result<bool>{false, result.error}; }

      return Result<bool>{true, true};
    } catch (const std::exception & e) { return Result<bool>{false, Error{Error::Type::Validation, e.what()}}; }
  }

  Result<bool> Device::modifyMediaProfile(const MediaProfile & profile) {
    if (!connected) { return Result<bool>{false, Error{Error::Type::Transport, "Not connected"}}; }

    try {
      auto msg = createMessage("ModifyProfile", SOAP::onvif::ServiceType::Media);

      XML::Document doc;
      XML::Node root = doc.createNode("trt:ModifyProfile");

      XML::Node profileToken = doc.createNode("ProfileToken");
      profileToken.setTextContent(profile.token);
      root.addChild(profileToken);

      XML::Node name = doc.createNode("trt:Name");
      name.setTextContent(profile.name);
      root.addChild(name);

      msg.setBodyNode(root);

      auto result = sendRequestVoid("media", "ModifyProfile", msg.toString());
      if (!result) { return Result<bool>{false, result.error}; }

      return Result<bool>{true, true};
    } catch (const std::exception & e) { return Result<bool>{false, Error{Error::Type::Validation, e.what()}}; }
  }

  Result<StreamingCapabilities> Device::getStreamingCapabilities() const {
    try {
      VERYHIGH_MSG("ONVIF: Getting streaming capabilities for %s:%d", host.c_str(), port);

      // First get device capabilities to find media service endpoint
      auto deviceCaps = getDeviceCapabilities();
      if (!deviceCaps) {
        WARN_MSG("ONVIF: Failed to get device capabilities - %s", deviceCaps.error.message.c_str());
        return Result<StreamingCapabilities>{false, deviceCaps.error};
      }

      // Find media service endpoint
      auto mediaXAddr = deviceCaps.value.mediaXAddr;
      if (mediaXAddr.empty()) {
        WARN_MSG("ONVIF: No media service endpoint found");
        return Result<StreamingCapabilities>{false, Error{Error::Type::Validation, "No media service endpoint"}};
      }
      INSANE_MSG("ONVIF: Using media service endpoint: %s", mediaXAddr.c_str());

      // Get media profiles first
      auto request = buildRequest("GetProfiles", [](XML::Document & doc, XML::Node & root) {
        root.setAttribute("xmlns", "http://www.onvif.org/ver10/media/wsdl");
      }, SOAP::onvif::ServiceType::Media);
      if (!request) {
        WARN_MSG("ONVIF: Failed to build profiles request - %s", request.error.message.c_str());
        return Result<StreamingCapabilities>{false, request.error};
      }

      // Send request to media service
      std::string response;
      auto result = sendRequestAndGetResponse("media", "GetProfiles", request.value, response);
      if (!result) {
        WARN_MSG("ONVIF: Failed to get profiles - %s", result.error.message.c_str());
        return Result<StreamingCapabilities>{false, result.error};
      }

      // Log raw response for debugging
      INSANE_MSG("ONVIF: Parsing media profiles response:");
      std::istringstream responseStream(response);
      std::string line;
      while (std::getline(responseStream, line)) { INSANE_MSG("ONVIF: | %s", line.c_str()); }

      // Extract profiles using response helper
      return extractResponse<StreamingCapabilities>(response, "GetProfilesResponse",
                                                    [](const XML::Document & doc) -> Result<StreamingCapabilities> {
        try {
          StreamingCapabilities caps;

          // Find profiles
          auto profiles = doc.evaluateXPathAll("//*[local-name()='Profiles']");
          if (profiles.empty()) {
            WARN_MSG("ONVIF: No profiles found in response");
            return Result<StreamingCapabilities>{true, caps}; // Return empty caps
          }

          INSANE_MSG("ONVIF: Found %zu profiles", profiles.size());

          // For each profile, get streaming capabilities
          for (const auto & profile : profiles) {
            auto token = profile.getAttribute("token");
            if (!token.empty()) {
              INSANE_MSG("ONVIF: Processing profile token: %s", token.c_str());

              // Check for streaming capabilities in profile
              auto streamingNode = profile.getChild("StreamingCapabilities");
              if (!streamingNode.isValid()) {
                streamingNode = doc.evaluateXPath("//*[local-name()='StreamingCapabilities']");
              }

              if (streamingNode.isValid()) {
                // Extract capabilities
                auto rtpMulticast = streamingNode.getChild("RTPMulticast");
                if (rtpMulticast.isValid()) {
                  caps.RTPMulticast = (rtpMulticast.getTextContent() == "true");
                  INSANE_MSG("ONVIF: RTP support - Multicast:%s TCP:%s RTSP:%s", caps.RTPMulticast ? "yes" : "no",
                             caps.RTP_TCP ? "yes" : "no", caps.RTP_RTSP_TCP ? "yes" : "no");
                }

                auto rtpTcp = streamingNode.getChild("RTP_TCP");
                if (rtpTcp.isValid()) {
                  caps.RTP_TCP = (rtpTcp.getTextContent() == "true");
                  INSANE_MSG("ONVIF: RTP support - Multicast:%s TCP:%s RTSP:%s", caps.RTPMulticast ? "yes" : "no",
                             caps.RTP_TCP ? "yes" : "no", caps.RTP_RTSP_TCP ? "yes" : "no");
                }

                auto rtpRtspTcp = streamingNode.getChild("RTP_RTSP_TCP");
                if (rtpRtspTcp.isValid()) {
                  caps.RTP_RTSP_TCP = (rtpRtspTcp.getTextContent() == "true");
                  INSANE_MSG("ONVIF: RTP support - Multicast:%s TCP:%s RTSP:%s", caps.RTPMulticast ? "yes" : "no",
                             caps.RTP_TCP ? "yes" : "no", caps.RTP_RTSP_TCP ? "yes" : "no");
                }
              }

              // Add default streaming profiles if none found
              if (caps.RTPMulticast) caps.streamingProfiles.push_back("RTP-Multicast");
              if (caps.RTP_TCP) caps.streamingProfiles.push_back("RTP-Unicast");
              if (caps.RTP_RTSP_TCP) caps.streamingProfiles.push_back("RTP-RTSP-TCP");

              // Add default transport protocols
              if (std::find(caps.transportProtocols.begin(), caps.transportProtocols.end(), "UDP") ==
                  caps.transportProtocols.end()) {
                caps.transportProtocols.push_back("UDP");
              }
              if ((caps.RTP_TCP || caps.RTP_RTSP_TCP) &&
                  std::find(caps.transportProtocols.begin(), caps.transportProtocols.end(), "TCP") ==
                    caps.transportProtocols.end()) {
                caps.transportProtocols.push_back("TCP");
                caps.transportProtocols.push_back("RTSP");
              }
            }
          }

          // Log capabilities summary
          HIGH_MSG("ONVIF: Streaming summary - %zu profiles, %zu protocols, Multicast:%s TCP:%s", caps.streamingProfiles.size(),
                   caps.transportProtocols.size(), caps.RTPMulticast ? "yes" : "no", caps.RTP_TCP ? "yes" : "no");

          VERYHIGH_MSG("ONVIF: Found %zu streaming profiles, %zu transport protocols", caps.streamingProfiles.size(),
                       caps.transportProtocols.size());

          return Result<StreamingCapabilities>{true, std::move(caps)};
        } catch (const std::exception & e) {
          return Result<StreamingCapabilities>{
            false, Error{Error::Type::Validation, std::string("Failed to parse streaming capabilities: ") + e.what()}};
        }
      });
    } catch (const std::runtime_error & e) {
      return Result<StreamingCapabilities>{false, Error{Error::Type::Validation, e.what()}};
    }
  }

  Result<std::vector<TransportDetails>> Device::getTransportDetails(const std::string & profileToken) const {
    try {
      // Build request using helper
      auto request = buildRequest("GetTransportDetails", [&](XML::Document & doc, XML::Node & root) {
        root.setAttribute("xmlns", "http://www.onvif.org/ver10/media/wsdl");
        auto tokenNode = doc.createNode("ProfileToken");
        tokenNode.setTextContent(profileToken);
        root.addChild(tokenNode);
      }, SOAP::onvif::ServiceType::Media);
      if (!request) return Result<std::vector<TransportDetails>>{false, request.error};

      // Send request and get response
      std::string response;
      auto result = sendRequestAndGetResponse("media", "GetTransportDetails", request.value, response);
      if (!result) return Result<std::vector<TransportDetails>>{false, result.error};

      // Extract transport details using response helper
      return extractResponse<std::vector<TransportDetails>>(
        response, "GetTransportDetailsResponse", [](const XML::Document & doc) -> Result<std::vector<TransportDetails>> {
        try {
          std::vector<TransportDetails> details;
          auto transportNodes = doc.evaluateXPathAll("//*[local-name()='Transport']");

          for (const auto & transportNode : transportNodes) {
            TransportDetails detail;
            detail.protocol = transportNode.getChild("Protocol").getTextContent();
            detail.port = std::stoi(transportNode.getChild("Port").getTextContent());
            detail.path = transportNode.getChild("Path").getTextContent();

            // Get options if any
            auto optionsNode = transportNode.getChild("Options");
            if (optionsNode.isValid()) {
              auto options = optionsNode.getChildren();
              for (const auto & option : options) { detail.options[option.getName()] = option.getTextContent(); }
            }

            details.push_back(detail);
          }

          return Result<std::vector<TransportDetails>>{true, details};
        } catch (const std::exception & e) {
          return Result<std::vector<TransportDetails>>{false, Error{Error::Type::Validation, e.what()}};
        }
      });
    } catch (const std::runtime_error & e) {
      return Result<std::vector<TransportDetails>>{false, Error{Error::Type::Validation, e.what()}};
    }
  }

  Result<std::string> Device::getStreamUri(const std::string & profileToken, const std::string & protocol,
                                           const std::string & transport) const {
    try {
      if (!connected) { return Result<std::string>{false, Error{Error::Type::Transport, "Not connected"}}; }

      if (profileToken.empty()) {
        return Result<std::string>{false, Error{Error::Type::Validation, "Profile token cannot be empty"}};
      }

      // Build request using helper
      auto request = buildRequest("GetStreamUri", [&](XML::Document & doc, XML::Node & root) {
        // Add required namespaces
        root.setAttribute("xmlns", "http://www.onvif.org/ver10/media/wsdl");
        root.setAttribute("xmlns:tt", "http://www.onvif.org/ver10/schema");

        // Add stream setup with correct namespaces
        auto setupNode = doc.createNode("StreamSetup");

        // Add stream type with tt: namespace
        auto streamNode = doc.createNode("tt:Stream");
        streamNode.setTextContent(protocol);
        setupNode.addChild(streamNode);

        // Add transport with tt: namespace
        auto transportNode = doc.createNode("tt:Transport");
        auto protocolNode = doc.createNode("tt:Protocol");
        protocolNode.setTextContent(transport);
        transportNode.addChild(protocolNode);
        setupNode.addChild(transportNode);
        root.addChild(setupNode);

        // Add profile token with correct namespace
        auto tokenNode = doc.createNode("ProfileToken");
        tokenNode.setTextContent(profileToken);
        root.addChild(tokenNode);
      }, SOAP::onvif::ServiceType::Media);
      if (!request) {
        WARN_MSG("ONVIF: Failed to build GetStreamUri request - %s", request.error.message.c_str());
        return Result<std::string>(false, request.error);
      }

      INSANE_MSG("ONVIF: Getting stream URI for profile %s", profileToken.c_str());
      INSANE_MSG("ONVIF: Stream URI request details - Protocol:%s Transport:%s", protocol.c_str(), transport.c_str());

      // Send request and get response
      std::string response;
      auto result = sendRequestAndGetResponse("media", "GetStreamUri", request.value, response);
      if (!result) {
        WARN_MSG("ONVIF: Failed to get stream URI - %s", result.error.message.c_str());
        return Result<std::string>(false, result.error);
      }

      // Parse response
      return extractResponse<std::string>(response, "GetStreamUriResponse", [](const XML::Document & doc) -> Result<std::string> {
        auto uriNode = doc.evaluateXPath("//*[local-name()='Uri']");
        if (!uriNode.isValid()) {
          WARN_MSG("ONVIF: Missing Uri in GetStreamUri response");
          return Result<std::string>(false, Error(Error::Type::Validation, "Missing Uri in response"));
        }
        std::string uri = uriNode.getTextContent();
        INSANE_MSG("ONVIF: Got stream URI: %s", uri.c_str());
        return Result<std::string>(true, uri);
      });
    } catch (const std::exception & e) {
      WARN_MSG("ONVIF: Exception in getStreamUri: %s", e.what());
      return Result<std::string>(false, Error(Error::Type::Transport, e.what()));
    }
  }

  // Discovery Implementation
  Discovery::Discovery() : asyncRunning(false), asyncStartTime(0), asyncTimeoutMs(0) {
    // Socket will be created when needed
  }

  Discovery::~Discovery() {
    stopAsyncDiscovery();
  }

  bool Discovery::sendCommand(const std::string & deviceId, const ::Device::PTZCommand & cmd) {
    // Parse device ID to get host and port
    size_t colonPos = deviceId.find(':');
    if (colonPos == std::string::npos) { return false; }

    std::string host = deviceId.substr(0, colonPos);
    uint16_t port;
    try {
      port = std::stoi(deviceId.substr(colonPos + 1));
    } catch (const std::exception &) { return false; }

    // Create temporary device and send command
    Device device(host, port);
    if (!device.connect()) { return false; }

    bool result = device.sendPTZ(cmd);
    device.disconnect();
    return result;
  }

  std::unique_ptr<::Device::Base> Discovery::createDevice(const ::Device::DeviceInfo & info) const {
    std::string host = info.host;
    uint16_t port = 80;
    std::string user, pass;
    auto protoIt = info.protocols.find("onvif");
    if (protoIt != info.protocols.end()) {
      if (!protoIt->second.address.empty()) host = protoIt->second.address;
      if (protoIt->second.port) port = protoIt->second.port;
      user = protoIt->second.username;
      pass = protoIt->second.password;
    }
    auto dev = std::unique_ptr<Device>(new Device(host, port));
    if (!user.empty() && !pass.empty()) dev->setCredentials(user, pass);
    return dev;
  }

  bool Discovery::parseProbeMatch(const std::string & response, ::Device::DeviceInfo & info) {
    try {
      // First check if we have a complete XML document
      if (response.find("<?xml") == std::string::npos && response.find("<SOAP-ENV") == std::string::npos &&
          response.find("<soap") == std::string::npos) {
        WARN_MSG("ONVIF: Response does not appear to be a valid XML document");
        return false;
      }

      XML::Document doc(response);
      XML::Node root = doc.getRoot();
      if (!root.isValid()) {
        WARN_MSG("ONVIF: Invalid XML root node");
        return false;
      }

      // Register all namespaces from the document
      std::map<std::string, std::string> namespaces = doc.getNamespaces();
      for (const auto & ns : namespaces) {
        doc.registerNamespace(ns.first, ns.second);
        // If we find wsdd namespace, alias it as wsd for compatibility
        if (ns.second.find("/discovery") != std::string::npos) { doc.registerNamespace("wsd", ns.second); }
      }

      // Use local-name() for all XPath queries to be namespace-independent
      XML::Node probeMatch = doc.evaluateXPath("//*[local-name()='ProbeMatch']");
      if (!probeMatch.isValid()) {
        WARN_MSG("ONVIF: No ProbeMatch found in response");
        return false;
      }

      // Extract fields using XPath with local-name()
      std::string xaddrs = doc.evaluateXPathString("string(//*[local-name()='ProbeMatch']/*[local-name()='XAddrs'])");
      std::string types = doc.evaluateXPathString("string(//*[local-name()='ProbeMatch']/*[local-name()='Types'])");
      std::string scopes = doc.evaluateXPathString("string(//*[local-name()='ProbeMatch']/*[local-name()='Scopes'])");

      // Trim whitespace from extracted values
      xaddrs = trim(xaddrs);
      types = trim(types);
      scopes = trim(scopes);

      // Parse XAddrs to get device URL
      if (!xaddrs.empty()) {
        std::deque<std::string> addresses;
        Util::splitString(xaddrs, ' ', addresses);
        if (!addresses.empty()) {
          // Use first address
          std::string url = addresses.front();

          // Parse URL to get host and port
          size_t schemeEnd = url.find("://");
          if (schemeEnd != std::string::npos) {
            size_t hostStart = schemeEnd + 3;
            size_t hostEnd = url.find(':', hostStart);
            if (hostEnd == std::string::npos) { hostEnd = url.find('/', hostStart); }
            if (hostEnd == std::string::npos) { hostEnd = url.length(); }

            std::string host = url.substr(hostStart, hostEnd - hostStart);
            uint16_t port = 80;

            size_t portStart = url.find(':', hostStart);
            if (portStart != std::string::npos && portStart < url.find('/', hostStart)) {
              port = std::stoi(url.substr(portStart + 1));
            }

            // Create protocol config
            ::Device::ProtocolConfig onvifConfig;
            onvifConfig.type = "onvif";
            onvifConfig.address = host;
            onvifConfig.port = port;

            // Set basic info
            info.name = host;
            info.host = host;
            info.id = host + ":" + std::to_string(port);
            info.protocols["onvif"] = onvifConfig;

            return true;
          }
        }
      }

      WARN_MSG("ONVIF: Failed to parse device URL from XAddrs: %s", xaddrs.c_str());
      return false;
    } catch (const std::exception & e) {
      WARN_MSG("ONVIF: Failed to parse probe match: %s", e.what());
      return false;
    }
  }

  void Discovery::logProbeMatch(const std::string & response) {
    try {
      XML::Document doc(response);
      XML::Node probeMatch = doc.evaluateXPath("//*[local-name()='ProbeMatch']");
      if (!probeMatch.isValid()) {
        WARN_MSG("ONVIF: No ProbeMatch found in response");
        return;
      }

      // Extract fields using local-name() for namespace independence
      std::string types = doc.evaluateXPathString("string(//*[local-name()='ProbeMatch']/*[local-name()='Types'])");
      std::string scopes = doc.evaluateXPathString("string(//*[local-name()='ProbeMatch']/*[local-name()='Scopes'])");
      std::string xaddrs = doc.evaluateXPathString("string(//*[local-name()='ProbeMatch']/*[local-name()='XAddrs'])");
      std::string version =
        doc.evaluateXPathString("string(//*[local-name()='ProbeMatch']/*[local-name()='MetadataVersion'])");

      INFO_MSG("ONVIF: Found device:");
      if (!types.empty()) { VERYHIGH_MSG("  Types: %s", types.c_str()); }
      if (!scopes.empty()) { VERYHIGH_MSG("  Scopes: %s", scopes.c_str()); }
      if (!xaddrs.empty()) { VERYHIGH_MSG("  XAddrs: %s", xaddrs.c_str()); }
      if (!version.empty()) { VERYHIGH_MSG("  Version: %s", version.c_str()); }
    } catch (const std::exception & e) { WARN_MSG("ONVIF: Failed to parse probe match: %s", e.what()); }
  }

  void Discovery::sendProbeResponse(const std::string & messageId, const std::string & relatesTo,
                                    const std::string & endpointRef, const std::string & types,
                                    const std::string & scopes, const std::string & xaddrs, uint32_t metadataVersion) {
    try {
      // Create message factory
      SOAP::onvif::ONVIFMessageFactory factory(SOAP::Version::SOAP_1_2);

      // Create WS-Discovery probe match message using factory
      auto msg = factory.createMessage(SOAP::onvif::ServiceType::Discovery, "http://schemas.xmlsoap.org/ws/2005/04/discovery/ProbeMatches",
                                       "http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous");

      // Add RelatesTo header
      msg.addHeader("wsa:RelatesTo", relatesTo, true);

      // Create probe match body
      XML::Document doc;
      XML::Node root = doc.createNode("wsd:ProbeMatches");
      XML::Node match = doc.createNode("wsd:ProbeMatch");

      // Add endpoint reference
      XML::Node endpoint = doc.createNode("wsa:EndpointReference");
      XML::Node address = doc.createNode("wsa:Address");
      address.setTextContent(endpointRef);
      endpoint.addChild(address);
      match.addChild(endpoint);

      // Add types
      XML::Node typesNode = doc.createNode("wsd:Types");
      typesNode.setTextContent(types);
      match.addChild(typesNode);

      // Add scopes
      XML::Node scopesNode = doc.createNode("wsd:Scopes");
      scopesNode.setValue(scopes);
      match.addChild(scopesNode);

      // Add XAddrs
      XML::Node xaddrsNode = doc.createNode("wsd:XAddrs");
      xaddrsNode.setValue(xaddrs);
      match.addChild(xaddrsNode);

      // Add metadata version
      XML::Node versionNode = doc.createNode("wsd:MetadataVersion");
      versionNode.setValue(std::to_string(metadataVersion));
      match.addChild(versionNode);

      root.addChild(match);
      msg.setBodyNode(root);

      // Send the response
      std::string response = msg.toString();
      socket->SendNow(response);

      VERYHIGH_MSG("ONVIF: Sent probe match response:\n%s", response.c_str());
    } catch (const std::exception & e) { WARN_MSG("ONVIF: Failed to send probe match response: %s", e.what()); }
  }

  bool Device::sendPTZ(const ::Device::PTZCommand & cmd) {
    INFO_MSG("ONVIF PTZ: Dispatching action %d to %s:%d", static_cast<int>(cmd.action), host.c_str(), port);

    if (defaultProfileToken.empty()) {
      auto profiles = getMediaProfiles();
      if (!profiles) {
        ERROR_MSG("ONVIF: Failed to get profiles for PTZ command");
        return false;
      }
      if (profiles.value.empty()) {
        ERROR_MSG("ONVIF: No profiles available for PTZ command");
        return false;
      }
      defaultProfileToken = profiles.value[0].token;
      HIGH_MSG("ONVIF PTZ: Using profile token '%s'", defaultProfileToken.c_str());
    }

    try {
      switch (cmd.action) {
        case ::Device::PTZAction::PanTilt:
          if (cmd.args.find("pan") != cmd.args.end() && cmd.args.find("tilt") != cmd.args.end()) {
            return continuousMove(defaultProfileToken, cmd.args.at("pan"), cmd.args.at("tilt"), 0.0f);
          }
          break;
        case ::Device::PTZAction::Zoom:
          if (cmd.args.find("zoom") != cmd.args.end()) {
            return continuousMove(defaultProfileToken, 0.0f, 0.0f, cmd.args.at("zoom"));
          }
          break;
        case ::Device::PTZAction::Stop: return stop(defaultProfileToken);
        case ::Device::PTZAction::Home: return gotoHomePosition(defaultProfileToken);
        case ::Device::PTZAction::Preset:
          if (cmd.args.find("preset") != cmd.args.end()) {
            int presetId = static_cast<int>(cmd.args.at("preset"));
            std::string presetToken = std::to_string(presetId);
            auto it = cmd.args.find("store");
            if (it != cmd.args.end() && it->second > 0.0f) {
              return setPreset(defaultProfileToken, presetToken, "Preset " + presetToken);
            } else {
              return gotoPreset(defaultProfileToken, presetToken);
            }
          }
          break;
        case ::Device::PTZAction::Focus: {
          std::string vsToken = getVideoSourceToken();
          if (vsToken.empty()) {
            ERROR_MSG("ONVIF: No video source token for focus control");
            return false;
          }
          float mode = cmd.args.count("mode") ? cmd.args.at("mode") : 0;
          if (mode == 1.0f) {
            ImagingSettings settings;
            settings.focus.mode = "AUTO";
            return setImagingSettings(vsToken, settings);
          }
          float value = cmd.args.count("value") ? cmd.args.at("value") : 0;
          return focusMove(vsToken, value / 100.0f);
        }
        case ::Device::PTZAction::Iris: {
          std::string vsToken = getVideoSourceToken();
          if (vsToken.empty()) {
            ERROR_MSG("ONVIF: No video source token for iris control");
            return false;
          }
          ImagingSettings settings;
          settings.exposure.mode = "MANUAL";
          settings.exposure.iris = cmd.args.count("value") ? cmd.args.at("value") : 0;
          return setImagingSettings(vsToken, settings);
        }
        case ::Device::PTZAction::WhiteBalance: {
          std::string vsToken = getVideoSourceToken();
          if (vsToken.empty()) {
            ERROR_MSG("ONVIF: No video source token for white balance control");
            return false;
          }
          ImagingSettings settings;
          float mode = cmd.args.count("mode") ? cmd.args.at("mode") : 0;
          settings.whiteBalance.mode = (mode == 1.0f) ? "AUTO" : "MANUAL";
          if (cmd.args.count("color_temp")) { settings.whiteBalance.crGain = cmd.args.at("color_temp"); }
          return setImagingSettings(vsToken, settings);
        }
        default: ERROR_MSG("ONVIF: Unsupported PTZ command: %d", static_cast<int>(cmd.action)); return false;
      }
      ERROR_MSG("ONVIF: Missing required PTZ arguments");
      return false;
    } catch (const std::exception & e) {
      ERROR_MSG("ONVIF: PTZ command failed: %s", e.what());
      return false;
    }
  }

  Result<XML::Document> Device::parseResponse(const HTTP::Parser & resp, bool allowEmptyBody) const {
    if (resp.body.empty() && !allowEmptyBody) {
      return Result<XML::Document>(false, Error(Error::Type::Transport, "Empty response body"));
    }

    try {
      XML::Document doc(resp.body);
      return Result<XML::Document>(true, std::move(doc));
    } catch (const std::exception & e) {
      return Result<XML::Document>(false, Error(Error::Type::Validation, std::string("Failed to parse XML response: ") + e.what()));
    }
  }

  Result<XML::Node> Device::findNode(const XML::Document & doc, const std::string & localName) const {
    try {
      // Use XPath to find node by local-name(), ignoring namespace
      std::string xpath = "//*[local-name()='" + localName + "']";
      auto nodes = doc.evaluateXPathAll(xpath);

      if (nodes.empty()) {
        return Result<XML::Node>(false, Error(Error::Type::Validation, "Node with local name '" + localName + "' not found"));
      }

      // Return first matching node
      return Result<XML::Node>(true, nodes[0]);
    } catch (const std::exception & e) {
      return Result<XML::Node>(
        false, Error(Error::Type::Validation, std::string("Failed to find node '") + localName + "': " + e.what()));
    }
  }

  Result<std::string> Device::getNodeText(const XML::Node & node, const std::string & localName) const {
    try {
      // First try direct child
      XML::Node child = node.getChild(localName);
      if (child.isValid()) { return Result<std::string>(true, child.getTextContent()); }

      // If no direct child found, return error
      return Result<std::string>(false, Error(Error::Type::Validation, "Node with local name '" + localName + "' not found"));
    } catch (const std::exception & e) {
      return Result<std::string>(
        false, Error(Error::Type::Validation, std::string("Failed to get text for '") + localName + "': " + e.what()));
    }
  }

  Result<SOAP::Fault> Device::extractSOAPFault(const XML::Document & doc) const {
    try {
      auto faultNode = findNode(doc, "Fault");
      if (!faultNode) { return Result<SOAP::Fault>(false, Error(Error::Type::Validation, "No SOAP fault found")); }

      SOAP::Fault fault;

      // Try SOAP 1.2 format first
      auto codeNode = findNode(doc, "Code");
      if (codeNode) {
        auto valueNode = findNode(doc, "Value");
        if (valueNode) { fault.code = valueNode.value.getTextContent(); }

        auto reasonNode = findNode(doc, "Reason");
        if (reasonNode) {
          auto textNode = findNode(doc, "Text");
          if (textNode) { fault.reason = textNode.value.getTextContent(); }
        }
      } else {
        // Try SOAP 1.1 format
        auto faultCodeNode = findNode(doc, "faultcode");
        if (faultCodeNode) { fault.code = faultCodeNode.value.getTextContent(); }

        auto faultStringNode = findNode(doc, "faultstring");
        if (faultStringNode) { fault.reason = faultStringNode.value.getTextContent(); }
      }

      auto detailNode = findNode(doc, "detail");
      if (detailNode) { fault.detail = detailNode.value.getTextContent(); }

      return Result<SOAP::Fault>(true, std::move(fault));
    } catch (const std::exception & e) {
      return Result<SOAP::Fault>(false, Error(Error::Type::Validation, std::string("Failed to extract SOAP fault: ") + e.what()));
    }
  }

  Result<XML::Document> Device::handleResponse(const HTTP::Parser & resp, bool allowEmptyBody) const {
    // Get status code from response URL (which contains the status code in HTTP::Parser)
    int status = atoi(resp.url.c_str());

    // Log response details
    HIGH_MSG("ONVIF: Received response - Status: %d %s", status, resp.method.c_str());

    // Log headers we care about
    std::vector<std::string> respHeaderNames = {"Content-Type", "Content-Length", "Connection", "WWW-Authenticate", "Transfer-Encoding"};
    for (const std::string & headerName : respHeaderNames) {
      const std::string & headerVal = resp.GetHeader(headerName);
      if (!headerVal.empty()) { HIGH_MSG("ONVIF: Response header: %s: %s", headerName.c_str(), headerVal.c_str()); }
    }

    if (status < 200 || status >= 300) {
      if (status == 401) {
        return Result<XML::Document>(
          false, Error(Error::Type::Authentication, "Authentication required. Please set credentials using setCredentials()"));
      }

      // For 400 errors, try to extract SOAP fault
      if (status == 400) {
        auto doc = parseResponse(resp, true);
        if (doc) {
          auto fault = extractSOAPFault(doc.value);
          if (fault) {
            return Result<XML::Document>(false,
                                         Error(Error::Type::SOAPFault,
                                               "SOAP Fault: " + fault.value.reason + " (Code: " + fault.value.code + ")",
                                               fault.value));
          }
        }
        return Result<XML::Document>(
          false, Error(Error::Type::Transport, "HTTP error 400: Bad Request - Check WS-Addressing headers and SOAP version"));
      }

      return Result<XML::Document>(
        false, Error(Error::Type::Transport, "HTTP error " + std::to_string(status) + ": " + resp.method));
    }

    // Verify Content-Type
    std::string contentType = resp.GetHeader("Content-Type");
    if (contentType.find("application/soap+xml") == std::string::npos) {
      WARN_MSG("ONVIF: Warning - Unexpected Content-Type in response: %s", contentType.c_str());
    }

    // Parse response and check for SOAP fault
    auto doc = parseResponse(resp, allowEmptyBody);
    if (!doc) return doc;

    auto fault = extractSOAPFault(doc.value);
    if (fault) {
      return Result<XML::Document>(
        false,
        Error(Error::Type::SOAPFault, "SOAP Fault: " + fault.value.reason + " (Code: " + fault.value.code + ")", fault.value));
    }

    return doc;
  }

  ::Device::AnalyticsCapabilities Device::queryAnalytics(const std::string & profileToken) const {
    ::Device::AnalyticsCapabilities analytics;

    if (!connected || services.find("analytics") == services.end() || services.at("analytics").empty()) {
      return analytics;
    }

    analytics.hasAnalytics = true;
    analytics.analyticsServiceUrl = services.at("analytics");
    HIGH_MSG("ONVIF: Querying analytics for profile %s", profileToken.c_str());

    // Use the profile token as the analytics configuration token
    // (many cameras use the same token for both)
    std::string configToken = profileToken;

    // Get supported analytics modules
    try {
      auto request = buildRequest("tan:GetSupportedAnalyticsModules", [&](XML::Document & doc, XML::Node & root) {
        auto tokenNode = doc.createNode("tan:ConfigurationToken");
        tokenNode.setTextContent(configToken);
        root.addChild(tokenNode);
      }, SOAP::onvif::ServiceType::Analytics);

      if (request) {
        std::string response;
        auto result = sendRequestAndGetResponse("analytics", "GetSupportedAnalyticsModules", request.value, response);
        if (result) {
          auto parseResult = extractResponse<std::vector<::Device::AnalyticsModule>>(
            response, "GetSupportedAnalyticsModulesResponse",
            [](const XML::Document & doc) -> Result<std::vector<::Device::AnalyticsModule>> {
            std::vector<::Device::AnalyticsModule> modules;
            auto nodes =
              doc.evaluateXPathAll("//*[local-name()='AnalyticsModule' or local-name()='AnalyticsModuleDescription']");
            for (const auto & node : nodes) {
              ::Device::AnalyticsModule mod;
              auto nameNode = node.getChild("Name");
              if (nameNode.isValid()) mod.name = nameNode.getTextContent();
              auto typeNode = node.getChild("Type");
              if (typeNode.isValid()) mod.type = typeNode.getTextContent();
              if (mod.name.empty() && mod.type.empty()) {
                mod.name = node.getAttribute("Name");
                mod.type = node.getAttribute("Type");
              }
              if (!mod.name.empty() || !mod.type.empty()) modules.push_back(mod);
            }
            return Result<std::vector<::Device::AnalyticsModule>>{true, modules};
          });
          if (parseResult) {
            analytics.supportedModules = parseResult.value;
            HIGH_MSG("ONVIF: Found %zu supported analytics modules", analytics.supportedModules.size());
          }
        }
      }
    } catch (const std::exception & e) {
      MEDIUM_MSG("ONVIF: Failed to query supported analytics modules: %s", e.what());
    }

    // Get active rules
    try {
      auto request = buildRequest("tan:GetRules", [&](XML::Document & doc, XML::Node & root) {
        auto tokenNode = doc.createNode("tan:ConfigurationToken");
        tokenNode.setTextContent(configToken);
        root.addChild(tokenNode);
      }, SOAP::onvif::ServiceType::Analytics);

      if (request) {
        std::string response;
        auto result = sendRequestAndGetResponse("analytics", "GetRules", request.value, response);
        if (result) {
          auto parseResult = extractResponse<std::vector<::Device::AnalyticsRule>>(
            response, "GetRulesResponse", [](const XML::Document & doc) -> Result<std::vector<::Device::AnalyticsRule>> {
            std::vector<::Device::AnalyticsRule> rules;
            auto nodes = doc.evaluateXPathAll("//*[local-name()='Rule']");
            for (const auto & node : nodes) {
              ::Device::AnalyticsRule rule;
              rule.name = node.getAttribute("Name");
              rule.type = node.getAttribute("Type");
              rule.active = true;
              if (!rule.name.empty() || !rule.type.empty()) rules.push_back(rule);
            }
            return Result<std::vector<::Device::AnalyticsRule>>{true, rules};
          });
          if (parseResult) {
            analytics.activeRules = parseResult.value;
            HIGH_MSG("ONVIF: Found %zu active analytics rules", analytics.activeRules.size());
          }
        }
      }
    } catch (const std::exception & e) { MEDIUM_MSG("ONVIF: Failed to query analytics rules: %s", e.what()); }

    return analytics;
  }

  // Imaging service methods
  std::string Device::getVideoSourceToken() const {
    if (!videoSourceToken_.empty()) return videoSourceToken_;
    auto profiles = getMediaProfiles();
    if (!profiles || profiles.value.empty()) return "";
    // Prefer the actual VideoSourceConfiguration.SourceToken over the profile token
    if (!profiles.value[0].videoSourceToken.empty()) {
      videoSourceToken_ = profiles.value[0].videoSourceToken;
    } else {
      videoSourceToken_ = profiles.value[0].token;
    }
    return videoSourceToken_;
  }

  Result<ImagingSettings> Device::getImagingSettings(const std::string & videoSourceToken) const {
    if (!connected) return Result<ImagingSettings>{false, Error{Error::Type::Transport, "Not connected"}};
    if (services.find("imaging") == services.end() || services.at("imaging").empty())
      return Result<ImagingSettings>{false, Error{Error::Type::Validation, "Imaging service not available"}};

    auto request = buildRequest("timg:GetImagingSettings", [&](XML::Document & doc, XML::Node & root) {
      auto token = doc.createNode("timg:VideoSourceToken");
      token.setTextContent(videoSourceToken);
      root.addChild(token);
    }, SOAP::onvif::ServiceType::Imaging);
    if (!request) return Result<ImagingSettings>{false, request.error};

    std::string response;
    auto result = sendRequestAndGetResponse("imaging", "GetImagingSettings", request.value, response);
    if (!result) return Result<ImagingSettings>{false, result.error};

    return extractResponse<ImagingSettings>(response, "GetImagingSettingsResponse",
                                            [](const XML::Document & doc) -> Result<ImagingSettings> {
      ImagingSettings settings;
      auto node = doc.evaluateXPath("//*[local-name()='ImagingSettings']");
      if (!node.isValid())
        return Result<ImagingSettings>{false, Error{Error::Type::Validation, "No ImagingSettings node"}};

      auto brightness = node.getChild("Brightness");
      if (brightness.isValid()) settings.brightness = std::stof(brightness.getTextContent());

      auto contrast = node.getChild("Contrast");
      if (contrast.isValid()) settings.contrast = std::stof(contrast.getTextContent());

      auto saturation = node.getChild("ColorSaturation");
      if (saturation.isValid()) settings.colorSaturation = std::stof(saturation.getTextContent());

      auto sharpness = node.getChild("Sharpness");
      if (sharpness.isValid()) settings.sharpness = std::stof(sharpness.getTextContent());

      auto focus = node.getChild("Focus");
      if (focus.isValid()) {
        auto mode = focus.getChild("AutoFocusMode");
        if (mode.isValid()) settings.focus.mode = mode.getTextContent();
        auto speed = focus.getChild("DefaultSpeed");
        if (speed.isValid()) settings.focus.defaultSpeed = std::stof(speed.getTextContent());
        auto near = focus.getChild("NearLimit");
        if (near.isValid()) settings.focus.nearLimit = std::stof(near.getTextContent());
        auto far = focus.getChild("FarLimit");
        if (far.isValid()) settings.focus.farLimit = std::stof(far.getTextContent());
      }

      auto exposure = node.getChild("Exposure");
      if (exposure.isValid()) {
        auto mode = exposure.getChild("Mode");
        if (mode.isValid()) settings.exposure.mode = mode.getTextContent();
        auto iris = exposure.getChild("Iris");
        if (iris.isValid()) settings.exposure.iris = std::stof(iris.getTextContent());
      }

      auto wb = node.getChild("WhiteBalance");
      if (wb.isValid()) {
        auto mode = wb.getChild("Mode");
        if (mode.isValid()) settings.whiteBalance.mode = mode.getTextContent();
        auto cr = wb.getChild("CrGain");
        if (cr.isValid()) settings.whiteBalance.crGain = std::stof(cr.getTextContent());
        auto cb = wb.getChild("CbGain");
        if (cb.isValid()) settings.whiteBalance.cbGain = std::stof(cb.getTextContent());
      }

      return Result<ImagingSettings>{true, settings};
    });
  }

  Result<bool> Device::setImagingSettings(const std::string & videoSourceToken, const ImagingSettings & settings) {
    if (!connected) return Result<bool>{false, Error{Error::Type::Transport, "Not connected"}};
    if (services.find("imaging") == services.end() || services.at("imaging").empty())
      return Result<bool>{false, Error{Error::Type::Validation, "Imaging service not available"}};

    auto request = buildRequest("timg:SetImagingSettings", [&](XML::Document & doc, XML::Node & root) {
      auto token = doc.createNode("timg:VideoSourceToken");
      token.setTextContent(videoSourceToken);
      root.addChild(token);

      auto imgSettings = doc.createNode("timg:ImagingSettings");

      if (settings.brightness >= 0) {
        auto n = doc.createNode("tt:Brightness");
        n.setTextContent(std::to_string(settings.brightness));
        imgSettings.addChild(n);
      }
      if (settings.contrast >= 0) {
        auto n = doc.createNode("tt:Contrast");
        n.setTextContent(std::to_string(settings.contrast));
        imgSettings.addChild(n);
      }
      if (settings.colorSaturation >= 0) {
        auto n = doc.createNode("tt:ColorSaturation");
        n.setTextContent(std::to_string(settings.colorSaturation));
        imgSettings.addChild(n);
      }
      if (settings.sharpness >= 0) {
        auto n = doc.createNode("tt:Sharpness");
        n.setTextContent(std::to_string(settings.sharpness));
        imgSettings.addChild(n);
      }
      if (!settings.focus.mode.empty()) {
        auto focus = doc.createNode("tt:Focus");
        auto mode = doc.createNode("tt:AutoFocusMode");
        mode.setTextContent(settings.focus.mode);
        focus.addChild(mode);
        imgSettings.addChild(focus);
      }
      if (!settings.exposure.mode.empty()) {
        auto exp = doc.createNode("tt:Exposure");
        auto mode = doc.createNode("tt:Mode");
        mode.setTextContent(settings.exposure.mode);
        exp.addChild(mode);
        if (settings.exposure.iris != 0) {
          auto iris = doc.createNode("tt:Iris");
          iris.setTextContent(std::to_string(settings.exposure.iris));
          exp.addChild(iris);
        }
        imgSettings.addChild(exp);
      }
      if (!settings.whiteBalance.mode.empty()) {
        auto wb = doc.createNode("tt:WhiteBalance");
        auto mode = doc.createNode("tt:Mode");
        mode.setTextContent(settings.whiteBalance.mode);
        wb.addChild(mode);
        if (settings.whiteBalance.crGain != 0) {
          auto cr = doc.createNode("tt:CrGain");
          cr.setTextContent(std::to_string(settings.whiteBalance.crGain));
          wb.addChild(cr);
        }
        if (settings.whiteBalance.cbGain != 0) {
          auto cb = doc.createNode("tt:CbGain");
          cb.setTextContent(std::to_string(settings.whiteBalance.cbGain));
          wb.addChild(cb);
        }
        imgSettings.addChild(wb);
      }

      root.addChild(imgSettings);
    }, SOAP::onvif::ServiceType::Imaging);
    if (!request) return Result<bool>{false, request.error};

    auto result = sendRequestVoid("imaging", "SetImagingSettings", request.value);
    if (!result) return Result<bool>{false, result.error};
    return Result<bool>{true, true};
  }

  Result<bool> Device::focusMove(const std::string & videoSourceToken, float speed) {
    if (!connected) return Result<bool>{false, Error{Error::Type::Transport, "Not connected"}};
    if (services.find("imaging") == services.end() || services.at("imaging").empty())
      return Result<bool>{false, Error{Error::Type::Validation, "Imaging service not available"}};

    auto request = buildRequest("timg:Move", [&](XML::Document & doc, XML::Node & root) {
      auto token = doc.createNode("timg:VideoSourceToken");
      token.setTextContent(videoSourceToken);
      root.addChild(token);

      auto focus = doc.createNode("timg:Focus");
      auto continuous = doc.createNode("tt:Continuous");
      auto speedNode = doc.createNode("tt:Speed");
      speedNode.setTextContent(std::to_string(speed));
      continuous.addChild(speedNode);
      focus.addChild(continuous);
      root.addChild(focus);
    }, SOAP::onvif::ServiceType::Imaging);
    if (!request) return Result<bool>{false, request.error};

    auto result = sendRequestVoid("imaging", "Move", request.value);
    if (!result) return Result<bool>{false, result.error};
    return Result<bool>{true, true};
  }

  Result<bool> Device::focusStop(const std::string & videoSourceToken) {
    if (!connected) return Result<bool>{false, Error{Error::Type::Transport, "Not connected"}};
    if (services.find("imaging") == services.end() || services.at("imaging").empty())
      return Result<bool>{false, Error{Error::Type::Validation, "Imaging service not available"}};

    auto request = buildRequest("timg:Stop", [&](XML::Document & doc, XML::Node & root) {
      auto token = doc.createNode("timg:VideoSourceToken");
      token.setTextContent(videoSourceToken);
      root.addChild(token);
    }, SOAP::onvif::ServiceType::Imaging);
    if (!request) return Result<bool>{false, request.error};

    auto result = sendRequestVoid("imaging", "Stop", request.value);
    if (!result) return Result<bool>{false, result.error};
    return Result<bool>{true, true};
  }

  // Events service methods
  Result<std::string> Device::createPullPointSubscription(int initialTerminationTimeSec) {
    if (!connected) return Result<std::string>{false, Error{Error::Type::Transport, "Not connected"}};
    if (services.find("events") == services.end() || services.at("events").empty())
      return Result<std::string>{false, Error{Error::Type::Validation, "Events service not available"}};

    auto request = buildRequest("tev:CreatePullPointSubscription", [&](XML::Document & doc, XML::Node & root) {
      auto termTime = doc.createNode("tev:InitialTerminationTime");
      termTime.setTextContent("PT" + std::to_string(initialTerminationTimeSec) + "S");
      root.addChild(termTime);
    }, SOAP::onvif::ServiceType::Events);
    if (!request) return Result<std::string>{false, request.error};

    std::string response;
    auto result = sendRequestAndGetResponse("events", "CreatePullPointSubscription", request.value, response);
    if (!result) return Result<std::string>{false, result.error};

    return extractResponse<std::string>(response, "CreatePullPointSubscriptionResponse",
                                        [](const XML::Document & doc) -> Result<std::string> {
      auto addrNode = doc.evaluateXPath("//*[local-name()='SubscriptionReference']/*[local-name()='Address']");
      if (!addrNode.isValid())
        return Result<std::string>{false, Error{Error::Type::Validation, "No subscription address in response"}};
      return Result<std::string>{true, addrNode.getTextContent()};
    });
  }

  Result<std::vector<ONVIFEvent>> Device::pullMessages(const std::string & subscriptionEndpoint, int timeoutSec, int messageLimit) {
    if (!connected) return Result<std::vector<ONVIFEvent>>{false, Error{Error::Type::Transport, "Not connected"}};

    // PullMessages must be sent to the subscription endpoint directly
    auto msg = createMessage("PullMessages", SOAP::onvif::ServiceType::Events);
    XML::Document doc;
    XML::Node root = doc.createNode("tev:PullMessages");
    auto timeout = doc.createNode("tev:Timeout");
    timeout.setTextContent("PT" + std::to_string(timeoutSec) + "S");
    root.addChild(timeout);
    auto limit = doc.createNode("tev:MessageLimit");
    limit.setTextContent(std::to_string(messageLimit));
    root.addChild(limit);
    msg.setBodyNode(root);

    std::string response;
    auto result = sendRequestAndGetResponse("events", "PullMessages", msg.toString(), response, subscriptionEndpoint);
    if (!result) return Result<std::vector<ONVIFEvent>>{false, result.error};

    return extractResponse<std::vector<ONVIFEvent>>(response, "PullMessagesResponse",
                                                    [](const XML::Document & doc) -> Result<std::vector<ONVIFEvent>> {
      std::vector<ONVIFEvent> events;
      auto msgNodes = doc.evaluateXPathAll("//*[local-name()='NotificationMessage']");
      for (const auto & msgNode : msgNodes) {
        ONVIFEvent evt;
        auto topic = msgNode.getChild("Topic");
        if (topic.isValid()) evt.topic = topic.getTextContent();

        auto message = msgNode.getChild("Message");
        if (message.isValid()) {
          // Look for Message child element
          auto innerMsg = message.getChild("Message");
          if (!innerMsg.isValid()) innerMsg = message;

          auto source = innerMsg.getChild("Source");
          if (source.isValid()) {
            auto simpleItem = source.getChild("SimpleItem");
            if (simpleItem.isValid()) {
              evt.source = simpleItem.getAttribute("Name");
              evt.sourceValue = simpleItem.getAttribute("Value");
            }
          }
          auto data = innerMsg.getChild("Data");
          if (data.isValid()) {
            auto simpleItem = data.getChild("SimpleItem");
            if (simpleItem.isValid()) {
              evt.dataName = simpleItem.getAttribute("Name");
              evt.dataValue = simpleItem.getAttribute("Value");
            }
          }
          evt.timestamp = innerMsg.getAttribute("UtcTime");
        }
        events.push_back(evt);
      }
      return Result<std::vector<ONVIFEvent>>{true, events};
    });
  }

  Result<bool> Device::renewSubscription(const std::string & subscriptionEndpoint, int terminationTimeSec) {
    if (!connected) return Result<bool>{false, Error{Error::Type::Transport, "Not connected"}};

    auto msg = createMessage("Renew", SOAP::onvif::ServiceType::Events);
    XML::Document doc;
    XML::Node root = doc.createNode("wsnt:Renew");
    auto termTime = doc.createNode("wsnt:TerminationTime");
    termTime.setTextContent("PT" + std::to_string(terminationTimeSec) + "S");
    root.addChild(termTime);
    msg.setBodyNode(root);

    auto result = sendRequestVoid("events", "Renew", msg.toString(), subscriptionEndpoint);
    if (!result) return Result<bool>{false, result.error};
    return Result<bool>{true, true};
  }

  Result<bool> Device::unsubscribe(const std::string & subscriptionEndpoint) {
    if (!connected) return Result<bool>{false, Error{Error::Type::Transport, "Not connected"}};

    auto msg = createMessage("Unsubscribe", SOAP::onvif::ServiceType::Events);
    XML::Document doc;
    XML::Node root = doc.createNode("wsnt:Unsubscribe");
    msg.setBodyNode(root);

    auto result = sendRequestVoid("events", "Unsubscribe", msg.toString(), subscriptionEndpoint);
    if (!result) return Result<bool>{false, result.error};
    return Result<bool>{true, true};
  }

  ::Device::DeviceInfo Device::queryCapabilities() const {
    ::Device::DeviceInfo info;

    try {
      HIGH_MSG("ONVIF: Querying device capabilities");

      // Get device capabilities
      auto capsResult = getDeviceCapabilities();
      if (!capsResult) {
        WARN_MSG("Failed to query ONVIF capabilities: %s", capsResult.error.message.c_str());
        return info;
      }
      const auto & caps = capsResult.value;

      // Get device info
      auto deviceInfoResult = getDeviceInformation();
      if (deviceInfoResult) {
        const auto & devInfo = deviceInfoResult.value;
        info.host = host;
        info.name = devInfo.model.empty() ? host : devInfo.model;
        info.manufacturer = devInfo.manufacturer;
        info.model = devInfo.model;
        info.firmwareVersion = devInfo.firmwareVersion;
        info.serialNumber = devInfo.serialNumber;
        info.status = isConnected() ? "connected" : "disconnected";

        HIGH_MSG("ONVIF: Got device info - Name: %s, Host: %s", info.name.c_str(), info.host.c_str());

        // Generate consistent ID
        info.id = info.host;
      }

      // Core capabilities
      info.hasPTZ = caps.ptz;
      info.hasAudio = false; // Will be updated from media profiles
      info.hasMetadata = caps.analytics;

      HIGH_MSG("ONVIF: Core capabilities - PTZ: %d, Audio: %d, Metadata: %d", info.hasPTZ, info.hasAudio, info.hasMetadata);

      // PTZ features
      if (caps.ptz) {
        info.ptzFeatures = {"pan_tilt", "zoom", "preset", "home", "stop"};
        VERYHIGH_MSG("ONVIF: Added PTZ features");
      }

      // Protocol config
      ::Device::ProtocolConfig onvifConfig;
      onvifConfig.type = "onvif";
      onvifConfig.address = host;
      onvifConfig.port = port;
      onvifConfig.username = username;
      onvifConfig.password = password;

      VERYHIGH_MSG("ONVIF: Created protocol config - Type: %s, Address: %s, Port: %d", onvifConfig.type.c_str(),
                   onvifConfig.address.c_str(), onvifConfig.port);

      // Protocol capabilities
      onvifConfig.capabilities.hasPTZ = caps.ptz;
      onvifConfig.capabilities.hasAudio = false; // Will be updated from media profiles
      onvifConfig.capabilities.hasMetadata = caps.analytics;
      onvifConfig.capabilities.hasVideo = caps.media;
      onvifConfig.capabilities.hasRecording = caps.recording;
      onvifConfig.capabilities.hasWebControl = false;
      onvifConfig.capabilities.hasTally = false;

      // Get streaming capabilities
      auto streamCapsResult = getStreamingCapabilities();
      if (streamCapsResult) {
        const auto & streamCaps = streamCapsResult.value;
        onvifConfig.capabilities.hasRTPMulticast = streamCaps.RTPMulticast;
        onvifConfig.capabilities.hasRTPTCP = streamCaps.RTP_TCP;
        onvifConfig.capabilities.hasRTPRTSPTCP = streamCaps.RTP_RTSP_TCP;

        // Add supported transports
        onvifConfig.capabilities.supportedTransports = streamCaps.transportProtocols;

        VERYHIGH_MSG("ONVIF: Added streaming capabilities - Multicast: %d, TCP: %d, RTSP/TCP: %d",
                     streamCaps.RTPMulticast, streamCaps.RTP_TCP, streamCaps.RTP_RTSP_TCP);
      }

      // Add supported commands for PTZ
      if (caps.ptz) {
        onvifConfig.capabilities.supportedCommands = {"pan_tilt", "zoom", "preset", "home", "stop"};
        VERYHIGH_MSG("ONVIF: Added PTZ commands to protocol capabilities");
      }

      // Add protocol to device info
      info.protocols["onvif"] = onvifConfig;

      INFO_MSG("ONVIF: Device configuration complete for %s", info.name.c_str());
      HIGH_MSG("ONVIF: Device has %zu protocols", info.protocols.size());
      for (const auto & proto : info.protocols) {
        HIGH_MSG("  - Protocol: %s, Address: %s, Port: %d", proto.first.c_str(), proto.second.address.c_str(),
                 proto.second.port);
      }

      // Get media profiles for stream info and audio capability
      auto profilesResult = getMediaProfiles();
      if (profilesResult) {
        VERYHIGH_MSG("ONVIF: Processing %zu media profiles", profilesResult.value.size());
        const auto & profiles = profilesResult.value;

        for (const auto & profile : profiles) {
          HIGH_MSG("ONVIF: Processing profile %s (token: %s)", profile.name.c_str(), profile.token.c_str());

          // Check for audio capability
          if (!profile.audio.encoding.empty()) {
            info.hasAudio = true;
            onvifConfig.capabilities.hasAudio = true;
            info.protocols["onvif"] = onvifConfig;
          }

          // Create stream endpoints for video
          if (!profile.video.encoding.empty()) {
            // Define supported transport combinations
            struct TransportInfo {
                std::string protocol;
                std::string transport;
                bool multicast;
            };

            std::vector<TransportInfo> transports;

            // Add multicast if supported
            if (streamCapsResult && streamCapsResult.value.RTPMulticast) {
              transports.push_back({"RTP-Multicast", "UDP", true});
            }

            // Add TCP if supported
            if (streamCapsResult && streamCapsResult.value.RTP_TCP) {
              transports.push_back({"RTP-Unicast", "RTSP", false});
            }

            // If no transports are supported, try default unicast RTSP
            if (transports.empty()) {
              HIGH_MSG("ONVIF: No explicit transport support found, trying default unicast RTSP");
              transports.push_back({"RTP-Unicast", "RTSP", false});
            }

            for (const auto & transport : transports) {
              ::Device::StreamEndpoint stream;
              stream.name = profile.name;
              stream.protocol = "rtsp";
              stream.format = profile.video.encoding;
              stream.transport = transport.transport;
              stream.width = profile.video.width;
              stream.height = profile.video.height;
              stream.fps = profile.video.fps;
              stream.bitrate = profile.video.bitrate;
              stream.resolution = std::to_string(profile.video.width) + "x" + std::to_string(profile.video.height);
              stream.framerate = std::to_string(profile.video.fps);
              stream.profile = profile.token;

              // Get stream URI for this transport combination
              VERYHIGH_MSG("ONVIF: Getting stream URI for profile %s with transport %s/%s", profile.token.c_str(),
                           transport.protocol.c_str(), transport.transport.c_str());
              auto uriResult = getStreamUri(profile.token, transport.protocol, transport.transport);
              if (uriResult) {
                stream.uri = uriResult.value;

                // Parse URI to get address, port, and path
                try {
                  HTTP::URL url(stream.uri);
                  stream.address = url.host;
                  stream.port = url.getPort();
                  stream.path = url.path;

                  // Add stream-specific options
                  stream.options["profile"] = profile.token;
                  stream.options["multicast"] = transport.multicast ? "true" : "false";

                  VERYHIGH_MSG("ONVIF: Adding stream endpoint - %s:%d %s [%s/%s]", stream.address.c_str(), stream.port,
                               stream.path.c_str(), stream.transport.c_str(), stream.format.c_str());

                  info.streams.push_back(stream);
                } catch (const std::exception & e) {
                  WARN_MSG("ONVIF: Failed to parse stream URI %s: %s", stream.uri.c_str(), e.what());
                }
              } else {
                WARN_MSG("ONVIF: Failed to get stream URI for profile %s with transport %s/%s: %s", profile.token.c_str(),
                         transport.protocol.c_str(), transport.transport.c_str(), uriResult.error.message.c_str());
              }
            }
          } else {
            WARN_MSG("ONVIF: Profile %s has no video encoding", profile.token.c_str());
          }
        }
        // Get snapshot URI from the first profile
        if (!profiles.empty()) {
          auto snapResult = getSnapshotUri(profiles[0].token);
          if (snapResult) {
            info.snapshotUri = snapResult.value;
            HIGH_MSG("ONVIF: Got snapshot URI: %s", info.snapshotUri.c_str());
          }

          // Query analytics capabilities using the first profile token
          if (caps.analytics && services.count("analytics") && !services.at("analytics").empty()) {
            info.analytics = queryAnalytics(profiles[0].token);
            HIGH_MSG("ONVIF: Analytics: hasAnalytics=%d, modules=%zu, rules=%zu", info.analytics.hasAnalytics,
                     info.analytics.supportedModules.size(), info.analytics.activeRules.size());
          }
        }
      } else {
        WARN_MSG("ONVIF: Failed to get media profiles: %s", profilesResult.error.message.c_str());
      }

      // Query imaging capabilities if available
      if (caps.imaging && services.count("imaging") && !services.at("imaging").empty()) {
        std::string vsToken = getVideoSourceToken();
        if (!vsToken.empty()) {
          auto imgSettings = getImagingSettings(vsToken);
          if (imgSettings) {
            if (!imgSettings.value.focus.mode.empty()) {
              info.ptzFeatures.push_back("focus");
              HIGH_MSG("ONVIF: Imaging: focus mode=%s", imgSettings.value.focus.mode.c_str());
            }
            if (!imgSettings.value.exposure.mode.empty()) {
              info.ptzFeatures.push_back("iris");
              HIGH_MSG("ONVIF: Imaging: exposure mode=%s", imgSettings.value.exposure.mode.c_str());
            }
            if (!imgSettings.value.whiteBalance.mode.empty()) {
              info.ptzFeatures.push_back("whitebalance");
              HIGH_MSG("ONVIF: Imaging: WB mode=%s", imgSettings.value.whiteBalance.mode.c_str());
            }
          }
        }
      }

      // Populate features list
      if (caps.ptz) info.features.push_back("PTZ");
      if (caps.imaging) info.features.push_back("Imaging");
      if (caps.analytics) info.features.push_back("Analytics");
      if (caps.media) info.features.push_back("Media");
      if (caps.events) info.features.push_back("Events");
      if (caps.recording) info.features.push_back("Recording");

    } catch (const std::exception & e) {
      WARN_MSG("ONVIF: Failed to query capabilities: %s", e.what());
      return info;
    }

    return info;
  }

  void Device::addAuthToMessage(SOAP::Message & msg) const {
    if (username.empty()) return;
    if (!useDigestAuth) return;
    SOAP::SecurityHeader sec;
    sec.usernameToken.username = username;
    sec.usernameToken.password = password;
    sec.usernameToken.nonce = SOAP::generateNonce();
    sec.usernameToken.created = SOAP::getUTCTime(0);
    sec.timestamp.created = sec.usernameToken.created;
    sec.timestamp.expires = SOAP::getUTCTime(300);
    msg.addSecurityHeader(sec);
  }

  SOAP::Message Device::createMessage(const std::string & action, SOAP::onvif::ServiceType service) const {
    auto msg = messageFactory.createMessage(service, action, "http://" + host + ":" + std::to_string(port) + path);
    addAuthToMessage(msg);
    return msg;
  }

  // Async Discovery Implementation
  bool Discovery::startAsyncDiscovery(::Device::DiscoveryCallback callback, uint32_t timeoutMs) {
    if (asyncRunning) {
      HIGH_MSG("ONVIF: Async discovery already running");
      return false;
    }

    INFO_MSG("ONVIF: Starting async discovery (timeout: %u ms)", timeoutMs);

    asyncCallback = callback;
    asyncTimeoutMs = timeoutMs;
    asyncStartTime =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    asyncRunning = true;
    asyncDevices.clear();

    setupAsyncSocket();
    return true;
  }

  void Discovery::stopAsyncDiscovery() {
    if (!asyncRunning) return;

    INFO_MSG("ONVIF: Stopping async discovery");
    asyncRunning = false;
    asyncCallback = nullptr;
    socket.reset();
  }

  void Discovery::setupAsyncSocket() {
    try {
      // Create probe message using DiscoveryFactory for proper namespaces and headers
      SOAP::onvif::DiscoveryFactory factory(SOAP::Version::SOAP_1_2);
      auto msg = factory.makeProbe();
      std::string probeMsg = msg.toString();

      // Create and bind UDP socket for multicast discovery
      socket.reset(new Socket::UDPConnection(false, AF_INET));
      socket->data.allocate(65536);
      socket->SetDestination("239.255.255.250", 3702);
      // Set socket options before binding
      int sock = socket->getSock();
      int reuse = 1;
      if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        WARN_MSG("ONVIF: Failed to set SO_REUSEADDR: %s", strerror(errno));
      }

      if (!socket->bind(0)) {
        ERROR_MSG("ONVIF: Failed to bind async discovery socket");
        stopAsyncDiscovery();
        return;
      }

      // Multicast options: TTL=4 (same site), disable loopback
      unsigned char ttl = 4;
      if (setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0) {
        WARN_MSG("ONVIF: Failed to set IP_MULTICAST_TTL: %s", strerror(errno));
      }
      unsigned char loop = 0;
      if (setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop)) < 0) {
        WARN_MSG("ONVIF: Failed to set IP_MULTICAST_LOOP: %s", strerror(errno));
      }

      // Enumerate interfaces and send probe on each
      struct ifaddrs *ifaddr = nullptr;
      if (getifaddrs(&ifaddr) == -1) {
        ERROR_MSG("ONVIF: Failed to get network interfaces");
        // Still try a generic send
        // Send probe multiple times per interface to increase robustness (matches sync behavior)
        for (int attempt = 0; attempt < 3; ++attempt) { socket->SendNow(probeMsg); }
        return;
      }
      std::unique_ptr<struct ifaddrs, decltype(&freeifaddrs)> ifaddrGuard(ifaddr, freeifaddrs);

      for (struct ifaddrs *ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) { continue; }
        if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK) || !(ifa->ifa_flags & (IFF_BROADCAST | IFF_MULTICAST))) {
          continue;
        }

        char addressBuffer[INET_ADDRSTRLEN];
        void *tmpAddrPtr = &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr;
        inet_ntop(AF_INET, tmpAddrPtr, addressBuffer, INET_ADDRSTRLEN);
        std::string currentIface(addressBuffer);

        VERYHIGH_MSG("ONVIF: Async probe on interface %s", currentIface.c_str());
        // Set multicast outgoing interface without rebinding the socket
        struct in_addr if_addr;
        if (inet_pton(AF_INET, currentIface.c_str(), &if_addr) == 1) {
          if (setsockopt(socket->getSock(), IPPROTO_IP, IP_MULTICAST_IF, &if_addr, sizeof(if_addr)) < 0) {
            WARN_MSG("ONVIF: Failed to set IP_MULTICAST_IF for %s", currentIface.c_str());
          }
        }
        // Send probe multiple times per interface to increase robustness (matches sync behavior)
        for (int attempt = 0; attempt < 3; ++attempt) { socket->SendNow(probeMsg); }
      }

    } catch (const std::exception & e) {
      ERROR_MSG("ONVIF: Failed to setup async discovery socket: %s", e.what());
      stopAsyncDiscovery();
    }
  }

  void Discovery::processSocketData() {
    if (!asyncRunning || !socket) return;

    try {
      while (socket->Receive()) {
        const Socket::Address & remoteAddr = socket->getRemoteAddr();
        std::string sender = remoteAddr.host();

        size_t respSize = socket->data.size();
        if (respSize == 0) continue;

        const char *dataPtr = static_cast<const char *>((const void *)socket->data);
        responseBuffers[sender].append(dataPtr, respSize);

        std::string & accum = responseBuffers[sender];

        bool hasXmlDecl = accum.find("<?xml") != std::string::npos;
        bool hasEnvelopeEnd = (accum.find(":Envelope>") != std::string::npos);
        if (!hasXmlDecl || !hasEnvelopeEnd) { continue; }

        ::Device::DeviceInfo info;
        if (parseProbeMatch(accum, info)) {
          bool found = false;
          for (const auto & dev : asyncDevices) {
            if (dev.host == info.host) {
              found = true;
              break;
            }
          }
          if (!found) {
            asyncDevices.push_back(info);
            if (asyncCallback) {
              std::vector<::Device::DeviceInfo> singleDevice = {info};
              asyncCallback(singleDevice);
            }
          }
        }

        responseBuffers.erase(sender);
      }
    } catch (const std::exception & e) { WARN_MSG("ONVIF: Error processing async socket data: %s", e.what()); }
  }

  bool Discovery::checkAsyncTimeout() {
    if (!asyncRunning) return false;

    if (asyncTimeoutMs == 0) return false; // No timeout

    uint64_t currentTime =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();

    if (currentTime - asyncStartTime >= asyncTimeoutMs) {
      INFO_MSG("ONVIF: Async discovery timeout reached, found %zu devices", asyncDevices.size());
      stopAsyncDiscovery();
      return true;
    }

    return false;
  }

} // namespace ONVIF
