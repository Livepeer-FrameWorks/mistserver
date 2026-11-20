#include INPUTTYPE
#include <iostream>

class UploadProbe : public mistIn {
  public:
    explicit UploadProbe(Util::Config *config) : mistIn(config) {}
    bool needsLock() { return false; }
    bool load(const std::string & path) {
      config->getOption("input", true).append(path);
      config->getOption("realtime", true).append(1u);
      standAlone = true;
      return preRun() && readHeader();
    }
    void print() {
      JSON::Value result;
      M.toJSON(result);
      for (size_t track : M.getValidTracks()) {
        if (M.getCodec(track) != "subtitle") { continue; }
        seek(0, track);
        for (size_t count = 0; count < 100; ++count) {
          getNext(track);
          if (!thisPacket) { break; }
          std::string text;
          thisPacket.getString("data", text);
          if (text.empty()) { continue; }
          if (!result.isMember("first_subtitle")) { result["first_subtitle"] = thisPacket.toJSON(); }
          result["last_subtitle"] = thisPacket.toJSON();
        }
      }
      for (size_t track : M.getValidTracks()) {
        if (M.getType(track) != "video") { continue; }
        seek(M.getFirstms(track) + 1000, track);
        getNext(track);
        if (thisPacket) { result["video_seek_time"] = thisPacket.getTime(); }
        break;
      }
      std::cout << result.toString() << std::endl;
    }
};
int main(int argc, char **argv) {
  if (argc != 2) { return 2; }
  Util::Config config("upload-metadata-probe");
  config.is_active = true;
  UploadProbe probe(&config);
  if (!probe.load(argv[1])) { return 1; }
  probe.print();
  return 0;
}
