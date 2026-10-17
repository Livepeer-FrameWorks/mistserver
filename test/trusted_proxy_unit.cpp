#include <mist/defines.h>
#include <mist/timing.h>
#include <mist/trusted_proxy.h>
#include <mist/util.h>

#include <iostream>
#include <string>
#include <unistd.h>

namespace {
  void removePage(IPC::sharedPage & page) {
    page.master = true;
    page.close();
  }

  bool contains(const std::set<std::string> & entries, const std::string & value) {
    return entries.count(value) == 1;
  }
} // namespace

int main() {
  const std::string pageName = "/MstTrustedProxyTest_" + std::to_string(getpid());
  IPC::sharedPage writer;

  if (!Util::publishTrustedProxyList(writer, pageName, "127.0.0.1 10.0.0.0/8")) {
    std::cerr << "first trusted-proxy publish failed" << std::endl;
    return 1;
  }
  {
    IPC::sharedPage reader(pageName, 0, false, false);
    const std::set<std::string> entries = Util::readTrustedProxyList(reader);
    if (entries.size() != 2 || !contains(entries, "127.0.0.1") || !contains(entries, "10.0.0.0/8")) {
      std::cerr << "first trusted-proxy list did not round-trip" << std::endl;
      removePage(writer);
      return 1;
    }
  }

  IPC::sharedPage previousReader(pageName, 0, false, false);
  Util::RelAccX previousView(previousReader.mapped, false);

  if (!Util::publishTrustedProxyList(writer, pageName, "192.0.2.1   2001:db8::/32 198.51.100.0/24")) {
    std::cerr << "trusted-proxy replacement failed" << std::endl;
    return 1;
  }
  if (!previousView.isReload()) {
    std::cerr << "replaced trusted-proxy page did not notify existing readers" << std::endl;
    removePage(writer);
    return 1;
  }
  {
    IPC::sharedPage reader(pageName, 0, false, false);
    const std::set<std::string> entries = Util::readTrustedProxyList(reader);
    if (entries.size() != 3 || !contains(entries, "192.0.2.1") || !contains(entries, "2001:db8::/32") ||
        !contains(entries, "198.51.100.0/24") || contains(entries, "127.0.0.1")) {
      std::cerr << "replacement trusted-proxy list was stale or malformed" << std::endl;
      removePage(writer);
      return 1;
    }
  }
  // A controller restarted in place (a rolling restart) publishes with a writer of its own while
  // the previous generation's page still exists: the longer list must be readable in full and the
  // previous generation's readers must be told to reload.
  writer.master = false;
  writer.close();
  {
    IPC::sharedPage generationReader(pageName, 0, false, false);
    Util::RelAccX generationView(generationReader.mapped, false);
    IPC::sharedPage nextGeneration;
    const std::string longer = "192.0.2.1 2001:db8::/32 198.51.100.0/24 203.0.113.0/24 100.64.0.0/10";
    if (!Util::publishTrustedProxyList(nextGeneration, pageName, longer)) {
      std::cerr << "trusted-proxy publish over a previous generation's page failed" << std::endl;
      return 1;
    }
    if (!generationView.isReload()) {
      std::cerr << "the previous generation's trusted-proxy readers were not told to reload" << std::endl;
      removePage(nextGeneration);
      return 1;
    }
    IPC::sharedPage reader(pageName, 0, false, false);
    const std::set<std::string> entries = Util::readTrustedProxyList(reader);
    if (entries.size() != 5 || !contains(entries, "203.0.113.0/24") || !contains(entries, "100.64.0.0/10")) {
      std::cerr << "the trusted-proxy list published over a previous generation's page is incomplete ("
                << entries.size() << " entries)" << std::endl;
      removePage(nextGeneration);
      return 1;
    }
    removePage(nextGeneration);
  }

  IPC::sharedPage incomplete(pageName, 128, true, false);
  const uint64_t start = Util::bootMS();
  if (!Util::readTrustedProxyList(incomplete).empty()) {
    std::cerr << "incomplete trusted-proxy page produced entries" << std::endl;
    removePage(incomplete);
    return 1;
  }
  if (Util::bootMS() - start > 250) {
    std::cerr << "incomplete trusted-proxy page blocked the reader" << std::endl;
    removePage(incomplete);
    return 1;
  }
  removePage(incomplete);
  return 0;
}
