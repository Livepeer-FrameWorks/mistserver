#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {
  volatile sig_atomic_t active = 1;
  int serverFd = -1;
  std::mutex logMutex;

  void stop(int) {
    active = 0;
    if (serverFd >= 0) { close(serverFd); }
  }

  bool sendAll(int fd, const char *data, size_t size) {
    while (size) {
      const ssize_t sent = send(fd, data, size, 0);
      if (sent <= 0) { return false; }
      data += sent;
      size -= sent;
    }
    return true;
  }

  size_t contentLength(const std::string & headers) {
    std::string lower = headers;
    for (size_t i = 0; i < lower.size(); ++i) {
      if (lower[i] >= 'A' && lower[i] <= 'Z') { lower[i] += 'a' - 'A'; }
    }
    const size_t field = lower.find("\r\ncontent-length:");
    if (field == std::string::npos) { return 0; }
    const char *number = lower.c_str() + field + 17;
    while (*number == ' ' || *number == '\t') { ++number; }
    char *end = 0;
    errno = 0;
    const unsigned long long value = strtoull(number, &end, 10);
    if (errno || end == number) { return 0; }
    return (size_t)value;
  }

  uint64_t segmentNumber(const std::string & request) {
    const size_t lineEnd = request.find("\r\n");
    const size_t extension = request.rfind(".ts", lineEnd);
    const size_t slash = request.rfind('/', extension);
    if (extension == std::string::npos || slash == std::string::npos) { return 0; }
    return strtoull(request.c_str() + slash + 1, 0, 10);
  }

  void handleConnection(int fd) {
    std::string request;
    char buffer[16384];
    size_t headerEnd = std::string::npos;
    while ((headerEnd = request.find("\r\n\r\n")) == std::string::npos) {
      const ssize_t received = recv(fd, buffer, sizeof(buffer), 0);
      if (received <= 0) {
        close(fd);
        return;
      }
      request.append(buffer, received);
      if (request.size() > 1024 * 1024) {
        close(fd);
        return;
      }
    }
    const size_t bodySize = contentLength(request.substr(0, headerEnd + 4));
    const size_t bodyStart = headerEnd + 4;
    while (request.size() - bodyStart < bodySize) {
      const ssize_t received = recv(fd, buffer, sizeof(buffer), 0);
      if (received <= 0) {
        close(fd);
        return;
      }
      request.append(buffer, received);
    }

    std::string lowerHeaders = request.substr(0, headerEnd + 4);
    for (size_t i = 0; i < lowerHeaders.size(); ++i) {
      if (lowerHeaders[i] >= 'A' && lowerHeaders[i] <= 'Z') { lowerHeaders[i] += 'a' - 'A'; }
    }
    if (lowerHeaders.find("\r\ncontent-resolution: 320x180\r\n") == std::string::npos) {
      const std::string response = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
      sendAll(fd, response.data(), response.size());
      close(fd);
      return;
    }

    const std::string prefix = "--mist-audit\r\nContent-Type: video/mp2t\r\nRendition-Name: audit\r\n\r\n";
    const std::string suffix = "\r\n--mist-audit--\r\n";
    const size_t responseSize = prefix.size() + bodySize + suffix.size();
    const std::string responseHeaders =
      "HTTP/1.1 200 OK\r\nContent-Type: multipart/mixed; boundary=mist-audit\r\nContent-Length: " + std::to_string(responseSize) +
      "\r\nConnection: close\r\n\r\n";
    // Return odd segments first. The processor must serialize insertion by
    // segment number even when parallel broadcaster responses complete out of
    // order. Slow even responses also exercise external backpressure.
    const uint64_t segment = segmentNumber(request);
    usleep(segment % 2 ? 100000 : 1800000);
    sendAll(fd, responseHeaders.data(), responseHeaders.size());
    sendAll(fd, prefix.data(), prefix.size());
    sendAll(fd, request.data() + bodyStart, bodySize);
    sendAll(fd, suffix.data(), suffix.size());
    close(fd);
    {
      std::lock_guard<std::mutex> logGuard(logMutex);
      fprintf(stdout, "responded %llu\n", (unsigned long long)segment);
      fflush(stdout);
    }
  }
} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s port\n", argv[0]);
    return 2;
  }
  char *end = 0;
  const long port = strtol(argv[1], &end, 10);
  if (!end || *end || port < 1 || port > 65535) {
    fprintf(stderr, "invalid port\n");
    return 2;
  }
  signal(SIGINT, stop);
  signal(SIGTERM, stop);

  serverFd = socket(AF_INET, SOCK_STREAM, 0);
  if (serverFd < 0) {
    perror("socket");
    return 1;
  }
  int enabled = 1;
  setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
  sockaddr_in address;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons((uint16_t)port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(serverFd, (sockaddr *)&address, sizeof(address)) || listen(serverFd, 16)) {
    perror("bind/listen");
    close(serverFd);
    return 1;
  }
  fprintf(stdout, "ready\n");
  fflush(stdout);
  while (active) {
    const int connection = accept(serverFd, 0, 0);
    if (connection < 0) {
      if (errno == EINTR) { continue; }
      break;
    }
    std::thread(handleConnection, connection).detach();
  }
  if (serverFd >= 0) { close(serverFd); }
  return 0;
}
