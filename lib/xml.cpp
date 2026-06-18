#include "xml.h"

#include <libxml/c14n.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/valid.h>
#include <libxml/xmlerror.h>
#include <libxml/xmlreader.h>
#include <libxml/xmlsave.h>
#include <libxml/xmlschemas.h>
#include <libxml/xmlstring.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace XML {

  // Ensure libxml2 is initialized
  class XMLCleanup {
    public:
      ~XMLCleanup() { xmlCleanupParser(); }
  };

  static XMLCleanup cleanup;

  void initLibXML() {
    static bool initialized = false;
    if (!initialized) {
      xmlInitParser();
      // Disable network access globally for security
      xmlSetExternalEntityLoader(nullptr);
      initialized = true;
    }
  }

  Node::Node() : node(nullptr), ownerDoc(nullptr) {}

  Node::Node(xmlNodePtr n, Document *doc) : node(n), ownerDoc(doc) {}

  Node::~Node() = default;

  // Copy operations
  Node::Node(const Node & other) : node(other.node), ownerDoc(other.ownerDoc) {}

  Node & Node::operator=(const Node & other) {
    if (this != &other) {
      node = other.node;
      ownerDoc = other.ownerDoc;
    }
    return *this;
  }

  // Move operations
  Node::Node(Node && other) noexcept : node(other.node), ownerDoc(other.ownerDoc) {
    other.node = nullptr;
    other.ownerDoc = nullptr;
  }

  Node & Node::operator=(Node && other) noexcept {
    if (this != &other) {
      node = other.node;
      ownerDoc = other.ownerDoc;
      other.node = nullptr;
      other.ownerDoc = nullptr;
    }
    return *this;
  }

  bool Node::isValid() const {
    return node != nullptr;
  }

  std::string Node::getName() const {
    if (!isValid()) return "";
    if (ownerDoc) {
      std::lock_guard<std::mutex> lock(ownerDoc->mutex);
      return reinterpret_cast<const char *>(node->name);
    }
    return reinterpret_cast<const char *>(node->name);
  }

  std::string Node::getValue() const {
    if (!isValid()) return "";
    if (ownerDoc) {
      std::lock_guard<std::mutex> lock(ownerDoc->mutex);
      xmlChar *content = xmlNodeGetContent(node);
      if (!content) return "";
      std::string result(reinterpret_cast<const char *>(content));
      xmlFree(content);
      return result;
    }
    xmlChar *content = xmlNodeGetContent(node);
    if (!content) return "";
    std::string result(reinterpret_cast<const char *>(content));
    xmlFree(content);
    return result;
  }

  void Node::setValue(const std::string & value) {
    if (!isValid()) return;
    if (ownerDoc) {
      std::lock_guard<std::mutex> lock(ownerDoc->mutex);
      xmlNodeSetContent(node, BAD_CAST value.c_str());
    } else {
      xmlNodeSetContent(node, BAD_CAST value.c_str());
    }
  }

  void Node::setAttribute(const std::string & name, const std::string & value) {
    if (!isValid()) return;
    if (ownerDoc) {
      std::lock_guard<std::mutex> lock(ownerDoc->mutex);
      xmlSetProp(node, BAD_CAST name.c_str(), BAD_CAST value.c_str());
    } else {
      xmlSetProp(node, BAD_CAST name.c_str(), BAD_CAST value.c_str());
    }
  }

  std::string Node::getAttribute(const std::string & name) const {
    if (!isValid()) return "";
    if (ownerDoc) {
      std::lock_guard<std::mutex> lock(ownerDoc->mutex);
      xmlChar *value = xmlGetProp(node, BAD_CAST name.c_str());
      if (!value) return "";
      std::string result(reinterpret_cast<const char *>(value));
      xmlFree(value);
      return result;
    }
    xmlChar *value = xmlGetProp(node, BAD_CAST name.c_str());
    if (!value) return "";
    std::string result(reinterpret_cast<const char *>(value));
    xmlFree(value);
    return result;
  }

  bool Node::hasAttribute(const std::string & name) const {
    if (!isValid()) return false;
    return xmlHasProp(node, BAD_CAST name.c_str()) != nullptr;
  }

  bool Node::removeAttribute(const std::string & name) {
    if (!isValid()) return false;
    if (ownerDoc) {
      std::lock_guard<std::mutex> lock(ownerDoc->mutex);
      return xmlUnsetProp(node, BAD_CAST name.c_str()) == 0;
    }
    return xmlUnsetProp(node, BAD_CAST name.c_str()) == 0;
  }

  void Node::addChild(const Node & child) {
    if (!isValid() || !child.node) return;
    if (ownerDoc) {
      std::lock_guard<std::mutex> lock(ownerDoc->mutex);
      xmlAddChild(node, child.node);
    } else {
      xmlAddChild(node, child.node);
    }
  }

  Node Node::getChild(const std::string & name) const {
    if (!isValid()) return Node();
    if (ownerDoc) {
      std::lock_guard<std::mutex> lock(ownerDoc->mutex);
      for (xmlNodePtr child = node->children; child; child = child->next) {
        if (xmlStrcmp(child->name, BAD_CAST name.c_str()) == 0) { return Node(child, ownerDoc); }
      }
      return Node();
    }
    for (xmlNodePtr child = node->children; child; child = child->next) {
      if (xmlStrcmp(child->name, BAD_CAST name.c_str()) == 0) { return Node(child, nullptr); }
    }
    return Node();
  }

  Node Node::getChildNS(const std::string & namespaceURI, const std::string & localName) const {
    if (!isValid()) return Node();
    if (ownerDoc) {
      std::lock_guard<std::mutex> lock(ownerDoc->mutex);
      xmlNodePtr c = node->children;
      while (c) {
        // compare local-name and namespace URI, not prefix
        if (c->type == XML_ELEMENT_NODE && xmlStrcmp(c->name, BAD_CAST localName.c_str()) == 0 && c->ns &&
            c->ns->href && xmlStrcmp(c->ns->href, BAD_CAST namespaceURI.c_str()) == 0) {
          return Node(c, ownerDoc);
        }
        c = c->next;
      }
      return Node();
    }
    xmlNodePtr c = node->children;
    while (c) {
      // compare local-name and namespace URI, not prefix
      if (c->type == XML_ELEMENT_NODE && xmlStrcmp(c->name, BAD_CAST localName.c_str()) == 0 && c->ns && c->ns->href &&
          xmlStrcmp(c->ns->href, BAD_CAST namespaceURI.c_str()) == 0) {
        return Node(c, nullptr);
      }
      c = c->next;
    }
    return Node();
  }

  std::vector<Node> Node::getChildren() const {
    std::vector<Node> children;
    if (!isValid()) return children;
    if (ownerDoc) {
      std::lock_guard<std::mutex> lock(ownerDoc->mutex);
      for (xmlNodePtr child = node->children; child; child = child->next) {
        if (child->type == XML_ELEMENT_NODE) { children.emplace_back(child, ownerDoc); }
      }
      return children;
    }
    for (xmlNodePtr child = node->children; child; child = child->next) {
      if (child->type == XML_ELEMENT_NODE) { children.emplace_back(child, nullptr); }
    }
    return children;
  }

  bool Node::hasChild(const std::string & name) const {
    if (!isValid()) return false;
    for (xmlNodePtr child = node->children; child; child = child->next) {
      if (xmlStrcmp(child->name, BAD_CAST name.c_str()) == 0) { return true; }
    }
    return false;
  }

  bool Node::removeChild(const Node & child) {
    if (!isValid() || !child.node) return false;
    if (ownerDoc) {
      std::lock_guard<std::mutex> lock(ownerDoc->mutex);
      xmlUnlinkNode(child.node);
      xmlFreeNode(child.node);
    } else {
      xmlUnlinkNode(child.node);
      xmlFreeNode(child.node);
    }
    return true;
  }

  std::string Node::getNamespace() const {
    if (!isValid()) return "";
    if (!node || !node->ns || !node->ns->href) return "";
    return reinterpret_cast<const char *>(node->ns->href);
  }

  std::string Node::getPrefix() const {
    if (!isValid()) return "";
    xmlNsPtr ns = node->ns;
    if (!ns || !ns->prefix) return "";
    return reinterpret_cast<const char *>(ns->prefix);
  }

  std::string Node::getLocalName() const {
    if (!isValid()) return "";
    return reinterpret_cast<const char *>(node->name);
  }

  // Node type checking
  bool Node::isElement() const {
    if (!isValid()) return false;
    return node->type == XML_ELEMENT_NODE;
  }

  bool Node::isText() const {
    if (!isValid()) return false;
    return node->type == XML_TEXT_NODE;
  }

  bool Node::isComment() const {
    if (!isValid()) return false;
    return node->type == XML_COMMENT_NODE;
  }

  bool Node::isCDATA() const {
    if (!isValid()) return false;
    return node->type == XML_CDATA_SECTION_NODE;
  }

  bool Node::isProcessingInstruction() const {
    if (!isValid()) return false;
    return node->type == XML_PI_NODE;
  }

  // Node comparison
  bool Node::isEqualNode(const Node & other) const {
    if (!isValid() || !other.isValid()) return false;

    // Compare node types
    if (node->type != other.node->type) return false;

    // Compare node names
    if (node->name != other.node->name) {
      if (!node->name || !other.node->name) return false;
      if (xmlStrcmp(node->name, other.node->name) != 0) return false;
    }

    // Compare node content for text nodes
    if (node->type == XML_TEXT_NODE || node->type == XML_CDATA_SECTION_NODE) {
      xmlChar *content1 = xmlNodeGetContent(node);
      xmlChar *content2 = xmlNodeGetContent(other.node);
      bool equal = (content1 && content2 && xmlStrcmp(content1, content2) == 0);
      if (content1) xmlFree(content1);
      if (content2) xmlFree(content2);
      return equal;
    }

    // Compare attributes
    xmlAttrPtr attr1 = node->properties;
    xmlAttrPtr attr2 = other.node->properties;
    while (attr1 && attr2) {
      if (xmlStrcmp(attr1->name, attr2->name) != 0) return false;
      xmlChar *value1 = xmlNodeGetContent(attr1->children);
      xmlChar *value2 = xmlNodeGetContent(attr2->children);
      bool equal = (value1 && value2 && xmlStrcmp(value1, value2) == 0);
      if (value1) xmlFree(value1);
      if (value2) xmlFree(value2);
      if (!equal) return false;
      attr1 = attr1->next;
      attr2 = attr2->next;
    }
    if (attr1 || attr2) return false; // Different number of attributes

    // Compare children recursively
    xmlNodePtr child1 = node->children;
    xmlNodePtr child2 = other.node->children;
    while (child1 && child2) {
      Node childNode1(child1, ownerDoc);
      Node childNode2(child2, other.ownerDoc);
      if (!childNode1.isEqualNode(childNode2)) return false;
      child1 = child1->next;
      child2 = child2->next;
    }
    return child1 == nullptr && child2 == nullptr; // Same number of children
  }

  bool Node::isSameNode(const Node & other) const {
    return node == other.node;
  }

  // Text content operations
  std::string Node::getTextContent() const {
    if (!isValid()) return "";
    xmlChar *content = xmlNodeGetContent(node);
    if (!content) return "";
    std::string result((const char *)content);
    xmlFree(content);
    return result;
  }

  void Node::setTextContent(const std::string & content) {
    if (!isValid()) return;
    xmlNodeSetContent(node, BAD_CAST content.c_str());
  }

  // CDATA operations
  bool Node::isCDATASection() const {
    if (!isValid()) return false;
    return node && node->type == XML_CDATA_SECTION_NODE;
  }

  void Node::wrapWithCDATA() {
    if (!isValid() || node->type != XML_TEXT_NODE) return;
    xmlChar *content = xmlNodeGetContent(node);
    if (!content) return;
    xmlNodePtr cdata = xmlNewCDataBlock(node->doc, content, xmlStrlen(content));
    xmlFree(content);
    if (cdata) {
      xmlReplaceNode(node, cdata);
      node = cdata;
    }
  }

  void Node::unwrapCDATA() {
    if (!isValid() || node->type != XML_CDATA_SECTION_NODE) return;
    xmlChar *content = xmlNodeGetContent(node);
    if (!content) return;
    xmlNodePtr text = xmlNewText(content);
    xmlFree(content);
    if (text) {
      xmlReplaceNode(node, text);
      node = text;
    }
  }

  void Node::clearChildren() {
    if (!isValid()) return;
    if (ownerDoc) {
      std::lock_guard<std::mutex> lock(ownerDoc->mutex);
      xmlNodePtr child = node->children;
      while (child) {
        xmlNodePtr next = child->next;
        xmlUnlinkNode(child);
        xmlFreeNode(child);
        child = next;
      }
      node->children = nullptr;
      node->last = nullptr;
    } else {
      xmlNodePtr child = node->children;
      while (child) {
        xmlNodePtr next = child->next;
        xmlUnlinkNode(child);
        xmlFreeNode(child);
        child = next;
      }
      node->children = nullptr;
      node->last = nullptr;
    }
  }

  std::map<std::string, std::string> Node::getAttributes() const {
    std::map<std::string, std::string> attrs;
    if (!isValid()) return attrs;
    for (xmlAttr *attr = node->properties; attr; attr = attr->next) {
      if (attr->name && attr->children && attr->children->content) {
        attrs[reinterpret_cast<const char *>(attr->name)] = reinterpret_cast<const char *>(attr->children->content);
      }
    }
    return attrs;
  }

  std::string Node::lookupNamespaceURI(const std::string & prefix) const {
    if (!isValid()) return "";

    xmlNs *ns = xmlSearchNs(node->doc, node, reinterpret_cast<const xmlChar *>(prefix.c_str()));
    if (ns && ns->href) { return reinterpret_cast<const char *>(ns->href); }
    return "";
  }

  std::string Node::lookupPrefix(const std::string & namespaceURI) const {
    if (!isValid()) return "";

    xmlNs *ns = xmlSearchNsByHref(node->doc, node, reinterpret_cast<const xmlChar *>(namespaceURI.c_str()));
    if (ns && ns->prefix) { return reinterpret_cast<const char *>(ns->prefix); }
    return "";
  }

  bool Node::isDefaultNamespace(const std::string & namespaceURI) const {
    if (!isValid()) return false;

    // Get the default namespace for this node
    xmlNs *ns = xmlSearchNs(node->doc, node, nullptr);
    if (!ns || !ns->href) return namespaceURI.empty();

    return namespaceURI == reinterpret_cast<const char *>(ns->href);
  }

  bool Node::hasAttributes() const {
    if (!isValid()) return false;
    return node->properties != nullptr;
  }

  void Node::setName(const std::string & name) {
    if (!isValid()) return;
    if (ownerDoc) {
      std::lock_guard<std::mutex> lock(ownerDoc->mutex);
      xmlNodeSetName(node, BAD_CAST name.c_str());
    } else {
      xmlNodeSetName(node, BAD_CAST name.c_str());
    }
  }

  Node Node::getFirstChild() const {
    if (!isValid()) return Node();
    if (ownerDoc) {
      std::lock_guard<std::mutex> lock(ownerDoc->mutex);
      for (xmlNodePtr child = node->children; child; child = child->next) {
        if (child->type == XML_ELEMENT_NODE) { return Node(child, ownerDoc); }
      }
      return Node();
    }
    for (xmlNodePtr child = node->children; child; child = child->next) {
      if (child->type == XML_ELEMENT_NODE) { return Node(child, nullptr); }
    }
    return Node();
  }

  Node Node::getNextSibling() const {
    if (!isValid()) return Node();
    if (ownerDoc) {
      std::lock_guard<std::mutex> lock(ownerDoc->mutex);
      for (xmlNodePtr sibling = node->next; sibling; sibling = sibling->next) {
        if (sibling->type == XML_ELEMENT_NODE) { return Node(sibling, ownerDoc); }
      }
      return Node();
    }
    for (xmlNodePtr sibling = node->next; sibling; sibling = sibling->next) {
      if (sibling->type == XML_ELEMENT_NODE) { return Node(sibling, nullptr); }
    }
    return Node();
  }

  Document::Document() : doc(nullptr), ctx(nullptr) {
    std::lock_guard<std::mutex> lock(mutex);
    initLibXML(); // Keep global mutex for libxml2 init

    // Create a new empty document
    doc = xmlNewDoc(BAD_CAST "1.0");
    if (!doc) { throw std::runtime_error("Failed to create new XML document"); }

    // Create XPath context
    ctx = xmlXPathNewContext(doc);
    if (!ctx) {
      xmlFreeDoc(doc);
      doc = nullptr;
      throw std::runtime_error("Failed to create XPath context");
    }

    // Create root element to hold namespaces
    xmlNodePtr root = xmlNewNode(nullptr, BAD_CAST "root");
    if (!root) {
      xmlXPathFreeContext(ctx);
      xmlFreeDoc(doc);
      doc = nullptr;
      ctx = nullptr;
      throw std::runtime_error("Failed to create root element");
    }
    xmlDocSetRootElement(doc, root);
  }

  Document::Document(const std::string & xml) : doc(nullptr), ctx(nullptr) {
    std::lock_guard<std::mutex> lock(mutex);
    initLibXML();

    // Parse XML with security flags to prevent XXE attacks
    int parserOptions = XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING;
    doc = xmlReadMemory(xml.c_str(), xml.length(), nullptr, nullptr, parserOptions);
    if (!doc) { throw std::runtime_error("Failed to parse XML document"); }

    // Create XPath context
    ctx = xmlXPathNewContext(doc);
    if (!ctx) {
      xmlFreeDoc(doc);
      doc = nullptr;
      throw std::runtime_error("Failed to create XPath context");
    }

    // Register all namespaces found in the document recursively
    xmlNodePtr root = xmlDocGetRootElement(doc);
    if (root) { registerNamespacesRecursive(root); }
  }

  Document::~Document() {
    std::lock_guard<std::mutex> lock(mutex);
    if (ctx) xmlXPathFreeContext(ctx);
    if (doc) xmlFreeDoc(doc);
  }

  Document::Document(const Document & other) : doc(nullptr), ctx(nullptr) {
    std::lock_guard<std::mutex> lock(other.mutex);
    if (other.doc) {
      doc = xmlCopyDoc(other.doc, 1);
      if (doc) {
        ctx = xmlXPathNewContext(doc);
        if (ctx) { registerNamespacesInContext(xmlDocGetRootElement(doc)); }
      }
    }
    lastError = other.lastError;
    errors = other.errors;
  }

  Document & Document::operator=(const Document & other) {
    if (this != &other) {
      std::lock(mutex, other.mutex);
      std::lock_guard<std::mutex> lk1(mutex, std::adopt_lock);
      std::lock_guard<std::mutex> lk2(other.mutex, std::adopt_lock);
      if (ctx) xmlXPathFreeContext(ctx);
      if (doc) xmlFreeDoc(doc);
      doc = nullptr;
      ctx = nullptr;

      if (other.doc) {
        doc = xmlCopyDoc(other.doc, 1);
        if (doc) {
          ctx = xmlXPathNewContext(doc);
          if (ctx) { registerNamespacesInContext(xmlDocGetRootElement(doc)); }
        }
      }
      lastError = other.lastError;
      errors = other.errors;
    }
    return *this;
  }

  Document::Document(Document && other) noexcept
    : doc(other.doc), ctx(other.ctx), lastError(std::move(other.lastError)), errors(std::move(other.errors)) {
    other.doc = nullptr;
    other.ctx = nullptr;
  }

  Document & Document::operator=(Document && other) noexcept {
    if (this != &other) {
      if (ctx) xmlXPathFreeContext(ctx);
      if (doc) xmlFreeDoc(doc);

      doc = other.doc;
      ctx = other.ctx;
      lastError = std::move(other.lastError);
      errors = std::move(other.errors);

      other.doc = nullptr;
      other.ctx = nullptr;
    }
    return *this;
  }

  Node Document::getRoot() const {
    std::lock_guard<std::mutex> lock(mutex);
    if (!doc) return Node();
    return Node(xmlDocGetRootElement(doc), const_cast<Document *>(this));
  }

  void Document::setRoot(const Node & root) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!doc || !root.isValid()) return;

    xmlNodePtr oldRoot = xmlDocGetRootElement(doc);
    if (oldRoot) {
      xmlUnlinkNode(oldRoot);
      xmlFreeNode(oldRoot);
    }

    xmlDocSetRootElement(doc, root.node);

    // Register all namespaces from new root recursively
    if (ctx) { registerNamespacesRecursive(root.node); }
  }

  Node Document::createNode(const std::string & name) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!doc) return Node();
    return Node(xmlNewNode(nullptr, BAD_CAST name.c_str()), this);
  }

  Node Document::createNamespacedNode(const std::string & prefix, const std::string & localName) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!doc || !ctx) return Node();

    xmlNsPtr ns = xmlSearchNs(doc, xmlDocGetRootElement(doc), BAD_CAST prefix.c_str());
    if (!ns) return Node();

    return Node(xmlNewNode(ns, BAD_CAST localName.c_str()), this);
  }

  void Document::addNamespace(const std::string & prefix, const std::string & uri) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!doc || !ctx) return;

    xmlNodePtr root = xmlDocGetRootElement(doc);
    if (!root) return;

    // Create new namespace declaration
    xmlNewNs(root, BAD_CAST uri.c_str(), BAD_CAST prefix.c_str());

    // Register in XPath context even if namespace already exists
    // This ensures the namespace is available for XPath queries
    xmlXPathRegisterNs(ctx, BAD_CAST prefix.c_str(), BAD_CAST uri.c_str());
  }

  Node Document::evaluateXPath(const std::string & xpath) const {
    std::lock_guard<std::mutex> lock(mutex);
    if (!doc || !ctx) return Node();

    xmlXPathObjectPtr result = xmlXPathEvalExpression(BAD_CAST xpath.c_str(), ctx);
    if (!result) return Node();

    Node node;
    if (result->nodesetval && result->nodesetval->nodeNr > 0) {
      node = Node(result->nodesetval->nodeTab[0], const_cast<Document *>(this));
    }

    xmlXPathFreeObject(result);
    return node;
  }

  std::vector<Node> Document::evaluateXPathAll(const std::string & xpath) const {
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<Node> nodes;
    if (!doc || !ctx) return nodes;

    xmlXPathObjectPtr result = xmlXPathEvalExpression(BAD_CAST xpath.c_str(), ctx);
    if (!result) return nodes;

    if (result->nodesetval) {
      for (int i = 0; i < result->nodesetval->nodeNr; i++) {
        nodes.push_back(Node(result->nodesetval->nodeTab[i], const_cast<Document *>(this)));
      }
    }

    xmlXPathFreeObject(result);
    return nodes;
  }

  std::string Document::evaluateXPathString(const std::string & xpath) const {
    std::lock_guard<std::mutex> lock(mutex);
    if (!doc || !ctx) return "";

    xmlXPathObjectPtr result = xmlXPathEvalExpression(BAD_CAST xpath.c_str(), ctx);
    if (!result) return "";

    xmlChar *str = xmlXPathCastToString(result);
    std::string value = str ? reinterpret_cast<char *>(str) : "";

    xmlFree(str);
    xmlXPathFreeObject(result);
    return value;
  }

  double Document::evaluateXPathNumber(const std::string & xpath) const {
    std::lock_guard<std::mutex> lock(mutex);
    if (!doc || !ctx) return 0.0;

    xmlXPathObjectPtr result = xmlXPathEvalExpression(BAD_CAST xpath.c_str(), ctx);
    if (!result) return 0.0;

    double value = xmlXPathCastToNumber(result);
    xmlXPathFreeObject(result);
    return value;
  }

  bool Document::evaluateXPathBoolean(const std::string & xpath) const {
    std::lock_guard<std::mutex> lock(mutex);
    if (!doc || !ctx) return false;

    xmlXPathObjectPtr result = xmlXPathEvalExpression(BAD_CAST xpath.c_str(), ctx);
    if (!result) return false;

    int value = xmlXPathCastToBoolean(result);
    xmlXPathFreeObject(result);
    return value != 0;
  }

  std::string Document::toString(bool prettyPrint) const {
    // Create a unique_ptr for automatic cleanup of the local doc copy
    struct DocDeleter {
        void operator()(xmlDocPtr d) {
          if (d) xmlFreeDoc(d);
        }
    };

    // First acquire the lock and make a copy
    std::unique_ptr<xmlDoc, DocDeleter> localDoc;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (!doc) throw std::runtime_error("Invalid XML document");
      localDoc.reset(xmlCopyDoc(doc, 1));
      if (!localDoc) throw std::runtime_error("Failed to copy XML document");
    }

    // Now work with the copy without holding the lock
    xmlChar *buffer = nullptr;
    int size = 0;

    // Create a scope for buffer cleanup
    struct BufferDeleter {
        void operator()(xmlChar *b) {
          if (b) xmlFree(b);
        }
    };
    std::unique_ptr<xmlChar, BufferDeleter> bufferGuard;

    xmlDocDumpFormatMemory(localDoc.get(), &buffer, &size, prettyPrint ? 1 : 0);
    bufferGuard.reset(buffer);

    if (!buffer) throw std::runtime_error("Failed to serialize XML document");

    return std::string(reinterpret_cast<char *>(buffer));
  }

  std::vector<Error> Document::getErrors() const {
    std::lock_guard<std::mutex> lock(mutex);
    return errors;
  }

  void Document::clearErrors() {
    std::lock_guard<std::mutex> lock(mutex);
    errors.clear();
    xmlResetLastError();
    xmlCtxtResetLastError(nullptr);
  }

  Node Document::importNode(const Node & node, bool deep) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!doc || !node.node) return Node();
    xmlNodePtr imported = xmlDocCopyNode(node.node, doc, deep ? 1 : 0);
    if (!imported) return Node();
    return Node(imported, this);
  }

  Node Document::adoptNode(Node & node) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!doc || !node.node) return Node(nullptr, nullptr);
    xmlNodePtr adopted = xmlDocCopyNode(node.node, doc, 1);
    if (!adopted) return Node(nullptr, nullptr);
    xmlUnlinkNode(node.node);
    xmlFreeNode(node.node);
    node.node = nullptr;
    return Node(adopted, this);
  }

  Node Document::cloneNode(const Node & node, bool deep) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!doc || !node.node) return Node(nullptr, nullptr);
    xmlNodePtr cloned = xmlCopyNode(node.node, deep ? 1 : 0);
    if (!cloned) return Node(nullptr, nullptr);
    return Node(cloned, this);
  }

  bool Document::isValid() const {
    std::lock_guard<std::mutex> lock(mutex);
    return doc != nullptr;
  }

  std::string Document::getLastError() const {
    std::lock_guard<std::mutex> lock(mutex);
    return lastError;
  }

  void Document::registerNamespacesInContext(xmlNodePtr node) {
    if (!node || !ctx) return;

    // Register the node's namespace if it exists
    if (node->ns && node->ns->prefix && node->ns->href) { xmlXPathRegisterNs(ctx, node->ns->prefix, node->ns->href); }

    // Register namespaces from attributes
    for (xmlAttrPtr attr = node->properties; attr; attr = attr->next) {
      if (attr->ns && attr->ns->prefix && attr->ns->href) { xmlXPathRegisterNs(ctx, attr->ns->prefix, attr->ns->href); }
    }
  }

  void Document::registerNamespacesRecursive(xmlNodePtr node) {
    if (!node) return;

    // Register namespaces for current node
    registerNamespacesInContext(node);

    // Recursively process children
    for (xmlNodePtr child = node->children; child; child = child->next) { registerNamespacesRecursive(child); }
  }

  void Document::handleStructuredError(void *ctx, const xmlError *error) {
    if (!ctx || !error) return;

    Document *doc = static_cast<Document *>(ctx);
    Error xmlError(Error::Level::Error);
    xmlError.message = error->message ? error->message : "Unknown error";
    xmlError.line = error->line;
    xmlError.column = error->int2; // int2 contains the column number

    doc->errors.push_back(xmlError);
  }

  std::string Document::getNamespaceURI(const std::string & prefix) const {
    if (!isValid()) return "";

    xmlNsPtr ns = xmlSearchNs(doc, xmlDocGetRootElement(doc), BAD_CAST prefix.c_str());
    if (ns && ns->href) { return std::string(reinterpret_cast<const char *>(ns->href)); }
    return "";
  }

  std::map<std::string, std::string> Document::getNamespaces() const {
    std::lock_guard<std::mutex> lock(mutex);
    std::map<std::string, std::string> namespaces;
    if (!doc) return namespaces;

    xmlNodePtr root = xmlDocGetRootElement(doc);
    if (!root) return namespaces;

    // Get all namespaces in scope
    xmlNsPtr *nsList = xmlGetNsList(doc, root);
    if (nsList) {
      for (xmlNsPtr *ns = nsList; *ns; ns++) {
        if ((*ns)->prefix && (*ns)->href) {
          std::string prefix(reinterpret_cast<const char *>((*ns)->prefix));
          std::string uri(reinterpret_cast<const char *>((*ns)->href));
          namespaces[prefix] = uri;
        }
      }
      xmlFree(nsList);
    }

    return namespaces;
  }

  void Document::setDefaultNamespace(const std::string & uri) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!doc) return;

    xmlNodePtr root = xmlDocGetRootElement(doc);
    if (!root) return;

    // Create new namespace without prefix
    xmlNewNs(root, BAD_CAST uri.c_str(), nullptr);
  }

  bool Document::hasNamespacePrefix(const std::string & prefix) const {
    if (!isValid()) return false;

    xmlNodePtr root = xmlDocGetRootElement(doc);
    if (!root) return false;

    xmlNsPtr ns = xmlSearchNs(doc, root, BAD_CAST prefix.c_str());
    return ns != nullptr;
  }

  bool Document::registerNamespace(const std::string & prefix, const std::string & uri) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!doc || !ctx) return false;

    // Register namespace in XPath context
    int result = xmlXPathRegisterNs(ctx, BAD_CAST prefix.c_str(), BAD_CAST uri.c_str());
    if (result != 0) return false;

    // Also add namespace declaration to root element if it doesn't exist
    xmlNodePtr root = xmlDocGetRootElement(doc);
    if (root) {
      // Check if namespace already exists
      xmlNsPtr existing = xmlSearchNs(doc, root, BAD_CAST prefix.c_str());
      if (!existing) {
        // Create new namespace declaration
        xmlNewNs(root, BAD_CAST uri.c_str(), BAD_CAST prefix.c_str());
      }
    }

    return true;
  }

} // namespace XML
