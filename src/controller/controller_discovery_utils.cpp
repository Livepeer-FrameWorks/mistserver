#include "controller_discovery.h"

#include <mist/url.h>

#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <string>

namespace Controller {

  namespace {
    std::string trimCopy(const std::string & input) {
      size_t a = 0, b = input.size();
      while (a < b && isspace((unsigned char)input[a])) ++a;
      while (b > a && isspace((unsigned char)input[b - 1])) --b;
      return input.substr(a, b - a);
    }

    bool isRawIPv6Literal(const std::string & input) {
      if (input.find("://") != std::string::npos) return false;
      if (input.size() > 1 && input[0] == '/' && input[1] == '/') return false;
      if (input.find('[') != std::string::npos || input.find(']') != std::string::npos) return false;
      if (input.find('/') != std::string::npos || input.find('?') != std::string::npos ||
          input.find('#') != std::string::npos) {
        return false;
      }
      return std::count(input.begin(), input.end(), ':') > 1;
    }

    std::string normalizeHost(std::string host) {
      host = trimCopy(host);
      if (host.size() > 2 && host.front() == '[' && host.back() == ']') { host = host.substr(1, host.size() - 2); }
      if (host.size() > 7 && host.substr(0, 7) == "::ffff:") {
        std::string tail = host.substr(7);
        if (tail.find('.') != std::string::npos) return tail;
      }
      size_t fpos = host.rfind(":ffff:");
      if (fpos != std::string::npos) {
        std::string tail = host.substr(fpos + 6);
        if (tail.find('.') != std::string::npos) return trimCopy(tail);
      }
      // Canonicalize a literal IPv6 address (compression + lowercasing) via a pton/ntop round-trip
      // so that different textual forms of the same address (e.g. 2001:db8::1 vs its expanded form)
      // produce the same dedup key. Preserve any zone id.
      if (host.find(':') != std::string::npos) {
        std::string addr = host, zone;
        size_t pct = addr.find('%');
        if (pct != std::string::npos) {
          zone = addr.substr(pct);
          addr = addr.substr(0, pct);
        }
        struct in6_addr a6;
        if (inet_pton(AF_INET6, addr.c_str(), &a6) == 1) {
          char buf[INET6_ADDRSTRLEN];
          if (inet_ntop(AF_INET6, &a6, buf, sizeof(buf))) { return std::string(buf) + zone; }
        }
      }
      std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return std::tolower(c); });
      return host;
    }

    std::string parseHostWithUrl(const std::string & address) {
      try {
        HTTP::URL url(address);
        if (!url.host.empty()) return normalizeHost(url.host);
      } catch (...) {}
      return "";
    }
  } // namespace

  std::string extractCleanIP(const std::string & address) {
    std::string cleanAddr = trimCopy(address);
    if (cleanAddr.empty()) return "";

    size_t startParen = cleanAddr.find('(');
    size_t endParen = cleanAddr.find(')');
    if (startParen != std::string::npos && endParen != std::string::npos && endParen > startParen) {
      size_t commaPos = cleanAddr.find(',', startParen);
      if (commaPos != std::string::npos && commaPos < endParen) {
        return extractCleanIP(cleanAddr.substr(commaPos + 1, endParen - commaPos - 1));
      }
    }

    if (isRawIPv6Literal(cleanAddr)) return normalizeHost(cleanAddr);

    std::string host = parseHostWithUrl(cleanAddr);
    if (!host.empty()) return host;
    if (cleanAddr.find("://") == std::string::npos &&
        !(cleanAddr.size() > 1 && cleanAddr[0] == '/' && cleanAddr[1] == '/') && !isRawIPv6Literal(cleanAddr)) {
      host = parseHostWithUrl("//" + cleanAddr);
      if (!host.empty()) return host;
    }

    size_t pathPos = cleanAddr.find_first_of("/?#");
    if (pathPos != std::string::npos) { cleanAddr.resize(pathPos); }
    cleanAddr = normalizeHost(cleanAddr);
    if (std::count(cleanAddr.begin(), cleanAddr.end(), ':') == 1) { cleanAddr.resize(cleanAddr.find(':')); }
    return trimCopy(cleanAddr);
  }

  std::string canonicalDeviceKey(const ::Device::DeviceInfo & dev) {
    for (const auto & proto : dev.protocols) {
      std::string key = protocolEndpointId(proto.first, proto.second, dev);
      if (!key.empty()) return key;
    }
    return dev.id;
  }

  std::string protocolEndpointId(const std::string & protocol, const ::Device::ProtocolConfig & config,
                                 const ::Device::DeviceInfo & dev) {
    if (!config.endpointId.empty()) return config.endpointId;

    std::string proto = protocol.empty() ? config.type : protocol;
    std::transform(proto.begin(), proto.end(), proto.begin(), [](unsigned char c) { return std::tolower(c); });
    if (proto.empty()) return "";

    if (proto == "ndi") {
      // NDI identity must be supplied explicitly as the exact SDK source name.
      return "";
    }

    std::string host = extractCleanIP(config.address.empty() ? dev.host : config.address);
    if (host.empty()) return "";
    if (host.find(':') != std::string::npos) host = "[" + host + "]";

    uint16_t port = config.port;
    if (!port) {
      if (proto == "onvif") port = config.scheme == "https" ? 443 : 80;
      if (proto == "visca") port = 52381;
    }
    if (proto == "onvif") {
      const std::string scheme = config.scheme.empty() ? "http" : config.scheme;
      const std::string path = config.path.empty() ? "/onvif/device_service" : config.path;
      return "onvif:" + scheme + "://" + host + ":" + std::to_string(port) + path;
    }
    return proto + ":" + host + ":" + std::to_string(port);
  }

  void updateDeviceInfo(::Device::DeviceInfo & device, const ::Device::DeviceInfo & devInfo) {
    HIGH_MSG("Updating device info for %s", device.name.c_str());

    VERYHIGH_MSG("Incoming device has %zu protocols:", devInfo.protocols.size());
    for (const auto & proto : devInfo.protocols) {
      VERYHIGH_MSG("  - Protocol: %s, Address: %s, Port: %d", proto.first.c_str(), proto.second.address.c_str(),
                   proto.second.port);
    }

    VERYHIGH_MSG("Existing device has %zu protocols:", device.protocols.size());
    for (const auto & proto : device.protocols) {
      VERYHIGH_MSG("  - Protocol: %s, Address: %s, Port: %d", proto.first.c_str(), proto.second.address.c_str(),
                   proto.second.port);
    }

    if (device.name.empty() || device.name == "Unknown") { device.name = devInfo.name; }
    if (device.manufacturer.empty() || device.manufacturer == "Unknown") { device.manufacturer = devInfo.manufacturer; }
    if (device.model.empty() || device.model == "Unknown") { device.model = devInfo.model; }
    if (device.firmwareVersion.empty()) { device.firmwareVersion = devInfo.firmwareVersion; }
    if (device.serialNumber.empty()) { device.serialNumber = devInfo.serialNumber; }

    device.hasAudio |= devInfo.hasAudio;
    device.hasMetadata |= devInfo.hasMetadata;
    device.hasPTZ |= devInfo.hasPTZ;

    for (const auto & newStream : devInfo.streams) {
      bool found = false;
      for (auto & existingStream : device.streams) {
        if (existingStream.uri == newStream.uri) {
          found = true;
          if (newStream.width) existingStream.width = newStream.width;
          if (newStream.height) existingStream.height = newStream.height;
          if (newStream.fps) existingStream.fps = newStream.fps;
          if (newStream.bitrate) existingStream.bitrate = newStream.bitrate;
          if (!newStream.format.empty()) existingStream.format = newStream.format;
          if (!newStream.resolution.empty()) existingStream.resolution = newStream.resolution;
          if (!newStream.framerate.empty()) existingStream.framerate = newStream.framerate;
          break;
        }
      }
      if (!found) {
        INFO_MSG("Adding new stream: %s", newStream.uri.c_str());
        device.streams.push_back(newStream);
      }
    }

    for (const auto & newProto : devInfo.protocols) {
      VERYHIGH_MSG("Processing protocol %s from new device info", newProto.first.c_str());
      auto & existingProto = device.protocols[newProto.first];

      if (existingProto.type.empty()) {
        HIGH_MSG("Adding new protocol %s to device", newProto.first.c_str());
        existingProto = newProto.second;
        continue;
      }

      VERYHIGH_MSG("Merging protocol %s", newProto.first.c_str());
      if (existingProto.type.empty()) { existingProto.type = newProto.second.type; }
      // The endpoint identity is stable, but its advertised network location may change
      // (notably for an NDI source that keeps the same exact source name).
      if (!newProto.second.address.empty()) { existingProto.address = newProto.second.address; }
      if (newProto.second.port != 0) { existingProto.port = newProto.second.port; }
      if (!newProto.second.username.empty()) { existingProto.username = newProto.second.username; }
      if (!newProto.second.password.empty()) { existingProto.password = newProto.second.password; }
      if (existingProto.endpointId.empty()) existingProto.endpointId = newProto.second.endpointId;
      if (!newProto.second.scheme.empty()) existingProto.scheme = newProto.second.scheme;
      if (existingProto.associationSource != "manual") {
        if (!newProto.second.transport.empty()) existingProto.transport = newProto.second.transport;
        if (!newProto.second.framing.empty()) existingProto.framing = newProto.second.framing;
      }
      if (!newProto.second.path.empty()) existingProto.path = newProto.second.path;
      if (!newProto.second.alternateEndpoints.empty()) {
        existingProto.alternateEndpoints = newProto.second.alternateEndpoints;
      }
      // TLS policy and a custom CA are configuration, not discovery data. They are
      // changed only through the explicit camera configuration API.
      if (!newProto.second.tlsStatus.empty()) existingProto.tlsStatus = newProto.second.tlsStatus;
      existingProto.lastError = newProto.second.lastError;
      if (!newProto.second.associationSource.empty()) {
        existingProto.associationSource = newProto.second.associationSource;
      }

      auto & existingCaps = existingProto.capabilities;
      const auto & newCaps = newProto.second.capabilities;

      existingCaps.hasPTZ |= newCaps.hasPTZ;
      existingCaps.hasAudio |= newCaps.hasAudio;
      existingCaps.hasMetadata |= newCaps.hasMetadata;
      existingCaps.hasVideo |= newCaps.hasVideo;
      existingCaps.hasRecording |= newCaps.hasRecording;
      existingCaps.hasWebControl |= newCaps.hasWebControl;
      existingCaps.hasTally |= newCaps.hasTally;
      existingCaps.hasRTPMulticast |= newCaps.hasRTPMulticast;
      existingCaps.hasRTPTCP |= newCaps.hasRTPTCP;
      existingCaps.hasRTPRTSPTCP |= newCaps.hasRTPRTSPTCP;

      for (const auto & format : newCaps.supportedFormats) {
        if (std::find(existingCaps.supportedFormats.begin(), existingCaps.supportedFormats.end(), format) ==
            existingCaps.supportedFormats.end()) {
          existingCaps.supportedFormats.push_back(format);
        }
      }
      for (const auto & transport : newCaps.supportedTransports) {
        if (std::find(existingCaps.supportedTransports.begin(), existingCaps.supportedTransports.end(), transport) ==
            existingCaps.supportedTransports.end()) {
          existingCaps.supportedTransports.push_back(transport);
        }
      }
      for (const auto & cmd : newCaps.supportedCommands) {
        if (std::find(existingCaps.supportedCommands.begin(), existingCaps.supportedCommands.end(), cmd) ==
            existingCaps.supportedCommands.end()) {
          existingCaps.supportedCommands.push_back(cmd);
        }
      }
      for (const auto & resolution : newCaps.supportedResolutions) {
        if (std::find(existingCaps.supportedResolutions.begin(), existingCaps.supportedResolutions.end(), resolution) ==
            existingCaps.supportedResolutions.end()) {
          existingCaps.supportedResolutions.push_back(resolution);
        }
      }
      for (const auto & framerate : newCaps.supportedFramerates) {
        if (std::find(existingCaps.supportedFramerates.begin(), existingCaps.supportedFramerates.end(), framerate) ==
            existingCaps.supportedFramerates.end()) {
          existingCaps.supportedFramerates.push_back(framerate);
        }
      }
      for (const auto & audioFormat : newCaps.supportedAudioFormats) {
        if (std::find(existingCaps.supportedAudioFormats.begin(), existingCaps.supportedAudioFormats.end(), audioFormat) ==
            existingCaps.supportedAudioFormats.end()) {
          existingCaps.supportedAudioFormats.push_back(audioFormat);
        }
      }
    }

    HIGH_MSG("Device %s now has %zu protocols", device.name.c_str(), device.protocols.size());
    for (const auto & proto : device.protocols) {
      HIGH_MSG("  - Protocol: %s, Address: %s, Port: %d", proto.first.c_str(), proto.second.address.c_str(),
               proto.second.port);
    }

    for (const auto & feature : devInfo.features) {
      if (std::find(device.features.begin(), device.features.end(), feature) == device.features.end()) {
        device.features.push_back(feature);
      }
    }

    for (const auto & feature : devInfo.ptzFeatures) {
      if (std::find(device.ptzFeatures.begin(), device.ptzFeatures.end(), feature) == device.ptzFeatures.end()) {
        device.ptzFeatures.push_back(feature);
      }
    }

    if (!devInfo.webControlUrl.empty()) device.webControlUrl = devInfo.webControlUrl;
    if (!devInfo.snapshotUri.empty()) device.snapshotUri = devInfo.snapshotUri;
    if (devInfo.analytics.hasAnalytics || !devInfo.analytics.analyticsServiceUrl.empty() ||
        !devInfo.analytics.supportedModules.empty() || !devInfo.analytics.activeModules.empty() ||
        !devInfo.analytics.supportedRules.empty() || !devInfo.analytics.activeRules.empty() ||
        !devInfo.analytics.objectClassifications.empty()) {
      device.analytics = devInfo.analytics;
    }

    if (device.status.empty() ||
        ((device.status == "disconnected" || device.status.find("Error") != std::string::npos) &&
         !devInfo.status.empty() && devInfo.status.find("Error") == std::string::npos)) {
      device.status = devInfo.status;
    }
  }

  static float clampF(float val, float lo, float hi) {
    return std::max(lo, std::min(hi, val));
  }

  bool createPTZCommand(const std::string & commandName, const JSON::Value & args, ::Device::PTZCommand & cmd) {
    if (commandName == "pan_tilt") {
      cmd.action = ::Device::PTZAction::PanTilt;
      if (args.isMember("pan") && args.isMember("tilt")) {
        cmd.args["pan"] = clampF(static_cast<float>(args["pan"].asInt()) / 100.0f, -1.0f, 1.0f);
        cmd.args["tilt"] = clampF(static_cast<float>(args["tilt"].asInt()) / 100.0f, -1.0f, 1.0f);
        return true;
      }
    } else if (commandName == "zoom") {
      cmd.action = ::Device::PTZAction::Zoom;
      if (args.isMember("speed")) {
        cmd.args["zoom"] = clampF(static_cast<float>(args["speed"].asInt()) / 100.0f, -1.0f, 1.0f);
        return true;
      }
    } else if (commandName == "stop") {
      cmd.action = ::Device::PTZAction::Stop;
      return true;
    } else if (commandName == "home") {
      cmd.action = ::Device::PTZAction::Home;
      return true;
    } else if (commandName == "preset") {
      cmd.action = ::Device::PTZAction::Preset;
      if (!args.isMember("token") || !args["token"].isString() || args["token"].asString().empty()) return false;
      cmd.presetToken = args["token"].asString();
      cmd.storePreset = args.isMember("store") && args["store"].asBool();
      return true;
    } else if (commandName == "focus") {
      cmd.action = ::Device::PTZAction::Focus;
      if (args.isMember("mode")) cmd.args["mode"] = static_cast<float>(args["mode"].asInt());
      if (args.isMember("value")) cmd.args["value"] = static_cast<float>(args["value"].asInt());
      return true;
    } else if (commandName == "iris") {
      cmd.action = ::Device::PTZAction::Iris;
      if (args.isMember("value")) {
        cmd.args["value"] = clampF(static_cast<float>(args["value"].asInt()), 0.0f, 100.0f);
        return true;
      }
    } else if (commandName == "white_balance") {
      cmd.action = ::Device::PTZAction::WhiteBalance;
      if (args.isMember("mode")) cmd.args["mode"] = static_cast<float>(args["mode"].asInt());
      if (args.isMember("color_temp")) cmd.args["color_temp"] = static_cast<float>(args["color_temp"].asInt());
      return true;
    }
    return false;
  }

} // namespace Controller
