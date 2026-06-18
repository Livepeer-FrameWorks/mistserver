#pragma once

#include <map>
#include <mutex>
#include <string>
#include <vector>

// Forward declarations for libxml2 types
struct _xmlNode;
struct _xmlDoc;
struct _xmlError;
typedef struct _xmlNode *xmlNodePtr;
typedef struct _xmlDoc *xmlDocPtr;
typedef struct _xmlNs *xmlNsPtr;
typedef struct _xmlXPathContext *xmlXPathContextPtr;
typedef struct _xmlError xmlError;

// XML parsing constants
#define XML_SUBSTITUTE_REF 1

/**
 * @brief XML Document and Node manipulation classes
 *
 * This module provides a C++ wrapper around libxml2 for XML document handling.
 *
 * THREAD SAFETY:
 * Each Document instance has a single mutex that protects all operations.
 * The mutex is acquired once at the start of each public method.
 * Node operations use their owning Document's mutex.
 * This prevents recursive deadlocks from internal libxml2 calls.
 */
namespace XML {

  // Forward declarations
  class Document;
  struct ProcessingOptions;
  struct ValidationOptions;

  /**
   * @brief Enhanced error handling structure
   */
  struct Error {
      enum class Level { None, Warning, Error, Fatal };

      Level level; ///< Severity level of the error
      std::string message; ///< Error message
      int line; ///< Line number where error occurred
      int column; ///< Column number where error occurred
      std::string context; ///< XML context where error occurred

      Error(Level lvl = Level::None) : level(lvl), line(0), column(0) {}
  };

  /**
   * @brief Options for XML document processing
   *
   * Thread-safe: This struct is meant to be copied and can be safely used across threads.
   */
  struct ProcessingOptions {
      bool preserveWhitespace = false; ///< Keep whitespace in text nodes
      bool validateOnParse = false; ///< Validate document during parsing
      bool resolveExternalEntities = false; ///< Resolve external entity references
  };

  /**
   * @brief Options for XML document validation
   *
   * Thread-safe: This struct is meant to be copied and can be safely used across threads.
   */
  struct ValidationOptions {
      bool validateDTD = false; ///< Validate against DTD
      bool validateSchema = false; ///< Validate against XML Schema
      bool resolveExternalEntities = false; ///< Resolve external entity references
      bool preserveWhitespace = false; ///< Keep whitespace in text nodes
      bool removeComments = false; ///< Remove comments during validation
      bool normalizeNamespaces = true; ///< Normalize namespace declarations
      std::string schemaLocation; ///< Location of XML Schema file
      std::string dtdLocation; ///< Location of DTD file
  };

  /**
   * @brief Represents an XML node in a document
   *
   * Thread safety: Uses owning Document's mutex for all operations.
   */
  class Node {
    public:
      /**
       * @brief Default constructor
       */
      Node();

      /**
       * @brief Construct node from libxml2 node pointer
       * @param node libxml2 node pointer
       * @param doc Owning document pointer
       */
      explicit Node(xmlNodePtr node, Document *doc = nullptr);

      /**
       * @brief Destructor
       */
      ~Node();

      /**
       * @brief Copy constructor
       * @param other Node to copy
       */
      Node(const Node & other);

      /**
       * @brief Copy assignment operator
       * @param other Node to copy
       * @return Reference to this node
       */
      Node & operator=(const Node & other);

      /**
       * @brief Move constructor
       * @param other Node to move from
       */
      Node(Node && other) noexcept;

      /**
       * @brief Move assignment operator
       * @param other Node to move from
       * @return Reference to this node
       */
      Node & operator=(Node && other) noexcept;

      // Basic node operations
      bool isValid() const;
      std::string getName() const;
      void setName(const std::string & name);
      std::string getValue() const;
      void setValue(const std::string & value);

      // Attribute operations
      void setAttribute(const std::string & name, const std::string & value);
      std::string getAttribute(const std::string & name) const;
      bool hasAttribute(const std::string & name) const;
      bool removeAttribute(const std::string & name);
      bool hasAttributes() const;
      std::map<std::string, std::string> getAttributes() const;

      // Child operations
      void addChild(const Node & child);
      Node getChild(const std::string & name) const;
      /// Find first child by namespace URI + local name
      Node getChildNS(const std::string & namespaceURI, const std::string & localName) const;
      std::vector<Node> getChildren() const;
      Node getFirstChild() const;
      Node getNextSibling() const;
      bool hasChild(const std::string & name) const;
      bool removeChild(const Node & child);
      void clearChildren();

      // Node type checking
      bool isElement() const;
      bool isText() const;
      bool isComment() const;
      bool isCDATA() const;
      bool isProcessingInstruction() const;

      // Node comparison
      bool isEqualNode(const Node & other) const;
      bool isSameNode(const Node & other) const;

      // Text content operations
      std::string getTextContent() const;
      void setTextContent(const std::string & content);

      // CDATA operations
      bool isCDATASection() const;
      void wrapWithCDATA();
      void unwrapCDATA();

      // Namespace operations
      std::string getNamespace() const;
      std::string getPrefix() const;
      std::string getLocalName() const;
      std::string lookupNamespaceURI(const std::string & prefix) const;
      std::string lookupPrefix(const std::string & namespaceURI) const;
      bool isDefaultNamespace(const std::string & namespaceURI) const;

    private:
      friend class Document;
      xmlNodePtr node;
      Document *ownerDoc; // Pointer to owning document for mutex access
  };

  /**
   * @brief Represents an XML document
   *
   * Thread safety: All operations are protected by a single mutex per instance.
   * The mutex is acquired once at the start of each public method.
   */
  class Document {
    public:
      /**
       * @brief Default constructor
       */
      Document();

      /**
       * @brief Construct document from XML string
       * @param xml XML string to parse
       */
      explicit Document(const std::string & xml);

      /**
       * @brief Destructor
       */
      ~Document();

      /**
       * @brief Copy constructor
       * @param other Document to copy
       */
      Document(const Document & other);

      /**
       * @brief Copy assignment operator
       * @param other Document to copy
       * @return Reference to this document
       */
      Document & operator=(const Document & other);

      /**
       * @brief Move constructor
       * @param other Document to move from
       */
      Document(Document && other) noexcept;

      /**
       * @brief Move assignment operator
       * @param other Document to move from
       * @return Reference to this document
       */
      Document & operator=(Document && other) noexcept;

      // Node operations
      Node getRoot() const;
      void setRoot(const Node & root);
      Node createNode(const std::string & name);
      Node createNamespacedNode(const std::string & prefix, const std::string & localName);
      void addNamespace(const std::string & prefix, const std::string & uri);

      // XPath operations
      Node evaluateXPath(const std::string & xpath) const;
      std::vector<Node> evaluateXPathAll(const std::string & xpath) const;
      std::string evaluateXPathString(const std::string & xpath) const;
      double evaluateXPathNumber(const std::string & xpath) const;
      bool evaluateXPathBoolean(const std::string & xpath) const;

      // Serialization
      std::string toString(bool prettyPrint = false) const;

      // Namespace operations
      std::string getNamespaceURI(const std::string & prefix) const;
      std::map<std::string, std::string> getNamespaces() const;
      void setDefaultNamespace(const std::string & uri);
      bool hasNamespacePrefix(const std::string & prefix) const;
      bool registerNamespace(const std::string & prefix, const std::string & uri);

      /**
       * @brief Get list of parsing/validation errors
       * @return Vector of Error objects
       */
      std::vector<Error> getErrors() const;

      /**
       * @brief Clear all parsing/validation errors
       */
      void clearErrors();

      /**
       * @brief Import a node from another document
       * @param node Node to import
       * @param deep Whether to import child nodes
       * @return Imported node
       */
      Node importNode(const Node & node, bool deep = true);

      /**
       * @brief Adopt a node from another document
       * @param node Node to adopt
       * @return Adopted node
       */
      Node adoptNode(Node & node);

      /**
       * @brief Clone a node
       * @param node Node to clone
       * @param deep Whether to clone child nodes
       * @return Cloned node
       */
      Node cloneNode(const Node & node, bool deep = true);

      // Status
      bool isValid() const;
      std::string getLastError() const;

    private:
      friend class Node;
      void handleStructuredError(void *ctx, const xmlError *error);

      /**
       * @brief Register namespaces recursively for a node
       * @param node Node to process
       */
      void registerNamespacesRecursive(xmlNodePtr node);

      /**
       * @brief Register namespaces in XPath context for a node
       * @param node Node to process
       */
      void registerNamespacesInContext(xmlNodePtr node);

      xmlDocPtr doc;
      xmlXPathContextPtr ctx;
      std::string lastError;
      std::vector<Error> errors;
      mutable std::mutex mutex; // Shared mutex for all xml2 lib operations
  };

} // namespace XML
