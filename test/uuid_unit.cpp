#include <mist/util.h>

#include <cctype>
#include <iostream>
#include <set>
#include <string>

int main() {
  std::set<std::string> generated;

  for (size_t sample = 0; sample < 1024; ++sample) {
    const std::string uuid = Util::generateUUID();
    if (uuid.size() != 36) {
      std::cerr << "UUID has length " << uuid.size() << " instead of 36: " << uuid << std::endl;
      return 1;
    }

    for (size_t i = 0; i < uuid.size(); ++i) {
      const bool hyphen = i == 8 || i == 13 || i == 18 || i == 23;
      if (hyphen) {
        if (uuid[i] != '-') {
          std::cerr << "UUID is missing a hyphen at position " << i << ": " << uuid << std::endl;
          return 1;
        }
      } else if (!std::isxdigit(static_cast<unsigned char>(uuid[i])) || (uuid[i] >= 'A' && uuid[i] <= 'F')) {
        std::cerr << "UUID contains a non-lowercase-hex character at position " << i << ": " << uuid << std::endl;
        return 1;
      }
    }

    if (uuid[14] != '4') {
      std::cerr << "UUID does not have the version 4 marker: " << uuid << std::endl;
      return 1;
    }
    if (uuid[19] != '8' && uuid[19] != '9' && uuid[19] != 'a' && uuid[19] != 'b') {
      std::cerr << "UUID does not have an RFC 4122 variant marker: " << uuid << std::endl;
      return 1;
    }
    generated.insert(uuid);
  }

  if (generated.size() != 1024) {
    std::cerr << "Generated duplicate UUIDs in a 1024-sample regression test" << std::endl;
    return 1;
  }

  return 0;
}
