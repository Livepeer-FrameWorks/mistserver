#include <mist/downloader.h>
#include <mist/http_parser.h>
#include <mist/socket.h>
#include <mist/timing.h>

#include <atomic>
#include <iostream>
#include <string>
#include <thread>

namespace {

  bool readRequest(Socket::Connection & conn) {
    HTTP::Parser request;
    const uint64_t deadline = Util::bootMS() + 3000;
    while (conn && Util::bootMS() < deadline) {
      conn.spool();
      if (request.Read(conn)) { return true; }
      Util::sleep(10);
    }
    return false;
  }

  HTTP::URL localUrl(const Socket::Server & server) {
    return HTTP::URL("http://127.0.0.1:" + std::to_string(server.getBoundAddr().port()) + "/test");
  }

  bool testHeadClosesAdvertisedConnection() {
    Socket::Server server(0, "127.0.0.1", false);
    if (!server.getBoundAddr().port()) { return false; }
    HTTP::URL url = localUrl(server);
    std::atomic<bool> requestRead(false);
    std::atomic<bool> responseSent(false);
    std::thread peer([&]() {
      Socket::Connection conn = server.accept();
      if (!readRequest(conn)) { return; }
      requestRead = true;
      HTTP::Parser response;
      response.protocol = "HTTP/1.1";
      response.SetHeader("Connection", "ClOsE");
      // A HEAD response advertises the corresponding GET body length without sending that body.
      response.SetHeader("Content-Length", "4");
      response.SendResponse("200", "OK", conn);
      responseSent = true;
      Util::sleep(500);
    });

    HTTP::Downloader downloader;
    downloader.retryCount = 1;
    downloader.dataTimeout = 1;
    const bool ok = downloader.head(url) && !downloader.getSocket();
    peer.join();
    if (!requestRead) { std::cerr << "HEAD peer did not parse the request" << std::endl; }
    if (!responseSent) { std::cerr << "HEAD peer did not send the response" << std::endl; }
    return ok;
  }

  bool testPostTracksResponsePhaseFailure() {
    Socket::Server server(0, "127.0.0.1", false);
    if (!server.getBoundAddr().port()) { return false; }
    HTTP::URL url = localUrl(server);
    std::thread peer([&]() {
      Socket::Connection conn = server.accept();
      readRequest(conn);
      conn.close();
    });

    HTTP::Downloader downloader;
    downloader.retryCount = 1;
    downloader.dataTimeout = 1;
    const bool result = downloader.post(url, std::string("request-body"), true);
    peer.join();
    return !result && downloader.requestWasSent();
  }

  bool testPostTracksPreSendFailure() {
    Socket::Server unused(0, "127.0.0.1", false);
    if (!unused.getBoundAddr().port()) { return false; }
    HTTP::URL url = localUrl(unused);
    unused.close();

    HTTP::Downloader downloader;
    downloader.retryCount = 1;
    downloader.dataTimeout = 1;
    const bool result = downloader.post(url, std::string("request-body"), true);
    return !result && !downloader.requestWasSent();
  }

  /// Answers two POSTs, each on its own connection. With `announceClose` the
  /// first response says Connection: close; without it the peer closes its
  /// keep-alive connection after a moment, the way a server ends an idle one.
  bool postsAfterPeerClose(bool announceClose) {
    Socket::Server server(0, "127.0.0.1", false);
    if (!server.getBoundAddr().port()) { return false; }
    HTTP::URL url = localUrl(server);
    std::atomic<int> accepted(0);
    std::thread peer([&]() {
      for (int i = 0; i < 2; ++i) {
        Socket::Connection conn = server.accept();
        ++accepted;
        if (!readRequest(conn)) { return; }
        HTTP::Parser response;
        response.protocol = "HTTP/1.1";
        if (announceClose) { response.SetHeader("Connection", "close"); }
        response.SetHeader("Content-Length", "2");
        response.SetBody("ok");
        response.SendResponse("200", "OK", conn);
        Util::sleep(50);
        conn.close();
      }
    });

    HTTP::Downloader downloader;
    // One attempt per POST, as Livepeer uploads use: a request may not depend on
    // the downloader's own re-send to get past a connection the peer closed.
    downloader.retryCount = 1;
    downloader.dataTimeout = 2;
    const bool first = downloader.post(url, std::string("first"), true);
    Util::sleep(300);
    const bool second = first && downloader.post(url, std::string("second"), true);
    // A failed POST may never have opened the second connection the peer waits for.
    if (accepted < 2) { Socket::Connection unblock("127.0.0.1", server.getBoundAddr().port(), false); }
    peer.join();
    return first && second;
  }

} // namespace

namespace {
  /// Returns the raw request line a local peer receives for a POST to path.
  std::string wireRequestLine(const std::string & path) {
    Socket::Server server(0, "127.0.0.1", false);
    if (!server.getBoundAddr().port()) { return ""; }
    HTTP::URL url("http://127.0.0.1:" + std::to_string(server.getBoundAddr().port()) + path);
    std::string line;
    std::thread peer([&]() {
      Socket::Connection conn = server.accept();
      std::string raw;
      const uint64_t deadline = Util::bootMS() + 3000;
      while (conn && Util::bootMS() < deadline && raw.find("\r\n") == std::string::npos) {
        if (conn.spool()) { raw += conn.Received().remove(conn.Received().bytes()); }
        Util::sleep(5);
      }
      line = raw.substr(0, raw.find("\r\n"));
      HTTP::Parser response;
      response.protocol = "HTTP/1.1";
      response.SetHeader("Content-Length", "0");
      response.SendResponse("200", "OK", conn);
      Util::sleep(100);
    });
    HTTP::Downloader downloader;
    downloader.retryCount = 1;
    downloader.dataTimeout = 1;
    downloader.post(url, std::string("segment"), true);
    peer.join();
    return line;
  }

  /// The request target is percent-encoded exactly once: a '+' in a stream name
  /// reaches the server as %2b, never as %252b.
  bool testPathEncodedOnce() {
    const std::string plus = wireRequestLine("/live/live+abc-UNbT3h5Q/0.ts");
    const std::string encodedPlus = wireRequestLine("/live/live%2Babc-UNbT3h5Q/0.ts");
    const std::string space = wireRequestLine("/dir with space/0.ts");
    bool ok = true;
    if (plus != "POST /live/live%2babc-UNbT3h5Q/0.ts HTTP/1.1") {
      std::cerr << "'+' path sent as: " << plus << std::endl;
      ok = false;
    }
    if (encodedPlus != plus) {
      std::cerr << "pre-encoded '+' path sent as: " << encodedPlus << std::endl;
      ok = false;
    }
    if (space != "POST /dir%20with%20space/0.ts HTTP/1.1") {
      std::cerr << "space path sent as: " << space << std::endl;
      ok = false;
    }
    return ok;
  }
} // namespace

int main() {
  const bool headClose = testHeadClosesAdvertisedConnection();
  const bool responseFailure = testPostTracksResponsePhaseFailure();
  const bool preSendFailure = testPostTracksPreSendFailure();
  const bool encodedOnce = testPathEncodedOnce();
  const bool postClose = postsAfterPeerClose(true);
  const bool postIdleClose = postsAfterPeerClose(false);

  if (!headClose) { std::cerr << "HEAD response did not close a Connection: close socket" << std::endl; }
  if (!responseFailure) { std::cerr << "POST response-phase failure was not classified as sent" << std::endl; }
  if (!preSendFailure) { std::cerr << "POST pre-send failure was incorrectly classified as sent" << std::endl; }
  if (!encodedOnce) { std::cerr << "request path was not percent-encoded exactly once" << std::endl; }
  if (!postClose) { std::cerr << "POST after a Connection: close response failed" << std::endl; }
  if (!postIdleClose) { std::cerr << "POST on a keep-alive connection the peer closed failed" << std::endl; }
  return headClose && responseFailure && preSendFailure && encodedOnce && postClose && postIdleClose ? 0 : 1;
}
