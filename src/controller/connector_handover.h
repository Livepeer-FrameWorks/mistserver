#pragma once
#include <mist/json.h>

#include <cstdint>
#include <string>

namespace Controller {

  /// A stopped connector gets this long to exit before it is killed, so the
  /// replacement can bind the address it held.
  static const uint64_t CONNECTOR_STOP_GRACE_MS = 5000;

  /// The listening socket a connector configuration binds, or an empty string
  /// when it declares none. Two configurations with the same key cannot run at
  /// the same time, whatever their other settings.
  inline std::string connectorBindKey(const JSON::Value & cnf, const JSON::Value & capa) {
    if (cnf.isMember("socket") && cnf["socket"].isString() && cnf["socket"].asStringRef().size()) {
      return "unix:" + cnf["socket"].asStringRef();
    }
    int64_t port = 0;
    if (cnf.isMember("port") && cnf["port"].asInt()) {
      port = cnf["port"].asInt();
    } else if (capa.isMember("optional") && capa["optional"].isMember("port")) {
      port = capa["optional"]["port"]["default"].asInt();
    }
    // Listeners bind the wildcard address by default, so the port alone decides
    // whether two configurations collide.
    if (port <= 0) { return ""; }
    return "tcp:" + std::to_string(port);
  }

  enum class ConnectorHandoverStep { Start, Wait, Kill };

  /// What to do with a new connector whose bind key a stopped one still holds.
  inline ConnectorHandoverStep connectorHandoverStep(bool previousActive, uint64_t msSinceStop) {
    if (!previousActive) { return ConnectorHandoverStep::Start; }
    if (msSinceStop < CONNECTOR_STOP_GRACE_MS) { return ConnectorHandoverStep::Wait; }
    return ConnectorHandoverStep::Kill;
  }

} // namespace Controller
