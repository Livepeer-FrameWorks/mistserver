#include <mist/soap.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <libxml/xmlmemory.h>

namespace {
  std::atomic<int64_t> allocations{0};
  void *trackedMalloc(size_t size) {
    void *result = std::malloc(size);
    if (result) { ++allocations; }
    return result;
  }
  void trackedFree(void *pointer) {
    if (pointer) { --allocations; }
    std::free(pointer);
  }
  void *trackedRealloc(void *pointer, size_t size) {
    if (!size) {
      trackedFree(pointer);
      return nullptr;
    }
    void *result = std::realloc(pointer, size);
    if (!pointer && result) { ++allocations; }
    return result;
  }
  char *trackedStrdup(const char *text) {
    const size_t length = std::strlen(text) + 1;
    char *result = static_cast<char *>(trackedMalloc(length));
    if (result) { std::memcpy(result, text, length); }
    return result;
  }
} // namespace

int main() {
  xmlFreeFunc previousFree;
  xmlMallocFunc previousMalloc;
  xmlReallocFunc previousRealloc;
  xmlStrdupFunc previousStrdup;
  const bool tracksAllocations = !xmlMemGet(&previousFree, &previousMalloc, &previousRealloc, &previousStrdup) &&
    !xmlMemSetup(trackedFree, trackedMalloc, trackedRealloc, trackedStrdup);
#ifndef __APPLE__
  if (!tracksAllocations) {
    std::cerr << "XML allocation hooks are unavailable; ownership was not tested\n";
    return 2;
  }
#endif
  const auto exercise = []() {
    SOAP::onvif::DiscoveryFactory factory;
    SOAP::onvif::DiscoveryOptions options;
    options.scopes = {"urn:scope:one", "urn:scope:two"};
    options.matchBy = true;
    options.matchByUri = "urn:match";
    for (const auto & kind : {"Probe", "Hello", "Bye"}) {
      auto message = std::strcmp(kind, "Probe") == 0 ? factory.makeProbe(options)
        : std::strcmp(kind, "Hello") == 0            ? factory.makeHello(options)
                                                     : factory.makeBye(options);
      const std::string text = message.toString();
      XML::Document parsed(text);
      if (!parsed.isValid() || text.find(kind) == std::string::npos ||
          text.find("urn:scope:one urn:scope:two") == std::string::npos || text.find("urn:match") == std::string::npos) {
        std::cerr << "WS-Discovery body lost its type, scope or match policy\n";
        return false;
      }
    }
    return true;
  };
  if (!exercise()) { return 1; }
  const int64_t before = allocations.load();
  for (int iteration = 0; iteration != 25; ++iteration) {
    if (!exercise()) { return 1; }
  }
  const int64_t retained = allocations.load() - before;
  if (tracksAllocations) { xmlMemSetup(previousFree, previousMalloc, previousRealloc, previousStrdup); }
  if (tracksAllocations && retained) {
    std::cerr << "WS-Discovery retained " << retained << " XML allocations after message destruction\n";
    return 1;
  }
  if (tracksAllocations) {
    std::cout << "PASS WS-Discovery releases every probe, hello and bye body allocation\n";
  } else {
    // Apple's system libxml2 does not expose allocator replacement hooks.
    std::cout << "PASS WS-Discovery message contents; allocation ownership requires the Linux test\n";
  }
}
