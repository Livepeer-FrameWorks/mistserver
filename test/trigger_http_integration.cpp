#include <mist/triggers.h>

#include <arpa/inet.h>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {
  struct Scenario {
      std::string trigger;
      std::string payload;
      std::string responseBody;
      std::string action;
      std::string reason;
      int status;
  };

  bool sendAll(int fd, const std::string & data) {
    size_t offset = 0;
    while (offset < data.size()) {
      const ssize_t sent = send(fd, data.data() + offset, data.size() - offset, 0);
      if (sent <= 0) { return false; }
      offset += sent;
    }
    return true;
  }

  size_t contentLength(const std::string & request) {
    std::string lower = request;
    for (size_t i = 0; i < lower.size(); ++i) {
      if (lower[i] >= 'A' && lower[i] <= 'Z') { lower[i] += 'a' - 'A'; }
    }
    const size_t field = lower.find("\r\ncontent-length:");
    if (field == std::string::npos) { return 0; }
    const char *value = lower.c_str() + field + 17;
    while (*value == ' ' || *value == '\t') { ++value; }
    return strtoull(value, NULL, 10);
  }

  Triggers::Result runScenario(const Scenario & scenario, const std::string & defaultResponse, Triggers::Action onFail,
                               std::string & serverError) {
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
      serverError = "could not create listener";
      return Triggers::Result();
    }
    int enabled = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(listener, (sockaddr *)&address, sizeof(address)) || listen(listener, 1)) {
      close(listener);
      serverError = "could not bind listener";
      return Triggers::Result();
    }
    socklen_t addressLength = sizeof(address);
    getsockname(listener, (sockaddr *)&address, &addressLength);

    std::thread server([&]() {
      const int connection = accept(listener, NULL, NULL);
      if (connection < 0) {
        serverError = "could not accept trigger request";
        return;
      }
      std::string request;
      size_t headerEnd = std::string::npos;
      char buffer[4096];
      while ((headerEnd = request.find("\r\n\r\n")) == std::string::npos) {
        const ssize_t received = recv(connection, buffer, sizeof(buffer), 0);
        if (received <= 0) {
          serverError = "trigger request ended before its headers";
          close(connection);
          return;
        }
        request.append(buffer, received);
      }
      const size_t bodyStart = headerEnd + 4;
      const size_t bodyLength = contentLength(request.substr(0, bodyStart));
      while (request.size() - bodyStart < bodyLength) {
        const ssize_t received = recv(connection, buffer, sizeof(buffer), 0);
        if (received <= 0) {
          serverError = "trigger request ended before its payload";
          close(connection);
          return;
        }
        request.append(buffer, received);
      }
      if (request.find("\r\nX-Trigger: " + scenario.trigger + "\r\n") == std::string::npos) {
        serverError = "trigger type header was not forwarded";
      } else if (request.substr(bodyStart, bodyLength) != scenario.payload) {
        serverError = "trigger payload was not forwarded exactly";
      }

      std::string headers = "HTTP/1.1 " + std::to_string(scenario.status) + (scenario.status == 200 ? " OK\r\n" : " Failed\r\n");
      headers += "Content-Length: " + std::to_string(scenario.responseBody.size()) + "\r\n";
      if (!scenario.action.empty()) { headers += "X-Mist-Trigger-Action: " + scenario.action + "\r\n"; }
      if (!scenario.reason.empty()) { headers += "X-Mist-Trigger-Reason: " + scenario.reason + "\r\n"; }
      headers += "Connection: close\r\n\r\n";
      sendAll(connection, headers + scenario.responseBody);
      close(connection);
    });

    const std::string url = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) + "/trigger";
    Triggers::Result result = Triggers::handleTrigger(scenario.trigger, url, scenario.payload, 1, defaultResponse, onFail);
    server.join();
    close(listener);
    return result;
  }

  int fail(const std::string & message) {
    std::cerr << message << std::endl;
    return 1;
  }

  bool matches(const Triggers::Result & result, Triggers::Action action, const std::string & response,
               bool failed = false, const std::string & reason = "") {
    return result.action == action && result.response == response && result.handlerFailed == failed &&
      (reason.empty() || result.reason == reason);
  }
} // namespace

int main() {
  setenv("MIST_TUUID", "audit-trigger-uuid", 1);
  setenv("MIST_TIME", "1700000000000", 1);
  setenv("MIST_DATE", "Tue, 14 Nov 2023 22:13:20 GMT", 1);

  std::string serverError;
  Triggers::Result result = runScenario({"PLAY_REWRITE", "old-stream\nclient", "new-stream", "value", "", 200},
                                        "default", Triggers::ACT_LEGACY, serverError);
  if (!serverError.empty()) { return fail(serverError); }
  if (!matches(result, Triggers::ACT_VALUE, "new-stream")) { return fail("typed value action was not preserved"); }

  result = runScenario({"PLAY_REWRITE", "old-stream", "ignored", "deny", "policy", 200}, "default", Triggers::ACT_LEGACY, serverError);
  if (!serverError.empty()) { return fail(serverError); }
  if (!matches(result, Triggers::ACT_DENY, "ignored", false, "policy")) {
    return fail("typed deny action or reason was not preserved");
  }

  result = runScenario({"PLAY_REWRITE", "old-stream", "ignored", "keep", "", 200}, "default", Triggers::ACT_LEGACY, serverError);
  if (!serverError.empty()) { return fail(serverError); }
  if (!matches(result, Triggers::ACT_KEEP, "ignored")) { return fail("typed keep action was not preserved"); }

  result = runScenario({"STREAM_SOURCE", "camera", "ignored", "offline", "maintenance", 200}, "configured",
                       Triggers::ACT_LEGACY, serverError);
  if (!serverError.empty()) { return fail(serverError); }
  if (!matches(result, Triggers::ACT_OFFLINE, "ignored", false, "maintenance")) {
    return fail("typed offline action or reason was not preserved");
  }

  result = runScenario({"STREAM_PROCESS", "camera", "ignored", "use-configured", "", 200}, "configured",
                       Triggers::ACT_LEGACY, serverError);
  if (!serverError.empty()) { return fail(serverError); }
  if (!matches(result, Triggers::ACT_CONFIGURED, "ignored")) {
    return fail("typed use-configured action was not preserved");
  }

  result = runScenario({"PLAY_REWRITE", "old-stream", "ignored", "offline", "", 200}, "default", Triggers::ACT_DENY, serverError);
  if (!serverError.empty()) { return fail(serverError); }
  if (!matches(result, Triggers::ACT_DENY, "", true, "trigger_unavailable")) {
    return fail("an invalid typed action did not use the configured failure action");
  }

  result = runScenario({"PLAY_REWRITE", "old-stream", "failure", "", "", 503}, "default", Triggers::ACT_KEEP, serverError);
  if (!serverError.empty()) { return fail(serverError); }
  if (!matches(result, Triggers::ACT_KEEP, "", true, "trigger_unavailable")) {
    return fail("an HTTP handler failure did not use the configured failure action");
  }

  result = runScenario({"PLAY_REWRITE", "old-stream", "legacy-result", "", "", 200}, "default", Triggers::ACT_DENY, serverError);
  if (!serverError.empty()) { return fail(serverError); }
  if (!matches(result, Triggers::ACT_VALUE, "legacy-result")) {
    return fail("a legacy HTTP response was not retained as a value");
  }

  result = runScenario({"STREAM_SOURCE", "camera", "offline:no signal", "", "", 200}, "configured", Triggers::ACT_LEGACY, serverError);
  if (!serverError.empty()) { return fail(serverError); }
  if (!matches(result, Triggers::ACT_OFFLINE, "offline:no signal", false, "no signal")) {
    return fail("the legacy STREAM_SOURCE offline response was not recognized");
  }

  result = Triggers::handleTrigger("STREAM_PROCESS", "", "camera", 1, "configured", Triggers::ACT_CONFIGURED);
  if (!matches(result, Triggers::ACT_CONFIGURED, "", true, "trigger_unavailable")) {
    return fail("a blank handler did not use the configured failure action");
  }

  unsetenv("MIST_TUUID");
  unsetenv("MIST_TIME");
  unsetenv("MIST_DATE");
  return 0;
}
