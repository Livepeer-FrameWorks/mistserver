#include <mist/comms.h>

#include <iostream>
#include <string>

namespace {
  int fail(const std::string & message) {
    std::cerr << message << std::endl;
    return 1;
  }
} // namespace

int main() {
  // The effective origin is the Origin header, or the Referer's origin when Origin is absent or "null".
  if (Comms::viewerSessionOrigin("https://Embed.Example", "https://other.example/x") != "https://embed.example") {
    return fail("Origin header must win and be lowercased");
  }
  if (Comms::viewerSessionOrigin("null", "https://embed.example:8443/watch?v=1") != "https://embed.example:8443") {
    return fail("a null Origin must fall back to the Referer's origin");
  }
  if (Comms::viewerSessionOrigin("", "") != "" || Comms::viewerSessionOrigin("", "not a url") != "") {
    return fail("no header must yield no origin");
  }

  // Both USER_NEW payloads carry origin and referer on lines 8 and 9, always, and end with a newline so
  // empty trailing lines survive parsing.
  std::string payload = Comms::userNewPayload("live+s", "203.0.113.9", "tok", "HLS", "http://edge/x", "sess", false,
                                              "https://embed.example", "https://embed.example/watch");
  if (payload != "live+s\n203.0.113.9\ntok\nHLS\nhttp://edge/x\nsess\nfalse\nhttps://embed.example\nhttps://embed.example/watch\n") {
    return fail("USER_NEW payload layout changed: " + payload);
  }
  if (Comms::userNewPayload("live+s", "h", "", "RTMP", "u", "sess", true, "", "") != "live+s\nh\n\nRTMP\nu\nsess\ntrue\n\n\n") {
    return fail("empty origin and referer must still be written");
  }

  // Two requests that differ only in origin must not share a session: USER_NEW admitted the first origin only.
  Comms::Connections connections;
  const std::string ip(16, '\0');
  const std::string allowed = connections.generateSession("live+s", ip, "tok", "HLS", 0x0F, "https://allowed.example");
  const std::string other = connections.generateSession("live+s", ip, "tok", "HLS", 0x0F, "https://other.example");
  const std::string none = connections.generateSession("live+s", ip, "tok", "HLS", 0x0F);
  if (allowed == other || allowed == none || other == none) {
    return fail("a different viewer origin must yield a different session");
  }
  if (allowed != connections.generateSession("live+s", ip, "tok", "HLS", 0x0F, "https://allowed.example")) {
    return fail("the same origin must keep its session");
  }
  return 0;
}
