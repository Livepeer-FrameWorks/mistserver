/**
 * @file visca.h
 * @brief VISCA (Video System Control Architecture) protocol implementation
 *
 * This file implements the VISCA protocol for camera control, supporting both
 * traditional serial VISCA and VISCA over IP (TCP/UDP). The implementation
 * provides camera control functions including PTZ, focus, exposure, and other
 * camera settings.
 */

#pragma once

#include "device.h"
#include "socket.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <map>
#include <memory>
#include <net/if.h>
#include <string>
#include <vector>

namespace VISCA {

  // VISCA error codes
  enum class Error { None, NotConnected, InvalidCommand, InvalidResponse, Timeout, SocketError, InvalidState };

  struct ErrorInfo {
      Error code;
      std::string message;

      ErrorInfo() : code(Error::None) {}
      ErrorInfo(Error e, const std::string & msg = "") : code(e), message(msg) {}
      bool empty() const { return code == Error::None && message.empty(); }
  };

  // Transport type for VISCA over IP
  enum class ViscaTransport {
    TCP, // VISCA over IP using TCP
    UDP // VISCA over IP using UDP
  };

  enum class ViscaFraming { Raw, OverIP };

  // VISCA camera settings
  enum class WhiteBalanceMode { Auto, Indoor, Outdoor, OnePush, Manual, ATW, ColorTemp };

  enum class FocusMode { Auto, Manual, OnePush, Infinity, NearLimit, FarLimit };

  enum class ExposureMode { Auto, Manual, ShutterPriority, IrisPriority, Bright, SpotMeter };

  enum class GainMode { Auto, Manual };

  enum class ImageFlipMode { None, Horizontal, Vertical, Both };

  struct PTZStatus {
      int pan;
      int tilt;
      int zoom;
      bool moving;
      std::string error;
  };

  struct ViscaInfo {
      ViscaTransport transportType;
      std::string lastCommand;
      std::string lastError;
  };

  /**
   * @brief Main VISCA device control class
   *
   * Implements VISCA over IP protocol communication with cameras, supporting both
   * TCP and UDP transport modes. Transport, framing, address and port are explicit
   * endpoint properties and are never guessed or changed while connecting.
   */
  class Device : public ::Device::Base {
    private:
      std::string address;
      uint16_t port;
      ViscaTransport transportType;
      bool connected;
      std::string lastCommand;
      ErrorInfo lastError;
      uint32_t connectionTimeout;
      uint32_t sequenceNum;
      uint32_t activeSequence;
      ViscaFraming framingType;
      std::string receiveBuffer;
      std::string lastResponse;
      PTZStatus ptzStatus;

      // Socket connections
      std::unique_ptr<Socket::Connection> tcpConn;
      std::unique_ptr<Socket::UDPConnection> udpConn;

      // Private helper methods
      bool createSocket();
      bool createUDP();
      bool initialize();
      bool sendCommand(const uint8_t *cmd, size_t len, uint32_t timeoutMs = 1000);
      bool transact(const uint8_t *cmd, size_t len, bool inquiry, std::string *response, uint32_t timeoutMs);
      bool sendPacket(const uint8_t *payload, size_t len);
      bool receiveFrame(std::string &response, uint32_t timeoutMs);
      bool extractFrame(std::string &response);
      bool validateResponse(const std::string & response);

    public:
      Device(const std::string & addr = "", uint16_t p = 52381,
             ViscaTransport transport = ViscaTransport::UDP, ViscaFraming framing = ViscaFraming::OverIP);
      virtual ~Device() override;

      // Static helper for validating VISCA responses
      static bool isValidViscaResponse(const std::string & response, uint8_t & type) {
        if (response.length() < 3) return false;

        uint8_t header = (uint8_t)response[0];
        type = (uint8_t)response[1];

        // The low nibble is the VISCA socket number.
        return header == 0x90 && ((type & 0xF0) == 0x40 || (type & 0xF0) == 0x50 || (type & 0xF0) == 0x60);
      }
      static bool decodeOverIpPacket(const std::string &packet, uint32_t expectedSequence,
                                     std::string &payload) {
        if (packet.size() < 8) return false;
        const uint8_t *p = reinterpret_cast<const uint8_t *>(packet.data());
        const size_t payloadLen = (static_cast<size_t>(p[2]) << 8) | p[3];
        const uint32_t sequence = (static_cast<uint32_t>(p[4]) << 24) |
                                  (static_cast<uint32_t>(p[5]) << 16) |
                                  (static_cast<uint32_t>(p[6]) << 8) | p[7];
        if (p[0] != 0x01 || p[1] != 0x11 || sequence != expectedSequence ||
            payloadLen > 4096 || packet.size() != payloadLen + 8) return false;
        payload.assign(packet, 8, payloadLen);
        return true;
      }

      // Device::Base interface implementation
      bool connect() override;
      void disconnect() override;
      bool isConnected() const override;
      bool sendPTZ(const ::Device::PTZCommand & cmd) override;
      ::Device::DeviceInfo queryCapabilities() const override;

      // VISCA-specific methods
      bool setPreset(uint8_t presetId);
      bool gotoPreset(uint8_t presetId);
      bool removePreset(uint8_t presetId);
      bool presetRecall(const std::string & preset);
      bool presetStore(const std::string & preset);
      bool home();
      bool stop();
      bool panTilt(int pan, int tilt);
      bool zoom(int speed);
      bool getStatus(int & pan, int & tilt, int & zoom);
      const PTZStatus & getPTZStatus() const { return ptzStatus; }

      // Absolute positioning
      bool absoluteMove(int pan, int tilt, uint8_t speed = 0x10);
      bool directZoom(uint16_t position);

      // Position inquiries
      bool getZoomPosition(uint16_t & position);
      bool getFocusPosition(uint16_t & position);

      // Camera control methods
      bool setFocus(FocusMode mode, int value = 0);
      bool setIris(int value);
      bool setWhiteBalance(WhiteBalanceMode mode, int colorTemp = 0);
      bool setExposure(ExposureMode mode);
      bool setGain(GainMode mode, int value = 0);
      bool setImageFlip(ImageFlipMode mode);
      bool setBacklight(bool enabled);
      bool setPower(bool on);

      // Connection management
      bool reconnect(uint32_t retries = 3);
      void setConnectionTimeout(uint32_t ms) { connectionTimeout = ms; }
      bool setAddress(const std::string & newAddress, uint16_t newPort);
      bool setTransportType(ViscaTransport type);

      // Error handling
      Error getLastError() const { return lastError.code; }
      std::string getErrorMessage() const { return lastError.message; }
      void clearError() { lastError = ErrorInfo(); }

      // Helper methods
      std::string getAddress() const { return address; }
      uint16_t getPort() const { return port; }
      ViscaTransport getTransportType() const { return transportType; }
      ViscaFraming getFramingType() const { return framingType; }
  };

  /**
   * @brief VISCA device discovery class
   *
   * Implements device discovery for VISCA over IP cameras on the network.
   * Discovery is performed on UDP ports 52381 and 1259, with control connections
   * defaulting to port 52381 for discovered devices.
   */
  class Discovery : public ::Device::Discovery {
    public:
      Discovery();
      ~Discovery() override;

      /**
       * @brief Discover VISCA devices on the network
       * @param timeoutMs Timeout in milliseconds
       * @return Vector of Device::Info for discovered devices
       */

      bool sendCommand(const std::string & deviceId, const ::Device::PTZCommand & cmd) override;
      bool sendCommand(const ::Device::DeviceInfo &info, const ::Device::PTZCommand &cmd);
      std::unique_ptr<::Device::Base> createDevice(const ::Device::DeviceInfo & info) const override;
      std::string getProtocolName() const override { return "visca"; }

      // Async discovery overrides
      bool startAsyncDiscovery(::Device::DiscoveryCallback callback, uint32_t timeoutMs = 0) override;
      void stopAsyncDiscovery() override;
      bool isAsyncDiscoveryRunning() const override { return asyncRunning; }

      // Public method to get socket for event loop registration
      int getSocket() const;

      // Method to be called by event loop when socket has data
      void processSocketData();

      // Method to check if async discovery should timeout
      bool checkAsyncTimeout();

      // Tally control via backlight LED
      bool setTally(const std::string & deviceId, bool on);
      bool setTally(const ::Device::DeviceInfo &info, bool on);

    private:
      std::unique_ptr<Socket::UDPConnection> discoverySocket;
      bool createDiscoverySocket();
      bool parseDiscoveryResponse(const std::string & response, ::Device::DeviceInfo & info);

      // Async discovery state
      ::Device::DiscoveryCallback asyncCallback;
      bool asyncRunning = false;
      std::vector<::Device::DeviceInfo> asyncDevices;
      uint64_t asyncStartTime = 0;
      uint32_t asyncTimeoutMs = 0;

      // Socket management for event loop integration
      bool setupAsyncSocket();

      // Connection cache for PTZ command performance
      std::map<std::string, std::unique_ptr<Device>> connectedDevices;
  };

} // namespace VISCA
