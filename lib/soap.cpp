// soap.cpp
// Implementation of soap::Message, a generic SOAP library tailored for ONVIF
#include "soap.h"

#include "encode.h"
#include "xml.h"

#include <ctime>
#include <random>
#include <sstream>
#include <stdexcept>

#ifdef SSL
#include <mbedtls/sha1.h>
#endif

namespace SOAP {

  // Static namespace maps initialization
  const std::map<std::string, std::string> Message::SOAP_1_2_NAMESPACES = {
    {"s", "http://www.w3.org/2003/05/soap-envelope"},
    {"wsa", "http://www.w3.org/2005/08/addressing"},
    {"wsd", "http://schemas.xmlsoap.org/ws/2005/04/discovery"},
    {"wsse", "http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd"},
    {"wsu", "http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd"}};

  const std::map<std::string, std::string> Message::SOAP_1_1_NAMESPACES = {
    {"SOAP-ENV", "http://schemas.xmlsoap.org/soap/envelope/"},
    {"wsa", "http://schemas.xmlsoap.org/ws/2004/08/addressing"},
    {"wsd", "http://schemas.xmlsoap.org/ws/2005/04/discovery"},
    {"wsse", "http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd"},
    {"wsu", "http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd"}};

  Message::Message(const std::string & actionUri, Version ver) : doc(), version(ver), action(actionUri) {

    // Create root element
    auto root = doc.createNode(this->getEnvelopeTag());
    doc.setRoot(root);
    envelope = root;

    // Create header and body with proper namespaces
    header = doc.createNode(this->getHeaderTag());
    body = doc.createNode(this->getBodyTag());
    envelope.addChild(header);
    envelope.addChild(body);

    // Add standard namespaces
    const auto & ns = (ver == Version::SOAP_1_2) ? SOAP_1_2_NAMESPACES : SOAP_1_1_NAMESPACES;
    for (const auto & pair : ns) { this->addNamespace(pair.first, pair.second); }

    // Add the Action header
    this->addWSAAction(actionUri);
  }

  Message::~Message() = default;

  Message::Message(Message && other) noexcept
    : doc(std::move(other.doc)), envelope(std::move(other.envelope)), header(std::move(other.header)),
      body(std::move(other.body)), version(other.version), action(std::move(other.action)),
      namespaces(std::move(other.namespaces)) {}

  Message & Message::operator=(Message && other) noexcept {
    if (this != &other) {
      doc = std::move(other.doc);
      envelope = std::move(other.envelope);
      header = std::move(other.header);
      body = std::move(other.body);
      version = other.version;
      action = std::move(other.action);
      namespaces = std::move(other.namespaces);
    }
    return *this;
  }

  void Message::addNamespace(const std::string & prefix, const std::string & uri) {
    // Only add if not already present
    if (namespaces.find(prefix) == namespaces.end()) {
      namespaces[prefix] = uri;
      doc.addNamespace(prefix, uri);
    }
  }

  void Message::addStandardNamespaces() {
    if (version == Version::SOAP_1_2) {
      addNamespace("s", SOAP12_ENV);
      addNamespace("enc", SOAP12_ENC);
    } else {
      addNamespace("SOAP-ENV", SOAP11_ENV);
      addNamespace("SOAP-ENC", SOAP11_ENC);
    }
  }

  void Message::addHeader(const std::string & name, const std::string & value, bool mustUnderstand) {
    // Split prefix and local name
    size_t colonPos = name.find(':');
    if (colonPos == std::string::npos) { throw std::runtime_error("Header name must include namespace prefix"); }

    std::string prefix = name.substr(0, colonPos);
    std::string localName = name.substr(colonPos + 1);

    auto node = doc.createNamespacedNode(prefix, localName);
    node.setTextContent(value);

    if (mustUnderstand) {
      std::string muPrefix = (version == Version::SOAP_1_2) ? "s" : "SOAP-ENV";
      node.setAttribute(muPrefix + ":mustUnderstand", version == Version::SOAP_1_2 ? "true" : "1");
    }

    header.addChild(node);
  }

  void Message::setBody(const std::string & xmlFragment) {
    if (xmlFragment.empty()) return;

    try {
      auto tempDoc = XML::Document(xmlFragment);
      auto content = tempDoc.getRoot();

      // Import the content into our document
      auto imported = doc.importNode(content);

      // Clear any existing body content
      body.clearChildren();

      // Add the new content
      body.addChild(imported);
    } catch (const std::exception &) {
      // If parsing fails, create new body with text content
      body.clearChildren();
      body.setTextContent(xmlFragment);
    }
  }

  void Message::setBodyNode(const XML::Node & node) {
    if (!doc.isValid() || !node.isValid()) { return; }

    // Import the node into this document
    XML::Node imported = doc.importNode(node, true);
    if (!imported.isValid()) { return; }

    // Clear existing body content and add the imported node
    body.clearChildren();
    body.addChild(imported);
  }

  std::string Message::toString(bool pretty) const {
    return doc.toString(pretty);
  }

  bool Message::hasFault() const {
    auto faultNode = body.getChild(this->getFaultTag());
    return faultNode.isValid();
  }

  Fault Message::getFault() const {
    Fault fault;
    auto faultNode = body.getChild(this->getFaultTag());

    if (!faultNode.isValid()) return fault;

    if (version == Version::SOAP_1_1) {
      fault.code = faultNode.getChild("faultcode").getTextContent();
      fault.reason = faultNode.getChild("faultstring").getTextContent();
      auto detail = faultNode.getChild("detail");
      if (detail.isValid()) { fault.detail = doc.toString(true); }
    } else {
      auto code = faultNode.getChild("Code");
      auto reason = faultNode.getChild("Reason");
      auto detail = faultNode.getChild("Detail");

      if (code.isValid()) { fault.code = code.getChild("Value").getTextContent(); }
      if (reason.isValid()) { fault.reason = reason.getChild("Text").getTextContent(); }
      if (detail.isValid()) { fault.detail = doc.toString(true); }
    }

    return fault;
  }

  XML::Node Message::getBodyNode() const {
    return body;
  }

  const XML::Document & Message::getDocument() const {
    return doc;
  }

  std::string Message::generateUUID() const {
    thread_local std::mt19937 gen(std::random_device{}());
    thread_local std::uniform_int_distribution<> dis(0, 15);
    thread_local std::uniform_int_distribution<> dis2(8, 11);

    std::stringstream ss;
    ss << std::hex;
    for (int i = 0; i < 8; i++) ss << dis(gen);
    ss << "-";
    for (int i = 0; i < 4; i++) ss << dis(gen);
    ss << "-4";
    for (int i = 0; i < 3; i++) ss << dis(gen);
    ss << "-";
    ss << dis2(gen);
    for (int i = 0; i < 3; i++) ss << dis(gen);
    ss << "-";
    for (int i = 0; i < 12; i++) ss << dis(gen);
    return ss.str();
  }

  void Message::addSecurityHeader(const SecurityHeader & security) {
    // Add WS-Security namespaces
    addNamespace("wsse", WSS_SECEXT);
    addNamespace("wsu", WSS_UTILITY);

    // Create Security element
    auto securityNode = doc.createNode("wsse:Security");
    securityNode.setAttribute("s:mustUnderstand", version == Version::SOAP_1_2 ? "true" : "1");

    // Add Timestamp
    auto timestampNode = doc.createNode("wsu:Timestamp");
    timestampNode.setAttribute("wsu:Id", "Timestamp");

    auto createdNode = doc.createNode("wsu:Created");
    createdNode.setTextContent(security.timestamp.created);
    timestampNode.addChild(createdNode);

    auto expiresNode = doc.createNode("wsu:Expires");
    expiresNode.setTextContent(security.timestamp.expires);
    timestampNode.addChild(expiresNode);

    securityNode.addChild(timestampNode);

    // Add UsernameToken
    auto tokenNode = doc.createNode("wsse:UsernameToken");
    tokenNode.setAttribute("wsu:Id", "UsernameToken");

    auto usernameNode = doc.createNode("wsse:Username");
    usernameNode.setTextContent(security.usernameToken.username);
    tokenNode.addChild(usernameNode);

    auto passwordNode = doc.createNode("wsse:Password");
    passwordNode.setAttribute("Type", WSS_PASSWORD_TYPE);
    passwordNode.setTextContent(generatePasswordDigest(security.usernameToken.nonce, security.usernameToken.created,
                                                       security.usernameToken.password));
    tokenNode.addChild(passwordNode);

    auto nonceNode = doc.createNode("wsse:Nonce");
    nonceNode.setAttribute("EncodingType", WSS_ENCODING_TYPE);
    nonceNode.setTextContent(security.usernameToken.nonce);
    tokenNode.addChild(nonceNode);

    auto tokenCreatedNode = doc.createNode("wsu:Created");
    tokenCreatedNode.setTextContent(security.usernameToken.created);
    tokenNode.addChild(tokenCreatedNode);

    securityNode.addChild(tokenNode);

    // Add to SOAP header
    header.addChild(securityNode);
  }

  Message Message::parse(const std::string & xml) {
    // Create new message outside the lock since this doesn't touch shared state
    auto parsedDoc = XML::Document(xml);
    auto root = parsedDoc.getRoot();

    // Determine SOAP version from envelope namespace
    Version ver = Version::SOAP_1_2;
    std::string envNs = root.getNamespace();
    if (envNs == SOAP11_ENV) { ver = Version::SOAP_1_1; }

    // Get Action from header
    std::string action;
    auto header = root.getChild("Header"); // Use local name only
    if (header.isValid()) {
      auto actionNode = header.getChild("Action"); // Use local name only
      if (actionNode.isValid()) { action = actionNode.getTextContent(); }
    }

    // Create new message
    Message msg(action, ver);

    // Now lock while modifying the message's internal state
    {
      // Import the parsed document with proper namespace handling
      msg.doc = std::move(parsedDoc);
      msg.envelope = root;
      msg.header = header;
      msg.body = root.getChild("Body"); // Use local name only

      // Preserve all namespaces from the incoming document
      auto incomingNamespaces = msg.doc.getNamespaces();
      for (const auto & pair : incomingNamespaces) {
        msg.namespaces[pair.first] = pair.second;
        // Register in XPath context for subsequent queries
        msg.doc.addNamespace(pair.first, pair.second);
      }
    }

    return msg;
  }

  namespace onvif {

    DiscoveryFactory::DiscoveryFactory(Version version) : version_(version) {}

    void DiscoveryFactory::addDiscoveryNamespaces(Message & msg) const {
      // Add all required namespaces in standard order
      msg.addStandardNamespaces();
      msg.addWSAddressingNamespaces();
      msg.addWSDiscoveryNamespaces();
    }

    void DiscoveryFactory::addDiscoveryHeaders(Message & msg, const std::string & action,
                                               const std::map<std::string, std::string> & extraHeaders) const {
      msg.addWSAAction(action, true);
      msg.addWSAMessageID();
      msg.addWSATo("urn:schemas-xmlsoap-org:ws:2005:04:discovery", true);
      msg.addWSAReplyTo("http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous", true);

      // Add any extra headers
      for (const auto & header : extraHeaders) { msg.addHeader(header.first, header.second, true); }
    }

    void DiscoveryFactory::createDiscoveryBody(Message & msg, const DiscoveryOptions & options, const std::string & elementName) const {
      // Create root element (e.g. <wsd:Probe>)
      auto doc = msg.getDocument();
      auto root = doc.createNode("wsd:" + elementName);

      // Add Types element
      if (!options.types.empty()) {
        auto types = doc.createNode("wsd:Types");
        types.setTextContent(options.types);
        root.addChild(types);
      }

      // Add Scopes element
      auto scopes = doc.createNode("wsd:Scopes");
      if (!options.scopes.empty()) {
        // Join scopes with spaces
        std::string scopeList;
        for (const auto & scope : options.scopes) {
          if (!scopeList.empty()) scopeList += " ";
          scopeList += scope;
        }
        scopes.setTextContent(scopeList);

        // Add MatchBy if specified
        if (options.matchBy && !options.matchByUri.empty()) { scopes.setAttribute("MatchBy", options.matchByUri); }
      }
      root.addChild(scopes);

      msg.setBodyNode(root);
    }

    Message DiscoveryFactory::makeProbe(const DiscoveryOptions & options) const {
      const std::string probeAction = "http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe";
      Message msg(probeAction, version_);

      msg.addStandardNamespaces();
      addDiscoveryNamespaces(msg);
      addDiscoveryHeaders(msg, probeAction, options.extraHeaders);
      createDiscoveryBody(msg, options, "Probe");

      return msg;
    }

    Message DiscoveryFactory::makeHello(const DiscoveryOptions & options) const {
      const std::string helloAction = "http://schemas.xmlsoap.org/ws/2005/04/discovery/Hello";
      Message msg(helloAction, version_);

      msg.addStandardNamespaces();
      addDiscoveryNamespaces(msg);
      addDiscoveryHeaders(msg, helloAction, options.extraHeaders);
      createDiscoveryBody(msg, options, "Hello");

      return msg;
    }

    Message DiscoveryFactory::makeBye(const DiscoveryOptions & options) const {
      const std::string byeAction = "http://schemas.xmlsoap.org/ws/2005/04/discovery/Bye";
      Message msg(byeAction, version_);

      msg.addStandardNamespaces();
      addDiscoveryNamespaces(msg);
      addDiscoveryHeaders(msg, byeAction, options.extraHeaders);
      createDiscoveryBody(msg, options, "Bye");

      return msg;
    }

    // Deprecated implementation that forwards to factory
    Message makeDiscoveryProbe(const std::string & types, Version version) {
      DiscoveryOptions options;
      options.types = types;

      DiscoveryFactory factory(version);
      return factory.makeProbe(options);
    }

    // Add ONVIFMessageFactory implementation
    ONVIFMessageFactory::ONVIFMessageFactory(Version version) : version_(version) {}

    std::string ONVIFMessageFactory::getFullActionUri(ServiceType service, const std::string & action) const {
      if (action.find("http://") != std::string::npos) { return action; }

      // Strip XML namespace prefix (e.g. "timg:", "tan:", "tev:") before building URI
      std::string cleanAction = action;
      auto colonPos = cleanAction.find(':');
      if (colonPos != std::string::npos) { cleanAction = cleanAction.substr(colonPos + 1); }

      switch (service) {
        case ServiceType::Device: return "http://www.onvif.org/ver10/device/wsdl/" + cleanAction;
        case ServiceType::Media: return "http://www.onvif.org/ver10/media/wsdl/" + cleanAction;
        case ServiceType::PTZ: return "http://www.onvif.org/ver20/ptz/wsdl/" + cleanAction;
        case ServiceType::Events: return "http://www.onvif.org/ver10/events/wsdl/" + cleanAction;
        case ServiceType::Analytics: return "http://www.onvif.org/ver20/analytics/wsdl/" + cleanAction;
        case ServiceType::Imaging: return "http://www.onvif.org/ver20/imaging/wsdl/" + cleanAction;
        case ServiceType::Discovery: return action;
        default: throw std::runtime_error("Unknown ONVIF service type");
      }
    }

    void ONVIFMessageFactory::addServiceNamespaces(Message & msg, ServiceType service) const {
      // Add standard namespaces first
      msg.addStandardNamespaces();
      msg.addWSAddressingNamespaces();

      // Add common schema namespace
      msg.addNamespace("tt", "http://www.onvif.org/ver10/schema");

      // Add service-specific namespaces
      switch (service) {
        case ServiceType::Device: msg.addNamespace("tds", "http://www.onvif.org/ver10/device/wsdl"); break;
        case ServiceType::Media: msg.addNamespace("trt", "http://www.onvif.org/ver10/media/wsdl"); break;
        case ServiceType::PTZ: msg.addNamespace("tptz", "http://www.onvif.org/ver20/ptz/wsdl"); break;
        case ServiceType::Events:
          msg.addNamespace("tev", "http://www.onvif.org/ver10/events/wsdl");
          msg.addNamespace("wsnt", "http://docs.oasis-open.org/wsn/b-2");
          break;
        case ServiceType::Analytics:
          msg.addNamespace("tan", "http://www.onvif.org/ver20/analytics/wsdl");
          msg.addNamespace("axt", "http://www.onvif.org/ver20/analytics");
          break;
        case ServiceType::Imaging: msg.addNamespace("timg", "http://www.onvif.org/ver20/imaging/wsdl"); break;
        case ServiceType::Discovery: msg.addWSDiscoveryNamespaces(); break;
      }
    }

    void ONVIFMessageFactory::addStandardHeaders(Message & msg, const std::string & action, const std::string & to) const {
      msg.addWSAMessageID();
      msg.addWSAAction(action);
      msg.addWSATo(to);
    }

    Message ONVIFMessageFactory::createMessage(ServiceType service, const std::string & action, const std::string & to) const {
      // Get full action URI if needed
      std::string fullAction = getFullActionUri(service, action);

      // Create message with proper version
      Message msg(fullAction, version_);

      // Add namespaces and headers
      addServiceNamespaces(msg, service);
      addStandardHeaders(msg, fullAction, to);

      return msg;
    }

  } // namespace onvif

  std::string generateNonce() {
    unsigned char nonce[20];
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);

    for (int i = 0; i < 20; i++) { nonce[i] = static_cast<unsigned char>(dis(gen)); }

    return Encodings::Base64::encode(std::string(reinterpret_cast<const char *>(nonce), 20));
  }

  std::string getUTCTime(int offsetSeconds) {
    time_t now;
    time(&now);
    now += offsetSeconds;
    struct tm *tm = gmtime(&now);
    char timestamp[30];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", tm);
    return std::string(timestamp);
  }

  std::string generatePasswordDigest(const std::string & nonce, const std::string & created, const std::string & password) {
#ifdef SSL
    // Decode base64 nonce
    std::string decodedNonce = Encodings::Base64::decode(nonce);

    // Combine nonce + created + password
    std::string combined = decodedNonce + created + password;

    // Generate SHA1 hash
    unsigned char hash[20];
    mbedtls_sha1_context ctx;
    mbedtls_sha1_init(&ctx);
    mbedtls_sha1_starts(&ctx);
    mbedtls_sha1_update(&ctx, reinterpret_cast<const unsigned char *>(combined.c_str()), combined.length());
    mbedtls_sha1_finish(&ctx, hash);
    mbedtls_sha1_free(&ctx);

    // Encode hash as base64
    return Encodings::Base64::encode(std::string(reinterpret_cast<char *>(hash), 20));
#else
    throw std::runtime_error("SSL support not enabled - cannot generate password digest");
#endif
  }
} // namespace SOAP
