#include <mist/socket.h>

#include <iostream>
#include <netdb.h>
#include <string>

namespace {
  int noAddressConfigured(const char *, const char *, const struct addrinfo *, struct addrinfo **) {
    return EAI_NONAME;
  }
} // namespace

int main() {
  Socket::setAddrInfoResolver(noAddressConfigured);
  const std::string zero(16, '\0');
  const struct {
      const char *text;
      std::string bytes;
      unsigned char prefix;
  } cases[] = {
    {"::/127", zero, 127},
    {"::1", std::string(15, '\0') + '\1', 128},
    {"::", zero, 128},
    {"192.0.2.7/24", std::string(10, '\0') + std::string("\xff\xff\xc0\x00\x02\x07", 6), 120},
    {"192.0.2.7", std::string(10, '\0') + std::string("\xff\xff\xc0\x00\x02\x07", 6), 128},
  };
  int failures = 0;
  for (const auto & test : cases) {
    std::string got = Socket::getBinForms(test.text);
    if (got.size() != 17 || got.substr(0, 16) != test.bytes || (unsigned char)got[16] != test.prefix) {
      std::cerr << "numeric address " << test.text << " returned " << got.size()
                << " bytes without configured address families; expected 17 with its network prefix" << std::endl;
      ++failures;
    }
  }
  if (!Socket::getBinForms("missing.example").empty()) {
    std::cerr << "unresolvable hostnames must still fail resolution" << std::endl;
    ++failures;
  }
  return failures ? 1 : 0;
}
