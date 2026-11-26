#include <mist/http_parser.h>
#include <mist/urireader.h>

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
  int failures = 0;
  void check(bool ok, const std::string & what) {
    if (!ok) {
      fprintf(stderr, "FAIL: %s\n", what.c_str());
      ++failures;
    }
  }

  void parserLength(size_t length) {
    const std::string label = std::to_string(length);
    const std::string head = "HTTP/1.1 200 OK\r\nContent-Length: " + label + "\r\n\r\n";
    try {
      HTTP::Parser parser;
      parser.headerOnly = true;
      std::string wire = head;
      check(parser.Read(wire), "HEAD completes without a body for " + label);
      check(parser.knownLength && parser.length == length, "HEAD preserves full length " + label);
      check(parser.body.empty() && parser.body.capacity() < 1024 * 1024, "HEAD does not reserve the advertised body for " + label);
    } catch (const std::exception & e) { check(false, "HEAD throws for " + label + ": " + e.what()); }
    if (!length) { return; }
    try {
      HTTP::Parser parser;
      std::string wire = head;
      check(!parser.Read(wire), "a body response stays incomplete for " + label);
      check(parser.knownLength && parser.length == length, "body metadata preserves length " + label);
      check(parser.body.capacity() < 1024 * 1024, "body headers do not allocate the whole file for " + label);
      wire = "x";
      check(parser.Read(wire) == (length == 1), "body completion follows bytes received for " + label);
      check(parser.body == "x", "body bytes are retained for " + label);
    } catch (const std::exception & e) { check(false, "body headers throw for " + label + ": " + e.what()); }
  }

  void malformedLength(const std::string & length) {
    try {
      HTTP::Parser parser;
      parser.headerOnly = true;
      std::string wire = "HTTP/1.1 200 OK\r\nContent-Length: " + length + "\r\n\r\n";
      check(!parser.Read(wire), "invalid length rejected: " + length);
      wire = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
      check(!parser.Read(wire), "invalid message cannot resume as a valid message: " + length);
      parser.Clean();
      wire = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
      check(parser.Read(wire), "explicit cleanup permits reuse after invalid length: " + length);
    } catch (const std::exception & e) { check(false, "invalid length throws: " + length + ": " + e.what()); }
  }

  void responseRange(size_t length) {
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) {
      check(false, "create response socket pair");
      return;
    }
    Socket::Connection conn(pair[0]);
    HTTP::Parser request, response;
    request.protocol = "HTTP/1.1";
    response.SetHeader("Content-Length", std::to_string(length));
    response.StartResponse("200", "OK", request, conn);
    char buffer[4096];
    ssize_t got = recv(pair[1], buffer, sizeof(buffer), MSG_DONTWAIT);
    std::string wire = got > 0 ? std::string(buffer, got) : "";
    const std::string expected = "Content-Range: bytes 0-" + std::to_string(length - 1) + "/" + std::to_string(length);
    check(wire.find(expected) != std::string::npos, "chunked response preserves full range " + std::to_string(length));
    conn.close();
    close(pair[1]);
  }

  void malformedSocket() {
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) {
      check(false, "create parser socket pair");
      return;
    }
    Socket::Connection conn(pair[0]);
    const std::string wire = "HTTP/1.1 200 OK\r\nContent-Length: 12x\r\n\r\n";
    send(pair[1], wire.data(), wire.size(), 0);
    conn.spool();
    HTTP::Parser parser;
    parser.headerOnly = true;
    check(!parser.Read(conn), "socket parser rejects invalid length");
    check(!conn.connected(), "invalid length closes the socket rather than accepting more body bytes");
    conn.close();
    close(pair[1]);
  }

  class LargeSource {
      int listener;
      std::atomic<bool> running;
      std::thread worker;
      void serve(int fd) {
        timeval timeout = {2, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        std::string request;
        char buf[2048];
        while (request.find("\r\n\r\n") == std::string::npos) {
          ssize_t got = recv(fd, buf, sizeof(buf), 0);
          if (got <= 0) { return; }
          request.append(buf, got);
        }
        const size_t first = request.find(' ') + 1;
        const size_t last = request.find(' ', first);
        const std::string value = request.substr(first + 1, last - first - 1);
        const bool head = request.compare(0, 5, "HEAD ") == 0;
        std::string reply;
        if (head) {
          reply = "HTTP/1.1 200 OK\r\nAccept-Ranges: bytes\r\nContent-Length: " + value + "\r\nConnection: close\r\n\r\n";
        } else {
          const size_t start = request.find("Range: bytes=");
          const std::string offset =
            start == std::string::npos ? "0" : request.substr(start + 13, request.find('-', start + 13) - start - 13);
          reply = "HTTP/1.1 206 Partial Content\r\nContent-Length: 8\r\nContent-Range: bytes " + offset + "-" +
            std::to_string(std::stoull(offset) + 7) + "/" + value + "\r\nConnection: close\r\n\r\ntaildata";
        }
        size_t sent = 0;
        while (sent < reply.size()) {
          ssize_t n = send(fd, reply.data() + sent, reply.size() - sent, 0);
          if (n <= 0) { break; }
          sent += n;
        }
      }

    public:
      std::string base;
      LargeSource() : listener(-1), running(true) {
        listener = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (listener < 0 || bind(listener, (sockaddr *)&addr, sizeof(addr)) || listen(listener, 8)) {
          check(false, "create large-file HTTP fixture");
          return;
        }
        socklen_t size = sizeof(addr);
        getsockname(listener, (sockaddr *)&addr, &size);
        base = "http://127.0.0.1:" + std::to_string(ntohs(addr.sin_port)) + "/";
        worker = std::thread([this]() {
          while (running) {
            int fd = accept(listener, 0, 0);
            if (fd < 0) { continue; }
            serve(fd);
            close(fd);
          }
        });
      }
      ~LargeSource() {
        running = false;
        shutdown(listener, SHUT_RDWR);
        close(listener);
        if (worker.joinable()) { worker.join(); }
      }
  };

  struct Tail : Util::DataCallback {
      size_t offset;
      std::string data;
      explicit Tail(size_t start) : offset(start) {}
      void dataCallback(const char *p, size_t n) { data.append(p, n); }
      size_t getDataCallbackPos() const { return offset + data.size(); }
  };

  void readerLength(LargeSource & source, size_t length) {
    const std::string label = std::to_string(length);
    try {
      HTTP::URIReader reader;
      check(reader.open(source.base + label), "URIReader opens " + label);
      check(reader.getSize() == length, "URIReader preserves full HEAD size " + label);
      reader.binaryMode();
      check(reader.seek(length - 8), "URIReader seeks above signed 32-bit range " + label);
      Tail tail(length - 8);
      for (unsigned int i = 0; i < 200 && tail.data.size() < 8 && reader; ++i) {
        reader.readSome(8 - tail.data.size(), tail);
        usleep(1000);
      }
      check(tail.data == "taildata", "URIReader reads correct tail bytes " + label);
      check(tail.getDataCallbackPos() == length, "the input callback ends at the full file length " + label);
    } catch (const std::exception & e) { check(false, "URIReader throws for " + label + ": " + e.what()); }
  }
} // namespace

int main() {
  signal(SIGPIPE, SIG_IGN);
  std::vector<size_t> lengths = {0, 1, 2147483647ULL, 2147483648ULL, 2907084768ULL, 3068515312ULL, 3419079637ULL};
  if (sizeof(size_t) > 4) {
    lengths.push_back(4294967296ULL);
    lengths.push_back(9000000000ULL);
  }
  lengths.push_back(std::numeric_limits<size_t>::max());
  for (size_t length : lengths) {
    parserLength(length);
    if (length) { responseRange(length); }
  }
  for (const char *bad : {"", "-1", "+1", "12x", "1,1", "18446744073709551616"}) { malformedLength(bad); }
  malformedSocket();
  LargeSource source;
  if (!source.base.empty()) {
    for (size_t length : lengths) {
      if (length > 8 && length != std::numeric_limits<size_t>::max()) { readerLength(source, length); }
    }
    try {
      HTTP::URIReader unknown;
      check(!unknown.open(source.base + std::to_string(std::numeric_limits<size_t>::max())),
            "URIReader rejects a length colliding with its unknown-size sentinel");
    } catch (const std::exception & e) {
      check(false, std::string("unrepresentable URIReader size throws: ") + e.what());
    }
  }
  return failures ? 1 : 0;
}
