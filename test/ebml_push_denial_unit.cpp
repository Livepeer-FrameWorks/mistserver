#include "output_ebml.h"

#include <mist/shared_memory.h>
#include <mist/util.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

class DeniedPush : public Mist::OutEBML {
  public:
    unsigned failures = 0;
    DeniedPush(Socket::Connection & connection, Util::Config & config, JSON::Value & capability)
      : Mist::OutEBML(connection, config, capability) {
      H.method = "PUT";
      H.protocol = "HTTP/1.1";
      streamName = "sk_denied";
      reqUrl = "http://localhost/sk_denied%2Emkv?publish_token=secret";
    }
    void onFail(const std::string & reason, bool critical = false) override {
      ++failures;
      Mist::HTTPOutput::onFail(reason, critical);
    }
};

int main(int argc, char **argv) {
  if (argc > 1 && std::string(argv[1]) == "PUSH_REWRITE") {
    char input[4096];
    while (read(STDIN_FILENO, input, sizeof(input)) > 0) {}
    return 0;
  }
  char directory[] = "/tmp/mist-denied-push-XXXXXX";
  if (!mkdtemp(directory)) { return 3; }
  setenv("TMP", directory, 1);
  setenv("UDP_API", "udp://127.0.0.1:0", 1);
  setenv("MIST_CONTROL", "1", 1);
  unsetenv("stream_raw");
  char name[NAME_BUFFER_SIZE];
  snprintf(name, sizeof(name), SHM_TRIGGER, "PUSH_REWRITE");
  unsigned failures = 0;
  std::string denialLog;
  {
    IPC::sharedPage page(name, 32768, true, false);
    if (!page.mapped) { return 3; }
    Util::RelAccX trigger(page.mapped, false);
    trigger.addField("url", RAX_128STRING);
    trigger.addField("sync", RAX_UINT);
    trigger.addField("streams", RAX_256RAW);
    trigger.addField("params", RAX_128STRING);
    trigger.addField("default", RAX_128STRING);
    trigger.addField("onfail", RAX_UINT);
    trigger.setReady();
    trigger.setString("url", argv[0], 0);
    trigger.setInt("sync", 1, 0);
    trigger.setInt("onfail", Triggers::ACT_DENY, 0);
    trigger.setString("params", "", 0);
    trigger.setString("default", "", 0);
    memset(trigger.getPointer("streams", 0), 0, 256);
    trigger.setRCount(1);
    trigger.setEndPos(1);
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) { return 3; }
    {
      Socket::Connection connection(pair[0]);
      Util::Config config("ebml-denied-push");
      JSON::Value capability;
      Mist::OutEBML::init(&config, capability);
      config.getOption("streamname", true).append("sk_denied");
      DeniedPush output(connection, config, capability);
      FILE *captured = tmpfile();
      const int savedStderr = dup(STDERR_FILENO);
      if (!captured || savedStderr < 0) { return 3; }
      fflush(stderr);
      if (dup2(fileno(captured), STDERR_FILENO) < 0) { return 3; }
      output.preHTTP();
      fflush(stderr);
      if (dup2(savedStderr, STDERR_FILENO) < 0) { return 3; }
      close(savedStderr);
      rewind(captured);
      char buffer[4096];
      size_t count;
      while ((count = fread(buffer, 1, sizeof(buffer), captured))) { denialLog.append(buffer, count); }
      fclose(captured);
      failures = output.failures;
    }
    close(pair[1]);
  }
  rmdir(directory);
  if (failures != 1) {
    fprintf(stderr, "Denied MKV push continued into source admission: %u failures, expected one\n", failures);
    return 1;
  }
  if (denialLog.find("blanked") == std::string::npos) {
    fprintf(stderr, "Denied MKV push did not record its refusal: %s\n", denialLog.c_str());
    return 1;
  }
  if (denialLog.find("sk_denied") != std::string::npos || denialLog.find("publish_token=secret") != std::string::npos) {
    fprintf(stderr, "Denied MKV push leaked publishing credentials: %s\n", denialLog.c_str());
    return 1;
  }
  return 0;
}
