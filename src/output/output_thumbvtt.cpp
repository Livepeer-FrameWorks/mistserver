#include "output_thumbvtt.h"
#include <sstream>
#include <mist/defines.h>
#include <mist/http_parser.h>
#include <mist/util.h>

namespace Mist{
  OutThumbVTT::OutThumbVTT(Socket::Connection &conn, Util::Config &_cfg, JSON::Value &_capa)
      : HTTPOutput(conn, _cfg, _capa){
    realTime = 0;
    pushMode = false;
    vttTrack = INVALID_TRACK_ID;
    spriteTrack = INVALID_TRACK_ID;
  }

  OutThumbVTT::~OutThumbVTT(){}

  bool OutThumbVTT::isReadyForPlay(){return true;}

  void OutThumbVTT::init(Util::Config *cfg, JSON::Value &capa){
    HTTPOutput::init(cfg, capa);
    capa["name"] = "ThumbVTT";
    capa["friendly"] = "Thumbnail sprite VTT over HTTP";
    capa["desc"] = "WebVTT output for thumbnail sprite sheets with correct timing";
    capa["url_match"].append("/$.thumbvtt");
    capa["codecs"][0u][0u].append("thumbvtt");
    capa["codecs"][0u][0u].append("JPEG");
    capa["methods"][0u]["handler"] = "http";
    capa["methods"][0u]["type"] = "html5/text/vtt";
    capa["methods"][0u]["hrn"] = "Thumbnail VTT progressive";
    capa["methods"][0u]["priority"] = 1;
    capa["methods"][0u]["url_rel"] = "/$.thumbvtt";
  }

  size_t OutThumbVTT::findSpriteTrack(){
    std::set<size_t> validTracks = M.getValidTracks();
    for (std::set<size_t>::iterator it = validTracks.begin(); it != validTracks.end(); ++it){
      if (M.getCodec(*it) == "JPEG" && M.getLang(*it) == "thu"){return *it;}
    }
    return INVALID_TRACK_ID;
  }

  void OutThumbVTT::pushPair(){
    if (pendingVtt.empty() || pendingJpeg.empty()){return;}

    std::stringstream part;
    part << "\r\n--" << boundary << "\r\n";
    part << "Content-Type: text/vtt; charset=utf-8\r\n";
    part << "Content-Length: " << pendingVtt.size() << "\r\n\r\n";
    myConn.SendNow(part.str());
    myConn.SendNow(pendingVtt);

    std::stringstream imgPart;
    imgPart << "\r\n--" << boundary << "\r\n";
    imgPart << "Content-Type: image/jpeg\r\n";
    imgPart << "Content-Length: " << pendingJpeg.size() << "\r\n\r\n";
    myConn.SendNow(imgPart.str());
    myConn.SendNow(pendingJpeg);

    pendingVtt.clear();
    pendingJpeg.clear();
  }

  void OutThumbVTT::sendHeader(){
    H.setCORSHeaders();
    if (pushMode){
      boundary = Util::getRandomAlphanumeric(24);
      H.SetHeader("Content-Type", "multipart/mixed; boundary=" + boundary);
    }else{
      H.SetHeader("Content-Type", "text/vtt; charset=utf-8");
    }
    H.protocol = "HTTP/1.0";
    H.SendResponse("200", "OK", myConn);
    sentHeader = true;
  }

  void OutThumbVTT::sendNext(){
    char *dataPointer = 0;
    size_t len = 0;
    thisPacket.getString("data", dataPointer, len);
    if (!len){return;}

    if (!pushMode){
      // Poll mode: send first VTT packet and close
      if (M.getCodec(thisIdx) == "thumbvtt"){
        myConn.SendNow(dataPointer, len);
        myConn.close();
      }
      return;
    }

    // Push mode: collect VTT + JPEG, send as multipart pair
    if (M.getCodec(thisIdx) == "thumbvtt"){
      pendingVtt.assign(dataPointer, len);
    }else if (M.getCodec(thisIdx) == "JPEG"){
      pendingJpeg.assign(dataPointer, len);
    }
    pushPair();
  }

  void OutThumbVTT::onHTTP(){
    std::string method = H.method;
    std::string mode = H.GetVar("mode");

    // Find VTT track
    vttTrack = INVALID_TRACK_ID;
    if (H.GetVar("track").size()){
      size_t tid = atoll(H.GetVar("track").c_str());
      if (M.getValidTracks().count(tid)){
        vttTrack = tid;
      }
    }
    if (vttTrack == INVALID_TRACK_ID){
      std::set<size_t> validTracks = M.getValidTracks();
      for (std::set<size_t>::iterator it = validTracks.begin(); it != validTracks.end(); ++it){
        if (M.getCodec(*it) == "thumbvtt"){
          vttTrack = *it;
          break;
        }
      }
    }

    spriteTrack = findSpriteTrack();

    H.Clean();
    H.setCORSHeaders();
    if (method == "OPTIONS" || method == "HEAD"){
      H.SetHeader("Content-Type", "text/vtt; charset=utf-8");
      H.protocol = "HTTP/1.0";
      H.SendResponse("200", "OK", myConn);
      H.Clean();
      return;
    }

    // Select tracks based on mode
    pushMode = (mode == "push");

    userSelect.clear();
    if (vttTrack != INVALID_TRACK_ID){
      userSelect[vttTrack].reload(streamName, vttTrack);
    }
    if (pushMode && spriteTrack != INVALID_TRACK_ID){
      userSelect[spriteTrack].reload(streamName, spriteTrack);
    }

    parseData = true;
    wantRequest = false;
  }
}// namespace Mist
