#pragma once
#include <cstring>
#include <stdint.h>

struct EmbeddedFile {
    const char *path;
    const char *data;
    uint32_t data_len;
    const char *mime;
    const char *etag;
};

inline const EmbeddedFile *vfsLookup(const EmbeddedFile *entries, int count, const char *path) {
  for (int i = 0; i < count; ++i) {
    if (strcmp(entries[i].path, path) == 0) return &entries[i];
  }
  return 0;
}
