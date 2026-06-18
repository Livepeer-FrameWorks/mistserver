#include "device_visca.h"

#include "defines.h"
#include "socket.h"
#include "util.h"

#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <ifaddrs.h>
#include <memory>
#include <net/if.h>
#include <thread>

/**
 * VISCA Protocol Implementation
 *
 * Transport Modes:
 * - Legacy UDP: VISCA over UDP on port 1259 (legacy protocol)
 * - TCP: VISCA over IP using TCP (common for newer IP cameras)
 * - UDP: VISCA over IP using UDP on port 52381 (used by some IP cameras)
 *
 * Protocol Usage:
 * - Discovery: Uses UDP broadcast on respective ports:
 *   - Legacy protocol: port 1259
 *   - Modern VISCA over IP: port 52381
 * - Control: Uses the specified transport mode (TCP/UDP)
 */

namespace VISCA {

  // Add at the top with other structs
  struct ViscaDiscoveryConfig {
      uint16_t port;
      bool useViscaOverIP;
  };

  Device::Device(const std::string & addr, uint16_t p, ViscaTransport transport)
    : address(addr), port(p), transportType(transport), connected(false), connectionTimeout(1000), sequenceNum(0) {

    // Validate and clean up address
    if (address.empty()) {
      WARN_MSG("VISCA: Empty address provided, device will not be usable");
      lastError = ErrorInfo(Error::InvalidCommand, "Empty address provided");
      return;
    }

    // Remove any whitespace
    address.erase(std::remove_if(address.begin(), address.end(), ::isspace), address.end());

    // Validate port based on transport type
    if (port == 0) {
      // Set default port based on transport type
      port = (transportType == ViscaTransport::TCP) ? 52381 : 1259;
      HIGH_MSG("VISCA: Using default port %d for %s transport", port, transportType == ViscaTransport::TCP ? "TCP" : "UDP");
    }

    HIGH_MSG("VISCA: Created device at %s:%d", address.c_str(), port);
  }

  Device::~Device() {
    if (connected) { disconnect(); }
  }

  bool Device::connect() {
    HIGH_MSG("VISCA: Attempting to connect to %s", address.c_str());

    if (connected) {
      VERYHIGH_MSG("VISCA: Already connected");
      return true;
    }

    // Define ports to try
    std::vector<uint16_t> ports = {1259, 52381}; // Try both legacy and standard VISCA over IP ports
    std::vector<ViscaTransport> transports = {ViscaTransport::UDP, ViscaTransport::TCP};

    // Try each port/transport combination
    for (const auto & tryPort : ports) {
      for (const auto & transport : transports) {
        port = tryPort;
        transportType = transport;

        HIGH_MSG("VISCA: Trying port %d with transport %s", port, transport == ViscaTransport::TCP ? "TCP" : "UDP");

        bool success = false;
        if (transport == ViscaTransport::UDP) {
          success = createUDP();
        } else {
          success = createSocket();
        }

        if (success) {
          INFO_MSG("VISCA: Successfully connected via %s on port %d", transport == ViscaTransport::TCP ? "TCP" : "UDP", port);
          connected = true;

          // Initialize device after connection
          if (!initialize()) {
            WARN_MSG("VISCA: Failed to initialize device on port %d with %s", port, transport == ViscaTransport::TCP ? "TCP" : "UDP");
            disconnect();
            continue; // Try next combination
          }

          HIGH_MSG("VISCA: Device initialized successfully");
          return true;
        }

        WARN_MSG("VISCA: Failed to connect via %s on port %d", transport == ViscaTransport::TCP ? "TCP" : "UDP", port);
      }
    }

    WARN_MSG("VISCA: Failed to connect via any port/transport combination");
    return false;
  }

  void Device::disconnect() {
    if (tcpConn) { tcpConn.reset(); }
    if (udpConn) { udpConn.reset(); }
    connected = false;
  }

  bool Device::isConnected() const {
    switch (transportType) {
      case ViscaTransport::TCP: return tcpConn && tcpConn->connected();
      case ViscaTransport::UDP: return udpConn != nullptr;
      default: return false;
    }
  }

  ::Device::DeviceInfo Device::queryCapabilities() const {
    INFO_MSG("VISCA: Querying device capabilities for %s:%d", address.c_str(), port);
    ::Device::DeviceInfo info;

    // Basic device info
    info.id = address + ":" + std::to_string(port);
    info.host = address;
    info.name = "VISCA PTZ Camera at " + address;
    info.manufacturer = "Generic VISCA";
    info.model = "VISCA PTZ Camera";
    info.firmwareVersion = ""; // Not available through VISCA
    info.serialNumber = ""; // Not available through VISCA
    info.status = isConnected() ? "connected" : "disconnected";

    INSANE_MSG("VISCA: Device info - Name: %s, Host: %s", info.name.c_str(), info.host.c_str());
    INSANE_MSG("VISCA: Core capabilities - PTZ=%d, Audio=%d, Metadata=%d", info.hasPTZ, info.hasAudio, info.hasMetadata);

    // Core capabilities
    info.hasPTZ = true; // VISCA is always PTZ
    info.hasAudio = false; // VISCA doesn't support audio
    info.hasMetadata = false; // VISCA doesn't support metadata

    HIGH_MSG("VISCA: Core capabilities - PTZ=%d, Audio=%d, Metadata=%d", info.hasPTZ, info.hasAudio, info.hasMetadata);

    info.ptzFeatures = {"pan_tilt", "zoom",     "focus",    "iris",       "whitebalance", "preset", "home",
                        "stop",     "absolute", "relative", "continuous", "exposure",     "gain",   "flip"};

    info.features = {"ptz", "preset", "focus", "iris", "whitebalance", "exposure", "gain", "flip"};

    // Protocol config
    ::Device::ProtocolConfig viscaConfig;
    viscaConfig.type = "visca";
    viscaConfig.address = address;
    viscaConfig.port = port;

    INSANE_MSG("VISCA: Created protocol config - Type: %s, Address: %s, Port: %d", viscaConfig.type.c_str(),
               viscaConfig.address.c_str(), viscaConfig.port);

    // Protocol capabilities
    viscaConfig.capabilities.hasPTZ = true;
    viscaConfig.capabilities.hasAudio = false;
    viscaConfig.capabilities.hasMetadata = false;
    viscaConfig.capabilities.hasVideo = false; // VISCA is control only
    viscaConfig.capabilities.hasRecording = false;
    viscaConfig.capabilities.hasWebControl = false;
    viscaConfig.capabilities.hasTally = false;
    viscaConfig.capabilities.hasRTPMulticast = false;
    viscaConfig.capabilities.hasRTPTCP = false;
    viscaConfig.capabilities.hasRTPRTSPTCP = false;

    // Add supported commands
    viscaConfig.capabilities.supportedCommands = {"pan_tilt",     "zoom", "focus", "iris",          "whitebalance",
                                                  "exposure",     "gain", "flip",  "preset_store",  "preset_recall",
                                                  "preset_clear", "home", "stop",  "absolute_move", "direct_zoom"};

    info.protocols["visca"] = viscaConfig;

    INFO_MSG("VISCA: Device configuration complete for %s", info.name.c_str());
    HIGH_MSG("VISCA: Device has %zu protocols configured", info.protocols.size());
    for (const auto & proto : info.protocols) {
      INSANE_MSG("VISCA: Protocol %s at %s:%d", proto.first.c_str(), proto.second.address.c_str(), proto.second.port);
    }

    return info;
  }

  bool Device::sendPTZ(const ::Device::PTZCommand & cmd) {
    if (!connected) {
      lastError = ErrorInfo(Error::NotConnected, "Not connected");
      return false;
    }

    try {
      switch (cmd.action) {
        case ::Device::PTZAction::PanTilt:
          if (cmd.args.find("pan") != cmd.args.end() && cmd.args.find("tilt") != cmd.args.end()) {
            return panTilt(cmd.args.at("pan") * 100.0f, cmd.args.at("tilt") * 100.0f);
          }
          break;
        case ::Device::PTZAction::Zoom:
          if (cmd.args.find("zoom") != cmd.args.end()) { return zoom(cmd.args.at("zoom") * 100.0f); }
          break;
        case ::Device::PTZAction::Stop: return stop();
        case ::Device::PTZAction::Home: return home();
        case ::Device::PTZAction::Preset:
          if (cmd.args.find("preset") != cmd.args.end()) {
            auto store = cmd.args.find("store");
            if (store != cmd.args.end() && store->second > 0) {
              return setPreset(static_cast<uint8_t>(cmd.args.at("preset")));
            } else {
              return gotoPreset(static_cast<uint8_t>(cmd.args.at("preset")));
            }
          }
          break;
        case ::Device::PTZAction::Focus: {
          FocusMode mode = FocusMode::Manual;
          int value = 0;
          auto modeIt = cmd.args.find("mode");
          if (modeIt != cmd.args.end()) mode = static_cast<FocusMode>(static_cast<int>(modeIt->second));
          auto valIt = cmd.args.find("value");
          if (valIt != cmd.args.end()) value = static_cast<int>(valIt->second);
          return setFocus(mode, value);
        }
        case ::Device::PTZAction::Iris:
          if (cmd.args.find("value") != cmd.args.end()) { return setIris(static_cast<int>(cmd.args.at("value"))); }
          break;
        case ::Device::PTZAction::WhiteBalance: {
          WhiteBalanceMode mode = WhiteBalanceMode::Auto;
          int colorTemp = 0;
          auto modeIt = cmd.args.find("mode");
          if (modeIt != cmd.args.end()) mode = static_cast<WhiteBalanceMode>(static_cast<int>(modeIt->second));
          auto tempIt = cmd.args.find("color_temp");
          if (tempIt != cmd.args.end()) colorTemp = static_cast<int>(tempIt->second);
          return setWhiteBalance(mode, colorTemp);
        }
        default: lastError = ErrorInfo(Error::InvalidCommand, "Unsupported PTZ action"); return false;
      }
      lastError = ErrorInfo(Error::InvalidCommand, "Invalid PTZ command arguments");
      return false;
    } catch (const std::exception & e) {
      lastError = ErrorInfo(Error::SocketError, e.what());
      return false;
    }
  }

  bool Device::sendCommand(const uint8_t *cmd, size_t len, uint32_t timeoutMs) {
    if (!isConnected()) {
      lastError = ErrorInfo(Error::NotConnected, "Device not connected");
      return false;
    }

    try {
      // Log command bytes for debugging
      std::string cmdHex;
      for (size_t i = 0; i < len; i++) {
        char hex[4];
        snprintf(hex, sizeof(hex), "%02X ", cmd[i]);
        cmdHex += hex;
      }
      VERYHIGH_MSG("VISCA: Sending command [%s] to %s:%d", cmdHex.c_str(), address.c_str(), port);

      // Store command for debugging
      lastCommand = std::string(reinterpret_cast<const char *>(cmd), len);

      // Send command based on transport type
      if (transportType == ViscaTransport::TCP) {
        bool result = sendViscaOverIP(cmd, len);
        if (!result) {
          WARN_MSG("VISCA: Failed to send command over TCP");
          return false;
        }
      } else {
        // For UDP, send raw VISCA command
        if (!udpConn) {
          WARN_MSG("VISCA: UDP connection not initialized");
          return false;
        }
        udpConn->SendNow(reinterpret_cast<const char *>(cmd), len);
        HIGH_MSG("VISCA: Command sent over UDP");
      }

      return true;
    } catch (const std::exception & e) {
      lastError = ErrorInfo(Error::SocketError, std::string("Failed to send command: ") + e.what());
      WARN_MSG("VISCA: Exception while sending command: %s", e.what());
      return false;
    }
  }

  bool Device::setPreset(uint8_t presetId) {
    if (!connected) {
      lastError = ErrorInfo(Error::NotConnected, "Device not connected");
      return false;
    }

    try {
      uint8_t cmd[] = {0x81, 0x01, 0x04, 0x3F, 0x01, presetId, 0xFF};
      return sendCommand(cmd, sizeof(cmd));
    } catch (const std::exception & e) {
      lastError = ErrorInfo(Error::SocketError, e.what());
      return false;
    }
  }

  bool Device::gotoPreset(uint8_t presetId) {
    if (!connected) {
      lastError = ErrorInfo(Error::NotConnected, "Device not connected");
      return false;
    }

    try {
      uint8_t cmd[] = {0x81, 0x01, 0x04, 0x3F, 0x02, presetId, 0xFF};
      return sendCommand(cmd, sizeof(cmd));
    } catch (const std::exception & e) {
      lastError = ErrorInfo(Error::SocketError, e.what());
      return false;
    }
  }

  bool Device::removePreset(uint8_t presetId) {
    if (!connected) {
      lastError = ErrorInfo(Error::NotConnected, "Device not connected");
      return false;
    }

    try {
      uint8_t cmd[] = {0x81, 0x01, 0x04, 0x3F, 0x00, presetId, 0xFF};
      return sendCommand(cmd, sizeof(cmd));
    } catch (const std::exception & e) {
      lastError = ErrorInfo(Error::SocketError, e.what());
      return false;
    }
  }

  bool Device::home() {
    if (!connected) {
      lastError = ErrorInfo(Error::NotConnected, "Device not connected");
      return false;
    }

    try {
      uint8_t cmd[] = {0x81, 0x01, 0x06, 0x04, 0xFF};
      return sendCommand(cmd, sizeof(cmd));
    } catch (const std::exception & e) {
      lastError = ErrorInfo(Error::SocketError, e.what());
      return false;
    }
  }

  bool Device::stop() {
    if (!connected) {
      lastError = ErrorInfo(Error::NotConnected, "Device not connected");
      return false;
    }

    try {
      uint8_t cmd[] = {0x81, 0x01, 0x06, 0x01, 0x00, 0x00, 0x03, 0x03, 0xFF};
      return sendCommand(cmd, sizeof(cmd));
    } catch (const std::exception & e) {
      lastError = ErrorInfo(Error::SocketError, e.what());
      return false;
    }
  }

  bool Device::createUDP() {
    HIGH_MSG("VISCA: Creating UDP connection to %s:%d", address.c_str(), port);

    // Get list of network interfaces
    struct ifaddrs *ifaddr, *ifa;
    if (getifaddrs(&ifaddr) == -1) {
      WARN_MSG("VISCA: Failed to get network interfaces");
      lastError = ErrorInfo(Error::SocketError, "Failed to get network interfaces");
      return false;
    }
    std::unique_ptr<struct ifaddrs, decltype(&freeifaddrs)> ifaddrGuard(ifaddr, freeifaddrs);

    // Iterate over interfaces
    bool foundInterface = false;
    for (ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
      if (!ifa->ifa_addr) continue;

      // Skip interfaces that are:
      // - Not IPv4
      // - Down
      // - Loopback
      // - Not supporting broadcast/multicast
      if (ifa->ifa_addr->sa_family != AF_INET || // Not IPv4
          !(ifa->ifa_flags & IFF_UP) || // Interface is down
          (ifa->ifa_flags & IFF_LOOPBACK) || // Loopback interface
          !(ifa->ifa_flags & (IFF_BROADCAST | IFF_MULTICAST))) { // No broadcast/multicast support
        continue;
      }

      // Get interface address
      char addressBuffer[INET_ADDRSTRLEN];
      void *tmpAddrPtr = &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr;
      inet_ntop(AF_INET, tmpAddrPtr, addressBuffer, INET_ADDRSTRLEN);
      std::string iface(addressBuffer);

      VERYHIGH_MSG("VISCA: Trying interface %s", iface.c_str());

      // Create UDP socket
      udpConn = std::unique_ptr<Socket::UDPConnection>(new Socket::UDPConnection(false, AF_INET));
      if (!udpConn) {
        WARN_MSG("VISCA: Failed to create UDP socket");
        continue;
      }

      // Set destination
      udpConn->SetDestination(address, port);

      // Bind to interface
      if (!udpConn->bind(0, iface)) {
        WARN_MSG("VISCA: Failed to bind to interface %s", iface.c_str());
        udpConn.reset();
        continue;
      }

      // Successfully bound to interface
      HIGH_MSG("VISCA: Successfully bound to interface %s", iface.c_str());
      foundInterface = true;
      break;
    }

    if (!foundInterface) {
      lastError = ErrorInfo(Error::SocketError, "Failed to find suitable interface for UDP socket");
      WARN_MSG("VISCA: Failed to find suitable interface for UDP socket");
      return false;
    }

    return true;
  }

  bool Device::createSocket() {
    if (address.empty()) {
      lastError = ErrorInfo(Error::InvalidCommand, "Empty address");
      return false;
    }

    // Close existing connections
    if (tcpConn) tcpConn.reset();
    if (udpConn) udpConn.reset();

    try {
      switch (transportType) {
        case ViscaTransport::TCP:
          HIGH_MSG("VISCA: Creating TCP connection to %s:%d", address.c_str(), port);
          tcpConn = std::unique_ptr<Socket::Connection>(new Socket::Connection());
          if (!tcpConn) {
            lastError = ErrorInfo(Error::SocketError, "Failed to create TCP connection");
            return false;
          }
          tcpConn->open(address, port, false);
          return tcpConn->connected();

        case ViscaTransport::UDP: return createUDP();

        default:
          FAIL_MSG("Unknown transport type");
          lastError = ErrorInfo(Error::InvalidCommand, "Unknown transport type");
          return false;
      }
    } catch (const std::exception & e) {
      WARN_MSG("VISCA: Socket creation failed: %s", e.what());
      lastError = ErrorInfo(Error::SocketError, e.what());
      return false;
    }
  }

  bool Device::initialize() {
    HIGH_MSG("VISCA: Initializing device at %s:%d", address.c_str(), port);

    // Send a simple power status inquiry to verify communication
    uint8_t inquiry[] = {0x81, 0x09, 0x04, 0x00, 0xFF};
    VERYHIGH_MSG("VISCA: Sending power status inquiry");
    if (!sendCommand(inquiry, sizeof(inquiry))) {
      WARN_MSG("VISCA: Failed to send power status inquiry");
      return false;
    }

    // Wait for response with increased timeout for initial connection
    std::string response;
    if (!waitForResponse(response, 4, 2000)) { // Increased timeout to 2 seconds for initial connection
      WARN_MSG("VISCA: No response to power status inquiry");
      return false;
    }

    HIGH_MSG("VISCA: Device initialization successful");
    lastError = ErrorInfo(); // Clear error
    return true;
  }

  bool Device::sendViscaOverIP(const uint8_t *payload, size_t len) {
    if (!isConnected()) {
      lastError = ErrorInfo(Error::NotConnected, "Device not connected");
      return false;
    }

    // Create packet with header
    std::vector<uint8_t> packet(len + 8);
    packet[0] = 0x01; // Message type
    packet[1] = 0x10; // VISCA command
    packet[2] = (len >> 8) & 0xFF; // Payload length MSB
    packet[3] = len & 0xFF; // Payload length LSB

    packet[4] = (sequenceNum >> 24) & 0xFF; // Sequence number byte 3 (MSB)
    packet[5] = (sequenceNum >> 16) & 0xFF; // Sequence number byte 2
    packet[6] = (sequenceNum >> 8) & 0xFF; // Sequence number byte 1
    packet[7] = sequenceNum & 0xFF; // Sequence number byte 0 (LSB)
    memcpy(packet.data() + 8, payload, len);
    sequenceNum++; // Increment sequence number

    switch (transportType) {
      case ViscaTransport::TCP:
        if (tcpConn) { tcpConn->SendNow(reinterpret_cast<const char *>(packet.data()), packet.size()); }
        break;
      case ViscaTransport::UDP:
        if (udpConn) { udpConn->SendNow(reinterpret_cast<const char *>(packet.data()), packet.size()); }
        break;
      default: lastError = ErrorInfo(Error::InvalidState, "Invalid transport type"); return false;
    }
    return true;
  }

  bool Device::waitForResponse(std::string & response, size_t expectedLen, uint32_t timeoutMs) {
    if (!isConnected()) {
      lastError = ErrorInfo(Error::NotConnected, "Device not connected");
      return false;
    }

    auto start = std::chrono::steady_clock::now();
    bool hasData = false;

    while (true) {
      if (transportType == ViscaTransport::TCP) {
        hasData = tcpConn->spool();
      } else if (transportType == ViscaTransport::UDP) {
        hasData = udpConn->Receive();
      }

      if (hasData) {
        if (transportType == ViscaTransport::TCP) {
          // Handle TCP response
          auto & buffer = tcpConn->Received();
          if (buffer.size() >= expectedLen) {
            response = buffer.remove(expectedLen);
            VERYHIGH_MSG("VISCA: Received %zu bytes over TCP", response.size());
            return validateResponse(response);
          }
        } else if (transportType == ViscaTransport::UDP) {
          // Handle UDP response
          response.assign(reinterpret_cast<const char *>((const void *)udpConn->data), udpConn->data.size());
          VERYHIGH_MSG("VISCA: Received %zu bytes over UDP", response.size());
          return validateResponse(response);
        }
      }

      // Check timeout
      auto now = std::chrono::steady_clock::now();
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start);
      if (elapsed.count() >= timeoutMs) {
        WARN_MSG("VISCA: Response timeout after %u ms", timeoutMs);
        lastError = ErrorInfo(Error::Timeout, "Response timeout");
        return false;
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  bool Device::validateResponse(const std::string & response) {
    if (response.length() < 3) {
      lastError = ErrorInfo(Error::InvalidResponse, "Response too short");
      WARN_MSG("VISCA: Response too short (got %zu bytes, need at least 3)", response.length());
      return false;
    }

    // Log the response being validated
    std::string respHex;
    for (size_t i = 0; i < response.length(); i++) {
      char hex[4];
      snprintf(hex, sizeof(hex), "%02X ", (uint8_t)response[i]);
      respHex += hex;
    }
    INSANE_MSG("VISCA: Validating VISCA response [%s]", respHex.c_str());

    uint8_t type;
    if (!isValidViscaResponse(response, type)) {
      lastError = ErrorInfo(Error::InvalidResponse, "Invalid response format");
      WARN_MSG("VISCA: Invalid response format");
      return false;
    }

    VERYHIGH_MSG("VISCA: Response validation successful (type: 0x%02X)", type);
    return true;
  }

  bool Device::panTilt(int pan, int tilt) {
    if (pan < -100 || pan > 100 || tilt < -100 || tilt > 100) {
      lastError = ErrorInfo(Error::InvalidCommand, "Pan/tilt values must be between -100 and 100");
      return false;
    }

    // Convert speed to VISCA format (-100 to 100 -> 0 to 24)
    uint8_t panSpeed = static_cast<uint8_t>(std::abs(pan) * 24 / 100);
    uint8_t tiltSpeed = static_cast<uint8_t>(std::abs(tilt) * 24 / 100);

    // Set direction
    uint8_t panDir = pan == 0 ? 0x03 : (pan > 0 ? 0x02 : 0x01);
    uint8_t tiltDir = tilt == 0 ? 0x03 : (tilt > 0 ? 0x02 : 0x01);

    // Send command
    uint8_t cmd[] = {0x81, 0x01, 0x06, 0x01, panSpeed, tiltSpeed, panDir, tiltDir, 0xFF};
    return sendCommand(cmd, sizeof(cmd));
  }

  bool Device::zoom(int speed) {
    if (speed < -100 || speed > 100) {
      lastError = ErrorInfo(Error::InvalidCommand, "Zoom speed must be between -100 and 100");
      return false;
    }

    // Convert speed to VISCA format (-100 to 100 -> 0 to 7)
    uint8_t zoomSpeed = static_cast<uint8_t>(std::abs(speed) * 7 / 100);
    uint8_t dir = speed == 0 ? 0x00 : (speed > 0 ? 0x20 : 0x30);

    // Send command
    uint8_t cmd[] = {0x81, 0x01, 0x04, 0x07, static_cast<uint8_t>(dir | zoomSpeed), 0xFF};
    return sendCommand(cmd, sizeof(cmd));
  }

  bool Device::presetRecall(const std::string & preset) {
    try {
      int presetNum = std::stoi(preset);
      if (presetNum < 0 || presetNum > 127) {
        lastError = ErrorInfo(Error::InvalidCommand, "Preset number must be between 0 and 127");
        return false;
      }

      uint8_t cmd[] = {0x81, 0x01, 0x04, 0x3F, 0x02, (uint8_t)presetNum, 0xFF};
      return sendCommand(cmd, sizeof(cmd));
    } catch (const std::exception &) {
      lastError = ErrorInfo(Error::InvalidCommand, "Invalid preset number format");
      return false;
    }
  }

  bool Device::presetStore(const std::string & preset) {
    try {
      int presetNum = std::stoi(preset);
      if (presetNum < 0 || presetNum > 127) {
        lastError = ErrorInfo(Error::InvalidCommand, "Invalid preset number format");
        return false;
      }

      uint8_t cmd[] = {0x81, 0x01, 0x04, 0x3F, 0x01, (uint8_t)presetNum, 0xFF};
      return sendCommand(cmd, sizeof(cmd));
    } catch (const std::exception &) {
      lastError = ErrorInfo(Error::InvalidCommand, "Invalid preset number format");
      return false;
    }
  }

  bool Device::getStatus(int & pan, int & tilt, int & zoom) {
    if (!isConnected()) {
      lastError = ErrorInfo(Error::NotConnected, "Device not connected");
      return false;
    }

    // Send inquiry command
    uint8_t cmd[] = {0x81, 0x09, 0x06, 0x12, 0xFF};
    if (!sendCommand(cmd, sizeof(cmd))) { return false; }

    // The response should be in the last received message
    // For this camera, the status is included in the 0x50 response
    // Format: 90 50 0F 0F 0D 0A 00 00 00 03 FF
    // pan = bytes 2-3 (0F 0F)
    // tilt = bytes 4-5 (0D 0A)
    // zoom = bytes 6-9 (00 00 00 03)

    std::string response;
    if (!waitForResponse(response, 11, 1000)) {
      lastError = ErrorInfo(Error::Timeout, "Timeout waiting for status response");
      return false;
    }

    if (response.length() >= 11 && (uint8_t)response[1] == 0x50) {
      pan = ((uint8_t)response[2] << 8) | (uint8_t)response[3];
      tilt = ((uint8_t)response[4] << 8) | (uint8_t)response[5];
      zoom = ((uint8_t)response[6] << 24) | ((uint8_t)response[7] << 16) | ((uint8_t)response[8] << 8) | (uint8_t)response[9];
      VERYHIGH_MSG("VISCA: Got status - pan: %d, tilt: %d, zoom: %d", pan, tilt, zoom);
      return true;
    }

    lastError = ErrorInfo(Error::InvalidResponse, "Invalid response format");
    return false;
  }

  bool Device::setFocus(FocusMode mode, int value) {
    uint8_t cmd[7] = {0x81, 0x01, 0x04, 0x38, 0x00, 0x00, 0xFF};

    switch (mode) {
      case FocusMode::Auto: cmd[4] = 0x02; break;
      case FocusMode::Manual: cmd[4] = 0x03; break;
      case FocusMode::OnePush: cmd[4] = 0x01; break;
      case FocusMode::Infinity: cmd[4] = 0x04; break;
      case FocusMode::NearLimit:
        if (value < 0 || value > 0xFFFF) {
          lastError = ErrorInfo(Error::InvalidCommand, "Near limit value out of range");
          return false;
        }
        cmd[4] = 0x05;
        cmd[5] = value & 0xFF;
        cmd[6] = (value >> 8) & 0xFF;
        break;
      case FocusMode::FarLimit:
        if (value < 0 || value > 0xFFFF) {
          lastError = ErrorInfo(Error::InvalidCommand, "Far limit value out of range");
          return false;
        }
        cmd[4] = 0x06;
        cmd[5] = value & 0xFF;
        cmd[6] = (value >> 8) & 0xFF;
        break;
      default: lastError = ErrorInfo(Error::InvalidCommand, "Unsupported focus mode"); return false;
    }

    return sendCommand(cmd, sizeof(cmd));
  }

  bool Device::setIris(int value) {
    if (value < 0 || value > 100) {
      lastError = ErrorInfo(Error::InvalidCommand, "Invalid argument");
      return false;
    }

    // Convert to VISCA format (0-100 -> 0-17)
    uint8_t irisPos = static_cast<uint8_t>(value * 17 / 100);
    uint8_t cmd[] = {0x81, 0x01, 0x04, 0x4B, irisPos, 0xFF};
    return sendCommand(cmd, sizeof(cmd));
  }

  bool Device::setWhiteBalance(WhiteBalanceMode mode, int colorTemp) {
    uint8_t cmd[7] = {0x81, 0x01, 0x04, 0x35, 0x00, 0x00, 0xFF};

    switch (mode) {
      case WhiteBalanceMode::Auto: cmd[4] = 0x00; break;
      case WhiteBalanceMode::Indoor: cmd[4] = 0x01; break;
      case WhiteBalanceMode::Outdoor: cmd[4] = 0x02; break;
      case WhiteBalanceMode::OnePush: cmd[4] = 0x03; break;
      case WhiteBalanceMode::Manual: cmd[4] = 0x04; break;
      case WhiteBalanceMode::ColorTemp:
        if (colorTemp < 2000 || colorTemp > 15000) {
          lastError = ErrorInfo(Error::InvalidCommand, "Color temperature out of range (2000-15000K)");
          return false;
        }
        cmd[4] = 0x05;
        cmd[5] = colorTemp & 0xFF;
        cmd[6] = (colorTemp >> 8) & 0xFF;
        break;
      case WhiteBalanceMode::ATW: cmd[4] = 0x06; break;
    }

    return sendCommand(cmd, sizeof(cmd));
  }

  bool Device::setBacklight(bool enabled) {
    uint8_t cmd[] = {0x81, 0x01, 0x04, 0x33, static_cast<uint8_t>(enabled ? 0x02 : 0x03), 0xFF};
    return sendCommand(cmd, sizeof(cmd));
  }

  bool Device::setPower(bool on) {
    uint8_t cmd[] = {0x81, 0x01, 0x04, 0x00, static_cast<uint8_t>(on ? 0x02 : 0x03), 0xFF};
    return sendCommand(cmd, sizeof(cmd));
  }

  bool Device::setExposure(ExposureMode mode) {
    uint8_t cmd[] = {0x81, 0x01, 0x04, 0x39, 0x00, 0xFF};
    switch (mode) {
      case ExposureMode::Auto: cmd[4] = 0x00; break;
      case ExposureMode::Manual: cmd[4] = 0x03; break;
      case ExposureMode::ShutterPriority: cmd[4] = 0x0A; break;
      case ExposureMode::IrisPriority: cmd[4] = 0x0B; break;
      case ExposureMode::Bright: cmd[4] = 0x0D; break;
      case ExposureMode::SpotMeter: cmd[4] = 0x0E; break;
      default: lastError = ErrorInfo(Error::InvalidCommand, "Unsupported exposure mode"); return false;
    }
    return sendCommand(cmd, sizeof(cmd));
  }

  bool Device::setGain(GainMode mode, int value) {
    if (mode == GainMode::Auto) {
      uint8_t cmd[] = {0x81, 0x01, 0x04, 0x0C, 0x02, 0xFF};
      return sendCommand(cmd, sizeof(cmd));
    }
    // Manual: direct gain value (0x04 0x4C 0p 0q 0r 0s)
    uint8_t cmd[] = {0x81,
                     0x01,
                     0x04,
                     0x4C,
                     static_cast<uint8_t>((value >> 12) & 0x0F),
                     static_cast<uint8_t>((value >> 8) & 0x0F),
                     static_cast<uint8_t>((value >> 4) & 0x0F),
                     static_cast<uint8_t>(value & 0x0F),
                     0xFF};
    return sendCommand(cmd, sizeof(cmd));
  }

  bool Device::setImageFlip(ImageFlipMode mode) {
    // LR reverse: 81 01 04 61 [02=on, 03=off]
    // Picture flip: 81 01 04 66 [02=on, 03=off]
    bool lr = (mode == ImageFlipMode::Horizontal || mode == ImageFlipMode::Both);
    bool flip = (mode == ImageFlipMode::Vertical || mode == ImageFlipMode::Both);
    uint8_t lrCmd[] = {0x81, 0x01, 0x04, 0x61, static_cast<uint8_t>(lr ? 0x02 : 0x03), 0xFF};
    if (!sendCommand(lrCmd, sizeof(lrCmd))) return false;
    uint8_t flipCmd[] = {0x81, 0x01, 0x04, 0x66, static_cast<uint8_t>(flip ? 0x02 : 0x03), 0xFF};
    return sendCommand(flipCmd, sizeof(flipCmd));
  }

  bool Device::absoluteMove(int pan, int tilt, uint8_t speed) {
    // VISCA absolute position: 81 01 06 02 VV WW 0Y 0Y 0Y 0Y 0Z 0Z 0Z 0Z FF
    // VV = pan speed (01-18), WW = tilt speed (01-14)
    if (speed < 1) speed = 1;
    if (speed > 0x18) speed = 0x18;
    uint8_t tiltSpeed = speed;
    if (tiltSpeed > 0x14) tiltSpeed = 0x14;
    uint8_t cmd[] = {0x81,
                     0x01,
                     0x06,
                     0x02,
                     speed,
                     tiltSpeed,
                     static_cast<uint8_t>((pan >> 12) & 0x0F),
                     static_cast<uint8_t>((pan >> 8) & 0x0F),
                     static_cast<uint8_t>((pan >> 4) & 0x0F),
                     static_cast<uint8_t>(pan & 0x0F),
                     static_cast<uint8_t>((tilt >> 12) & 0x0F),
                     static_cast<uint8_t>((tilt >> 8) & 0x0F),
                     static_cast<uint8_t>((tilt >> 4) & 0x0F),
                     static_cast<uint8_t>(tilt & 0x0F),
                     0xFF};
    return sendCommand(cmd, sizeof(cmd));
  }

  bool Device::directZoom(uint16_t position) {
    // VISCA direct zoom: 81 01 04 47 0p 0q 0r 0s FF
    uint8_t cmd[] = {0x81,
                     0x01,
                     0x04,
                     0x47,
                     static_cast<uint8_t>((position >> 12) & 0x0F),
                     static_cast<uint8_t>((position >> 8) & 0x0F),
                     static_cast<uint8_t>((position >> 4) & 0x0F),
                     static_cast<uint8_t>(position & 0x0F),
                     0xFF};
    return sendCommand(cmd, sizeof(cmd));
  }

  bool Device::getZoomPosition(uint16_t & position) {
    uint8_t cmd[] = {0x81, 0x09, 0x04, 0x47, 0xFF};
    if (!sendCommand(cmd, sizeof(cmd))) return false;

    std::string response;
    if (!waitForResponse(response, 7, 1000)) {
      lastError = ErrorInfo(Error::Timeout, "Timeout waiting for zoom position");
      return false;
    }
    if (response.length() >= 7 && (uint8_t)response[1] == 0x50) {
      position = ((uint8_t)response[2] & 0x0F) << 12 | ((uint8_t)response[3] & 0x0F) << 8 |
        ((uint8_t)response[4] & 0x0F) << 4 | ((uint8_t)response[5] & 0x0F);
      return true;
    }
    lastError = ErrorInfo(Error::InvalidResponse, "Invalid zoom position response");
    return false;
  }

  bool Device::getFocusPosition(uint16_t & position) {
    uint8_t cmd[] = {0x81, 0x09, 0x04, 0x48, 0xFF};
    if (!sendCommand(cmd, sizeof(cmd))) return false;

    std::string response;
    if (!waitForResponse(response, 7, 1000)) {
      lastError = ErrorInfo(Error::Timeout, "Timeout waiting for focus position");
      return false;
    }
    if (response.length() >= 7 && (uint8_t)response[1] == 0x50) {
      position = ((uint8_t)response[2] & 0x0F) << 12 | ((uint8_t)response[3] & 0x0F) << 8 |
        ((uint8_t)response[4] & 0x0F) << 4 | ((uint8_t)response[5] & 0x0F);
      return true;
    }
    lastError = ErrorInfo(Error::InvalidResponse, "Invalid focus position response");
    return false;
  }

  bool Device::reconnect(uint32_t retries) {
    for (uint32_t i = 0; i < retries; i++) {
      disconnect();
      if (connect()) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
    return false;
  }

  bool Device::setAddress(const std::string & newAddress, uint16_t newPort) {
    if (isConnected()) { disconnect(); }

    // Validate new address
    if (newAddress.empty()) {
      lastError = ErrorInfo(Error::InvalidCommand, "Empty address provided");
      WARN_MSG("VISCA: Cannot set empty address");
      return false;
    }

    // Clean up address (remove whitespace)
    address = newAddress;
    address.erase(std::remove_if(address.begin(), address.end(), ::isspace), address.end());

    // Validate and set port
    if (newPort == 0) {
      // Keep existing port if new port is 0
      if (port == 0) {
        // If no port was set before, use default based on transport type
        port = (transportType == ViscaTransport::TCP) ? 52381 : 1259;
        INFO_MSG("VISCA: Using default port %d for %s transport", port, transportType == ViscaTransport::TCP ? "TCP" : "UDP");
      }
    } else {
      port = newPort;
    }

    // Set transport type based on port
    if (port == 1259) {
      // Legacy port 1259 is always UDP
      transportType = ViscaTransport::UDP;
      INFO_MSG("VISCA: Setting UDP transport for legacy port 1259 device at %s", address.c_str());
    } else if (port == 52381) {
      // Modern VISCA over IP port defaults to TCP but can be changed
      transportType = ViscaTransport::TCP;
      INFO_MSG("VISCA: Setting TCP transport for modern port 52381 device at %s", address.c_str());
    } else {
      // For other ports, keep existing transport type
      INFO_MSG("VISCA: Using %s transport for device at %s:%d", transportType == ViscaTransport::TCP ? "TCP" : "UDP",
               address.c_str(), port);
    }

    // Clear any previous error state
    lastError = ErrorInfo();
    return true;
  }

  bool Device::setTransportType(ViscaTransport type) {
    if (isConnected()) { disconnect(); }

    transportType = type;
    return true;
  }

  // Discovery implementations
  Discovery::Discovery() {}

  Discovery::~Discovery() {
    stopAsyncDiscovery();
  }

  // Add helper function to format VISCA over IP packets
  std::vector<uint8_t> formatViscaOverIPPacket(const uint8_t *payload, size_t len) {
    std::vector<uint8_t> packet(len + 8);
    packet[0] = 0x01; // Message type
    packet[1] = 0x10; // VISCA command
    packet[2] = (len >> 8) & 0xFF; // Payload length MSB
    packet[3] = len & 0xFF; // Payload length LSB

    // Use static sequence number
    static uint32_t sequenceNum = 0;
    packet[4] = (sequenceNum >> 24) & 0xFF;
    packet[5] = (sequenceNum >> 16) & 0xFF;
    packet[6] = (sequenceNum >> 8) & 0xFF;
    packet[7] = sequenceNum & 0xFF;
    memcpy(packet.data() + 8, payload, len);
    sequenceNum++;

    return packet;
  }

  bool Discovery::sendCommand(const std::string & deviceId, const ::Device::PTZCommand & cmd) {
    // Check connection cache first
    auto it = connectedDevices.find(deviceId);
    if (it != connectedDevices.end() && it->second && it->second->isConnected()) { return it->second->sendPTZ(cmd); }

    // Parse device ID to get address and port
    size_t colonPos = deviceId.find(':');
    if (colonPos == std::string::npos) { return false; }

    std::string address = deviceId.substr(0, colonPos);
    uint16_t port;
    try {
      port = std::stoi(deviceId.substr(colonPos + 1));
    } catch (const std::exception &) { return false; }

    // Create device, connect, cache it
    auto device = std::unique_ptr<Device>(new Device(address, port));
    if (!device->connect()) { return false; }

    bool result = device->sendPTZ(cmd);
    connectedDevices[deviceId] = std::move(device);
    return result;
  }

  bool Discovery::setTally(const std::string & deviceId, bool on) {
    // Reuse cached connection or create new one (same pattern as sendCommand)
    auto it = connectedDevices.find(deviceId);
    if (it != connectedDevices.end() && it->second && it->second->isConnected()) {
      return it->second->setBacklight(on);
    }

    size_t colonPos = deviceId.find(':');
    if (colonPos == std::string::npos) return false;

    std::string address = deviceId.substr(0, colonPos);
    uint16_t port;
    try {
      port = std::stoi(deviceId.substr(colonPos + 1));
    } catch (const std::exception &) { return false; }

    auto device = std::unique_ptr<Device>(new Device(address, port));
    if (!device->connect()) return false;

    bool result = device->setBacklight(on);
    connectedDevices[deviceId] = std::move(device);
    return result;
  }

  std::unique_ptr<::Device::Base> Discovery::createDevice(const ::Device::DeviceInfo & info) const {
    std::string address = info.host;
    uint16_t port = 52381;
    auto protoIt = info.protocols.find("visca");
    if (protoIt != info.protocols.end()) {
      if (!protoIt->second.address.empty()) address = protoIt->second.address;
      if (protoIt->second.port) port = protoIt->second.port;
    }
    return std::unique_ptr<Device>(new Device(address, port));
  }

  bool Discovery::parseDiscoveryResponse(const std::string & response, ::Device::DeviceInfo & info) {
    if (response.empty()) {
      WARN_MSG("VISCA: Empty response");
      return false;
    }

    // Log received data for debugging
    std::string respHex;
    for (size_t i = 0; i < response.length(); i++) {
      char hex[4];
      snprintf(hex, sizeof(hex), "%02X ", (uint8_t)response[i]);
      respHex += hex;
    }
    INSANE_MSG("VISCA: Parsing response: [%s]", respHex.c_str());

    // Check if this is a VISCA over IP response (has 8-byte header)
    std::string viscaResponse = response;
    // bool isViscaOverIP = false;

    if (response.length() > 8) {
      // Check VISCA over IP header
      uint8_t msgType = (uint8_t)response[0];
      uint8_t payloadType = (uint8_t)response[1];
      uint16_t payloadLen = ((uint8_t)response[2] << 8) | (uint8_t)response[3];

      // Validate VISCA over IP header
      if (msgType == 0x01 && (payloadType == 0x11 || payloadType == 0x10)) {
        // isViscaOverIP = true;
        // Extract VISCA payload
        if (response.length() >= 8 + payloadLen) {
          viscaResponse = response.substr(8, payloadLen);
          VERYHIGH_MSG("VISCA: Found VISCA over IP header - payload length: %u", payloadLen);
        } else {
          HIGH_MSG("VISCA: Invalid VISCA over IP response - payload length mismatch");
          return false;
        }
      }
    }

    // Validate VISCA response
    uint8_t responseType;
    if (!Device::isValidViscaResponse(viscaResponse, responseType)) {
      HIGH_MSG("VISCA: Invalid VISCA response format");
      return false;
    }

    // Parse response type
    switch (responseType) {
      case 0x50: // Information return
        VERYHIGH_MSG("VISCA: Got %s response",
                     responseType == 0x90     ? "information"
                       : responseType == 0x60 ? "error"
                       : responseType == 0x41 ? "ACK/completion"
                                              : "unknown");
        break;
      case 0x51: // Error message
        WARN_MSG("VISCA: Got error response");
        return false;
      case 0x41: // ACK
      case 0x42: // Completion
        VERYHIGH_MSG("VISCA: Got ACK/completion response");
        break;
      default: WARN_MSG("VISCA: Unknown response type: 0x%02X", responseType); return false;
    }

    // Generate unique device ID based on host
    if (info.host.empty()) {
      WARN_MSG("VISCA: No host address available in device info");
      return false;
    }

    info.id = "visca_" + info.host;
    info.name = "VISCA PTZ Camera at " + info.host;
    info.manufacturer = "Generic VISCA";
    info.model = "VISCA PTZ Camera";
    info.status = "discovered";

    // Core capabilities
    info.hasPTZ = true;
    info.hasAudio = false;
    info.hasMetadata = false;

    // Protocol capabilities
    ::Device::ProtocolConfig viscaConfig;
    viscaConfig.type = "visca";
    viscaConfig.address = info.host;
    viscaConfig.capabilities.hasPTZ = true;
    viscaConfig.capabilities.hasAudio = false;
    viscaConfig.capabilities.hasMetadata = false;
    viscaConfig.capabilities.hasVideo = false;
    viscaConfig.capabilities.hasRecording = false;
    viscaConfig.capabilities.hasWebControl = false;
    viscaConfig.capabilities.hasTally = false;
    viscaConfig.capabilities.hasRTPMulticast = false;
    viscaConfig.capabilities.hasRTPTCP = false;
    viscaConfig.capabilities.hasRTPRTSPTCP = false;

    // Add supported commands
    viscaConfig.capabilities.supportedCommands = {"pan_tilt",     "zoom", "focus", "iris",          "whitebalance",
                                                  "exposure",     "gain", "flip",  "preset_store",  "preset_recall",
                                                  "preset_clear", "home", "stop",  "absolute_move", "direct_zoom"};

    info.protocols["visca"] = viscaConfig;

    INFO_MSG("VISCA: Successfully parsed device info for %s", info.host.c_str());
    return true;
  }

  // Async Discovery Implementation
  bool Discovery::startAsyncDiscovery(::Device::DiscoveryCallback callback, uint32_t timeoutMs) {
    if (asyncRunning) {
      HIGH_MSG("VISCA: Async discovery already running");
      return false;
    }

    INFO_MSG("VISCA: Starting async discovery (timeout: %u ms)", timeoutMs);

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

    INFO_MSG("VISCA: Stopping async discovery");
    asyncRunning = false;
    asyncCallback = nullptr;
    discoverySocket.reset();
    responseBuffers.clear();
  }

  void Discovery::setupAsyncSocket() {
    try {
      // Create UDP discovery socket
      discoverySocket.reset(new Socket::UDPConnection(false, AF_INET));
      discoverySocket->data.allocate(4096); // Buffer for responses

      if (!discoverySocket->bind(0)) {
        ERROR_MSG("VISCA: Failed to bind async discovery socket");
        stopAsyncDiscovery();
        return;
      }

      // Enable broadcast for discovery traffic
      int broadcastEnable = 1;
      if (setsockopt(discoverySocket->getSock(), SOL_SOCKET, SO_BROADCAST, &broadcastEnable, sizeof(broadcastEnable)) < 0) {
        WARN_MSG("VISCA: Failed to enable broadcast on async discovery socket");
      }

      // Enumerate interfaces
      struct ifaddrs *ifaddr = nullptr;
      if (getifaddrs(&ifaddr) == -1) { WARN_MSG("VISCA: Failed to get network interfaces"); }
      std::unique_ptr<struct ifaddrs, decltype(&freeifaddrs)> ifaddrGuard(ifaddr, freeifaddrs);

      // VISCA legacy and over-IP ports
      std::vector<uint16_t> ports = {52381, 1259};

      // VISCA IF_Clear command (over-IP discovery packet header) and legacy inquiry
      const uint8_t legacyInquiry[] = {0x81, 0x09, 0x04, 0x00, 0xFF};
      uint8_t viscaCmd[] = {0x01, 0x00, 0x01, 0xFF};
      auto overIpPacket = formatViscaOverIPPacket(viscaCmd, sizeof(viscaCmd));

      auto sendToBroadcast = [&](const std::string & bcast) {
        for (uint16_t port : ports) {
          discoverySocket->SetDestination(bcast, port);
          if (port == 52381) {
            discoverySocket->SendNow(std::string(reinterpret_cast<const char *>(overIpPacket.data()), overIpPacket.size()));
          } else {
            discoverySocket->SendNow(reinterpret_cast<const char *>(legacyInquiry), sizeof(legacyInquiry));
          }
          HIGH_MSG("VISCA: Sent async discovery to %s port %u", bcast.c_str(), port);
        }
      };

      if (ifaddr) {
        for (struct ifaddrs *ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
          if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) { continue; }
          if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_BROADCAST) == 0 || (ifa->ifa_flags & IFF_LOOPBACK)) {
            continue;
          }
          // Prefer specific broadcast address if available
          std::string bcastStr = "255.255.255.255";
          if (ifa->ifa_broadaddr && ifa->ifa_broadaddr->sa_family == AF_INET) {
            char bcastBuf[INET_ADDRSTRLEN];
            void *baddr = &((struct sockaddr_in *)ifa->ifa_broadaddr)->sin_addr;
            if (inet_ntop(AF_INET, baddr, bcastBuf, INET_ADDRSTRLEN)) { bcastStr = bcastBuf; }
          }
          sendToBroadcast(bcastStr);
        }
      } else {
        // Fallback single send if no interfaces obtained
        sendToBroadcast("255.255.255.255");
      }

    } catch (const std::exception & e) {
      ERROR_MSG("VISCA: Failed to setup async discovery socket: %s", e.what());
      stopAsyncDiscovery();
    }
  }

  int Discovery::getSocket() const {
    return (discoverySocket && asyncRunning) ? discoverySocket->getSock() : -1;
  }

  void Discovery::processSocketData() {
    if (!asyncRunning || !discoverySocket) return;

    try {
      while (discoverySocket->Receive()) {
        const Socket::Address & remoteAddr = discoverySocket->getRemoteAddr();
        std::string sender = remoteAddr.host();

        size_t respSize = discoverySocket->data.size();
        if (respSize == 0) continue;

        responseBuffers[sender].append(static_cast<const char *>((const void *)discoverySocket->data), respSize);
        std::string & accum = responseBuffers[sender];

        if (accum.size() < 5) { continue; }

        ::Device::DeviceInfo info;
        // Set host/id before parse so parser can rely on it
        info.host = sender;
        info.id = "visca_" + sender;
        if (parseDiscoveryResponse(accum, info)) {
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
          responseBuffers.erase(sender);
        } else {
          bool alreadyFound = false;
          for (const auto & dev : asyncDevices) {
            if (dev.host == info.host) {
              alreadyFound = true;
              break;
            }
          }
          if (alreadyFound || accum.size() > 4096) { responseBuffers.erase(sender); }
        }
      }
    } catch (const std::exception & e) { WARN_MSG("VISCA: Error processing async socket data: %s", e.what()); }
  }

  bool Discovery::checkAsyncTimeout() {
    if (!asyncRunning) return false;

    if (asyncTimeoutMs == 0) return false; // No timeout

    uint64_t currentTime =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();

    if (currentTime - asyncStartTime >= asyncTimeoutMs) {
      INFO_MSG("VISCA: Async discovery timeout reached, found %zu devices", asyncDevices.size());
      stopAsyncDiscovery();
      return true;
    }

    return false;
  }

} // namespace VISCA
