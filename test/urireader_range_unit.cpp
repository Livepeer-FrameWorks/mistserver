#include <mist/defines.h>
#include <mist/timing.h>
#include <mist/urireader.h>
#include <mist/util.h>

#include <arpa/inet.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

// URIReader over HTTP against a loopback server that serves one resource with range support and
// records every request. A seek to the end of a resource (an MP4 whose moov box ends the file is
// read up to there, then skipped past) needs no request: the server could only answer 416. An
// optional resource the server does not have is not fetched with a GET after its HEAD failed.
namespace {
  int failures = 0;

  void check(bool ok, const char *what) {
    if (!ok) {
      fprintf(stderr, "FAIL: %s\n", what);
      ++failures;
    }
  }

  const std::string body(4096, 'x');
  std::mutex requestMutex;
  std::vector<std::string> requests; // "<METHOD> <path> <range start or ->"
  std::atomic<bool> serving{true};

  void answer(int fd) {
    std::string request;
    char buffer[2048];
    while (request.find("\r\n\r\n") == std::string::npos) {
      const ssize_t got = recv(fd, buffer, sizeof(buffer), 0);
      if (got <= 0) { return; }
      request.append(buffer, got);
    }
    const std::string method = request.substr(0, request.find(' '));
    const size_t pathStart = request.find(' ') + 1;
    const std::string path = request.substr(pathStart, request.find(' ', pathStart) - pathStart);
    long long rangeStart = -1;
    const size_t range = request.find("Range: bytes=");
    if (range != std::string::npos) { rangeStart = atoll(request.c_str() + range + 13); }
    {
      std::lock_guard<std::mutex> guard(requestMutex);
      requests.push_back(method + " " + path + " " + (rangeStart < 0 ? "-" : std::to_string(rangeStart)));
    }
    std::string response;
    if (path != "/media") {
      response = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    } else if (rangeStart >= (long long)body.size()) {
      response = "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */" + std::to_string(body.size()) +
        "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    } else {
      const size_t from = rangeStart < 0 ? 0 : (size_t)rangeStart;
      const std::string part = body.substr(from);
      response = std::string(rangeStart < 0 ? "HTTP/1.1 200 OK\r\n" : "HTTP/1.1 206 Partial Content\r\n") +
        "Accept-Ranges: bytes\r\nContent-Length: " + std::to_string(part.size()) + "\r\n" +
        (rangeStart < 0 ? ""
                        : "Content-Range: bytes " + std::to_string(from) + "-" + std::to_string(body.size() - 1) + "/" +
             std::to_string(body.size()) + "\r\n") +
        "Connection: close\r\n\r\n";
      if (method != "HEAD") { response += part; }
    }
    size_t sent = 0;
    while (sent < response.size()) {
      const ssize_t n = send(fd, response.data() + sent, response.size() - sent, MSG_NOSIGNAL);
      if (n <= 0) { break; }
      sent += n;
    }
  }

  bool requested(const std::string & what) {
    std::lock_guard<std::mutex> guard(requestMutex);
    for (const std::string & r : requests) {
      if (r.find(what) == 0) { return true; }
    }
    return false;
  }

  size_t rangeRequestsFrom(size_t start) {
    std::lock_guard<std::mutex> guard(requestMutex);
    size_t count = 0;
    for (const std::string & r : requests) {
      const size_t space = r.rfind(' ');
      const std::string from = r.substr(space + 1);
      if (from != "-" && (size_t)atoll(from.c_str()) >= start) { ++count; }
    }
    return count;
  }

  // Reports its absolute read position like the inputs do (readPos plus what it buffered), which
  // is how URIReader counts what a read delivered and where a resumed request continues.
  struct Collector : public Util::DataCallback {
      explicit Collector(size_t from) : start(from) {}
      size_t start;
      std::string data;
      void dataCallback(const char *ptr, size_t size) { data.append(ptr, size); }
      size_t getDataCallbackPos() const { return start + data.size(); }
  };
} // namespace

int main() {
  const int listener = socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (bind(listener, (sockaddr *)&addr, sizeof(addr)) || listen(listener, 16)) {
    perror("loopback server");
    return 1;
  }
  socklen_t len = sizeof(addr);
  getsockname(listener, (sockaddr *)&addr, &len);
  const std::string base = "http://127.0.0.1:" + std::to_string(ntohs(addr.sin_port));
  std::thread server([listener]() {
    while (serving) {
      const int fd = accept(listener, 0, 0);
      if (fd < 0) { continue; }
      answer(fd);
      close(fd);
    }
  });

  {
    HTTP::URIReader reader(base + "/media");
    check((bool)reader, "the resource opens");
    check(reader.getSize() == body.size(), "the resource size is known from its HEAD");
    check(reader.seek(body.size() / 2), "a seek inside the resource succeeds");
    // A read that has nothing yet returns 0; readers retry until EOF, as readAll does.
    Collector half(body.size() / 2);
    const uint64_t deadline = Util::bootMS() + 5000;
    while (reader && half.data.size() < body.size() / 2 && Util::bootMS() < deadline) {
      if (!reader.readSome(body.size(), half)) { Util::sleep(10); }
    }
    check(half.data.size() == body.size() / 2, "the second half reads after the seek");
    check(reader.seek(body.size()), "a seek to the end succeeds");
    check(!reader, "a reader seeked to the end is at EOF");
    // A request the seek sent reaches the server thread asynchronously.
    Util::sleep(300);
    check(rangeRequestsFrom(body.size()) == 0, "no range request starts at or past the end (a server answers 416)");
  }

  {
    HTTP::URIReader optional;
    optional.setOptional(true);
    check(!optional.open(base + "/missing.dtsh"), "an absent optional resource does not open");
    check(requested("HEAD /missing.dtsh"), "the optional resource was looked up");
    check(!requested("GET /missing.dtsh"), "an absent optional resource is not fetched after its HEAD failed");
  }

  serving = false;
  shutdown(listener, SHUT_RDWR);
  close(listener);
  server.join();
  if (failures) {
    fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  fprintf(stderr, "URIReader ranges: OK\n");
  return 0;
}
