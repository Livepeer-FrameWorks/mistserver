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

} // namespace

int main() {
  const bool headClose = testHeadClosesAdvertisedConnection();
  const bool responseFailure = testPostTracksResponsePhaseFailure();
  const bool preSendFailure = testPostTracksPreSendFailure();

  if (!headClose) { std::cerr << "HEAD response did not close a Connection: close socket" << std::endl; }
  if (!responseFailure) { std::cerr << "POST response-phase failure was not classified as sent" << std::endl; }
  if (!preSendFailure) { std::cerr << "POST pre-send failure was incorrectly classified as sent" << std::endl; }
  return headClose && responseFailure && preSendFailure ? 0 : 1;
}
