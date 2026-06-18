#include <mist/device_visca.h>

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

  bool testResponseValidationTable() {
    struct ResponseCase {
        std::string name;
        std::string payload;
        bool valid;
        uint8_t type;
    };

    const std::vector<ResponseCase> cases = {
      {"ack", std::string("\x90\x41\xFF", 3), true, 0x41},
      {"completion", std::string("\x90\x42\xFF", 3), true, 0x42},
      {"info", std::string("\x90\x50\xAA", 3), true, 0x50},
      {"error", std::string("\x90\x51\xEE", 3), true, 0x51},
      {"completion_data", std::string("\x90\x52\x12", 3), true, 0x52},
      {"too_short", std::string("\x90\x41", 2), false, 0x00},
      {"wrong_header", std::string("\x80\x41\xFF", 3), false, 0x00},
      {"wrong_type", std::string("\x90\x47\xFF", 3), false, 0x00},
    };

    bool ok = true;
    for (size_t i = 0; i < cases.size(); ++i) {
      uint8_t type = 0;
      bool valid = VISCA::Device::isValidViscaResponse(cases[i].payload, type);
      ok &= expect(valid == cases[i].valid, cases[i].name + ": validity mismatch");
      if (cases[i].valid) { ok &= expect(type == cases[i].type, cases[i].name + ": response type mismatch"); }
    }
    return ok;
  }

  bool testQueryCapabilitiesFeatures() {
    VISCA::Device device("10.1.2.3", 52381);
    Device::DeviceInfo info = device.queryCapabilities();

    bool ok = true;
    ok &= expect(info.id == "10.1.2.3:52381", "id should include host and port");
    ok &= expect(info.host == "10.1.2.3", "host should match constructor");
    ok &= expect(info.hasPTZ, "VISCA should report PTZ support");
    ok &= expect(!info.hasAudio, "VISCA should report no audio support");
    ok &= expect(!info.hasMetadata, "VISCA should report no metadata support");
    ok &= expect(hasValue(info.features, "exposure"), "features should include exposure");
    ok &= expect(hasValue(info.features, "gain"), "features should include gain");
    ok &= expect(hasValue(info.features, "flip"), "features should include flip");
    ok &= expect(hasValue(info.ptzFeatures, "absolute"), "ptzFeatures should include absolute");
    ok &= expect(hasValue(info.ptzFeatures, "continuous"), "ptzFeatures should include continuous");
    return ok;
  }

  bool testQueryCapabilitiesSupportedCommands() {
    VISCA::Device device("192.168.10.5", 1259, VISCA::ViscaTransport::UDP);
    Device::DeviceInfo info = device.queryCapabilities();

    bool ok = true;
    ok &= expect(info.protocols.count("visca") == 1, "visca protocol entry should exist");
    if (info.protocols.count("visca") == 1) {
      const Device::ProtocolConfig & cfg = info.protocols["visca"];
      ok &= expect(cfg.port == 1259, "protocol port should reflect constructor");
      ok &= expect(hasValue(cfg.capabilities.supportedCommands, "exposure"), "commands should include exposure");
      ok &= expect(hasValue(cfg.capabilities.supportedCommands, "gain"), "commands should include gain");
      ok &= expect(hasValue(cfg.capabilities.supportedCommands, "flip"), "commands should include flip");
      ok &= expect(hasValue(cfg.capabilities.supportedCommands, "absolute_move"), "commands should include absolute_move");
      ok &= expect(hasValue(cfg.capabilities.supportedCommands, "direct_zoom"), "commands should include direct_zoom");
    }
    return ok;
  }

  bool testCreateDeviceFromProtocolEndpoint() {
    VISCA::Discovery discovery;
    Device::DeviceInfo info;
    info.host = "172.16.0.10";

    Device::ProtocolConfig cfg;
    cfg.type = "visca";
    cfg.address = "172.16.0.44";
    cfg.port = 4567;
    info.protocols["visca"] = cfg;

    std::unique_ptr<Device::Base> created = discovery.createDevice(info);
    VISCA::Device *viscaDevice = dynamic_cast<VISCA::Device *>(created.get());

    bool ok = true;
    ok &= expect(viscaDevice != 0, "createDevice should return VISCA::Device");
    if (viscaDevice) {
      ok &= expect(viscaDevice->getAddress() == "172.16.0.44", "createDevice should prefer protocol address");
      ok &= expect(viscaDevice->getPort() == 4567, "createDevice should prefer protocol port");
    }
    return ok;
  }

  bool testCreateDeviceFallbackDefaults() {
    VISCA::Discovery discovery;
    Device::DeviceInfo info;
    info.host = "172.16.0.99";

    std::unique_ptr<Device::Base> created = discovery.createDevice(info);
    VISCA::Device *viscaDevice = dynamic_cast<VISCA::Device *>(created.get());

    bool ok = true;
    ok &= expect(viscaDevice != 0, "createDevice should return VISCA::Device");
    if (viscaDevice) {
      ok &= expect(viscaDevice->getAddress() == "172.16.0.99", "createDevice should fall back to host");
      ok &= expect(viscaDevice->getPort() == 52381, "createDevice should fall back to default port");
    }
    return ok;
  }

  bool testSetAddressValidationAndTrim() {
    VISCA::Device device("10.0.0.2", 52381);
    bool ok = true;

    ok &= expect(!device.setAddress("", 52381), "setAddress should reject empty address");
    ok &= expect(device.getLastError() == VISCA::Error::InvalidCommand, "empty address should set InvalidCommand error");

    ok &= expect(device.setAddress(" 192.168.50.10 ", 1259), "setAddress should accept non-empty address");
    ok &= expect(device.getAddress() == "192.168.50.10", "setAddress should trim whitespace");
    ok &= expect(device.getPort() == 1259, "setAddress should update port");
    ok &= expect(device.getTransportType() == VISCA::ViscaTransport::UDP, "port 1259 should force UDP");
    return ok;
  }

  bool testSetAddressPortRules() {
    VISCA::Device device("10.0.0.3", 52381, VISCA::ViscaTransport::UDP);
    bool ok = true;

    ok &= expect(device.setAddress("10.0.0.4", 0), "setAddress should allow zero port and keep current");
    ok &= expect(device.getPort() == 52381, "zero new port should keep existing non-zero port");

    ok &= expect(device.setAddress("10.0.0.5", 52381), "setAddress should allow modern VISCA port");
    ok &= expect(device.getTransportType() == VISCA::ViscaTransport::TCP, "port 52381 should default to TCP");
    return ok;
  }

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "Usage: " << argv[0] << " <test_case>" << std::endl;
    return 2;
  }

  const std::string testCase = argv[1];
  if (testCase == "response_validation_table") { return testResponseValidationTable() ? 0 : 1; }
  if (testCase == "query_capabilities_features") { return testQueryCapabilitiesFeatures() ? 0 : 1; }
  if (testCase == "query_capabilities_supported_commands") { return testQueryCapabilitiesSupportedCommands() ? 0 : 1; }
  if (testCase == "create_device_from_protocol_endpoint") { return testCreateDeviceFromProtocolEndpoint() ? 0 : 1; }
  if (testCase == "create_device_fallback_defaults") { return testCreateDeviceFallbackDefaults() ? 0 : 1; }
  if (testCase == "set_address_validation_and_trim") { return testSetAddressValidationAndTrim() ? 0 : 1; }
  if (testCase == "set_address_port_rules") { return testSetAddressPortRules() ? 0 : 1; }

  std::cerr << "Unknown test case: " << testCase << std::endl;
  return 2;
}
