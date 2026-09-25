#include "../src/controller/connector_handover.h"

#include <iostream>

static int fail(const char *msg) {
  std::cerr << msg << "\n";
  return 1;
}

int main() {
  using namespace Controller;
  JSON::Value httpCapa;
  httpCapa["optional"]["port"]["default"] = 8080;

  JSON::Value oldHttp;
  oldHttp["connector"] = "HTTP";
  oldHttp["pubaddr"] = "http://localhost:18090/view/";
  JSON::Value newHttp = oldHttp;
  newHttp["pubaddr"] = "http://edge:8082/";
  if (connectorBindKey(oldHttp, httpCapa) != "tcp:8080" || connectorBindKey(newHttp, httpCapa) != "tcp:8080") {
    return fail("a connector without an explicit port binds its capability default");
  }

  JSON::Value moved = newHttp;
  moved["port"] = 8081;
  if (connectorBindKey(moved, httpCapa) != "tcp:8081") { return fail("an explicit port overrides the default"); }

  JSON::Value unixSock;
  unixSock["connector"] = "HTTP";
  unixSock["socket"] = "http.sock";
  if (connectorBindKey(unixSock, httpCapa) != "unix:http.sock") { return fail("a socket path is its own bind key"); }

  JSON::Value noPortCapa;
  JSON::Value dtsc;
  dtsc["connector"] = "DTSC";
  if (connectorBindKey(dtsc, noPortCapa) != "") { return fail("a connector that binds nothing never waits"); }

  if (connectorHandoverStep(false, 0) != ConnectorHandoverStep::Start) {
    return fail("the replacement starts once the previous listener exited");
  }
  if (connectorHandoverStep(true, CONNECTOR_STOP_GRACE_MS - 1) != ConnectorHandoverStep::Wait) {
    return fail("the replacement waits while the previous listener is still stopping");
  }
  if (connectorHandoverStep(true, CONNECTOR_STOP_GRACE_MS) != ConnectorHandoverStep::Kill) {
    return fail("a listener that outlives the grace period is killed");
  }
  return 0;
}
