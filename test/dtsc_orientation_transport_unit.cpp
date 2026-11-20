#include <mist/dtsc.h>

#include <cstdio>

int main() {
  DTSC::Meta meta;
  meta.reInit("", true);
  JSON::Value source;
  source["type"] = "video";
  source["codec"] = "VP9";
  source["trackid"] = 1;
  source["width"] = 320;
  source["height"] = 180;
  source["rotation"] = 90;
  source["mirror"] = true;
  std::string wire = source.toPacked();
  meta.addTrackFrom(DTSC::Scan(&wire[0], wire.size()));
  DTSC::Meta copy;
  copy.reInit("", true);
  copy.merge(meta, false, false);
  JSON::Value result;
  copy.toJSON(result);
  jsonForEachConst (result["tracks"], track) {
    if ((*track)["rotation"].asInt() == 90 && (*track)["mirror"].asBool()) { return 0; }
  }
  fprintf(stderr, "DTSC metadata transport dropped the source display rotation or reflection\n");
  return 1;
}
