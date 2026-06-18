// soap.h
// A generic SOAP library, with built-in helpers for ONVIF WS-Discovery
#pragma once

#include "xml.h"

#include <map>
#include <string>
#include <vector>

/**
 * @brief SOAP message handling and WS-* protocol support
 */
namespace SOAP {

  // SOAP namespaces
  constexpr const char *SOAP11_ENV = "http://schemas.xmlsoap.org/soap/envelope/";
  constexpr const char *SOAP11_ENC = "http://schemas.xmlsoap.org/soap/encoding/";
  constexpr const char *SOAP12_ENV = "http://www.w3.org/2003/05/soap-envelope";
  constexpr const char *SOAP12_ENC = "http://www.w3.org/2003/05/soap-encoding";

  // WS-Security namespaces
  constexpr const char *WSS_SECEXT =
    "http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd";
  constexpr const char *WSS_UTILITY =
    "http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd";
  constexpr const char *WSS_PASSWORD_TYPE = "http://docs.oasis-open.org/wss/2004/01/"
                                            "oasis-200401-wss-username-token-profile-1.0#PasswordDigest";
  constexpr const char *WSS_ENCODING_TYPE = "http://docs.oasis-open.org/wss/2004/01/"
                                            "oasis-200401-wss-soap-message-security-1.0#Base64Binary";

  /**
   * @brief SOAP protocol version enumeration
   */
  enum class Version { SOAP_1_1, SOAP_1_2 };

  /**
   * @brief Represents a SOAP Fault message
   *
   * SOAP Faults are used to carry error information within a SOAP message.
   * The structure is compatible with both SOAP 1.1 and 1.2 fault formats.
   */
  struct Fault {
      std::string code; ///< The fault code (e.g. "Sender", "Receiver")
      std::string reason; ///< Human-readable explanation of the fault
      std::string detail; ///< Additional fault-specific diagnostic information
  };

  /**
   * @brief WS-Security timestamp information
   */
  struct Timestamp {
      std::string created; ///< Creation time in ISO 8601 format
      std::string expires; ///< Expiration time in ISO 8601 format
  };

  /**
   * @brief WS-Security username token information
   */
  struct UsernameToken {
      std::string username; ///< Username
      std::string password; ///< Password digest
      std::string nonce; ///< Random nonce (Base64)
      std::string created; ///< Creation time in ISO 8601 format
  };

  /**
   * @brief WS-Security header information
   */
  struct SecurityHeader {
      Timestamp timestamp; ///< Security timestamp
      UsernameToken usernameToken; ///< Username token
  };

  /**
   * @brief Core SOAP message/envelope class
   *
   * This class provides functionality to create, manipulate, and serialize SOAP messages.
   * It handles both SOAP 1.1 and 1.2 formats and includes support for:
   * - WS-Addressing headers
   * - Custom namespace declarations
   * - Header elements with mustUnderstand attribute
   * - XML body content
   *
   * Thread safety: All operations are internally synchronized.
   * However, for best performance, avoid concurrent operations on the same Message object.
   */
  class Message {
    public:
      /**
       * @brief Construct a new SOAP message
       *
       * @param actionUri WS-Addressing Action URI for the message
       * @param version SOAP protocol version to use
       */
      explicit Message(const std::string & actionUri, Version version = Version::SOAP_1_2);

      ~Message();

      // Disable copy operations
      Message(const Message &) = delete;
      Message & operator=(const Message &) = delete;

      // Enable move operations
      Message(Message && other) noexcept;
      Message & operator=(Message && other) noexcept;

      // —— Namespace Management ——————————————————————————————————————————————————————

      /**
       * @brief Declare a namespace on the SOAP Envelope
       *
       * @param prefix Namespace prefix (e.g. "wsa", "s")
       * @param uri Namespace URI
       */
      void addNamespace(const std::string & prefix, const std::string & uri);

      /**
       * @brief Add standard SOAP namespaces based on version
       *
       * For SOAP 1.1: SOAP-ENV and SOAP-ENC namespaces
       * For SOAP 1.2: s and enc namespaces
       */
      void addStandardNamespaces();

      /**
       * @brief Add standard WS-Addressing namespaces
       */
      void addWSAddressingNamespaces() {
        addNamespace("wsa", version == Version::SOAP_1_2 ? "http://www.w3.org/2005/08/addressing" : "http://schemas.xmlsoap.org/ws/2004/08/addressing");
      }

      /**
       * @brief Add standard WS-Security namespaces
       */
      void addWSSecurityNamespaces() {
        addNamespace("wsse", WSS_SECEXT);
        addNamespace("wsu", WSS_UTILITY);
      }

      /**
       * @brief Add standard WS-Discovery namespaces
       */
      void addWSDiscoveryNamespaces() {
        addNamespace("wsd", "http://schemas.xmlsoap.org/ws/2005/04/discovery");
        addNamespace("dn", "http://www.onvif.org/ver10/network/wsdl");
      }

      // —— Header Management ———————————————————————————————————————————————————————

      /**
       * @brief Add a header element to the SOAP message
       *
       * @param name Qualified name including prefix (e.g. "wsa:Action")
       * @param value Text content of the header element
       * @param mustUnderstand If true, adds mustUnderstand="1" (SOAP 1.1) or "true" (SOAP 1.2)
       */
      void addHeader(const std::string & name, const std::string & value, bool mustUnderstand = false);

      /**
       * @brief Add WS-Security header to the SOAP message
       *
       * This method adds a WS-Security header containing:
       * - Timestamp with creation and expiration times
       * - UsernameToken with username, password digest, nonce and creation time
       *
       * The header is marked as mustUnderstand="1" (SOAP 1.1) or "true" (SOAP 1.2)
       *
       * @param security Security header information including timestamp and username token
       */
      void addSecurityHeader(const SecurityHeader & security);

      /**
       * @brief Add a WS-Addressing Action header
       * @param uri The action URI
       * @param mustUnderstand Whether the header must be understood (defaults to true)
       */
      void addWSAAction(const std::string & uri, bool mustUnderstand = true) {
        addHeader("wsa:Action", uri, mustUnderstand);
      }

      /**
       * @brief Add a WS-Addressing MessageID header with a generated UUID
       * @param mustUnderstand Whether the header must be understood (defaults to true)
       * @return The generated message ID
       */
      std::string addWSAMessageID(bool mustUnderstand = true) {
        std::string messageId = "urn:uuid:" + generateUUID();
        addHeader("wsa:MessageID", messageId, mustUnderstand);
        return messageId;
      }

      /**
       * @brief Add a WS-Addressing To header
       * @param uri The destination URI
       * @param mustUnderstand Whether the header must be understood (defaults to true)
       */
      void addWSATo(const std::string & uri, bool mustUnderstand = true) { addHeader("wsa:To", uri, mustUnderstand); }

      /**
       * @brief Add a WS-Addressing RelatesTo header
       * @param messageId The message ID this message relates to
       * @param mustUnderstand Whether the header must be understood (defaults to true)
       */
      void addWSARelatesTo(const std::string & messageId, bool mustUnderstand = true) {
        addHeader("wsa:RelatesTo", messageId, mustUnderstand);
      }

      /**
       * @brief Add a WS-Addressing ReplyTo header
       * @param uri The reply-to URI
       * @param mustUnderstand Whether the header must be understood (defaults to true)
       */
      void addWSAReplyTo(const std::string & uri, bool mustUnderstand = true) {
        addHeader("wsa:ReplyTo", uri, mustUnderstand);
      }

      // —— Body Management ————————————————————————————————————————————————————————

      /**
       * @brief Set the SOAP body content from an XML fragment
       * @param xmlFragment XML string to use as body content
       */
      void setBody(const std::string & xmlFragment);

      /**
       * @brief Set the SOAP body content from an XML node
       * @param node XML node to use as body content
       */
      void setBodyNode(const XML::Node & node);

      // —— Serialization & Parsing ———————————————————————————————————————————————

      /**
       * @brief Convert the SOAP message to a string
       * @param pretty Whether to format the output with indentation
       * @return String representation of the SOAP message
       */
      std::string toString(bool pretty = false) const;

      /**
       * @brief Parse a SOAP message from XML string
       * @param xml XML string containing SOAP message
       * @return Parsed SOAP message object
       * @throw XML::Exception if parsing fails
       */
      static Message parse(const std::string & xml);

      // —— Message Information ————————————————————————————————————————————————————

      /**
       * @brief Check if the message contains a SOAP Fault
       *
       * @return true if the body contains a Fault element
       */
      bool hasFault() const;

      /**
       * @brief Get fault information if present
       *
       * @return Fault object containing fault details
       * @throws SOAPException if message has no fault
       */
      Fault getFault() const;

      /**
       * @brief Get the message body node for XML operations
       * @return XML::Node reference to the body element
       */
      XML::Node getBodyNode() const;

      /**
       * @brief Get the underlying XML document
       * @return Const reference to the XML document
       */
      const XML::Document & getDocument() const;

      /**
       * @brief Generate a random UUID
       * @return UUID string in standard format
       */
      std::string generateUUID() const;

    private:
      // Static namespace maps
      static const std::map<std::string, std::string> SOAP_1_2_NAMESPACES;
      static const std::map<std::string, std::string> SOAP_1_1_NAMESPACES;

      // Helper methods for tag names
      std::string getEnvelopeTag() const { return version == Version::SOAP_1_2 ? "s:Envelope" : "SOAP-ENV:Envelope"; }

      std::string getHeaderTag() const { return version == Version::SOAP_1_2 ? "s:Header" : "SOAP-ENV:Header"; }

      std::string getBodyTag() const { return version == Version::SOAP_1_2 ? "s:Body" : "SOAP-ENV:Body"; }

      std::string getFaultTag() const { return version == Version::SOAP_1_2 ? "s:Fault" : "SOAP-ENV:Fault"; }

      // Document structure
      XML::Document doc;
      XML::Node envelope;
      XML::Node header;
      XML::Node body;
      Version version;
      std::string action;
      std::map<std::string, std::string> namespaces;
  };

  /**
   * @brief ONVIF-specific SOAP message helpers
   */
  namespace onvif {

    /**
     * @brief Options for WS-Discovery messages
     */
    struct DiscoveryOptions {
        std::string types = "dn:NetworkVideoTransmitter"; ///< Device types to probe for
        std::vector<std::string> scopes; ///< Optional scopes to filter by
        std::map<std::string, std::string> extraHeaders; ///< Additional WS-Addressing headers
        bool matchBy = false; ///< Whether to include MatchBy in probe
        std::string matchByUri; ///< URI for MatchBy algorithm if enabled
    };

    /**
     * @brief Factory class for creating WS-Discovery messages
     */
    class DiscoveryFactory {
      public:
        /**
         * @brief Construct a new Discovery Factory
         * @param version SOAP version to use for discovery messages
         */
        explicit DiscoveryFactory(Version version = Version::SOAP_1_2);

        /**
         * @brief Create a WS-Discovery Probe message
         * @param options Discovery configuration options
         * @return Ready-to-send probe Message
         */
        Message makeProbe(const DiscoveryOptions & options = DiscoveryOptions()) const;

        /**
         * @brief Create a WS-Discovery Hello message
         * @param options Discovery configuration options
         * @return Ready-to-send hello Message
         */
        Message makeHello(const DiscoveryOptions & options = DiscoveryOptions()) const;

        /**
         * @brief Create a WS-Discovery Bye message
         * @param options Discovery configuration options
         * @return Ready-to-send bye Message
         */
        Message makeBye(const DiscoveryOptions & options = DiscoveryOptions()) const;

      private:
        Version version_;

        /**
         * @brief Add standard WS-Discovery namespaces to a message
         * @param msg SOAP message to add namespaces to
         */
        void addDiscoveryNamespaces(Message & msg) const;

        /**
         * @brief Add WS-Discovery headers to a message
         * @param msg SOAP message to add headers to
         * @param action WS-Addressing Action URI
         * @param extraHeaders Additional headers to include
         */
        void addDiscoveryHeaders(Message & msg, const std::string & action,
                                 const std::map<std::string, std::string> & extraHeaders = {}) const;

        /**
         * @brief Create WS-Discovery message body
         * @param msg SOAP message to add body to
         * @param options Discovery options including types and scopes
         * @param elementName Root element name for the body
         */
        void createDiscoveryBody(Message & msg, const DiscoveryOptions & options, const std::string & elementName) const;
        /**
         * @brief Create a WS-Discovery Probe message
         * @param types Device types to probe for (default: NetworkVideoTransmitter)
         * @param scopes Optional scopes to filter by
         * @param extraHeaders Additional WS-Addressing headers
         * @return Complete SOAP message for WS-Discovery Probe
         */
        Message makeDiscoveryProbe(const std::string & types = "dn:NetworkVideoTransmitter",
                                   const std::vector<std::string> & scopes = {},
                                   const std::map<std::string, std::string> & extraHeaders = {}) const;
    };

    // Deprecated - use DiscoveryFactory instead
    [[deprecated("Use DiscoveryFactory::makeProbe() instead")]]
    Message makeDiscoveryProbe(const std::string & types = "dn:NetworkVideoTransmitter", Version version = Version::SOAP_1_2);

    /**
     * @brief ONVIF service types
     */
    enum class ServiceType { Device, Media, PTZ, Events, Discovery, Analytics, Imaging };

    /**
     * @brief Factory class for creating ONVIF SOAP messages
     */
    class ONVIFMessageFactory {
      public:
        /**
         * @brief Construct a new ONVIF Message Factory
         * @param version SOAP version to use for messages
         */
        explicit ONVIFMessageFactory(Version version = Version::SOAP_1_2);

        /**
         * @brief Create a SOAP message for an ONVIF service
         * @param service The ONVIF service type
         * @param action The action name or full action URI
         * @param to Optional destination URI (defaults to anonymous)
         * @return Ready-to-send SOAP message
         */
        Message createMessage(ServiceType service, const std::string & action,
                              const std::string & to = "http://schemas.xmlsoap.org/ws/2004/08/"
                                                       "addressing/role/anonymous") const;

      private:
        Version version_;

        /**
         * @brief Get the full action URI for a service and action
         * @param service The service type
         * @param action The action name
         * @return Full action URI if needed, or original action if already a URI
         */
        std::string getFullActionUri(ServiceType service, const std::string & action) const;

        /**
         * @brief Add service-specific namespaces to a message
         * @param msg SOAP message to add namespaces to
         * @param service The service type
         */
        void addServiceNamespaces(Message & msg, ServiceType service) const;

        /**
         * @brief Add standard WS-Addressing headers to a message
         * @param msg SOAP message to add headers to
         * @param action The full action URI
         * @param to The destination URI
         */
        void addStandardHeaders(Message & msg, const std::string & action, const std::string & to) const;
    };

  } // namespace onvif

  /**
   * @brief Generate a random nonce for WS-Security
   * @return Base64-encoded random nonce string
   */
  std::string generateNonce();

  /**
   * @brief Get current UTC time with optional offset
   * @param offsetSeconds Number of seconds to add/subtract from current time
   * @return ISO 8601 formatted UTC time string
   */
  std::string getUTCTime(int offsetSeconds = 0);

  /**
   * @brief Generate password digest for WS-Security
   * @param nonce Base64 encoded nonce
   * @param created Creation timestamp
   * @param password Plain text password
   * @return Base64 encoded password digest
   */
  std::string generatePasswordDigest(const std::string & nonce, const std::string & created, const std::string & password);
} // namespace SOAP
