#include "trusted_proxy.h"

#include "defines.h"
#include "util.h"

#include <sstream>

namespace Util {
  bool publishTrustedProxyList(IPC::sharedPage & page, const std::string & pageName, const std::string & trustedList) {
    // The page this writer published before, or else one a previous controller generation left
    // behind (a rolling restart keeps shared memory): readers holding it are told to reload, and it
    // is removed so the list is written into a fresh page instead of into the old, ready one.
    if (!page) { page.init(pageName, 0, false, false); }
    if (page) {
      RelAccX oldPage(page.mapped, false);
      if (oldPage.isReady()) { oldPage.setReload(); }
      page.master = true;
      page.close();
    }

    page.init(pageName, trustedList.size() + 100, true, false);
    if (!page) { return false; }

    RelAccX output(page.mapped, false);
    output.addField("proxy_data", RAX_STRING, trustedList.size() + 1);
    output.setString("proxy_data", trustedList);
    output.setRCount(1);
    output.setEndPos(1);
    output.setReady();
    page.master = false;
    return true;
  }

  std::set<std::string> readTrustedProxyList(const IPC::sharedPage & page) {
    std::set<std::string> result;
    if (!page || !page.mapped) { return result; }

    RelAccX input(page.mapped, false);
    if (!input.isReady() || !input.hasField("proxy_data")) { return result; }
    char *data = input.getPointer("proxy_data");
    if (!data) { return result; }

    std::istringstream entries(std::string(data, input.getSize("proxy_data")));
    std::string entry;
    while (entries >> entry) { result.insert(entry); }
    return result;
  }
} // namespace Util
