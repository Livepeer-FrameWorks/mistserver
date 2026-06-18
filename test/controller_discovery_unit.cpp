#include "../src/controller/controller_discovery.h"

#include <algorithm>
#include <cmath>
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

  bool expectNear(float got, float want, const std::string & message) {
    if (std::fabs(got - want) > 0.0001f) {
      std::cerr << "FAIL: " << message << " got=" << got << " want=" << want << std::endl;
      return false;
    }
    return true;
  }

  bool hasValue(const std::vector<std::string> & items, const std::string & needle) {
    return std::find(items.begin(), items.end(), needle) != items.end();
  }

  bool testExtractCleanIPTable() {
    struct TestCase {
        std::string input;
        std::string want;
    };
    const std::vector<TestCase> cases = {
      {"", ""},
      {"http://10.0.0.1:8080/path", "10.0.0.1"},
      {"https://[2001:db8::1]:443/", "2001:db8::1"},
      {"https://[::ffff:192.168.10.20]/", "192.168.10.20"},
      {"Friendly Cam (Front, 172.16.0.9)", "172.16.0.9"},
      {"rtsp://camera.local:554/stream1", "camera.local"},
      {"camera.local:8554", "camera.local"},
      {"2001:db8::2", "2001:db8::2"},
      {"  10.0.0.5  ", "10.0.0.5"},
      {"::ffff:10.1.2.3", "10.1.2.3"},
    };

    bool ok = true;
    for (size_t i = 0; i < cases.size(); ++i) {
      const std::string got = Controller::extractCleanIP(cases[i].input);
      ok &= expect(got == cases[i].want, "extractCleanIP mismatch for case " + std::to_string(i));
    }
    return ok;
  }

  bool testUpdateDeviceInfoMerge() {
    Device::DeviceInfo existing;
    existing.name = "Unknown";
    existing.manufacturer = "";
    existing.model = "Unknown";
    existing.status = "Error: timeout";
    existing.features.push_back("ptz");
    existing.ptzFeatures.push_back("pan");
    Device::StreamEndpoint streamA;
    streamA.uri = "rtsp://cam/main";
    existing.streams.push_back(streamA);
    Device::ProtocolConfig onvifOld;
    onvifOld.type = "onvif";
    onvifOld.port = 0;
    onvifOld.username = "olduser";
    onvifOld.password = "oldpass";
    onvifOld.capabilities.supportedCommands.push_back("move");
    existing.protocols["onvif"] = onvifOld;

    Device::DeviceInfo incoming;
    incoming.name = "Lobby Cam";
    incoming.manufacturer = "Acme";
    incoming.model = "X1";
    incoming.firmwareVersion = "1.0.1";
    incoming.serialNumber = "SN-42";
    incoming.status = "connected";
    incoming.hasAudio = true;
    incoming.hasMetadata = true;
    incoming.hasPTZ = true;
    incoming.features.push_back("ptz");
    incoming.features.push_back("audio");
    incoming.ptzFeatures.push_back("pan");
    incoming.ptzFeatures.push_back("zoom");
    incoming.streams.push_back(streamA);
    Device::StreamEndpoint streamB;
    streamB.uri = "rtsp://cam/sub";
    incoming.streams.push_back(streamB);

    Device::ProtocolConfig onvifNew;
    onvifNew.type = "onvif";
    onvifNew.address = "10.1.1.20";
    onvifNew.port = 80;
    onvifNew.username = "newuser";
    onvifNew.capabilities.hasPTZ = true;
    onvifNew.capabilities.supportedCommands.push_back("move");
    onvifNew.capabilities.supportedCommands.push_back("stop");
    onvifNew.capabilities.supportedFormats.push_back("h264");
    incoming.protocols["onvif"] = onvifNew;

    Device::ProtocolConfig viscaNew;
    viscaNew.type = "visca";
    viscaNew.address = "10.1.1.20";
    viscaNew.port = 52381;
    incoming.protocols["visca"] = viscaNew;

    Controller::updateDeviceInfo(existing, incoming);

    bool ok = true;
    ok &= expect(existing.name == "Lobby Cam", "name should be filled from incoming");
    ok &= expect(existing.manufacturer == "Acme", "manufacturer should be filled from incoming");
    ok &= expect(existing.model == "X1", "model should be filled from incoming");
    ok &= expect(existing.firmwareVersion == "1.0.1", "firmware should be filled from incoming");
    ok &= expect(existing.serialNumber == "SN-42", "serial number should be filled from incoming");
    ok &= expect(existing.hasAudio && existing.hasMetadata && existing.hasPTZ, "flags should OR-merge");
    ok &= expect(existing.streams.size() == 2, "streams should deduplicate by URI");
    ok &= expect(existing.status == "connected", "non-error incoming status should replace existing error");
    ok &= expect(existing.protocols.count("visca") == 1, "new protocol should be added");
    ok &= expect(existing.protocols["onvif"].address == "10.1.1.20", "empty protocol address should be filled");
    ok &= expect(existing.protocols["onvif"].port == 80, "empty protocol port should be filled");
    ok &= expect(existing.protocols["onvif"].username == "newuser", "non-empty username should overwrite");
    ok &= expect(existing.protocols["onvif"].password == "oldpass", "empty incoming password should not overwrite");
    ok &= expect(existing.protocols["onvif"].capabilities.supportedCommands.size() == 2, "supportedCommands should deduplicate");
    ok &= expect(hasValue(existing.features, "audio"), "features should merge");
    ok &= expect(hasValue(existing.ptzFeatures, "zoom"), "ptzFeatures should merge");
    return ok;
  }

  bool testUpdateDeviceInfoStatusRule() {
    Device::DeviceInfo existing;
    existing.status = "connected";

    Device::DeviceInfo incoming;
    incoming.status = "Error: auth";

    Controller::updateDeviceInfo(existing, incoming);
    bool ok = true;
    ok &= expect(existing.status == "connected", "non-error status should not be replaced by error");

    existing.status = "";
    Controller::updateDeviceInfo(existing, incoming);
    ok &= expect(existing.status == "Error: auth", "empty status should be filled");
    return ok;
  }

  bool testCreatePTZCommandPanTiltZoomClamp() {
    bool ok = true;

    Device::PTZCommand panTilt;
    JSON::Value panTiltArgs;
    panTiltArgs["pan"] = 450;
    panTiltArgs["tilt"] = -450;
    ok &= expect(Controller::createPTZCommand("pan_tilt", panTiltArgs, panTilt), "pan_tilt command should parse");
    ok &= expect(panTilt.action == Device::PTZAction::PanTilt, "pan_tilt action should be set");
    ok &= expectNear(panTilt.args["pan"], 1.0f, "pan should clamp to +1");
    ok &= expectNear(panTilt.args["tilt"], -1.0f, "tilt should clamp to -1");

    Device::PTZCommand zoom;
    JSON::Value zoomArgs;
    zoomArgs["speed"] = -200;
    ok &= expect(Controller::createPTZCommand("zoom", zoomArgs, zoom), "zoom command should parse");
    ok &= expect(zoom.action == Device::PTZAction::Zoom, "zoom action should be set");
    ok &= expectNear(zoom.args["zoom"], -1.0f, "zoom should clamp to -1");
    return ok;
  }

  bool testCreatePTZCommandPresetAndOtherActions() {
    bool ok = true;

    Device::PTZCommand stop;
    JSON::Value noArgs;
    ok &= expect(Controller::createPTZCommand("stop", noArgs, stop), "stop should parse without args");
    ok &= expect(stop.action == Device::PTZAction::Stop, "stop action should be set");

    Device::PTZCommand home;
    ok &= expect(Controller::createPTZCommand("home", noArgs, home), "home should parse without args");
    ok &= expect(home.action == Device::PTZAction::Home, "home action should be set");

    Device::PTZCommand preset;
    JSON::Value presetArgs;
    presetArgs["index"] = 500;
    presetArgs["store"] = true;
    ok &= expect(Controller::createPTZCommand("preset", presetArgs, preset), "preset should parse from index");
    ok &= expectNear(preset.args["preset"], 255.0f, "preset should clamp to 255");
    ok &= expectNear(preset.args["store"], 1.0f, "store should map to 1.0");

    Device::PTZCommand badPreset;
    JSON::Value badPresetArgs;
    badPresetArgs["preset"] = -1;
    ok &= expect(!Controller::createPTZCommand("preset", badPresetArgs, badPreset), "negative preset should be rejected");

    Device::PTZCommand focus;
    JSON::Value focusArgs;
    focusArgs["mode"] = 2;
    focusArgs["value"] = 10;
    ok &= expect(Controller::createPTZCommand("focus", focusArgs, focus), "focus should parse");
    ok &= expect(focus.action == Device::PTZAction::Focus, "focus action should be set");
    ok &= expectNear(focus.args["mode"], 2.0f, "focus mode should be copied");
    ok &= expectNear(focus.args["value"], 10.0f, "focus value should be copied");

    Device::PTZCommand iris;
    JSON::Value irisArgs;
    irisArgs["value"] = 200;
    ok &= expect(Controller::createPTZCommand("iris", irisArgs, iris), "iris should parse");
    ok &= expectNear(iris.args["value"], 100.0f, "iris value should clamp");

    Device::PTZCommand wb;
    JSON::Value wbArgs;
    wbArgs["mode"] = 3;
    wbArgs["color_temp"] = 4200;
    ok &= expect(Controller::createPTZCommand("white_balance", wbArgs, wb), "white_balance should parse");
    ok &= expectNear(wb.args["mode"], 3.0f, "white balance mode should copy");
    ok &= expectNear(wb.args["color_temp"], 4200.0f, "white balance color temp should copy");

    Device::PTZCommand unknown;
    ok &= expect(!Controller::createPTZCommand("not_a_command", noArgs, unknown), "unknown command should fail");
    return ok;
  }

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "Usage: " << argv[0] << " <test_case>" << std::endl;
    return 2;
  }

  const std::string testCase = argv[1];
  if (testCase == "extract_clean_ip_table") { return testExtractCleanIPTable() ? 0 : 1; }
  if (testCase == "update_device_info_merge") { return testUpdateDeviceInfoMerge() ? 0 : 1; }
  if (testCase == "update_device_info_status_rule") { return testUpdateDeviceInfoStatusRule() ? 0 : 1; }
  if (testCase == "create_ptz_pan_tilt_zoom_clamp") { return testCreatePTZCommandPanTiltZoomClamp() ? 0 : 1; }
  if (testCase == "create_ptz_preset_and_other_actions") { return testCreatePTZCommandPresetAndOtherActions() ? 0 : 1; }

  std::cerr << "Unknown test case: " << testCase << std::endl;
  return 2;
}
