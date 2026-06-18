#include <mist/xml.h>

#include <iostream>
#include <string>
#include <utility>

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

  bool testNodeMoveAssignmentDoesNotCorruptTree() {
    XML::Document doc;
    XML::Node root = doc.createNode("root");
    doc.setRoot(root);

    XML::Node attached = doc.createNode("attached");
    root.addChild(attached);

    XML::Node incoming = doc.createNode("incoming");
    attached = std::move(incoming);

    bool ok = true;
    ok &= expect(!incoming.isValid(), "moved-from node should be invalid");
    ok &= expect(attached.isValid(), "move target should stay valid");
    ok &= expect(root.hasChild("attached"), "existing tree node should remain after move assignment");

    root.addChild(attached);
    const std::string xml = doc.toString();
    ok &= expect(contains(xml, "<attached"), "document should still contain original attached node");
    ok &= expect(contains(xml, "<incoming"), "document should accept moved-in replacement node");
    return ok;
  }

  bool testDocumentCopyConstructorDeepCopy() {
    XML::Document original("<root><child>before</child></root>");
    XML::Document copy(original);

    original.getRoot().getChild("child").setTextContent("after");

    bool ok = true;
    ok &= expect(original.getRoot().getChild("child").getTextContent() == "after", "original value should update after mutation");
    ok &= expect(copy.getRoot().getChild("child").getTextContent() == "before", "copy-constructed document should remain independent");
    return ok;
  }

  bool testDocumentCopyAssignmentDeepCopy() {
    XML::Document source("<root><value>one</value></root>");
    XML::Document target("<other><value>zero</value></other>");

    target = source;
    source.getRoot().getChild("value").setTextContent("two");

    bool ok = true;
    ok &= expect(source.getRoot().getChild("value").getTextContent() == "two", "source should reflect mutation");
    ok &= expect(target.getRoot().getChild("value").getTextContent() == "one", "copy-assigned target should remain independent");
    return ok;
  }

  bool testNamespacedNodeCreation() {
    XML::Document doc;
    XML::Node root = doc.createNode("s:Envelope");
    doc.setRoot(root);
    doc.addNamespace("s", "http://www.w3.org/2003/05/soap-envelope");
    doc.addNamespace("wsa", "http://www.w3.org/2005/08/addressing");

    XML::Node action = doc.createNamespacedNode("wsa", "Action");
    action.setTextContent("urn:test");
    root.addChild(action);

    const std::string xml = doc.toString();
    bool ok = true;
    ok &= expect(action.isValid(), "namespaced node should be created for known prefix");
    ok &= expect(contains(xml, "wsa:Action"), "serialized XML should contain namespaced node");
    ok &= expect(contains(xml, "urn:test"), "serialized XML should contain node content");
    return ok;
  }

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "Usage: " << argv[0] << " <test_case>" << std::endl;
    return 2;
  }

  const std::string testCase = argv[1];
  if (testCase == "node_move_assignment_tree_integrity") { return testNodeMoveAssignmentDoesNotCorruptTree() ? 0 : 1; }
  if (testCase == "copy_constructor_deep_copy") { return testDocumentCopyConstructorDeepCopy() ? 0 : 1; }
  if (testCase == "copy_assignment_deep_copy") { return testDocumentCopyAssignmentDeepCopy() ? 0 : 1; }
  if (testCase == "namespaced_node_creation") { return testNamespacedNodeCreation() ? 0 : 1; }

  std::cerr << "Unknown test case: " << testCase << std::endl;
  return 2;
}
