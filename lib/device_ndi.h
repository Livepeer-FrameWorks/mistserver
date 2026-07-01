/**
 * @file ndi.h
 * @brief NDI (Network Device Interface) implementation for device discovery and I/O operations
 *
 * The NDI runtime is loaded at runtime via dlopen() + NDIlib_v5_load() (no link-time
 * dependency on libndi); see lib/device_ndi.cpp. NDI::initialize() loads the runtime and
 * is reference-counted and thread-safe — every successful initialize() must be paired with
 * a deinitialize(). When the NDI runtime is not installed, initialize() returns false and
 * all NDI functionality is a graceful no-op (callers must check the return value).
 */

#pragma once

#include "device.h"

#include <cstddef>
#include <cstdlib>
#include <Processing.NDI.Lib.h>
#include <string>
#include <vector>

const char *NDIlib_FourCC_type_string(NDIlib_FourCC_video_type_e fourcc);

namespace NDI {
  enum class Error {
    None,
    NotConnected,
    InvalidCommand,
    InvalidResponse,
    Timeout,
    SocketError,
    InvalidState,
    NDIError
  };

  struct ErrorInfo {
      Error code;
      std::string message;

      ErrorInfo() : code(Error::None) {}
      ErrorInfo(Error e, const std::string & msg = "") : code(e), message(msg) {}
  };

  /**
   * @brief Initialize the NDI library
   * @return true if initialization was successful, false otherwise
   * @note Must be called before using any NDI functionality
   */
  bool initialize();

  /**
   * @brief Clean up and shut down the NDI library
   * @note Must be called when NDI functionality is no longer needed
   */
  void deinitialize();

  /**
   * @brief Access the dynamically-loaded NDI runtime function table
   * @return Pointer to the loaded NDIlib_v5 table, or nullptr if the NDI runtime is unavailable
   * @note Populated by initialize(); the NDI runtime is dlopen()'d at runtime (no link-time dependency)
   */
  const ::NDIlib_v5 *lib();

  /**
   * @brief Get the loaded NDI runtime version string
   * @return Version string, or "unknown" if the runtime is not loaded
   */
  const char *version();

  /**
   * @brief Video format information for NDI streams
   */
  struct VideoFormat {
      uint32_t width;
      uint32_t height;
      uint32_t fps_n;
      uint32_t fps_d;
      float frameRate; // Calculated from fps_n/fps_d
      std::string colorFormat;
  };

  /**
   * @brief Audio format information for NDI streams
   */
  struct AudioFormat {
      uint32_t sampleRate;
      uint32_t channels;
      uint32_t bitDepth;
      std::string format; // Audio format string (e.g. "PCM", "AAC")
  };

  /**
   * @brief Capabilities of an NDI source including supported formats
   */
  struct SourceCapabilities {
      bool hasVideo;
      bool hasAudio;
      bool hasPTZ;
      bool hasMetadata;
      bool hasWebControl;
      bool hasRecording;
      bool hasTally;
      std::vector<VideoFormat> videoFormats;
      std::vector<AudioFormat> audioFormats;
      std::vector<std::string> supportedColorFormats;
      std::vector<std::string> supportedBandwidths;
      std::string webControlUrl;

      SourceCapabilities()
        : hasVideo(false), hasAudio(false), hasPTZ(false), hasMetadata(false), hasWebControl(false),
          hasRecording(false), hasTally(false) {}
  };

  /**
   * @brief Configuration options for NDI input
   */
  struct InputConfig {
      std::string colorFormat = "UYVY_BGRA"; // "UYVY_BGRA", "UYVY_RGBA", "BGRX_BGRA"
      std::string bandwidth = "highest"; // "lowest", "low", "highest"
      bool allowVideoFields = true;
  };

  // Forward declarations for NDI SDK types
  typedef ::NDIlib_source_t NDIlib_source_t;
  typedef ::NDIlib_video_frame_v2_t NDIlib_video_frame_v2_t;
  typedef ::NDIlib_audio_frame_v3_t NDIlib_audio_frame_v3_t;
  typedef ::NDIlib_metadata_frame_t NDIlib_metadata_frame_t;
  typedef ::NDIlib_frame_type_e NDIlib_frame_type_e;
  typedef ::NDIlib_find_instance_t NDIlib_find_instance_t;
  typedef ::NDIlib_recv_instance_t NDIlib_recv_instance_t;
  typedef ::NDIlib_send_instance_t NDIlib_send_instance_t;
  typedef ::NDIlib_recv_create_v3_t NDIlib_recv_create_v3_t;

  /**
   * @brief Main NDI device class for both discovery and I/O operations
   *
   * This class has three distinct usage patterns:
   *
   * 1. Discovery Mode (used by controller_discovery.cpp):
   *    - connect() - Creates a receiver for querying capabilities
   *    - queryCapabilities() - Gets supported formats
   *    - disconnect() - Cleans up resources
   *
   * 2. Input Mode (used by input_ndi.cpp):
   *    - open(source) - Stores NDI source info
   *    - setConfig(config) - Creates receiver with specified config
   *    - receive* methods - Gets frames
   *    - disconnect() - Cleans up resources
   *
   * 3. Output Mode (used by output_ndi.cpp):
   *    - openOutput() - Creates NDI sender
   *    - send* methods - Sends frames
   *    - disconnect() - Cleans up resources
   */
  class Device : public ::Device::Base {
    private:
      NDIlib_recv_instance_t receiver;
      NDIlib_send_instance_t sender;
      bool connected;
      std::string name;
      std::string address;
      NDIlib_recv_create_v3_t receiverConfig;

      uint16_t port;
      ErrorInfo lastError;
      uint32_t connectionTimeout;
      NDIlib_source_t ownedSource;
      bool hasSource = false;
      SourceCapabilities sourceCaps;

      // Performance tracking (per-instance, not static)
      mutable uint64_t perfLastCheckTime = 0;
      mutable NDIlib_recv_performance_t perfLastTotal = {};
      mutable NDIlib_recv_performance_t perfLastDropped = {};

      // Internal capability queries
      struct DeviceCapabilities {
          bool hasPTZ = false;
          bool hasRecording = false;
          bool hasTally = false;
          bool hasWebControl = false;
          std::vector<std::string> supportedCommands;
      };

      DeviceCapabilities getDeviceCaps() const;
      SourceCapabilities getSourceCaps() const;

      // Helper functions
      int getConnectionCount() const;

    public:
      /**
       * @brief Construct a new NDI device
       * @note Initializes finder and receiver with default settings
       */
      Device();

      /**
       * @brief Destroy the NDI device and clean up resources
       */
      virtual ~Device() override;

      // Base class virtual methods
      virtual bool connect() override;
      virtual bool isConnected() const override;
      virtual void disconnect() override;
      virtual bool sendPTZ(const ::Device::PTZCommand & cmd) override;
      virtual ::Device::DeviceInfo queryCapabilities() const override;

      // Source management
      /**
       * @brief Set the NDI source to connect to
       * @param source Pointer to NDIlib_source_t containing source info
       * @return true if source was set successfully, false otherwise
       */
      bool setSource(const NDIlib_source_t *source);

      /**
       * @brief Configure the NDI receiver settings
       * @param config InputConfig struct with desired settings
       * @return true if configuration was successful, false otherwise
       */
      bool setConfig(const InputConfig & config);

      /**
       * @brief Get the current NDI source
       * @return Pointer to current NDIlib_source_t, or nullptr if none set
       */
      const NDIlib_source_t *getCurrentSource() const { return hasSource ? &ownedSource : nullptr; }

      void setName(const std::string & n) { name = n; }

      // Frame handling
      /**
       * @brief Receive any available NDI frame (video, audio, or metadata)
       * @param video Output parameter for received video frame
       * @param audio Output parameter for received audio frame
       * @param metadata Output parameter for received metadata frame
       * @return NDIlib_frame_type_e indicating type of frame received
       * @note Caller must free the received frame using appropriate free method
       */
      NDIlib_frame_type_e
        receiveAny(NDIlib_video_frame_v2_t & video, NDIlib_audio_frame_v3_t & audio, NDIlib_metadata_frame_t & metadata);

      /**
       * @brief Free a video frame's resources
       * @param frame Video frame to free
       */
      void freeVideo(NDIlib_video_frame_v2_t & frame);

      /**
       * @brief Receive an audio frame
       * @param frame Output parameter for received audio frame
       * @return true if frame was received, false otherwise
       */
      bool receiveAudio(NDIlib_audio_frame_v2_t & frame);

      /**
       * @brief Free an audio frame's resources (v3)
       * @param frame Audio frame to free
       */
      void freeAudio(NDIlib_audio_frame_v3_t & frame);

      /**
       * @brief Free an audio frame's resources (v2)
       * @param frame Audio frame to free
       */
      void freeAudio(NDIlib_audio_frame_v2_t & frame);

      /**
       * @brief Free a metadata frame's resources
       * @param frame Metadata frame to free
       */
      void freeMetadata(NDIlib_metadata_frame_t & frame);

      // Metadata handling
      /**
       * @brief Add metadata to the NDI connection
       * @param metadata Metadata frame to add
       * @return true if successful, false otherwise
       * @note Uses NDIlib_recv_add_connection_metadata
       */
      bool addConnectionMetadata(const NDIlib_metadata_frame_t & metadata);

      /**
       * @brief Clear all connection metadata
       * @return true if successful, false otherwise
       * @note Uses NDIlib_recv_clear_connection_metadata
       */
      bool clearMetadata();

      /**
       * @brief Send metadata back to the NDI source (for receivers)
       * @param metadata Metadata frame to send
       * @return true if successful, false otherwise
       * @note Uses NDIlib_recv_send_metadata
       */
      bool sendReceiverMetadata(const NDIlib_metadata_frame_t & metadata);

      /**
       * @brief Send metadata to NDI receivers (for senders)
       * @param metadata Metadata frame to send
       * @return true if successful, false otherwise
       * @note Uses NDIlib_send_send_metadata
       */
      bool sendMetadata(const NDIlib_metadata_frame_t & metadata);

      // Tally state
      /**
       * @brief Set the tally state for this connection
       * @param tally Tally state to set
       * @return true if successful, false otherwise
       * @note Uses NDIlib_recv_set_tally
       */
      bool setTally(const NDIlib_tally_t & tally);

      // Performance monitoring
      /**
       * @brief Get performance statistics for video and audio
       * @param video_fps Output parameter for video frames per second
       * @param dropped_fps Output parameter for dropped frames per second
       * @param audio_fps Output parameter for audio frames per second
       * @return true if statistics were retrieved, false otherwise
       * @note Uses NDIlib_recv_get_performance
       */
      bool getPerformanceInfo(float & video_fps, float & dropped_fps, float & audio_fps) const;

      /**
       * @brief Get current frame queue information
       * @param video_frames Output parameter for queued video frames
       * @param audio_frames Output parameter for queued audio frames
       * @param metadata_frames Output parameter for queued metadata frames
       * @return true if queue info was retrieved, false otherwise
       * @note Uses NDIlib_recv_get_queue
       */
      bool getQueueInfo(int & video_frames, int & audio_frames, int & metadata_frames) const;

      // Output mode
      /**
       * @brief Create an NDI sender instance
       * @return true if sender was created successfully, false otherwise
       * @note Uses NDIlib_send_create with clocking disabled
       */
      bool openOutput();

      /**
       * @brief Send a video frame asynchronously
       * @param frame Video frame to send, or nullptr to synchronize
       * @return true if successful, false otherwise
       * @note Uses send_send_video_async_v2 (the v2 async API, via the dynamically-loaded table)
       * @note The frame data must remain valid until the next sync point:
       *       - Another call to sendVideoAsync
       *       - A call to sendVideo
       *       - A call to disconnect
       */
      bool sendVideoAsync(const NDIlib_video_frame_v2_t *frame);

      /**
       * @brief Send a video frame
       * @param frame Video frame to send
       * @return true if successful, false otherwise
       * @note Uses NDIlib_send_send_video_v2
       */
      bool sendVideo(const NDIlib_video_frame_v2_t & frame);

      /**
       * @brief Send an audio frame
       * @param frame Audio frame to send
       * @return true if successful, false otherwise
       * @note Uses NDIlib_send_send_audio_v3
       */
      bool sendAudio(const NDIlib_audio_frame_v3_t & frame);

      // PTZ control
      /**
       * @brief Check if device supports PTZ control
       * @return true if PTZ is supported, false otherwise
       * @note Uses NDIlib_recv_ptz_is_supported
       */
      bool hasPTZControl() const;

      /**
       * @brief Control PTZ zoom
       * @param zoom Zoom speed (-1.0 to +1.0)
       * @return true if successful, false otherwise
       * @note Uses NDIlib_recv_ptz_zoom_speed
       */
      bool ptzZoom(float zoom);

      /**
       * @brief Control PTZ pan and tilt
       * @param pan Pan speed (-1.0 to +1.0)
       * @param tilt Tilt speed (-1.0 to +1.0)
       * @return true if successful, false otherwise
       * @note Uses NDIlib_recv_ptz_pan_tilt_speed
       */
      bool ptzPanTilt(float pan, float tilt);

      /**
       * @brief Stop all PTZ movement
       * @return true if successful, false otherwise
       * @note Sets all PTZ speeds to 0
       */
      bool ptzStop();

      // Recording control
      /**
       * @brief Check if device supports recording control
       * @return true if recording control is supported, false otherwise
       */
      bool hasRecordingControl() const;

      /**
       * @brief Start recording
       * @return true if successful, false otherwise
       */
      bool startRecording();

      /**
       * @brief Stop recording
       * @return true if successful, false otherwise
       */
      bool stopRecording();

      /**
       * @brief Check if device is currently recording
       * @return true if recording, false otherwise
       */
      bool isRecording() const;

      std::string getWebControlUrl() const;

      // Operator overloads
      operator bool() const { return isConnected(); }
      std::string getConnectionStatus() const { return isConnected() ? "Connected" : "Disconnected"; }
      // Error handling
      Error getLastError() const { return lastError.code; }
      std::string getErrorMessage() const { return lastError.message; }
      void clearError() { lastError = ErrorInfo(); }
  };

  /**
   * @brief Discovery class for finding NDI sources on the network
   */
  class Discovery : public ::Device::Discovery {
    public:
      /**
       * @brief Construct a new NDI discovery instance
       */
      Discovery();

      /**
       * @brief Destroy the discovery instance
       */
      ~Discovery() override;

      /**
       * @brief Get list of available NDI sources
       * @param count Output parameter for number of sources found
       * @return Pointer to array of NDIlib_source_t, nullptr if none found
       * @note Uses NDIlib_find_get_current_sources
       */
      const NDIlib_source_t *getSources(uint32_t & count);

      /**
       * @brief Get current NDI sources without waiting/blocking
       * @param count Output parameter for number of sources found
       * @return Pointer to array of NDIlib_source_t, nullptr if none found
       * @note Uses NDIlib_find_get_current_sources and never blocks
       */
      const NDIlib_source_t *getCurrentSourcesNoWait(uint32_t & count);

      /**
       * @brief Connects a given node to an NDI source by name
       * @param node The node to connect to
       * @param target The name or address of the NDI source to connect to
       * @return true if connection was successful, false otherwise
       */
      bool connectByName(Device & node, const std::string & target);

      /**
       * @brief Discover NDI devices on the network
       * @param timeoutMs Timeout in milliseconds
       * @return Vector of Device::Info for discovered devices
       * @note Uses NDIlib_find_wait_for_sources and NDIlib_find_get_current_sources
       */

      /**
       * @brief Get the protocol name
       * @return "ndi" as the protocol identifier
       */
      std::string getProtocolName() const override { return "ndi"; }

      /**
       * @brief Send a PTZ command to a device
       * @param deviceId Device ID to send command to
       * @param cmd PTZ command to send
       * @return true if command was sent successfully, false otherwise
       */
      bool sendCommand(const std::string & deviceId, const ::Device::PTZCommand & cmd) override;
      std::unique_ptr<::Device::Base> createDevice(const ::Device::DeviceInfo & info) const override;

      // Async discovery overrides
      bool startAsyncDiscovery(::Device::DiscoveryCallback callback, uint32_t timeoutMs = 0) override;
      void stopAsyncDiscovery() override;
      bool isAsyncDiscoveryRunning() const override { return asyncRunning; }

      // Method to check for new sources (called by timer callback)
      bool checkForNewSources();

      // Tally control — manages persistent receivers so the sender sees tally state
      bool setTally(const std::string & deviceId, bool program, bool preview);
      void cleanupTallyReceivers();

    private:
      NDIlib_find_instance_t finder;

      // Async discovery state
      ::Device::DiscoveryCallback asyncCallback;
      bool asyncRunning = false;
      std::vector<::Device::DeviceInfo> asyncDevices;
      uint64_t asyncStartTime = 0;
      uint32_t asyncTimeoutMs = 0;
      uint32_t lastSourceCount = 0; // Track source count changes

      // Persistent receivers for tally state (sender reads tally from connected receivers)
      std::map<std::string, std::unique_ptr<Device>> tallyReceivers_;
  };
} // namespace NDI
