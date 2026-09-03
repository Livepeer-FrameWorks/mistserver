#pragma once

#include <mist/json.h>
#include <mist/triggers.h>

#include <string>

namespace Controller {
  inline bool validateTriggerOnFailConfig(const JSON::Value & triggers, std::string & error) {
    error.clear();
    if (!triggers.isObject()) { return true; }
    jsonForEachConst (triggers, triggerIt) {
      if (!triggerIt->isArray()) { continue; }
      jsonForEachConst (*triggerIt, handlerIt) {
        if (!handlerIt->isObject() || !handlerIt->isMember("onfail") || (*handlerIt)["onfail"].isNull() ||
            !(*handlerIt)["onfail"].asStringRef().size()) {
          continue;
        }
        const std::string & configured = (*handlerIt)["onfail"].asStringRef();
        if (!Triggers::onFailAllowed(triggerIt.key(), configured, (*handlerIt)["sync"].asBool())) {
          error = "Invalid onfail action '" + configured + "' for " + triggerIt.key() + " trigger";
          return false;
        }
      }
    }
    return true;
  }
} // namespace Controller
