#include "device.h"

namespace Device {

  JSON::Value DeviceInfo::toJSON() const {
    JSON::Value json;
    json["id"] = id;
    json["host"] = host;
    json["name"] = name;
    json["manufacturer"] = manufacturer;
    json["model"] = model;
    json["firmwareVersion"] = firmwareVersion;
    json["serialNumber"] = serialNumber;
    json["status"] = status;
    json["hasPTZ"] = hasPTZ;
    json["hasAudio"] = hasAudio;
    json["hasMetadata"] = hasMetadata;
    json["webControlUrl"] = webControlUrl;
    json["snapshotUri"] = snapshotUri;

    // Serialize features array (ensure empty serializes as [])
    JSON::Value featuresArray;
    for (const auto & feature : features) { featuresArray.append(feature); }
    if (features.empty()) {
      featuresArray.append();
      featuresArray.shrink(0);
    }
    json["features"] = featuresArray;

    // Serialize PTZ features array (ensure empty serializes as [])
    JSON::Value ptzFeaturesArray;
    for (const auto & feature : ptzFeatures) { ptzFeaturesArray.append(feature); }
    if (ptzFeatures.empty()) {
      ptzFeaturesArray.append();
      ptzFeaturesArray.shrink(0);
    }
    json["ptzFeatures"] = ptzFeaturesArray;

    if (!ptzProtocol.empty()) json["ptzProtocol"] = ptzProtocol;
    if (defaultStream >= 0) json["defaultStream"] = defaultStream;

    // Serialize streams array (ensure empty serializes as [])
    JSON::Value streamsArray;
    for (const auto & stream : streams) {
      JSON::Value streamObj;
      streamObj["uri"] = stream.uri;
      streamObj["protocol"] = stream.protocol;
      streamObj["format"] = stream.format;
      streamObj["transport"] = stream.transport;
      streamObj["name"] = stream.name;
      streamObj["width"] = (int)stream.width;
      streamObj["height"] = (int)stream.height;
      streamObj["fps"] = (int)stream.fps;
      streamObj["bitrate"] = (int)stream.bitrate;
      streamObj["profile"] = stream.profile;
      streamObj["address"] = stream.address;
      streamObj["port"] = (int)stream.port;
      streamObj["path"] = stream.path;
      streamsArray.append(streamObj);
    }
    if (streams.empty()) {
      streamsArray.append();
      streamsArray.shrink(0);
    }
    json["streams"] = streamsArray;

    // Serialize protocols map (ensure empty serializes as [])
    JSON::Value protocolsArray;
    for (const auto & pair : protocols) {
      JSON::Value protoObj;
      protoObj["type"] = pair.second.type;
      protoObj["address"] = pair.second.address;
      protoObj["port"] = (int)pair.second.port;
      protoObj["username"] = pair.second.username;
      protoObj["password"] = pair.second.password;

      // Serialize protocol capabilities
      JSON::Value capsObj;
      capsObj["hasPTZ"] = pair.second.capabilities.hasPTZ;
      capsObj["hasAudio"] = pair.second.capabilities.hasAudio;
      capsObj["hasMetadata"] = pair.second.capabilities.hasMetadata;
      capsObj["hasVideo"] = pair.second.capabilities.hasVideo;
      capsObj["hasRecording"] = pair.second.capabilities.hasRecording;
      capsObj["hasWebControl"] = pair.second.capabilities.hasWebControl;
      capsObj["hasTally"] = pair.second.capabilities.hasTally;
      capsObj["hasRTPMulticast"] = pair.second.capabilities.hasRTPMulticast;
      capsObj["hasRTPTCP"] = pair.second.capabilities.hasRTPTCP;
      capsObj["hasRTPRTSPTCP"] = pair.second.capabilities.hasRTPRTSPTCP;

      // Serialize arrays (ensure empty serializes as [])
      JSON::Value transportsArray;
      for (const auto & transport : pair.second.capabilities.supportedTransports) { transportsArray.append(transport); }
      if (pair.second.capabilities.supportedTransports.empty()) {
        transportsArray.append();
        transportsArray.shrink(0);
      }
      capsObj["supportedTransports"] = transportsArray;

      JSON::Value commandsArray;
      for (const auto & command : pair.second.capabilities.supportedCommands) { commandsArray.append(command); }
      if (pair.second.capabilities.supportedCommands.empty()) {
        commandsArray.append();
        commandsArray.shrink(0);
      }
      capsObj["supportedCommands"] = commandsArray;

      JSON::Value formatsArray;
      for (const auto & format : pair.second.capabilities.supportedFormats) { formatsArray.append(format); }
      if (pair.second.capabilities.supportedFormats.empty()) {
        formatsArray.append();
        formatsArray.shrink(0);
      }
      capsObj["supportedFormats"] = formatsArray;

      protoObj["capabilities"] = capsObj;
      protocolsArray.append(protoObj);
    }
    if (protocols.empty()) {
      protocolsArray.append();
      protocolsArray.shrink(0);
    }
    json["protocols"] = protocolsArray;

    // Serialize analytics capabilities
    JSON::Value analyticsObj;
    analyticsObj["hasAnalytics"] = analytics.hasAnalytics;
    analyticsObj["analyticsServiceUrl"] = analytics.analyticsServiceUrl;

    JSON::Value supportedModulesArray;
    for (const auto & mod : analytics.supportedModules) {
      JSON::Value modObj;
      modObj["name"] = mod.name;
      modObj["type"] = mod.type;
      modObj["active"] = mod.active;
      JSON::Value paramsArray;
      for (const auto & p : mod.parameters) { paramsArray.append(p); }
      if (mod.parameters.empty()) {
        paramsArray.append();
        paramsArray.shrink(0);
      }
      modObj["parameters"] = paramsArray;
      supportedModulesArray.append(modObj);
    }
    if (analytics.supportedModules.empty()) {
      supportedModulesArray.append();
      supportedModulesArray.shrink(0);
    }
    analyticsObj["supportedModules"] = supportedModulesArray;

    JSON::Value activeModulesArray;
    for (const auto & mod : analytics.activeModules) {
      JSON::Value modObj;
      modObj["name"] = mod.name;
      modObj["type"] = mod.type;
      modObj["active"] = mod.active;
      JSON::Value paramsArray;
      for (const auto & p : mod.parameters) { paramsArray.append(p); }
      if (mod.parameters.empty()) {
        paramsArray.append();
        paramsArray.shrink(0);
      }
      modObj["parameters"] = paramsArray;
      activeModulesArray.append(modObj);
    }
    if (analytics.activeModules.empty()) {
      activeModulesArray.append();
      activeModulesArray.shrink(0);
    }
    analyticsObj["activeModules"] = activeModulesArray;

    JSON::Value supportedRulesArray;
    for (const auto & rule : analytics.supportedRules) {
      JSON::Value ruleObj;
      ruleObj["name"] = rule.name;
      ruleObj["type"] = rule.type;
      ruleObj["active"] = rule.active;
      JSON::Value paramsArray;
      for (const auto & p : rule.parameters) { paramsArray.append(p); }
      if (rule.parameters.empty()) {
        paramsArray.append();
        paramsArray.shrink(0);
      }
      ruleObj["parameters"] = paramsArray;
      supportedRulesArray.append(ruleObj);
    }
    if (analytics.supportedRules.empty()) {
      supportedRulesArray.append();
      supportedRulesArray.shrink(0);
    }
    analyticsObj["supportedRules"] = supportedRulesArray;

    JSON::Value activeRulesArray;
    for (const auto & rule : analytics.activeRules) {
      JSON::Value ruleObj;
      ruleObj["name"] = rule.name;
      ruleObj["type"] = rule.type;
      ruleObj["active"] = rule.active;
      JSON::Value paramsArray;
      for (const auto & p : rule.parameters) { paramsArray.append(p); }
      if (rule.parameters.empty()) {
        paramsArray.append();
        paramsArray.shrink(0);
      }
      ruleObj["parameters"] = paramsArray;
      activeRulesArray.append(ruleObj);
    }
    if (analytics.activeRules.empty()) {
      activeRulesArray.append();
      activeRulesArray.shrink(0);
    }
    analyticsObj["activeRules"] = activeRulesArray;

    JSON::Value classificationsArray;
    for (const auto & cls : analytics.objectClassifications) { classificationsArray.append(cls); }
    if (analytics.objectClassifications.empty()) {
      classificationsArray.append();
      classificationsArray.shrink(0);
    }
    analyticsObj["objectClassifications"] = classificationsArray;

    json["analytics"] = analyticsObj;

    return json;
  }

  DeviceInfo DeviceInfo::fromJSON(const JSON::Value & json) {
    DeviceInfo info;
    if (json.isMember("id")) info.id = json["id"].asString();
    if (json.isMember("host")) info.host = json["host"].asString();
    if (json.isMember("name")) info.name = json["name"].asString();
    if (json.isMember("manufacturer")) info.manufacturer = json["manufacturer"].asString();
    if (json.isMember("model")) info.model = json["model"].asString();
    if (json.isMember("firmwareVersion")) info.firmwareVersion = json["firmwareVersion"].asString();
    if (json.isMember("serialNumber")) info.serialNumber = json["serialNumber"].asString();
    if (json.isMember("status")) info.status = json["status"].asString();
    if (json.isMember("hasPTZ")) info.hasPTZ = json["hasPTZ"].asBool();
    if (json.isMember("hasAudio")) info.hasAudio = json["hasAudio"].asBool();
    if (json.isMember("hasMetadata")) info.hasMetadata = json["hasMetadata"].asBool();
    if (json.isMember("webControlUrl")) info.webControlUrl = json["webControlUrl"].asString();
    if (json.isMember("snapshotUri")) info.snapshotUri = json["snapshotUri"].asString();

    // Parse features array
    if (json.isMember("features")) {
      const JSON::Value & featuresList = json["features"];
      for (size_t i = 0; i < featuresList.size(); i++) { info.features.push_back(featuresList[i].asString()); }
    }

    // Parse PTZ features array
    if (json.isMember("ptzFeatures")) {
      const JSON::Value & ptzFeaturesList = json["ptzFeatures"];
      for (size_t i = 0; i < ptzFeaturesList.size(); i++) { info.ptzFeatures.push_back(ptzFeaturesList[i].asString()); }
    }

    if (json.isMember("ptzProtocol")) info.ptzProtocol = json["ptzProtocol"].asString();
    if (json.isMember("defaultStream")) info.defaultStream = json["defaultStream"].asInt();

    // Parse streams array
    if (json.isMember("streams")) {
      const JSON::Value & streamList = json["streams"];
      for (size_t i = 0; i < streamList.size(); i++) {
        const JSON::Value & streamObj = streamList[i];
        StreamEndpoint stream;
        if (streamObj.isMember("uri")) stream.uri = streamObj["uri"].asString();
        if (streamObj.isMember("protocol")) stream.protocol = streamObj["protocol"].asString();
        if (streamObj.isMember("format")) stream.format = streamObj["format"].asString();
        if (streamObj.isMember("transport")) stream.transport = streamObj["transport"].asString();
        if (streamObj.isMember("name")) stream.name = streamObj["name"].asString();
        if (streamObj.isMember("width")) stream.width = streamObj["width"].asInt();
        if (streamObj.isMember("height")) stream.height = streamObj["height"].asInt();
        if (streamObj.isMember("fps")) stream.fps = streamObj["fps"].asInt();
        if (streamObj.isMember("bitrate")) stream.bitrate = streamObj["bitrate"].asInt();
        if (streamObj.isMember("profile")) stream.profile = streamObj["profile"].asString();
        if (streamObj.isMember("address")) stream.address = streamObj["address"].asString();
        if (streamObj.isMember("port")) stream.port = streamObj["port"].asInt();
        if (streamObj.isMember("path")) stream.path = streamObj["path"].asString();
        info.streams.push_back(stream);
      }
    }

    // Parse protocols array
    if (json.isMember("protocols")) {
      const JSON::Value & protocolsList = json["protocols"];
      for (size_t i = 0; i < protocolsList.size(); i++) {
        const JSON::Value & protoObj = protocolsList[i];
        ProtocolConfig config;
        if (protoObj.isMember("type")) config.type = protoObj["type"].asString();
        if (protoObj.isMember("address")) config.address = protoObj["address"].asString();
        if (protoObj.isMember("port")) config.port = protoObj["port"].asInt();
        if (protoObj.isMember("username")) config.username = protoObj["username"].asString();
        if (protoObj.isMember("password")) config.password = protoObj["password"].asString();

        // Parse protocol capabilities
        if (protoObj.isMember("capabilities")) {
          const JSON::Value & capsObj = protoObj["capabilities"];
          if (capsObj.isMember("hasPTZ")) config.capabilities.hasPTZ = capsObj["hasPTZ"].asBool();
          if (capsObj.isMember("hasAudio")) config.capabilities.hasAudio = capsObj["hasAudio"].asBool();
          if (capsObj.isMember("hasMetadata")) config.capabilities.hasMetadata = capsObj["hasMetadata"].asBool();
          if (capsObj.isMember("hasVideo")) config.capabilities.hasVideo = capsObj["hasVideo"].asBool();
          if (capsObj.isMember("hasRecording")) config.capabilities.hasRecording = capsObj["hasRecording"].asBool();
          if (capsObj.isMember("hasWebControl")) config.capabilities.hasWebControl = capsObj["hasWebControl"].asBool();
          if (capsObj.isMember("hasTally")) config.capabilities.hasTally = capsObj["hasTally"].asBool();
          if (capsObj.isMember("hasRTPMulticast"))
            config.capabilities.hasRTPMulticast = capsObj["hasRTPMulticast"].asBool();
          if (capsObj.isMember("hasRTPTCP")) config.capabilities.hasRTPTCP = capsObj["hasRTPTCP"].asBool();
          if (capsObj.isMember("hasRTPRTSPTCP")) config.capabilities.hasRTPRTSPTCP = capsObj["hasRTPRTSPTCP"].asBool();

          // Parse arrays
          if (capsObj.isMember("supportedTransports")) {
            const JSON::Value & transportsArray = capsObj["supportedTransports"];
            for (size_t j = 0; j < transportsArray.size(); j++) {
              config.capabilities.supportedTransports.push_back(transportsArray[j].asString());
            }
          }

          if (capsObj.isMember("supportedCommands")) {
            const JSON::Value & commandsArray = capsObj["supportedCommands"];
            for (size_t j = 0; j < commandsArray.size(); j++) {
              config.capabilities.supportedCommands.push_back(commandsArray[j].asString());
            }
          }

          if (capsObj.isMember("supportedFormats")) {
            const JSON::Value & formatsArray = capsObj["supportedFormats"];
            for (size_t j = 0; j < formatsArray.size(); j++) {
              config.capabilities.supportedFormats.push_back(formatsArray[j].asString());
            }
          }
        }

        info.protocols[config.type] = config;
      }
    }

    // Parse analytics capabilities
    if (json.isMember("analytics")) {
      const JSON::Value & aObj = json["analytics"];
      if (aObj.isMember("hasAnalytics")) info.analytics.hasAnalytics = aObj["hasAnalytics"].asBool();
      if (aObj.isMember("analyticsServiceUrl"))
        info.analytics.analyticsServiceUrl = aObj["analyticsServiceUrl"].asString();

      auto parseModules = [](const JSON::Value & arr, std::vector<AnalyticsModule> & out) {
        for (size_t i = 0; i < arr.size(); i++) {
          AnalyticsModule mod;
          if (arr[i].isMember("name")) mod.name = arr[i]["name"].asString();
          if (arr[i].isMember("type")) mod.type = arr[i]["type"].asString();
          if (arr[i].isMember("active")) mod.active = arr[i]["active"].asBool();
          if (arr[i].isMember("parameters")) {
            for (size_t j = 0; j < arr[i]["parameters"].size(); j++) {
              mod.parameters.push_back(arr[i]["parameters"][j].asString());
            }
          }
          out.push_back(mod);
        }
      };
      auto parseRules = [](const JSON::Value & arr, std::vector<AnalyticsRule> & out) {
        for (size_t i = 0; i < arr.size(); i++) {
          AnalyticsRule rule;
          if (arr[i].isMember("name")) rule.name = arr[i]["name"].asString();
          if (arr[i].isMember("type")) rule.type = arr[i]["type"].asString();
          if (arr[i].isMember("active")) rule.active = arr[i]["active"].asBool();
          if (arr[i].isMember("parameters")) {
            for (size_t j = 0; j < arr[i]["parameters"].size(); j++) {
              rule.parameters.push_back(arr[i]["parameters"][j].asString());
            }
          }
          out.push_back(rule);
        }
      };

      if (aObj.isMember("supportedModules")) parseModules(aObj["supportedModules"], info.analytics.supportedModules);
      if (aObj.isMember("activeModules")) parseModules(aObj["activeModules"], info.analytics.activeModules);
      if (aObj.isMember("supportedRules")) parseRules(aObj["supportedRules"], info.analytics.supportedRules);
      if (aObj.isMember("activeRules")) parseRules(aObj["activeRules"], info.analytics.activeRules);

      if (aObj.isMember("objectClassifications")) {
        for (size_t i = 0; i < aObj["objectClassifications"].size(); i++) {
          info.analytics.objectClassifications.push_back(aObj["objectClassifications"][i].asString());
        }
      }
    }

    return info;
  }

} // namespace Device
