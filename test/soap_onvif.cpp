#include <iostream>
#include <string>

#define private public
#include <mist/soap.h>
#undef private

namespace {

  bool expect(bool condition, const std::string & message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << std::endl;
      return false;
    }
    return true;
  }

  bool contains(const std::string & haystack, const std::string & needle) {
    return haystack.find(needle) != std::string::npos;
  }

  bool testGetFullActionUri() {
    SOAP::onvif::ONVIFMessageFactory factory;
    bool ok = true;
    ok &= expect(factory.getFullActionUri(SOAP::onvif::ServiceType::Imaging, "timg:GetImagingSettings") ==
                   "http://www.onvif.org/ver20/imaging/wsdl/GetImagingSettings",
                 "imaging prefix should be stripped and URI expanded");
    ok &= expect(factory.getFullActionUri(SOAP::onvif::ServiceType::Analytics, "tan:GetSupportedAnalyticsModules") ==
                   "http://www.onvif.org/ver20/analytics/wsdl/GetSupportedAnalyticsModules",
                 "analytics prefix should be stripped and URI expanded");
    ok &= expect(factory.getFullActionUri(SOAP::onvif::ServiceType::Device, "GetDeviceInformation") ==
                   "http://www.onvif.org/ver10/device/wsdl/GetDeviceInformation",
                 "device action should be expanded");
    ok &= expect(factory.getFullActionUri(SOAP::onvif::ServiceType::Discovery, "wsd:Probe") == "wsd:Probe",
                 "discovery action should pass through");
    ok &= expect(factory.getFullActionUri(SOAP::onvif::ServiceType::PTZ, "http://example.com/custom/action") == "http://example.com/custom/action",
                 "full URI action should pass through unchanged");
    return ok;
  }

  bool testAddServiceNamespaces() {
    SOAP::onvif::ONVIFMessageFactory factory;
    SOAP::Message analyticsMsg("urn:test");
    factory.addServiceNamespaces(analyticsMsg, SOAP::onvif::ServiceType::Analytics);
    const std::string analyticsXml = analyticsMsg.toString();

    SOAP::Message discoveryMsg("urn:test");
    factory.addServiceNamespaces(discoveryMsg, SOAP::onvif::ServiceType::Discovery);
    const std::string discoveryXml = discoveryMsg.toString();

    bool ok = true;
    ok &= expect(contains(analyticsXml, "xmlns:tan=\"http://www.onvif.org/ver20/analytics/wsdl\""),
                 "analytics namespace should be present");
    ok &= expect(contains(analyticsXml, "xmlns:axt=\"http://www.onvif.org/ver20/analytics\""),
                 "analytics extension namespace should be present");
    ok &= expect(contains(discoveryXml, "xmlns:wsd=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\""),
                 "ws-discovery namespace should be present");
    ok &= expect(contains(discoveryXml, "xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\""),
                 "discovery dn namespace should be present");
    return ok;
  }

  bool testAddStandardHeaders() {
    SOAP::onvif::ONVIFMessageFactory factory;
    SOAP::Message msg("urn:test");
    factory.addStandardHeaders(msg, "http://www.onvif.org/ver20/imaging/wsdl/GetImagingSettings", "http://camera/imaging");
    const std::string xml = msg.toString();

    bool ok = true;
    ok &= expect(contains(xml, "wsa:Action"), "Action header element should be present");
    ok &= expect(contains(xml, "wsa:To"), "To header element should be present");
    ok &= expect(contains(xml, "wsa:MessageID"), "MessageID header element should be present");
    ok &= expect(contains(xml, "http://camera/imaging"), "To header value should be present");
    ok &= expect(contains(xml, "urn:uuid:"), "MessageID value should be generated");
    return ok;
  }

  bool testAddSecurityHeaderUsesDigest() {
    SOAP::Message msg("urn:test");
    SOAP::SecurityHeader sec;
    sec.timestamp.created = "2026-02-12T00:00:00Z";
    sec.timestamp.expires = "2026-02-12T00:10:00Z";
    sec.usernameToken.username = "admin";
    sec.usernameToken.password = "password123";
    sec.usernameToken.nonce = "AQIDBAUGBwgJCgsMDQ4PEA==";
    sec.usernameToken.created = "2026-02-12T00:00:00Z";

    const std::string digest =
      SOAP::generatePasswordDigest(sec.usernameToken.nonce, sec.usernameToken.created, sec.usernameToken.password);
    msg.addSecurityHeader(sec);
    const std::string xml = msg.toString();

    bool ok = true;
    ok &= expect(contains(xml, digest), "security header should contain password digest");
    ok &= expect(!contains(xml, ">password123<"), "security header should not contain plaintext password");
    ok &= expect(contains(xml, "wsse:Nonce"), "security header should contain nonce");
    return ok;
  }

  bool testGenerateUUIDFormat() {
    SOAP::Message msg("urn:test");
    const std::string uuid = msg.generateUUID();

    bool ok = true;
    ok &= expect(uuid.size() == 36, "UUID should be 36 characters");
    ok &= expect(uuid[8] == '-' && uuid[13] == '-' && uuid[18] == '-' && uuid[23] == '-', "UUID should contain hyphens in RFC positions");
    ok &= expect(uuid[14] == '4', "UUID version nibble should be 4");
    ok &= expect(uuid[19] == '8' || uuid[19] == '9' || uuid[19] == 'a' || uuid[19] == 'b', "UUID variant nibble should be 8,9,a,b");
    return ok;
  }

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "Usage: " << argv[0] << " <test_case>" << std::endl;
    return 2;
  }

  const std::string testCase = argv[1];
  if (testCase == "get_full_action_uri") { return testGetFullActionUri() ? 0 : 1; }
  if (testCase == "add_service_namespaces") { return testAddServiceNamespaces() ? 0 : 1; }
  if (testCase == "add_standard_headers") { return testAddStandardHeaders() ? 0 : 1; }
  if (testCase == "add_security_header_uses_digest") { return testAddSecurityHeaderUsesDigest() ? 0 : 1; }
  if (testCase == "generate_uuid_format") { return testGenerateUUIDFormat() ? 0 : 1; }

  std::cerr << "Unknown test case: " << testCase << std::endl;
  return 2;
}
