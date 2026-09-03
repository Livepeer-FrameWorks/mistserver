#include <mist/stream.h>
#include <mist/triggers.h>

#include <iostream>

int main() {
  if (Triggers::actionFromString("DENY") != Triggers::ACT_DENY ||
      Triggers::actionFromString("use-configured") != Triggers::ACT_CONFIGURED ||
      Triggers::actionFromString("unknown") != Triggers::ACT_LEGACY) {
    std::cerr << "trigger action parsing failed" << std::endl;
    return 1;
  }
  if (!Triggers::actionAllowed("STREAM_SOURCE", Triggers::ACT_OFFLINE) ||
      Triggers::actionAllowed("PLAY_REWRITE", Triggers::ACT_OFFLINE) ||
      !Triggers::actionAllowed("PLAY_REWRITE", Triggers::ACT_DENY)) {
    std::cerr << "trigger action compatibility failed" << std::endl;
    return 1;
  }
  if (!Triggers::onFailAllowed("PLAY_REWRITE", "deny", true) || !Triggers::onFailAllowed("STREAM_SOURCE", "offline", true) ||
      !Triggers::onFailAllowed("PLAY_REWRITE", "legacy", false) ||
      !Triggers::onFailAllowed("PLAY_REWRITE", "LEGACY", false) || Triggers::onFailAllowed("PLAY_REWRITE", "offline", true) ||
      Triggers::onFailAllowed("PLAY_REWRITE", "deny", false) || Triggers::onFailAllowed("PLAY_REWRITE", "unknown", true)) {
    std::cerr << "trigger onfail validation failed" << std::endl;
    return 1;
  }
  std::string empty;
  Util::sanitizeName(empty);
  if (!empty.empty()) {
    std::cerr << "empty stream name sanitation changed the value" << std::endl;
    return 1;
  }
  return 0;
}
