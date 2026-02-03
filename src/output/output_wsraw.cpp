#include "output_wsraw.h"
#include <mist/bitfields.h>
#include <mist/mp4_generic.h>
#include <mist/stream.h>

namespace Mist{
  OutWSRaw::OutWSRaw(Socket::Connection & conn, Util::Config & cfg, JSON::Value & _capa)
    : HTTPOutput(conn, cfg, _capa){
    wsCmds = true;
    keysOnly = targetParams.count("keysonly")?1:0;
  }

  void OutWSRaw::onWebsocketConnect() {
    capa["name"] = "Raw/WS";
    idleInterval = 1000;
    maxSkipAhead = 0;
  }

  void OutWSRaw::onWebsocketFrame() {
    JSON::Value command = JSON::fromString(webSock->data, webSock->data.size());
    if (!command.isMember("type")) {
      JSON::Value r;
      r["type"] = "error";
      r["data"] = "type field missing from command";
      webSock->sendFrame(r.toString());
      return;
    }
    
    if (command["type"] == "request_codec_data") {
      //If no supported codecs are passed, assume autodetected capabilities
      if (command.isMember("supported_codecs")) {
        capa.removeMember("exceptions");
        capa["codecs"].null();
        std::set<std::string> dupes;
        jsonForEach(command["supported_codecs"], i){
          if (dupes.count(i->asStringRef())){continue;}
          dupes.insert(i->asStringRef());
          JSON::Value arr;
          arr.append(i->asStringRef());
          capa["codecs"][0u].append(arr);
        }
      }
      if (command.isMember("supported_combinations")) {
        capa.removeMember("exceptions");
        capa["codecs"] = command["supported_combinations"];
      }
      selectDefaultTracks();
      sendWebsocketCodecData("codec_data");
      initialSeek();
      return;
    }
  }

  void OutWSRaw::sendWebsocketCodecData(const std::string & type) {
    JSON::Value r;
    r["type"] = type;
    r["data"]["current"] = currentTime();
    for (const auto & it : userSelect) {
      // Skip future tracks
      if (prevVidTrack != INVALID_TRACK_ID && M.getType(it.first) == "video" && it.first != prevVidTrack) { continue; }
      const std::string & mistCodec = M.getCodec(it.first);
      std::string codec = Util::codecString(mistCodec, M.getInit(it.first), true);
      r["data"]["codecs"].append(codec.size() ? codec : ("!" + mistCodec));
      r["data"]["tracks"].append((uint64_t)it.first);
    }
    webSock->sendFrame(r.toString());
  }

  void OutWSRaw::init(Util::Config *cfg, JSON::Value & capa){
    HTTPOutput::init(cfg, capa);
    capa["name"] = "WSRaw";
    capa["friendly"] = "Raw WebSocket";
    capa["desc"] = "Raw codec data over WebSocket";
    capa["url_rel"] = "/$.raw";
    capa["url_match"] = "/$.raw";
    capa["codecs"][0u][0u].append("+*");

    capa["methods"][0u]["handler"] = "ws";
    capa["methods"][0u]["type"] = "ws/video/raw";
    capa["methods"][0u]["hrn"] = "Raw WebSocket";
    capa["methods"][0u]["priority"] = 2;
    capa["methods"][0u]["url_rel"] = "/$.raw";
  }

  void OutWSRaw::sendNext(){
    // Call parent handler for generic websocket handling
    HTTPOutput::sendNext();
    if (!thisPacket) { return; }

    if (keysOnly && !thisPacket.getFlag("keyframe")) { return; }
    if (!webSock) { return; }

    char *dataPointer = 0;
    size_t len = 0;
    thisPacket.getString("data", dataPointer, len);

    webBuf.truncate(0);
    webBuf.append("\000\000\000\000\000\000\000\000\000\000\000\000", 12);
    webBuf[0] = thisIdx;
    webBuf[1] = thisPacket.getFlag("keyframe") ? 1 : 0;
    Bit::htobll(webBuf + 2, thisTime);
    if (thisPacket.hasMember("offset")) {
      Bit::htobs(webBuf + 10, thisPacket.getInt("offset"));
    } else {
      Bit::htobs(webBuf + 10, 0);
    }
    webBuf.append(dataPointer, len);
    webSock->sendFrame(webBuf, webBuf.size(), 2);
  }

  void OutWSRaw::sendHeader() {
    if (!webSock) { return; }

    JSON::Value r;
    r["type"] = "info";
    r["data"]["msg"] = "Sending header";
    for (const auto & it : userSelect) { r["data"]["tracks"].append((uint64_t)it.first); }
    webSock->sendFrame(r.toString());

    for (const auto & it : userSelect) {
      const std::string init = M.getInit(it.first);
      if (init.size()){
        Util::ResizeablePointer headerData;
        headerData.append("\000\000\000\000\000\000\000\000\000\000\000\000", 12);
        headerData[0] = it.first;
        headerData[1] = 2;
        headerData.append(init);
        webSock->sendFrame(headerData, headerData.size(), 2);
      }
    }
    handleWebsocketIdle();

    sentHeader = true;
  }

  void OutWSRaw::respondHTTP(const HTTP::Parser & req, bool headersOnly){
    //Set global defaults
    HTTPOutput::respondHTTP(req, headersOnly);
    H.SendResponse("406", "Not acceptable", myConn);
  }

}// namespace Mist
