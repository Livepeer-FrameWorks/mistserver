#include "controller_discovery.h"

#include <mist/url.h>

#include <algorithm>
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
      if (input.find('/') != std::string::npos || input.find('?') != std::string::npos || input.find('#') != std::string::npos) {
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
    std::string key = extractCleanIP(dev.host);
    if (key.empty()) {
      for (const auto & proto : dev.protocols) {
        key = extractCleanIP(proto.second.address);
        if (!key.empty()) break;
      }
    }
    if (key.empty()) key = dev.id;
    return key;
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
      if (existingProto.address.empty()) { existingProto.address = newProto.second.address; }
      if (existingProto.port == 0) { existingProto.port = newProto.second.port; }
      if (!newProto.second.username.empty()) { existingProto.username = newProto.second.username; }
      if (!newProto.second.password.empty()) { existingProto.password = newProto.second.password; }

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

    if (device.status.empty() ||
        (device.status.find("Error") != std::string::npos && devInfo.status.find("Error") == std::string::npos)) {
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
      int presetVal = -1;
      if (args.isMember("preset")) {
        presetVal = args["preset"].asInt();
      } else if (args.isMember("index")) {
        presetVal = args["index"].asInt();
      }
      if (presetVal >= 0) {
        cmd.args["preset"] = clampF(static_cast<float>(presetVal), 0.0f, 255.0f);
        if (args.isMember("store")) { cmd.args["store"] = args["store"].asBool() ? 1.0f : 0.0f; }
        return true;
      }
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
