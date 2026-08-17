#pragma once
#include <functional>
#include <string>

namespace Triggers {
  static const std::string empty;

  enum Action { ACT_LEGACY = 0, ACT_VALUE, ACT_DENY, ACT_KEEP, ACT_OFFLINE, ACT_CONFIGURED };

  struct Result {
      std::string response;
      Action action;
      std::string reason;
      bool handlerFailed;

      Result() : action(ACT_VALUE), handlerFailed(false) {}
  };

  Action actionFromString(const std::string & value);
  const char *actionName(Action action);
  bool actionAllowed(const std::string & triggerType, Action action);

  bool doTrigger(const std::string & triggerType, const std::string & payload, const std::string & streamName,
                 bool dryRun, std::string & response, std::function<bool(const char *)> paramsCB = 0);

  bool doTrigger(const std::string & triggerType, const std::string & payload, const std::string & streamName,
                 bool dryRun, Result & result, std::function<bool(const char *)> paramsCB = 0);

  Result handleTrigger(const std::string & triggerType, const std::string & value, const std::string & payload,
                       int sync, const std::string & defaultResponse, Action onFail = ACT_LEGACY);

  // All of the below are just shorthands for specific usage of the doTrigger function above:

  bool shouldTrigger(const std::string & triggerType, const std::string & streamName = empty,
                     std::function<bool(const char *)> paramsCB = 0);

  bool doTrigger(const std::string & triggerType, const std::string & payload = empty, const std::string & streamName = empty);
} // namespace Triggers
