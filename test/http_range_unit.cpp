#include "../src/output/http_range.h"

#include <cstdio>

int main() {
  const struct {
      const char *header;
      bool valid;
      uint64_t start, end;
  } cases[] = {{"bytes=0-0", true, 0, 0},
               {"bytes=1-9", true, 1, 9},
               {"bytes=99-", true, 99, 99},
               {"bytes=0-1000", true, 0, 99},
               {"bytes=-1", true, 99, 99},
               {"bytes=-99", true, 1, 99},
               {"bytes=-100", true, 0, 99},
               {"bytes=-101", true, 0, 99},
               {"bytes=100-", false, 0, 0},
               {"bytes=2-1", false, 0, 0},
               {"bytes=-0", false, 0, 0},
               {"bytes=-", false, 0, 0},
               {"bytes=", false, 0, 0},
               {"bytes=0", false, 0, 0},
               {"bytes=0-1,2-3", false, 0, 0},
               {"bytes=0-1junk", false, 0, 0},
               {"bytes=18446744073709551616-", false, 0, 0},
               {"bytes=0-18446744073709551616", false, 0, 0},
               {"bytes=-18446744073709551616", false, 0, 0},
               {"bytes=0-18446744073709551615", true, 0, 99},
               {"bytes=-18446744073709551615", true, 0, 99},
               {"items=0-1", false, 0, 0}};
  for (const auto & item : cases) {
    uint64_t start = 0, end = 99;
    const bool valid = Mist::parseSingleByteRange(item.header, start, end);
    if (valid != item.valid || (valid && (start != item.start || end != item.end)) || (!valid && end)) {
      fprintf(stderr, "Incorrect range: %s\n", item.header);
      return 1;
    }
  }
  uint64_t start = 0, end = 0;
  if (!Mist::parseSingleByteRange("bytes=-1", start, end) || start || end) { return 1; }
  return 0;
}
