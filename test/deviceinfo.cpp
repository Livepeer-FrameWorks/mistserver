#include <mist/device.h>

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace {

  bool expect(bool condition, const std::string & message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << std::endl;
      return false;
    }
    return true;
  }

  bool hasValue(const std::vector<std::string> & items, const std::string & needle) {
    return std::find(items.begin(), items.end(), needle) != items.end();
  }

  bool testEmptyJsonArrays() {
    Device::DeviceInfo info;
    JSON::Value json = info.toJSON();

    bool ok = true;
    ok &= expect(json.isMember("snapshotUri"), "snapshotUri field should be present");
    ok &= expect(json["features"].isArray() && json["features"].size() == 0, "features should serialize as empty array");
    ok &= expect(json["ptzFeatures"].isArray() && json["ptzFeatures"].size() == 0, "ptzFeatures should serialize as empty array");
    ok &= expect(json["streams"].isArray() && json["streams"].size() == 0, "streams should serialize as empty array");
    ok &= expect(json["protocols"].isArray() && json["protocols"].size() == 0, "protocols should serialize as empty array");
    ok &= expect(json["analytics"].isObject(), "analytics should serialize as object");
    ok &= expect(json["analytics"]["supportedModules"].isArray() && json["analytics"]["supportedModules"].size() == 0,
                 "analytics.supportedModules should serialize as empty array");
    ok &= expect(json["analytics"]["activeModules"].isArray() && json["analytics"]["activeModules"].size() == 0,
                 "analytics.activeModules should serialize as empty array");
    ok &= expect(json["analytics"]["supportedRules"].isArray() && json["analytics"]["supportedRules"].size() == 0,
                 "analytics.supportedRules should serialize as empty array");
    ok &= expect(json["analytics"]["activeRules"].isArray() && json["analytics"]["activeRules"].size() == 0,
                 "analytics.activeRules should serialize as empty array");
    ok &= expect(json["analytics"]["objectClassifications"].isArray() && json["analytics"]["objectClassifications"].size() == 0,
                 "analytics.objectClassifications should serialize as empty array");
    return ok;
  }

  bool testAnalyticsRoundtripPopulated() {
    Device::DeviceInfo info;
    info.id = "cam-123";
    info.host = "10.0.0.20";
    info.snapshotUri = "http://10.0.0.20/snapshot.jpg";
    info.hasPTZ = true;
    info.features.push_back("ptz");
    info.ptzFeatures.push_back("pan_tilt");

    Device::ProtocolConfig onvif;
    onvif.type = "onvif";
    onvif.address = "10.0.0.20";
    onvif.port = 8899;
    onvif.username = "admin";
    onvif.password = "secret";
    onvif.capabilities.hasPTZ = true;
    onvif.capabilities.supportedTransports.push_back("rtsp");
    onvif.capabilities.supportedCommands.push_back("continuous_move");
    onvif.capabilities.supportedFormats.push_back("h264");
    info.protocols[onvif.type] = onvif;

    Device::AnalyticsModule module;
    module.name = "object_detector";
    module.type = "classification";
    module.active = true;
    module.parameters.push_back("sensitivity");

    Device::AnalyticsRule rule;
    rule.name = "line_crossing";
    rule.type = "event";
    rule.active = true;
    rule.parameters.push_back("line_a");

    info.analytics.hasAnalytics = true;
    info.analytics.analyticsServiceUrl = "http://10.0.0.20/onvif/analytics";
    info.analytics.supportedModules.push_back(module);
    info.analytics.activeModules.push_back(module);
    info.analytics.supportedRules.push_back(rule);
    info.analytics.activeRules.push_back(rule);
    info.analytics.objectClassifications.push_back("person");
    info.analytics.objectClassifications.push_back("vehicle");

    Device::DeviceInfo parsed = Device::DeviceInfo::fromJSON(info.toJSON());

    bool ok = true;
    ok &= expect(parsed.snapshotUri == info.snapshotUri, "snapshotUri should round-trip");
    ok &= expect(parsed.analytics.hasAnalytics, "analytics.hasAnalytics should round-trip");
    ok &= expect(parsed.analytics.analyticsServiceUrl == info.analytics.analyticsServiceUrl, "analyticsServiceUrl should round-trip");
    ok &= expect(parsed.analytics.supportedModules.size() == 1, "supportedModules size mismatch");
    ok &= expect(parsed.analytics.supportedModules[0].parameters.size() == 1, "module parameters size mismatch");
    ok &= expect(parsed.analytics.supportedRules.size() == 1, "supportedRules size mismatch");
    ok &= expect(parsed.analytics.objectClassifications.size() == 2, "objectClassifications size mismatch");
    ok &= expect(parsed.protocols.count("onvif") == 1, "onvif protocol should round-trip");
    ok &= expect(parsed.protocols["onvif"].capabilities.supportedCommands.size() == 1, "supportedCommands should round-trip");
    return ok;
  }

  bool testAnalyticsPartialFromJson() {
    JSON::Value json;
    json["id"] = "cam-456";
    json["analytics"]["hasAnalytics"] = true;
    json["analytics"]["supportedModules"][uint32_t(0)]["name"] = "motion";
    json["analytics"]["supportedModules"][uint32_t(0)]["parameters"].append("threshold");

    Device::DeviceInfo parsed = Device::DeviceInfo::fromJSON(json);

    bool ok = true;
    ok &= expect(parsed.id == "cam-456", "id should parse");
    ok &= expect(parsed.analytics.hasAnalytics, "analytics.hasAnalytics should parse");
    ok &= expect(parsed.analytics.analyticsServiceUrl.empty(), "analyticsServiceUrl should stay default");
    ok &= expect(parsed.analytics.supportedModules.size() == 1, "supportedModules should parse");
    ok &= expect(parsed.analytics.supportedModules[0].name == "motion", "supported module name should parse");
    ok &= expect(parsed.analytics.supportedModules[0].active == false, "missing active should default to false");
    ok &= expect(parsed.analytics.activeModules.empty(), "activeModules should default empty");
    ok &= expect(parsed.analytics.supportedRules.empty(), "supportedRules should default empty");
    ok &= expect(parsed.analytics.objectClassifications.empty(), "objectClassifications should default empty");
    return ok;
  }

  bool testStreamRoundtrip() {
    Device::DeviceInfo info;
    Device::StreamEndpoint stream;
    stream.name = "main";
    stream.uri = "rtsp://10.0.0.20/stream1";
    stream.protocol = "rtsp";
    stream.transport = "tcp";
    stream.format = "h264";
    stream.width = 1920;
    stream.height = 1080;
    stream.fps = 30;
    stream.bitrate = 4096;
    stream.profile = "high";
    stream.address = "10.0.0.20";
    stream.port = 554;
    stream.path = "/stream1";
    info.streams.push_back(stream);

    Device::DeviceInfo parsed = Device::DeviceInfo::fromJSON(info.toJSON());

    bool ok = true;
    ok &= expect(parsed.streams.size() == 1, "streams size should round-trip");
    ok &= expect(parsed.streams[0].width == 1920, "stream width should round-trip");
    ok &= expect(parsed.streams[0].height == 1080, "stream height should round-trip");
    ok &= expect(parsed.streams[0].fps == 30, "stream fps should round-trip");
    ok &= expect(parsed.streams[0].bitrate == 4096, "stream bitrate should round-trip");
    ok &= expect(parsed.streams[0].port == 554, "stream port should round-trip");
    return ok;
  }

  bool testProtocolCapabilityFlagsRoundtrip() {
    Device::DeviceInfo info;
    Device::ProtocolConfig visca;
    visca.type = "visca";
    visca.address = "10.0.0.40";
    visca.port = 52381;
    visca.capabilities.hasPTZ = true;
    visca.capabilities.hasAudio = false;
    visca.capabilities.hasMetadata = false;
    visca.capabilities.hasVideo = false;
    visca.capabilities.hasRecording = false;
    visca.capabilities.hasWebControl = false;
    visca.capabilities.hasTally = false;
    visca.capabilities.hasRTPMulticast = false;
    visca.capabilities.hasRTPTCP = false;
    visca.capabilities.hasRTPRTSPTCP = false;
    visca.capabilities.supportedTransports.push_back("tcp");
    visca.capabilities.supportedCommands.push_back("absolute_move");
    visca.capabilities.supportedFormats.push_back("control");
    info.protocols[visca.type] = visca;

    Device::DeviceInfo parsed = Device::DeviceInfo::fromJSON(info.toJSON());

    bool ok = true;
    ok &= expect(parsed.protocols.count("visca") == 1, "visca protocol should round-trip");
    const Device::ProtocolConfig & roundtrip = parsed.protocols["visca"];
    ok &= expect(roundtrip.capabilities.hasPTZ, "hasPTZ flag should round-trip");
    ok &= expect(roundtrip.capabilities.supportedTransports.size() == 1, "supportedTransports size mismatch");
    ok &= expect(hasValue(roundtrip.capabilities.supportedCommands, "absolute_move"), "supportedCommands should include absolute_move");
    ok &= expect(hasValue(roundtrip.capabilities.supportedFormats, "control"), "supportedFormats should include control");
    return ok;
  }

  bool testProtocolHelperMethods() {
    Device::DeviceInfo info;
    bool ok = true;
    ok &= expect(!info.supportsProtocol("visca"), "supportsProtocol should be false before addProtocol");

    info.addProtocol("visca", "10.0.0.40", 52381, "user", "pass");
    ok &= expect(info.supportsProtocol("visca"), "supportsProtocol should be true after addProtocol");
    ok &= expect(info.protocols["visca"].address == "10.0.0.40", "addProtocol should set address");
    ok &= expect(info.protocols["visca"].port == 52381, "addProtocol should set port");
    ok &= expect(info.protocols["visca"].username == "user", "addProtocol should set username");
    ok &= expect(info.protocols["visca"].password == "pass", "addProtocol should set password");
    return ok;
  }

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "Usage: " << argv[0] << " <test_case>" << std::endl;
    return 2;
  }

  const std::string testCase = argv[1];
  if (testCase == "empty_json_arrays") { return testEmptyJsonArrays() ? 0 : 1; }
  if (testCase == "analytics_roundtrip_populated") { return testAnalyticsRoundtripPopulated() ? 0 : 1; }
  if (testCase == "analytics_partial_from_json") { return testAnalyticsPartialFromJson() ? 0 : 1; }
  if (testCase == "stream_roundtrip") { return testStreamRoundtrip() ? 0 : 1; }
  if (testCase == "protocol_capability_flags_roundtrip") { return testProtocolCapabilityFlagsRoundtrip() ? 0 : 1; }
  if (testCase == "protocol_helper_methods") { return testProtocolHelperMethods() ? 0 : 1; }

  std::cerr << "Unknown test case: " << testCase << std::endl;
  return 2;
}
