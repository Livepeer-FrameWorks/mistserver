#include <mist/http_parser.h>

#include <iostream>
#include <string>

namespace {
  std::string requestLine(HTTP::Parser & H) {
    const std::string & wire = H.BuildRequest();
    return wire.substr(0, wire.find("\r\n"));
  }

  bool expectLine(HTTP::Parser & H, const std::string & want, const char *description) {
    const std::string got = requestLine(H);
    if (got == want) { return true; }
    std::cerr << description << ": got '" << got << "' want '" << want << "'" << std::endl;
    return false;
  }

  /// HTTP::Downloader composes url as path plus a pre-encoded query; the request
  /// line must keep the separator and the encoded values as composed.
  bool queryKeptAsComposed() {
    HTTP::Parser H;
    H.method = "GET";
    H.protocol = "HTTP/1.1";
    H.url = "/_frameworks/balancer/v2/edge-node-1/demo-media/1788964732/sig-_x/source/by-node/"
            "edge-node-1?source=live%2bdemo_live_stream_001";
    return expectLine(H, "GET /_frameworks/balancer/v2/edge-node-1/demo-media/1788964732/sig-_x/source/by-node/edge-node-1?source=live%2bdemo_live_stream_001 HTTP/1.1",
                      "url with pre-encoded query");
  }

  /// Path characters outside the unreserved set are still percent-encoded.
  bool pathStillEncoded() {
    HTTP::Parser H;
    H.method = "GET";
    H.protocol = "HTTP/1.1";
    H.url = "/dir with space/file?x=1&y=a%20b";
    return expectLine(H, "GET /dir%20with%20space/file?x=1&y=a%20b HTTP/1.1", "path encoded, query untouched");
  }

  /// Parser vars are appended only when the url carries no query of its own.
  bool varsAppendedWithoutQuery() {
    HTTP::Parser H;
    H.method = "GET";
    H.protocol = "HTTP/1.1";
    H.url = "/source/by-node/edge-node-1";
    H.SetVar("source", "live+demo");
    return expectLine(H, "GET /source/by-node/edge-node-1?source=live%2bdemo HTTP/1.1", "vars appended");
  }
} // namespace

int main() {
  bool ok = true;
  if (!queryKeptAsComposed()) { ok = false; }
  if (!pathStillEncoded()) { ok = false; }
  if (!varsAppendedWithoutQuery()) { ok = false; }
  return ok ? 0 : 1;
}
