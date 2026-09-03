#pragma once

#include <mist/json.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace Controller {
  struct PushControlRequest {
      std::vector<uint32_t> killIds;
      std::vector<uint32_t> reinitializeIds;
      std::vector<std::pair<std::string, std::string>> reinitializeTargets;
      size_t invalidReinitializeRequests = 0;
  };

  inline PushControlRequest parsePushControlRequest(const JSON::Value & request) {
    PushControlRequest result;
    if (request.isMember("push_kill")) {
      if (request["push_kill"].isArray()) {
        jsonForEachConst (request["push_kill"], it) { result.killIds.push_back(it->asInt()); }
      } else {
        result.killIds.push_back(request["push_kill"].asInt());
      }
    }

    if (!request.isMember("push_reinit")) { return result; }
    auto appendReinitialize = [&](const JSON::Value & value) {
      if (value.isInt()) {
        result.reinitializeIds.push_back(value.asInt());
      } else if (value.isObject() && value.isMember("streamname") && value.isMember("target")) {
        result.reinitializeTargets.push_back(std::make_pair(value["streamname"].asStringRef(), value["target"].asStringRef()));
      } else {
        ++result.invalidReinitializeRequests;
      }
    };
    if (request["push_reinit"].isArray()) {
      jsonForEachConst (request["push_reinit"], it) { appendReinitialize(*it); }
    } else {
      appendReinitialize(request["push_reinit"]);
    }
    return result;
  }
} // namespace Controller
