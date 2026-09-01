#pragma once

#include <mist/http_parser.h>
#include <mist/json.h>
#include <string>

namespace Controller {
  std::string resolveLang(const HTTP::Parser &header);
  std::string tr(const std::string &message, const std::string &lang);
  void translateCapabilities(JSON::Value &capabilities, const std::string &lang);
}
