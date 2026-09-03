#include <mist/http_parser.h>
#include <mist/socket.h>
#include <mist/timing.h>
#include <mist/urireader.h>

#include <atomic>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

namespace {

  bool readRequest(Socket::Connection & conn, HTTP::Parser & request) {
    const uint64_t deadline = Util::bootMS() + 3000;
    while (conn && Util::bootMS() < deadline) {
      conn.spool();
      if (request.Read(conn)) { return true; }
      Util::sleep(10);
    }
    return false;
  }

  void sendResponse(Socket::Connection & conn, const std::string & code, const std::string & message, const std::string & body) {
    HTTP::Parser response;
    response.protocol = "HTTP/1.1";
    response.body = body;
    response.SetHeader("Connection", "close");
    response.SetHeader("Content-Length", std::to_string(body.empty() ? 4 : body.size()));
    response.SendResponse(code, message, conn);
  }

} // namespace

int main() {
  Socket::Server server(0, "127.0.0.1", false);
  if (!server.getBoundAddr().port()) {
    std::cerr << "Could not bind URIReader fixture" << std::endl;
    return 1;
  }

  std::atomic<bool> servedAll(false);
  std::thread peer([&]() {
    Socket::Connection headConn = server.accept();
    HTTP::Parser headRequest;
    if (!readRequest(headConn, headRequest) || headRequest.method != "HEAD") { return; }
    HTTP::Parser headResponse;
    headResponse.protocol = "HTTP/1.1";
    headResponse.SetHeader("Accept-Ranges", "bytes");
    headResponse.SetHeader("Connection", "close");
    headResponse.SetHeader("Content-Length", "4");
    headResponse.SendResponse("200", "OK", headConn);

    // Initial range request plus the downloader's five retries: close each one without a
    // response so continueNonBlocking reaches its incomplete terminal state.
    for (size_t i = 0; i < 6; ++i) {
      Socket::Connection failedRange = server.accept();
      HTTP::Parser failedRequest;
      if (!readRequest(failedRange, failedRequest) || failedRequest.method != "GET" || failedRequest.GetHeader("Range") != "bytes=0-") {
        return;
      }
      failedRange.close();
    }

    Socket::Connection retriedRange = server.accept();
    HTTP::Parser retriedRequest;
    if (!readRequest(retriedRange, retriedRequest) || retriedRequest.method != "GET" || retriedRequest.GetHeader("Range") != "bytes=0-") {
      return;
    }
    sendResponse(retriedRange, "206", "Partial Content", "data");
    retriedRange.close();
    servedAll = true;
  });

  const HTTP::URL url("http://127.0.0.1:" + std::to_string(server.getBoundAddr().port()) + "/asset");
  HTTP::URIReader reader(url);
  for (size_t i = 0; i < 20 && !reader.isEOF(); ++i) {
    reader.readSome(1, reader);
    Util::sleep(10);
  }
  const bool rejectedTerminalResponse = reader.isEOF();
  const bool reopened = reader.seek(0);
  char *data = 0;
  size_t len = 0;
  const bool readRetry = reader.readSome(data, len, 4) == 4 && len == 4 && !std::memcmp(data, "data", 4);

  server.close();
  peer.join();
  if (!rejectedTerminalResponse) { std::cerr << "Terminal HTTP error did not close URIReader state" << std::endl; }
  if (!reopened) { std::cerr << "Seek did not reopen a ranged URIReader after terminal response" << std::endl; }
  if (!readRetry) { std::cerr << "Reopened URIReader did not return the retried range" << std::endl; }
  if (!servedAll) { std::cerr << "URIReader fixture did not receive all expected requests" << std::endl; }
  return rejectedTerminalResponse && reopened && readRetry && servedAll ? 0 : 1;
}
