#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <netinet/in.h>
#include <set>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {
  volatile sig_atomic_t active = 1;
  int serverFd = -1;
  std::mutex logMutex;
  // With LIVEPEER_STUB_REJECT_FIRST=1 the first upload of every segment is
  // answered 422, the way a gateway rejects segments of a manifest it is still
  // setting up; the re-sent segment is transcoded normally.
  bool rejectFirst = false;
  // With LIVEPEER_STUB_NO_RESULT_FIRST=1 that first answer is 503 instead: the
  // gateway had no result within the segment's budget yet.
  bool noResultFirst = false;
  std::mutex rejectedMutex;
  std::set<uint64_t> rejectedOnce;
  // With LIVEPEER_STUB_CLAIM_FILE set, stubs sharing that path decide on their
  // first upload which of them is the failing gateway: the stub that creates
  // the file first rejects every upload with 422 after
  // LIVEPEER_STUB_REJECT_DELAY_MS, the way a gateway answers when its
  // orchestrators fail; every other stub transcodes normally.
  const char *claimFile = 0;
  uint64_t rejectDelayMs = 0;
  std::mutex claimMutex;
  int claimed = -1;
  // While the file named by LIVEPEER_STUB_HOLD_FILE exists, every response waits, the way a
  // gateway stalls; the test removes the file to let them through.
  const char *holdFile = 0;
  // While the file named by LIVEPEER_STUB_ALT_FLAG exists, every rendition carries the MPEG-TS
  // bytes of LIVEPEER_STUB_ALT_FILE instead of the uploaded segment: an orchestrator that encodes
  // differently (another resolution).
  const char *altFlag = 0;
  std::string altSegment;
  // While the file named by LIVEPEER_STUB_NO_RESULT_FILE exists, every upload is answered 503
  // after 500 ms: the gateway has no result for any segment.
  const char *noResultFile = 0;
  // With LIVEPEER_STUB_NO_RESULT_SEGMENT set, uploads of that segment are answered 503 after
  // 500 ms until LIVEPEER_STUB_NO_RESULT_MS passed since its first upload: the gateway misses
  // that one segment's budget and transcodes it when it is sent again later.
  long long noResultSegment = -1;
  uint64_t noResultMs = 0;
  std::mutex noResultMutex;
  struct timespec noResultSince = {0, 0};
  // With LIVEPEER_STUB_SESSION_SETUP_MS set, the stub answers like a gateway whose orchestrator
  // sets the stream's session up first: the first warm-up or segment it gets starts that setup,
  // and every segment is transcoded LIVEPEER_STUB_SESSION_WORK_MS (default 50) after both its
  // first upload and the end of the setup. A segment that cannot be answered that way by its
  // budget (its deadlineMs less the gateway's 500 ms answer margin, from its first upload) is
  // answered 503 at that point and on every later upload ("budgetmiss" in the log). Every other
  // segment is answered without the odd/even response delays.
  uint64_t sessionSetupMs = 0;
  uint64_t sessionWorkMs = 50;
  // With LIVEPEER_STUB_REFUSE_WARMUP=1 warm-ups are answered 400 and start nothing, as by a
  // gateway without the session warm-up endpoint.
  bool refuseWarmup = false;
  std::mutex sessionMutex;
  bool sessionStarted = false;
  uint64_t sessionReadyMs = 0;
  std::map<uint64_t, uint64_t> segmentFirstUploadMs;

  uint64_t monotonicMs() {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
  }

  /// Starts the session setup at nowMs unless it started before; returns when it ends.
  uint64_t sessionReadyAt(uint64_t nowMs) {
    std::lock_guard<std::mutex> guard(sessionMutex);
    if (!sessionStarted) {
      sessionStarted = true;
      sessionReadyMs = nowMs + sessionSetupMs;
    }
    return sessionReadyMs;
  }

  void sleepUntilMs(uint64_t atMs) {
    const uint64_t now = monotonicMs();
    if (atMs > now) { usleep((useconds_t)((atMs - now) * 1000)); }
  }

  bool fileExists(const char *path) {
    return path && access(path, F_OK) == 0;
  }

  bool segmentHasNoResultYet(uint64_t segment) {
    if (noResultSegment < 0 || segment != (uint64_t)noResultSegment) { return false; }
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    std::lock_guard<std::mutex> guard(noResultMutex);
    if (!noResultSince.tv_sec && !noResultSince.tv_nsec) { noResultSince = now; }
    const uint64_t elapsedMs = (uint64_t)(now.tv_sec - noResultSince.tv_sec) * 1000 + (now.tv_nsec - noResultSince.tv_nsec) / 1000000;
    return elapsedMs < noResultMs;
  }

  bool failsEverything() {
    if (!claimFile) { return false; }
    std::lock_guard<std::mutex> guard(claimMutex);
    if (claimed < 0) {
      const int fd = open(claimFile, O_CREAT | O_EXCL | O_WRONLY, 0600);
      claimed = fd >= 0 ? 1 : 0;
      if (fd >= 0) { close(fd); }
    }
    return claimed == 1;
  }

  void stop(int) {
    active = 0;
    if (serverFd >= 0) { close(serverFd); }
  }

  bool sendAll(int fd, const char *data, size_t size) {
    while (size) {
      // A client killed mid-response must not take the stub down with SIGPIPE.
      const ssize_t sent = send(fd, data, size, MSG_NOSIGNAL);
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

    const size_t requestLineEnd = request.find("\r\n");
    if (request.rfind("/session ", requestLineEnd) != std::string::npos) {
      // The session warm-up: the stream's first keyframe, MPEG-TS, with the source frame rate the
      // gateway cannot probe from one frame (the test publishes 25 fps).
      const bool valid = bodySize >= 188 && request[bodyStart] == 0x47 &&
        lowerHeaders.find("\r\nlivepeer-transcode-configuration: {") != std::string::npos &&
        lowerHeaders.find("\r\ncontent-frame-rate: 25.000\r\n") != std::string::npos;
      const bool accept = valid && !refuseWarmup;
      if (accept && sessionSetupMs) { sessionReadyAt(monotonicMs()); }
      const std::string response = accept
        ? "HTTP/1.1 202 Accepted\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
        : "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
      sendAll(fd, response.data(), response.size());
      close(fd);
      std::lock_guard<std::mutex> logGuard(logMutex);
      fprintf(stdout, "%s %zu\n", accept ? "warmup" : (valid ? "warmup-refused" : "warmup-invalid"), bodySize);
      fflush(stdout);
      return;
    }

    const uint64_t segment = segmentNumber(request);
    unsigned long long segmentDeadlineMs = 0;
    {
      // Every upload carries the segment's budget as deadlineMs in the
      // transcode configuration header.
      const size_t field = lowerHeaders.find("\"deadlinems\":");
      const unsigned long long deadline = field == std::string::npos ? 0 : strtoull(lowerHeaders.c_str() + field + 13, 0, 10);
      std::lock_guard<std::mutex> logGuard(logMutex);
      fprintf(stdout, "deadline %llu %llu\n", (unsigned long long)segment, deadline);
      fflush(stdout);
      segmentDeadlineMs = deadline;
    }
    if (sessionSetupMs) {
      const uint64_t now = monotonicMs();
      uint64_t firstUpload;
      {
        std::lock_guard<std::mutex> guard(sessionMutex);
        firstUpload = segmentFirstUploadMs.insert(std::make_pair(segment, now)).first->second;
      }
      const uint64_t ready = std::max(sessionReadyAt(now), now) + sessionWorkMs;
      const uint64_t budget = segmentDeadlineMs ? segmentDeadlineMs : 3000;
      const uint64_t respondBy = firstUpload + (budget > 500 ? budget - 500 : 0);
      if (ready > respondBy) {
        sleepUntilMs(respondBy);
        const std::string response =
          "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        sendAll(fd, response.data(), response.size());
        close(fd);
        std::lock_guard<std::mutex> logGuard(logMutex);
        fprintf(stdout, "budgetmiss %llu\nnoresult %llu\n", (unsigned long long)segment, (unsigned long long)segment);
        fflush(stdout);
        return;
      }
      sleepUntilMs(ready);
    }
    if (fileExists(noResultFile) || segmentHasNoResultYet(segment)) {
      usleep(500000);
      const std::string response = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
      sendAll(fd, response.data(), response.size());
      close(fd);
      std::lock_guard<std::mutex> logGuard(logMutex);
      fprintf(stdout, "noresult %llu\n", (unsigned long long)segment);
      fflush(stdout);
      return;
    }
    if (failsEverything()) {
      usleep(rejectDelayMs * 1000);
      const std::string response =
        "HTTP/1.1 422 Unprocessable Entity\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
      sendAll(fd, response.data(), response.size());
      close(fd);
      std::lock_guard<std::mutex> logGuard(logMutex);
      fprintf(stdout, "rejected %llu\n", (unsigned long long)segment);
      fflush(stdout);
      return;
    }
    if (rejectFirst) {
      bool reject = false;
      {
        std::lock_guard<std::mutex> guard(rejectedMutex);
        reject = rejectedOnce.insert(segment).second;
      }
      if (reject) {
        const std::string response = noResultFirst
          ? "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
          : "HTTP/1.1 422 Unprocessable Entity\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        sendAll(fd, response.data(), response.size());
        close(fd);
        std::lock_guard<std::mutex> logGuard(logMutex);
        fprintf(stdout, "rejected %llu\n", (unsigned long long)segment);
        fflush(stdout);
        return;
      }
    }

    // Return odd segments first. The processor must serialize insertion by
    // segment number even when parallel broadcaster responses complete out of
    // order. Slow even responses also exercise external backpressure.
    if (!sessionSetupMs) { usleep(segment % 2 ? 100000 : 1800000); }
    if (fileExists(holdFile)) {
      const time_t heldFrom = time(0);
      while (fileExists(holdFile) && active) { usleep(100000); }
      std::lock_guard<std::mutex> logGuard(logMutex);
      fprintf(stdout, "held %llu %llds\n", (unsigned long long)segment, (long long)(time(0) - heldFrom));
      fflush(stdout);
    }
    const bool alternate = fileExists(altFlag) && altSegment.size();
    const char *body = alternate ? altSegment.data() : request.data() + bodyStart;
    const size_t bodyLength = alternate ? altSegment.size() : bodySize;
    const std::string prefix = "--mist-audit\r\nContent-Type: video/mp2t\r\nRendition-Name: audit\r\n\r\n";
    const std::string suffix = "\r\n--mist-audit--\r\n";
    const size_t responseSize = prefix.size() + bodyLength + suffix.size();
    const std::string responseHeaders =
      "HTTP/1.1 200 OK\r\nContent-Type: multipart/mixed; boundary=mist-audit\r\nContent-Length: " + std::to_string(responseSize) +
      "\r\nConnection: close\r\n\r\n";
    sendAll(fd, responseHeaders.data(), responseHeaders.size());
    sendAll(fd, prefix.data(), prefix.size());
    sendAll(fd, body, bodyLength);
    sendAll(fd, suffix.data(), suffix.size());
    close(fd);
    {
      std::lock_guard<std::mutex> logGuard(logMutex);
      fprintf(stdout, "responded %llu%s\n", (unsigned long long)segment, alternate ? " alternate" : "");
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
  const char *rejectFirstEnv = getenv("LIVEPEER_STUB_REJECT_FIRST");
  rejectFirst = rejectFirstEnv && !strcmp(rejectFirstEnv, "1");
  const char *noResultFirstEnv = getenv("LIVEPEER_STUB_NO_RESULT_FIRST");
  noResultFirst = noResultFirstEnv && !strcmp(noResultFirstEnv, "1");
  if (noResultFirst) { rejectFirst = true; }
  claimFile = getenv("LIVEPEER_STUB_CLAIM_FILE");
  if (claimFile && !*claimFile) { claimFile = 0; }
  const char *rejectDelayEnv = getenv("LIVEPEER_STUB_REJECT_DELAY_MS");
  if (rejectDelayEnv) { rejectDelayMs = strtoull(rejectDelayEnv, 0, 10); }
  holdFile = getenv("LIVEPEER_STUB_HOLD_FILE");
  if (holdFile && !*holdFile) { holdFile = 0; }
  altFlag = getenv("LIVEPEER_STUB_ALT_FLAG");
  if (altFlag && !*altFlag) { altFlag = 0; }
  noResultFile = getenv("LIVEPEER_STUB_NO_RESULT_FILE");
  if (noResultFile && !*noResultFile) { noResultFile = 0; }
  const char *noResultSegmentEnv = getenv("LIVEPEER_STUB_NO_RESULT_SEGMENT");
  if (noResultSegmentEnv && *noResultSegmentEnv) { noResultSegment = strtoll(noResultSegmentEnv, 0, 10); }
  const char *noResultMsEnv = getenv("LIVEPEER_STUB_NO_RESULT_MS");
  if (noResultMsEnv) { noResultMs = strtoull(noResultMsEnv, 0, 10); }
  const char *sessionSetupEnv = getenv("LIVEPEER_STUB_SESSION_SETUP_MS");
  if (sessionSetupEnv) { sessionSetupMs = strtoull(sessionSetupEnv, 0, 10); }
  const char *sessionWorkEnv = getenv("LIVEPEER_STUB_SESSION_WORK_MS");
  if (sessionWorkEnv && *sessionWorkEnv) { sessionWorkMs = strtoull(sessionWorkEnv, 0, 10); }
  const char *refuseWarmupEnv = getenv("LIVEPEER_STUB_REFUSE_WARMUP");
  refuseWarmup = refuseWarmupEnv && !strcmp(refuseWarmupEnv, "1");
  const char *altFileEnv = getenv("LIVEPEER_STUB_ALT_FILE");
  if (altFileEnv && *altFileEnv) {
    FILE *alt = fopen(altFileEnv, "rb");
    if (!alt) {
      perror("LIVEPEER_STUB_ALT_FILE");
      return 1;
    }
    char chunk[65536];
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), alt)) > 0) { altSegment.append(chunk, got); }
    fclose(alt);
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
