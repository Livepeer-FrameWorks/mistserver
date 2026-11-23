#include <mist/dtsc.h>

#include <cstdio>

int main(int argc, char **argv) {
  if (argc != 2) { return 2; }
  DTSC::trackValidMask = TRACK_VALID_ALL;
  DTSC::Meta meta;
  meta.reInit(argv[1], false, false);
  if (!meta) { return 1; }
  JSON::Value result;
  for (const size_t track : meta.getValidTracks()) {
    JSON::Value & row = result[std::to_string(track)];
    row["type"] = meta.getType(track);
    row["codec"] = meta.getCodec(track);
    row["first_ms"] = meta.getFirstms(track);
    row["last_ms"] = meta.getLastms(track);
    row["claimed"] = meta.isClaimed(track);
    const DTSC::Parts parts(meta.parts(track));
    row["parts_first"] = uint64_t(parts.getFirstValid());
    row["parts_end"] = uint64_t(parts.getEndValid());
  }
  printf("%s\n", result.toString().c_str());
}
