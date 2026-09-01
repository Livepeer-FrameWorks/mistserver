#pragma once
#include <string>

// Marks a source string for gettext extraction. Runtime translation happens
// on the per-request capabilities copy in Controller::translateCapabilities.
inline const char *tr(const char *message) { return message; }
inline const std::string &tr(const std::string &message) { return message; }
