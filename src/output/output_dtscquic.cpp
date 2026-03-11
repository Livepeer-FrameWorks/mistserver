#include "output_dtscquic.h"

#include <mist/auth.h>
#include <mist/bitfields.h>
#include <mist/defines.h>
#include <mist/encode.h>
#include <mist/http_parser.h>
#include <mist/stream.h>
#include <mist/triggers.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <sys/select.h>
#include <sys/stat.h>

namespace Mist{
  OutDTSCQuic::OutDTSCQuic(Socket::Connection &conn, Util::Config & _cfg, JSON::Value & _capa) : Output(conn, _cfg, _capa){
    JSON::Value prep;
    isSyncReceiver = false;
    lastActive = Util::epoch();
    if (config->getString("target").size()){
      streamName = config->getString("streamname");
      pushUrl = HTTP::URL(config->getString("target"));
      setSyncMode(true);
      if (!pushUrl.path.size()){pushUrl.path = streamName;}
      INFO_MSG("About to push stream %s out. Host: %s, port: %d, target stream: %s", streamName.c_str(),
               pushUrl.host.c_str(), pushUrl.getPort(), pushUrl.path.c_str());
      initialize();
      initialSeek();
      if (!myConn){
        onFail("Could not start push, aborting", true);
        return;
      }
      myConn.setBlocking(false);
      prep["cmd"] = "push";
      prep["version"] = APPIDENT;
      prep["stream"] = pushUrl.path;
      std::map<std::string, std::string> args;
      HTTP::parseVars(pushUrl.args, args);
      if (args.count("pass")){prep["password"] = args["pass"];}
      if (args.count("pw")){prep["password"] = args["pw"];}
      if (args.count("password")){prep["password"] = args["password"];}
      if (pushUrl.pass.size()){prep["password"] = pushUrl.pass;}
      sendCmd(prep);
      wantRequest = true;
      parseData = true;
      return;
    }

    setSyncMode(false);
    setBlocking(true);
    prep["cmd"] = "hi";
    prep["version"] = APPIDENT;
    prep["pack_method"] = 2;
    salt = Secure::md5("mehstuff" + JSON::Value((uint64_t)time(0)).asString());
    prep["salt"] = salt;
    /// \todo Make this securererer.
    sendCmd(prep);
  }

  OutDTSCQuic::~OutDTSCQuic(){}

  
  bool OutDTSCQuic::isFileTarget(){
    if (!isRecording()){return false;}
    pushUrl = HTTP::URL(config->getString("target"));
    if (pushUrl.protocol == "dtscquic"){return false;}
    return true;
  }

  void OutDTSCQuic::stats(bool force){
    unsigned long long int now = Util::epoch();
    if (now - lastActive > 1 && !pushing){
      JSON::Value prep;
      prep["cmd"] = "ping";
      sendCmd(prep);
      lastActive = now;
    }
    Output::stats(force);
  }

  void OutDTSCQuic::sendCmd(const JSON::Value &data){
    MEDIUM_MSG("Sending DTCM: %s", data.toString().c_str());
    myConn.SendNow("DTCM");
    char sSize[4] ={0, 0, 0, 0};
    Bit::htobl(sSize, data.packedSize());
    myConn.SendNow(sSize, 4);
    data.sendTo(myConn);
  }

  void OutDTSCQuic::sendOk(const std::string &msg){
    JSON::Value err;
    err["cmd"] = "ok";
    err["msg"] = msg;
    sendCmd(err);
  }

  void OutDTSCQuic::init(Util::Config *cfg, JSON::Value & capa){
    Output::init(cfg, capa);
    capa["name"] = "DTSCQuic";
    capa["friendly"] = "DTSCQuic";
    capa["desc"] = "Real time streaming over DTSC (proprietary protocol for efficient inter-server streaming)";
    capa["deps"] = "";
    capa["codecs"][0u][0u].append("+*");
    cfg->addStandardPushCapabilities(capa);
    capa["push_urls"].append("dtscquic://*");
    capa["incoming_push_url"] = "dtsc://$host:$port/$stream?pass=$password";

    capa["required"]["cert"]["name"] = "Certificate";
    capa["required"]["cert"]["help"] = "Path(s) to certificate file(s)";
    capa["required"]["cert"]["option"] = "--cert";
    capa["required"]["cert"]["short"] = "C";
    capa["required"]["cert"]["default"] = "";
    capa["required"]["cert"]["type"] = "str";
    capa["required"]["key"]["name"] = "Key";
    capa["required"]["key"]["help"] = "Path(s) to (certificate) key file(s)";
    capa["required"]["key"]["option"] = "--key";
    capa["required"]["key"]["short"] = "K";
    capa["required"]["key"]["default"] = "";
    capa["required"]["key"]["type"] = "str";

    capa["optional"]["ca"]["name"] = "Certificate Authority";
    capa["optional"]["ca"]["help"] = "Path(s) to certificate authority file(s)";
    capa["optional"]["ca"]["option"] = "--ca";
    capa["optional"]["ca"]["short"] = "A";
    capa["optional"]["ca"]["default"] = "";
    capa["optional"]["ca"]["type"] = "str";

    JSON::Value & pp = capa["push_parameters"];

    pp["cert"]["name"] = "Certificate";
    pp["cert"]["help"] = "Path to certificate file to use for authentication with server side";
    pp["cert"]["type"] = "string";
    pp["cert"]["sort"] = "001";
    pp["key"]["name"] = "Key";
    pp["key"]["help"] = "Path to key file to use for authentication with server side";
    pp["key"]["type"] = "string";
    pp["key"]["sort"] = "002";
    pp["ca"]["name"] = "Certificate Authority";
    pp["ca"]["help"] = "Path to certificate file to authenticate server side against (not set = accept any certificate)";
    pp["ca"]["type"] = "string";
    pp["ca"]["sort"] = "003";

    capa["url_rel"] = "/$";

    capa["methods"][0u]["handler"] = "dtscquic";
    capa["methods"][0u]["type"] = "dtsc";
    capa["methods"][0u]["hrn"] = "DTSC over QUIC";
    capa["methods"][0u]["priority"] = 10;


    JSON::Value opt;
    opt["arg"] = "string";
    opt["default"] = "";
    opt["arg_num"] = 1;
    opt["help"] = "Target DTSC URL to push out towards.";
    cfg->addOption("target", opt);
    cfg->addOption("streamname", JSON::fromString("{\"arg\":\"string\",\"short\":\"s\",\"long\":"
                                                  "\"stream\",\"help\":\"The name of the stream to "
                                                  "push out, when pushing out.\"}"));

    opt.null();
    opt["arg"] = "string";
    opt["default"] = "";
    opt["short"] = "U";
    opt["long"] = "pushurl";
    opt["help"] = "Target DTSC URL to pretend pushing out towards, when not actually connected to another host";
    cfg->addOption("pushurl", opt);

    cfg->addConnectorOptions(4200, capa);
  }

  std::string OutDTSCQuic::getStatsName(){return (pushing ? "INPUT:DTSCQuic" : "OUTPUT:DTSCQuic");}

  void OutDTSCQuic::sendNext(){
    DTSC::Packet p(thisPacket, thisIdx+1);
    myConn.SendNow(p.getData(), p.getDataLen());
    lastActive = Util::epoch();

    // If selectable tracks changed, set sentHeader to false to force it to send init data
    static uint64_t lastMeta = 0;
    if (Util::epoch() > lastMeta + 5){
      lastMeta = Util::epoch();
      if (selectDefaultTracks()){
        INFO_MSG("Track selection changed - resending headers and continuing");
        sentHeader = false;
        return;
      }
    }

    veryLiveSeek();
  }

  bool OutDTSCQuic::veryLiveSeek(){
    uint64_t seekPos = 0;
    size_t mainTrack = getMainSelectedTrack();
    // cancel if there are no keys in the main track
    if (mainTrack == INVALID_TRACK_ID){return false;}
    DTSC::Keys mainKeys(meta.getKeys(mainTrack));
    if (!mainKeys.getValidCount()){return false;}

    for (uint32_t keyNum = mainKeys.getEndValid() - 1; keyNum >= mainKeys.getFirstValid(); keyNum--){
      seekPos = mainKeys.getTime(keyNum);
      // Only skip forward if we can win a decent amount (500ms)
      if (seekPos <= thisTime + 500){break;}
      bool good = true;
      // check if all tracks have data for this point in time
      for (std::map<size_t, Comms::Users>::iterator ti = userSelect.begin(); ti != userSelect.end(); ++ti){
        if (ti->first == INVALID_TRACK_ID){
          HIGH_MSG("Skipping track %zu, not in tracks", ti->first);
          continue;
        }// ignore missing tracks
        if (meta.getNowms(ti->first) < seekPos){
          good = false;
          break;
        }
        if (mainTrack == ti->first){continue;}// skip self
        if (meta.getLastms(ti->first) == meta.getFirstms(ti->first)){
          HIGH_MSG("Skipping track %zu, last equals first", ti->first);
          continue;
        }// ignore point-tracks
        HIGH_MSG("Track %zu is good", ti->first);
      }
      // if yes, seek here
      if (good){
        HIGH_MSG("Skipping forward %" PRIu64 "ms", seekPos - thisTime);
        seek(seekPos);
        return true;
      }
    }
    return false;
  }

  void OutDTSCQuic::sendHeader(){
    if (!sentHeader && config->getString("pushurl").size()){
      pushUrl = HTTP::URL(config->getString("pushurl"));
      if (pushUrl.protocol != "dtsc"){
        WARN_MSG("Invalid push URL format, must start with dtsc:// - %s", config->getString("pushURI").c_str());
      }else{
        if (!pushUrl.path.size()){pushUrl.path = streamName;}
        INFO_MSG("About to push stream %s out. target stream: %s", streamName.c_str(), pushUrl.path.c_str());
        JSON::Value prep;
        prep["cmd"] = "push";
        prep["version"] = APPIDENT;
        prep["stream"] = pushUrl.path;
        std::map<std::string, std::string> args;
        HTTP::parseVars(pushUrl.args, args);
        if (args.count("pass")){prep["password"] = args["pass"];}
        if (args.count("pw")){prep["password"] = args["pw"];}
        if (args.count("password")){prep["password"] = args["password"];}
        if (pushUrl.pass.size()){prep["password"] = pushUrl.pass;}
        if (getSyncMode()){prep["sync"] = true;}
        sendCmd(prep);
      }
    }
    sentHeader = true;
    std::set<size_t> selectedTracks;
    for (std::map<size_t, Comms::Users>::iterator it = userSelect.begin(); it != userSelect.end(); it++){
      selectedTracks.insert(it->first);
    }
    M.send(myConn, true, selectedTracks, true);
    if (M.getLive()){realTime = 0;}
  }

  void OutDTSCQuic::onFail(const std::string &msg, bool critical){
    JSON::Value err;
    err["cmd"] = "error";
    err["msg"] = msg;
    sendCmd(err);
    Output::onFail(msg, critical);
  }

  void OutDTSCQuic::onRequest(){
    while (myConn.Received().available(8)){
      if (myConn.Received().copy(4) == "DTCM"){
        // Command message
        std::string toRec = myConn.Received().copy(8);
        unsigned long rSize = Bit::btohl(toRec.c_str() + 4);
        if (!myConn.Received().available(8 + rSize)){return;}// abort - not enough data yet
        myConn.Received().remove(8);
        std::string dataPacket = myConn.Received().remove(rSize);
        DTSC::Scan dScan((char *)dataPacket.data(), rSize);
        HIGH_MSG("Received DTCM: %s", dScan.asJSON().toString().c_str());
        if (dScan.getMember("cmd").asString() == "ok"){
          INFO_MSG("Remote OK: %s", dScan.getMember("msg").asString().c_str());
          continue;
        }
        if (dScan.getMember("cmd").asString() == "push"){
          handlePush(dScan);
          continue;
        }
        if (dScan.getMember("cmd").asString() == "play"){
          handlePlay(dScan);
          continue;
        }
        if (dScan.getMember("cmd").asString() == "ping"){
          sendOk("Pong!");
          continue;
        }
        if (dScan.getMember("cmd").asString() == "ok"){
          INFO_MSG("Ok: %s", dScan.getMember("msg").asString().c_str());
          continue;
        }
        if (dScan.getMember("cmd").asString() == "hi"){
          INFO_MSG("Connected to server running version %s", dScan.getMember("version").asString().c_str());
          continue;
        }
        if (dScan.getMember("cmd").asString() == "error"){
          ERROR_MSG("%s", dScan.getMember("msg").asString().c_str());
          continue;
        }
        if (dScan.getMember("cmd").asString() == "reset"){
          userSelect.clear();
          sendOk("Internal state reset");
          continue;
        }
        if (dScan.getMember("cmd").asString() == "check_key_duration"){
          size_t idx = dScan.getMember("id").asInt() - 1;
          size_t dur = dScan.getMember("duration").asInt();
          if (!M.trackValid(idx)){
            ERROR_MSG("Cannot check key duration %zu for track %zu: not valid", dur, idx);
            return;
          }
          uint32_t longest_key = 0;
          // Note: specifically uses `keys` instead of `getKeys` since we want _all_ data, regardless of limiting
          DTSC::Keys Mkeys(M.keys(idx));
          uint32_t firstKey = Mkeys.getFirstValid();
          uint32_t endKey = Mkeys.getEndValid();
          for (uint32_t k = firstKey; k+1 < endKey; k++){
            uint64_t kDur = Mkeys.getDuration(k);
            if (kDur > longest_key){longest_key = kDur;}
          }
          if (dur > longest_key*1.2){
            onFail("Key duration mismatch; disconnecting "+myConn.getHost()+" to recover ("+JSON::Value(longest_key).asString()+" -> "+JSON::Value((uint64_t)dur).asString()+")", true);
            return;
          }else{
            sendOk("Key duration matches upstream");
          }
          continue;
        }
        WARN_MSG("Unhandled DTCM command: '%s'", dScan.getMember("cmd").asString().c_str());
      }else if (myConn.Received().copy(4) == "DTSC"){
        // Header packet
        if (!isPushing()){
          onFail("DTSC_HEAD ignored: you are not cleared for pushing data!", true);
          return;
        }
        std::string toRec = myConn.Received().copy(8);
        unsigned long rSize = Bit::btohl(toRec.c_str() + 4);
        if (!myConn.Received().available(8 + rSize)){return;}// abort - not enough data yet
        std::string dataPacket = myConn.Received().remove(8 + rSize);
        DTSC::Packet metaPack(dataPacket.data(), dataPacket.size());
        DTSC::Scan metaScan = metaPack.getScan();
        meta.reloadReplacedPagesIfNeeded();
        size_t prevTracks = meta.getValidTracks().size();

        size_t tNum = metaScan.getMember("tracks").getSize();
        for (int i = 0; i < tNum; i++){
          DTSC::Scan trk = metaScan.getMember("tracks").getIndice(i);
          size_t trackID = trk.getMember("trackid").asInt();
          if (meta.trackIDToIndex(trackID, getpid()) == INVALID_TRACK_ID){
            MEDIUM_MSG("Adding track: %s", trk.asJSON().toString().c_str());
            meta.addTrackFrom(trk, true);
          }else{
            HIGH_MSG("Already had track: %s", trk.asJSON().toString().c_str());
          }
        }
        meta.reloadReplacedPagesIfNeeded();
        // Unix Time at zero point of a stream
        if (metaScan.hasMember("unixzero")){
          meta.setBootMsOffset(metaScan.getMember("unixzero").asInt() - Util::unixMS() + Util::bootMS());
        }else{
          MEDIUM_MSG("No member \'unixzero\' found in DTSC::Scan. Calculating locally.");
          int64_t lastMs = 0;
          std::set<size_t> tracks = M.getValidTracks();
          for (std::set<size_t>::iterator it = tracks.begin(); it != tracks.end(); it++){
            if (M.getLastms(*it) > lastMs){
              lastMs = M.getLastms(*it);
            }
          }
          meta.setBootMsOffset(Util::bootMS() - lastMs);
        }
        std::stringstream rep;
        rep << "DTSC_HEAD parsed, we went from " << prevTracks << " to " << meta.getValidTracks().size() << " tracks. Bring on those data packets!";
        sendOk(rep.str());
      }else if (myConn.Received().copy(4) == "DTP2"){
        if (!isPushing()){
          onFail("DTSC_V2 ignored: you are not cleared for pushing data!", true);
          return;
        }
        // Data packet
        std::string toRec = myConn.Received().copy(8);
        unsigned long rSize = Bit::btohl(toRec.c_str() + 4);
        if (!myConn.Received().available(8 + rSize)){return;}// abort - not enough data yet
        std::string dataPacket = myConn.Received().remove(8 + rSize);
        DTSC::Packet inPack(dataPacket.data(), dataPacket.size(), true);
        size_t tid = M.trackIDToIndex(inPack.getTrackId(), getpid());
        if (tid == INVALID_TRACK_ID){
          //WARN_MSG("Received data for unknown track: %zu", inPack.getTrackId());
          onFail("DTSC_V2 received for a track that was not announced in a header!", true);
          return;
        }
        if (!userSelect.count(tid)){
          userSelect[tid].reload(streamName, tid, COMM_STATUS_SOURCE);
        }
        char *data;
        size_t dataLen;
        inPack.getString("data", data, dataLen);
        thisTime = inPack.getTime();
        bufferLivePacket(thisTime, inPack.getInt("offset"), tid, data, dataLen, inPack.getInt("bpos"), inPack.getFlag("keyframe"));
        
        // If we're receiving packets in sync, we now know until 1ms ago all tracks are up-to-date
        if (isSyncReceiver && thisTime){
          std::map<size_t, Comms::Users>::iterator uIt;
          for (uIt = userSelect.begin(); uIt != userSelect.end(); ++uIt){
            if (uIt->first == tid){continue;}
            if (M.getNowms(uIt->first) < thisTime - 1){meta.setNowms(uIt->first, thisTime - 1);}
          }
        }
        
      }else{
        // Invalid
        onFail("Invalid packet header received. Aborting.", true);
        return;
      }
    }
  }

  void OutDTSCQuic::handlePlay(DTSC::Scan &dScan){
    streamName = dScan.getMember("stream").asString();
    Util::sanitizeName(streamName);
    Util::setStreamName(streamName);
    HTTP::URL qUrl;
    qUrl.protocol = "dtsc";
    qUrl.host = myConn.getBoundAddress();
    qUrl.port = config->getOption("port").asString();
    qUrl.path = streamName;
    reqUrl = qUrl.getUrl();
    parseData = true;
    INFO_MSG("Handled play for stream %s", streamName.c_str());
    setBlocking(false);
  }

  void OutDTSCQuic::handlePush(DTSC::Scan &dScan){
    streamName = dScan.getMember("stream").asString();
    std::string passString = dScan.getMember("password").asString();
    Util::sanitizeName(streamName);
    Util::setStreamName(streamName);
    HTTP::URL qUrl;
    qUrl.protocol = "dtscquic";
    qUrl.host = myConn.getBoundAddress();
    qUrl.port = config->getOption("port").asString();
    qUrl.path = streamName;
    qUrl.pass = passString;
    reqUrl = qUrl.getUrl();
    if (Triggers::shouldTrigger("PUSH_REWRITE")){
      std::string payload = reqUrl + "\n" + getConnectedHost() + "\n" + streamName;
      std::string newStream = streamName;
      Triggers::doTrigger("PUSH_REWRITE", payload, "", false, newStream);
      if (!newStream.size()){
        FAIL_MSG("Push from %s to URL %s rejected - PUSH_REWRITE trigger blanked the URL",
                 getConnectedHost().c_str(), reqUrl.c_str());
        Util::logExitReason(ER_TRIGGER,
            "Push from %s to URL %s rejected - PUSH_REWRITE trigger blanked the URL",
            getConnectedHost().c_str(), reqUrl.c_str());
        onFail("Push not allowed - rejected by trigger");
        return;
      }else{
        streamName = newStream;
        Util::sanitizeName(streamName);
        Util::setStreamName(streamName);
      }
    }
    if (!allowPush(passString)){
      onFail("Push not allowed - stream and/or password incorrect", true);
      return;
    }
    if (dScan.getMember("sync").asBool()){isSyncReceiver = true;}
    sendOk("You're cleared for pushing! DTSC_HEAD please?");
  }

}// namespace Mist



Socket::UDPConnection servSock(true);

void handleUSR1(int signum, siginfo_t *sigInfo, void *ignore){
  HIGH_MSG("USR1 received - triggering rolling restart");
  Util::Config::is_restarting = true;
  Util::logExitReason(ER_CLEAN_SIGNAL, "signal USR1");
  Util::Config::is_active = false;
}


// GnuTLS state/session variables
gnutls_certificate_credentials_t cred_;
gnutls_datum_t session_ticket_key_;
gnutls_anti_replay_t anti_replay_;
bool must_verify_certs = false;


int anti_replay_db_add_func(void *dbf, time_t exp_time,
                            const gnutls_datum_t *key,
                            const gnutls_datum_t *data) {
  return 0;
}

/// Creates a thread with a mistOut instance, returning a socket consisting of a bidirectional anonymous pipe that is connected to it
Socket::Connection createPipedThread(){
  int hostToThread[2], threadToHost[2];
  int ret;
  ret = pipe(hostToThread);
  if (ret){
    FAIL_MSG("Could not create host-to-thread pipe: %s", strerror(errno));
    return Socket::Connection();
  }
  ret = pipe(threadToHost);
  if (ret){
    FAIL_MSG("Could not create thread-to-host pipe: %s", strerror(errno));
    close(hostToThread[0]);
    close(hostToThread[1]);
    return Socket::Connection();
  }
  pid_t pid = fork();
  if (pid == 0){
    // Child
    close(threadToHost[0]);
    close(hostToThread[1]);
    Socket::Connection S(threadToHost[1], hostToThread[0]);
    JSON::Value capa;
    Util::Config conf;
    mistOut tmp(S, conf, capa);
    tmp.run();
    S.close();
    INFO_MSG("QUIC child handler exiting");
    exit(0);
  }else{
    // Parent
    close(hostToThread[0]);
    close(threadToHost[1]);
  }
  return Socket::Connection(hostToThread[1], threadToHost[0]);
}

// Ahead declaration of QuicConn so we can store pointers to them somewhere
class QuicConn;
/// Lookup table from connection ID to handler.
std::map<std::string, QuicConn *> findHandler;

/// Random callback for ngtcp2 lib
void cb_rand(uint8_t * dest, size_t destlen, const ngtcp2_rand_ctx* ctx){
  Util::getRandomBytes(dest, destlen);
}

/// Logging callback for ngtcp2 lib
void cb_log(void * qc, const char * msg, ...){
  char buffer[512];
  va_list argptr;
  va_start(argptr,msg);
  vsnprintf(buffer, 512, msg, argptr);
  va_end(argptr);
  HIGH_MSG("ngtcp2: %s", buffer);
}

int cb_new_cid(ngtcp2_conn * conn, ngtcp2_cid * cid, uint8_t * token, size_t cidlen, void * qc){
  for (size_t i = 0; i < cidlen; ++i){cid->data[i] = rand() % 255;}
  cid->datalen = cidlen;
  findHandler[std::string((char*)cid->data, cidlen)] = (QuicConn*)qc;
  ngtcp2_crypto_generate_stateless_reset_token(token, (uint8_t*)"koekjes", 7, cid);
  return 0;
}

int cb_del_cid(ngtcp2_conn * conn, const ngtcp2_cid * cid, void * qc){
  findHandler.erase(std::string((char*)cid->data, cid->datalen));
  return 0;
}

int cb_handshake(ngtcp2_conn * conn, void * qc);

int cb_strmopen(ngtcp2_conn * conn, int64_t strmId, void * qc);

int cb_recv_stream_data(ngtcp2_conn *conn, uint32_t flags, int64_t stream_id, uint64_t offset, const uint8_t *data, size_t datalen, void *user_data, void *qc);

int cb_ack_data(ngtcp2_conn *conn, int64_t stream_id, uint64_t offset, uint64_t datalen, void *qc, void *sqc);

ngtcp2_conn* getqConn(ngtcp2_crypto_conn_ref* r){
  return *((ngtcp2_conn **)r->user_data);
}

class QuicConn{
  private:
    ngtcp2_conn * qConn;
    bool valid;
    ngtcp2_path path;
    struct sockaddr_in6 localAddr;
    struct sockaddr_in6 remoteAddr;
    gnutls_session_t tlsSess;
    ngtcp2_crypto_conn_ref connRef;
    Util::ResizeablePointer sendBuffer;
    size_t outBytes;
    std::map<size_t, Util::ResizeablePointer> outBuffers;
    Socket::UDPConnection * uConn;
    bool server;
    int64_t streamId;
  public:
    Socket::Connection S;
    uint64_t needsTick(){
      uint64_t nextTime = ngtcp2_conn_get_expiry(qConn);
      if (nextTime == UINT64_MAX){return 5000000;}
      int64_t remain = nextTime - (Util::getMicros() * 1000);
      if (remain <= 0){return 0;}
      return remain;
    }
    void tick(bool expired = true){
      if (expired){
        int ret = ngtcp2_conn_handle_expiry(qConn, Util::getMicros() * 1000);
        if (ret == NGTCP2_ERR_IDLE_CLOSE){
          INFO_MSG("Closing Quic socket due to idle timeout");
          close();
          return;
        }
      }
      ngtcp2_ssize ssize = 1;
      bool sent = false;
      while (ssize > 0){
        sendBuffer.truncate(0);
        ssize = ngtcp2_conn_writev_stream(qConn, 0, 0, (uint8_t*)(char*)sendBuffer, sendBuffer.rsize(), 0, 0, -1, 0, 0, Util::getMicros() * 1000);
        if (ssize > 0){
          sendBuffer.append(0, ssize);
          uConn->SendNow(sendBuffer, sendBuffer.size(), (sockaddr*)&remoteAddr, sizeof(remoteAddr));
          sent = true;
        }
      }
      if (ssize < 0){
        WARN_MSG("QUIC error: %s", ngtcp2_strerror(ssize));
        return;
      }
      if (sent){
        ngtcp2_conn_update_pkt_tx_time(qConn, Util::getMicros() * 1000);
      }
    }
    QuicConn(bool _server = true){
      server = _server;
      streamId = -1;
      outBytes = 0;
      qConn = 0;
      valid = false;

      int ret;
      if ((ret = gnutls_init(&tlsSess, (server?GNUTLS_SERVER:GNUTLS_CLIENT) | GNUTLS_ENABLE_EARLY_DATA |
                                     GNUTLS_NO_AUTO_SEND_TICKET |
                                     GNUTLS_NO_END_OF_EARLY_DATA)) != 0){
        FAIL_MSG("Could not init TLS session! %s", gnutls_strerror(ret));
        return;
      }

      std::string priority = "%DISABLE_TLS13_COMPAT_MODE:NORMAL:-VERS-ALL:+VERS-TLS1.3:-CIPHER-ALL:+AES-128-GCM:+AES-256-GCM:+CHACHA20-POLY1305:+AES-128-CCM:-GROUP-ALL:+GROUP-X25519:+GROUP-SECP256R1:+GROUP-SECP384R1:+GROUP-SECP521R1";
      if ((ret = gnutls_priority_set_direct(tlsSess, priority.c_str(), 0)) != 0){
        FAIL_MSG("Could not set TLS priority! %s", gnutls_strerror(ret));
        return;
      }

      if (server){
        if ((ret = gnutls_session_ticket_enable_server(tlsSess, &session_ticket_key_)) != 0){
          FAIL_MSG("Could not set TLS priority! %s", gnutls_strerror(ret));
          return;
        }

        if (ngtcp2_crypto_gnutls_configure_server_session(tlsSess) != 0) {
          FAIL_MSG("ngtcp2_crypto_gnutls_configure_server_session failed");
          return;
        }
      }else{
        if (ngtcp2_crypto_gnutls_configure_client_session(tlsSess) != 0) {
          FAIL_MSG("ngtcp2_crypto_gnutls_configure_client_session failed");
          return;
        }
      }

      gnutls_anti_replay_enable(tlsSess, anti_replay_);
      gnutls_record_set_max_early_data_size(tlsSess, 0xffffffffu);
      connRef.get_conn = getqConn;
      connRef.user_data = (void*)&qConn;
      gnutls_session_set_ptr(tlsSess, &connRef);

      if ((ret = gnutls_credentials_set(tlsSess, GNUTLS_CRD_CERTIFICATE, cred_)) != 0){
        FAIL_MSG("gnutls_credentials_set failed! %s", gnutls_strerror(ret));
        return;
      }

      gnutls_datum_t alpn[2];
      alpn[0].data = (uint8_t *)"dtsc";
      alpn[0].size = 4;
      gnutls_alpn_set_protocols(tlsSess, alpn, 2, GNUTLS_ALPN_MANDATORY | GNUTLS_ALPN_SERVER_PRECEDENCE);

      if (must_verify_certs){
        gnutls_certificate_server_set_request(tlsSess, GNUTLS_CERT_REQUIRE);
      }
    }
    ~QuicConn(){
      close();
      if (qConn){ngtcp2_conn_del(qConn);}
      gnutls_deinit(tlsSess);

      std::set<std::string> toErase;
      for (std::map<std::string, QuicConn *>::iterator it = findHandler.begin(); it != findHandler.end(); ++it){
        if (it->second == this){
          toErase.insert(it->first);
        }
      }
      for (std::set<std::string>::iterator it = toErase.begin(); it != toErase.end(); ++it){
        findHandler.erase(*it);
      }
      INFO_MSG("Erased %zu connection IDs, %zu left", toErase.size(), findHandler.size());
    }
    void close(){
      sendBuffer.truncate(0);
      ngtcp2_ccerr err;
      err.type = NGTCP2_CCERR_TYPE_APPLICATION;
      err.error_code = 0;
      err.frame_type = 0;
      err.reason = 0;
      err.reasonlen = 0;
      ngtcp2_ssize ssize;
      ssize = ngtcp2_conn_write_connection_close(qConn, 0, 0, (uint8_t*)(char*)sendBuffer, sendBuffer.rsize(), &err, Util::getMicros() * 1000);
      if (ssize > 0){
        sendBuffer.append(0, ssize);
        uConn->SendNow(sendBuffer, sendBuffer.size(), (sockaddr*)&remoteAddr, sizeof(remoteAddr));
      }

      S.close();
      valid = false;
    }
    operator bool() const{return valid;}
    void onPkt(Socket::UDPConnection & U){
      if (!valid || !qConn){return;}

      // Fill with last-received packet sender address
      path.remote.addr = (sockaddr*)&remoteAddr;
      path.remote.addrlen = U.getRemoteAddr().size();
      memcpy(&remoteAddr, U.getRemoteAddr(), path.remote.addrlen);

      int ret = ngtcp2_conn_read_pkt(qConn, &path, 0, (uint8_t*)(char*)U.data, U.data.size(), Util::getMicros() * 1000);
      // On success, do nothing
      if (!ret){
        tick(false);
        return;
      }
      if (ret == NGTCP2_ERR_DROP_CONN){
        INFO_MSG("Connection dropped");
        close();
        return;
      }
      if (ret == NGTCP2_ERR_DRAINING){
        INFO_MSG("Connection closed by remote");
        close();
        return;
      }
      if (ret == NGTCP2_ERR_CLOSING){
        INFO_MSG("Connection closed by us");
        close();
        return;
      }
      if (ret == NGTCP2_ERR_CRYPTO){
        WARN_MSG("Crypto error: %s", gnutls_alert_get_name((gnutls_alert_description_t)ngtcp2_conn_get_tls_alert(qConn)));
        close();
        return;
      }

      WARN_MSG("Read error: %s", ngtcp2_strerror(ret));
    }
    int onData(const char* data, size_t datalen){
      if (!valid || !qConn){return -1;}
      std::string s(data, datalen);
      S.SendNow(data, datalen);
      if (streamId != -1){ngtcp2_conn_extend_max_stream_offset(qConn, streamId, datalen);}
      ngtcp2_conn_extend_max_offset(qConn, datalen);
      return 0;
    }
    void streamOpened(int64_t sId){
      streamId = sId;
      INFO_MSG("Opened stream %" PRId64, streamId);
    }
    void connected(){
      INFO_MSG("QUIC handshake completed!");
      if (!server){
        int ret = ngtcp2_conn_open_bidi_stream(qConn, &streamId, 0);
        if (!ret){
          INFO_MSG("Opened stream %" PRId64, streamId);
        }else{
          FAIL_MSG("Could not open Quic stream: %s", ngtcp2_strerror(ret));
        }
      }

    }
    void onAck(uint64_t offset){
      while (outBuffers.size()){
        std::map<size_t, Util::ResizeablePointer>::iterator i = outBuffers.begin();
        if (i->first + i->second.size() <= offset){
          outBuffers.erase(i);
        }else{
          break;
        }
      }
      HIGH_MSG("ACK'd up to %" PRIu64 ", %zu blocks still cached", offset, outBuffers.size());
    }
    void spool(){
      // Prevent spooling if no stream is open yet
      if (streamId == -1){return;}
      size_t sendAble = ngtcp2_conn_get_send_quantum(qConn);
      if (sendAble && (S.Received().size() || S.spool())){
        ngtcp2_ssize ssize = 1;
        ngtcp2_ssize actuallySent;
        bool sent = false;
        bool sentStreamData = false;
        while (ssize > 0 && !sent){
          sendBuffer.truncate(0);
          if (!sentStreamData && S.Received().bytes(sendAble)){
            while (S.Received().bytes(sendAble) < sendAble && S.spool()){}
            std::string toSend = S.Received().copy(S.Received().bytes(sendAble));
            outBuffers[outBytes].append(toSend.data(), toSend.size());
            ngtcp2_vec sendVec;
            sendVec.base = (uint8_t *)(char*)outBuffers[outBytes];
            sendVec.len = toSend.size();

            ssize = ngtcp2_conn_writev_stream(qConn, 0, 0, (uint8_t*)(char*)sendBuffer, sendBuffer.rsize(), &actuallySent, 0, streamId, &sendVec, 1, Util::getMicros() * 1000);
            HIGH_MSG("Sent %zd/%zu/%zu bytes", actuallySent, toSend.size(), sendAble);
            if (actuallySent > 0){
              outBytes += actuallySent;
              S.Received().remove(actuallySent);
              sentStreamData = true;
            }else{
              outBuffers.erase(outBytes);
            }
            sendAble -= actuallySent;
          }else{
            ssize = ngtcp2_conn_writev_stream(qConn, 0, 0, (uint8_t*)(char*)sendBuffer, sendBuffer.rsize(), 0, 0, -1, 0, 0, Util::getMicros() * 1000);
          }
          if (ssize > 0){
            sendBuffer.append(0, ssize);
            uConn->SendNow(sendBuffer, sendBuffer.size(), (sockaddr*)&remoteAddr, sizeof(remoteAddr));
            sent = true;
          }
        }
        if (ssize < 0){
          WARN_MSG("QUIC error: %s", ngtcp2_strerror(ssize));
        }
        if (sent){
          ngtcp2_conn_update_pkt_tx_time(qConn, Util::getMicros() * 1000);
        }
      }
      if (!S){
        INFO_MSG("Local DTSC connection closed -> closing QUIC connection to match");
        close();
      }
    }
    bool init(Socket::UDPConnection & U, ngtcp2_pkt_hd & hd){
      uConn = &U;
      // Outside of the scope since the pointer must remain valid for a while
      {
        // Grab local address using getsockname
        socklen_t len = sizeof(localAddr);
        if (getsockname(U.getSock(), (sockaddr *)&localAddr, &len)){
          FAIL_MSG("Failed to read local path! %s", strerror(errno));
          return false;
        }
        path.local.addr = (sockaddr *)&localAddr;
        path.local.addrlen = len;

        // Fill with last-received packet sender address
        path.remote.addr = (sockaddr*)&remoteAddr;
        path.remote.addrlen = U.getRemoteAddr().size();
        memcpy(&remoteAddr, U.getRemoteAddr(), path.remote.addrlen);

        // Convert remote socket address to human-readable string
        // both for internal reasons and for debug message printing
        std::string host;
        uint32_t port;
        char addrconv[INET6_ADDRSTRLEN];
        if (remoteAddr.sin6_family == AF_INET6){
          host = inet_ntop(AF_INET6, &(remoteAddr.sin6_addr), addrconv, INET6_ADDRSTRLEN);
          if (host.substr(0, 7) == "::ffff:"){host = host.substr(7);}
          port = ntohs(remoteAddr.sin6_port);
          INFO_MSG("Remote IPv6 addr [%s:%" PRIu32 "]", host.c_str(), port);
        }
        if (remoteAddr.sin6_family == AF_INET){
          host = inet_ntop(AF_INET, &(((sockaddr_in *)&remoteAddr)->sin_addr), addrconv, INET6_ADDRSTRLEN);
          port = ntohs(((sockaddr_in *)&remoteAddr)->sin_port);
          INFO_MSG("Remote IPv4 addr [%s:%" PRIu32 "]", host.c_str(), port);
        }
        // Set the remote host on the bidir pipe
        S.setHost(host);
      }

      // Generate a local connection ID
      ngtcp2_cid scid;
      scid.datalen = 18;
      Util::getRandomBytes(scid.data, scid.datalen);
      findHandler[std::string((char*)scid.data, scid.datalen)] = this;

      // Set callbacks
      ngtcp2_callbacks cb;
      // Init all of them to null
      memset(&cb, 0, sizeof(cb));
      // Callbacks provided by crypto helper
      cb.recv_client_initial = ngtcp2_crypto_recv_client_initial_cb;
      cb.recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb;
      cb.encrypt = ngtcp2_crypto_encrypt_cb;
      cb.decrypt = ngtcp2_crypto_decrypt_cb;
      cb.hp_mask = ngtcp2_crypto_hp_mask_cb;
      cb.update_key = ngtcp2_crypto_update_key_cb;
      cb.delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
      cb.delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
      cb.get_path_challenge_data = ngtcp2_crypto_get_path_challenge_data_cb;
      cb.version_negotiation = ngtcp2_crypto_version_negotiation_cb;
      // Callbacks we provide
      cb.rand = cb_rand;
      cb.get_new_connection_id = cb_new_cid;
      cb.remove_connection_id = cb_del_cid;
      cb.recv_stream_data = cb_recv_stream_data;
      cb.handshake_completed = cb_handshake;
      cb.stream_open = cb_strmopen;
      cb.acked_stream_data_offset = cb_ack_data;

      // Set settings
      ngtcp2_settings settings;
      ngtcp2_settings_default(&settings);
      settings.log_printf = cb_log;
      settings.initial_ts = Util::getMicros() * 1000;
      settings.max_window = 64 * 1024 * 1024;
      settings.max_stream_window = 64 * 1024 * 1024;
      sendBuffer.allocate(settings.max_tx_udp_payload_size);

      // Set transport params
      ngtcp2_transport_params params;
      ngtcp2_transport_params_default(&params);
      params.original_dcid = hd.dcid;
      params.original_dcid_present = 1;
      params.initial_max_streams_bidi = 1;
      params.initial_max_stream_data_bidi_local = 1024 * 1024;
      params.initial_max_stream_data_bidi_remote = 1024 * 1024;
      params.initial_max_data = 1024 * 1024;

      // Create connection
      int r = ngtcp2_conn_server_new(&qConn, &hd.scid, &scid, &path, hd.version, &cb, &settings, &params, 0, this);
      if (r){
        FAIL_MSG("ngtcp2_conn_server_new: %s", ngtcp2_strerror(r));
        return false;
      }
      ngtcp2_conn_set_tls_native_handle(qConn, tlsSess);
      valid = true;
      return true;
    }
    bool initClient(Socket::UDPConnection & U){
      uConn = &U;
      // Outside of the scope since the pointer must remain valid for a while
      {
        // Grab local address using getsockname
        socklen_t len = sizeof(localAddr);
        if (getsockname(U.getSock(), (sockaddr *)&localAddr, &len)){
          FAIL_MSG("Failed to read local path! %s", strerror(errno));
          return false;
        }
        path.local.addr = (sockaddr *)&localAddr;
        path.local.addrlen = len;

        // Fill with last-received packet sender address
        path.remote.addr = (sockaddr*)&remoteAddr;
        path.remote.addrlen = U.getRemoteAddr().size();
        memcpy(&remoteAddr, U.getRemoteAddr(), path.remote.addrlen);

        // Convert remote socket address to human-readable string
        // both for internal reasons and for debug message printing
        std::string host;
        uint32_t port;
        char addrconv[INET6_ADDRSTRLEN];
        if (remoteAddr.sin6_family == AF_INET6){
          host = inet_ntop(AF_INET6, &(remoteAddr.sin6_addr), addrconv, INET6_ADDRSTRLEN);
          if (host.substr(0, 7) == "::ffff:"){host = host.substr(7);}
          port = ntohs(remoteAddr.sin6_port);
          INFO_MSG("Remote IPv6 addr [%s:%" PRIu32 "]", host.c_str(), port);
        }
        if (remoteAddr.sin6_family == AF_INET){
          host = inet_ntop(AF_INET, &(((sockaddr_in *)&remoteAddr)->sin_addr), addrconv, INET6_ADDRSTRLEN);
          port = ntohs(((sockaddr_in *)&remoteAddr)->sin_port);
          INFO_MSG("Remote IPv4 addr [%s:%" PRIu32 "]", host.c_str(), port);
        }
        // Set the remote host on the bidir pipe
        S.setHost(host);
      }

      // Generate a local connection ID
      ngtcp2_cid scid;
      scid.datalen = 18;
      Util::getRandomBytes(scid.data, scid.datalen);
      ngtcp2_cid dcid;
      dcid.datalen = 18;
      Util::getRandomBytes(dcid.data, dcid.datalen);

      // Set callbacks
      ngtcp2_callbacks cb;
      // Init all of them to null
      memset(&cb, 0, sizeof(cb));
      // Callbacks provided by crypto helper
      cb.client_initial = ngtcp2_crypto_client_initial_cb;
      cb.recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb;
      cb.encrypt = ngtcp2_crypto_encrypt_cb;
      cb.decrypt = ngtcp2_crypto_decrypt_cb;
      cb.hp_mask = ngtcp2_crypto_hp_mask_cb;
      cb.update_key = ngtcp2_crypto_update_key_cb;
      cb.delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
      cb.delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
      cb.get_path_challenge_data = ngtcp2_crypto_get_path_challenge_data_cb;
      cb.version_negotiation = ngtcp2_crypto_version_negotiation_cb;
      cb.recv_retry = ngtcp2_crypto_recv_retry_cb;
      // Callbacks we provide
      cb.rand = cb_rand;
      cb.get_new_connection_id = cb_new_cid;
      cb.remove_connection_id = cb_del_cid;
      cb.recv_stream_data = cb_recv_stream_data;
      cb.handshake_completed = cb_handshake;
      cb.stream_open = cb_strmopen;
      cb.acked_stream_data_offset = cb_ack_data;

      // Set settings
      ngtcp2_settings settings;
      ngtcp2_settings_default(&settings);
      settings.log_printf = cb_log;
      settings.initial_ts = Util::getMicros() * 1000;
      settings.max_window = 64 * 1024 * 1024;
      settings.max_stream_window = 64 * 1024 * 1024;
      sendBuffer.allocate(settings.max_tx_udp_payload_size);

      // Set transport params
      ngtcp2_transport_params params;
      ngtcp2_transport_params_default(&params);
      params.initial_max_stream_data_bidi_local = 10 * 1024;
      params.initial_max_stream_data_bidi_remote = 10 * 1024;
      params.initial_max_data = 10 * 1024;

      // Create connection
      int r = ngtcp2_conn_client_new(&qConn, &dcid, &scid, &path, NGTCP2_PROTO_VER_V1, &cb, &settings, &params, 0, this);
      if (r){
        FAIL_MSG("ngtcp2_conn_client_new: %s", ngtcp2_strerror(r));
        return false;
      }
      ngtcp2_conn_set_tls_native_handle(qConn, tlsSess);
      tick(false);
      valid = true;
      return true;
    }
};

int cb_recv_stream_data(ngtcp2_conn *conn, uint32_t flags, int64_t stream_id, uint64_t offset, const uint8_t *data, size_t datalen, void *qc, void *sqc){
  ((QuicConn*)qc)->onData((const char*)data, datalen);
  if (flags & NGTCP2_STREAM_DATA_FLAG_FIN){((QuicConn*)qc)->close();}
  return 0;
}

int cb_ack_data(ngtcp2_conn *conn, int64_t stream_id, uint64_t offset, uint64_t datalen, void *qc, void *sqc){
  ((QuicConn*)qc)->onAck(offset+datalen);
  return 0;
}

int cb_handshake(ngtcp2_conn * conn, void * qc){
  ((QuicConn*)qc)->connected();
  return 0;
}

int cb_strmopen(ngtcp2_conn * conn, int64_t strmId, void * qc){
  ((QuicConn*)qc)->streamOpened(strmId);
  return 0;
}


void sleepNanos(uint64_t nextTick){
  // Use select to wait max 5s or until a packet arrives or until next tick.
  fd_set rfds;
  struct timeval T;
  T.tv_sec = nextTick / 1000000;
  T.tv_usec = nextTick % 1000000;
  // Watch configured FD's for input
  FD_ZERO(&rfds);
  int maxFD = servSock.getSock();
  FD_SET(maxFD, &rfds);
  select(maxFD + 1, &rfds, NULL, NULL, &T);
}

int main(int argc, char *argv[]){
  DTSC::trackValidMask = TRACK_VALID_EXT_HUMAN;
  Util::redirectLogsIfNeeded();
  Util::Config conf(argv[0]);
  JSON::Value capa;
  mistOut::init(&conf, capa);
  if (conf.parseArgs(argc, argv)){
    if (conf.getBool("json")){
      capa["version"] = PACKAGE_VERSION;
      std::cout << capa.toString() << std::endl;
      return -1;
    }
    {
      std::string defTrkSrt = conf.getString("default_track_sorting");
      if (!defTrkSrt.size()){
        //defTrkSrt = Util::getGlobalConfig("default_track_sorting").asString();
      }
      if (defTrkSrt.size()){
        if (defTrkSrt == "bps_lth"){Util::defaultTrackSortOrder = Util::TRKSORT_BPS_LTH;}
        if (defTrkSrt == "bps_htl"){Util::defaultTrackSortOrder = Util::TRKSORT_BPS_HTL;}
        if (defTrkSrt == "id_lth"){Util::defaultTrackSortOrder = Util::TRKSORT_ID_LTH;}
        if (defTrkSrt == "id_htl"){Util::defaultTrackSortOrder = Util::TRKSORT_ID_HTL;}
        if (defTrkSrt == "res_lth"){Util::defaultTrackSortOrder = Util::TRKSORT_RES_LTH;}
        if (defTrkSrt == "res_htl"){Util::defaultTrackSortOrder = Util::TRKSORT_RES_HTL;}
      }
    }
    conf.activate();



    if (conf.getString("target").size()){
      HTTP::URL pushUrl(conf.getString("target"));
      std::map<std::string, std::string> args;
      HTTP::parseVars(pushUrl.args, args);
      if (args.count("cert")){conf.getOption("cert", true).append(args["cert"]);}
      if (args.count("key")){conf.getOption("key", true).append(args["key"]);}
      if (args.count("ca")){conf.getOption("ca", true).append(args["ca"]);}
    }

    servSock.allocateDestination();

    // Init gnutls
    gnutls_anti_replay_init(&anti_replay_);
    gnutls_anti_replay_set_add_function(anti_replay_, anti_replay_db_add_func);
    gnutls_anti_replay_set_ptr(anti_replay_, 0);
    int ret = 0;
    if ((ret = gnutls_certificate_allocate_credentials(&cred_)) != 0){
      FAIL_MSG("gnutls_certificate_allocate_credentials failed: %s", gnutls_strerror(ret));
      goto CLEAN_GNUTLS;
    }
    if (conf.getString("ca").size()){
      must_verify_certs = true;
      std::string ca = conf.getString("ca");
      if ((ret = gnutls_certificate_set_x509_trust_file(cred_, ca.c_str(), GNUTLS_X509_FMT_PEM)) < 0){
        FAIL_MSG("gnutls_certificate_set_x509_trust_file(%s) failed: %s", ca.c_str(), gnutls_strerror(ret));
      }else{
        INFO_MSG("Loaded %d Certificate Authorities from %s", ret, ca.c_str());
      }
    }else{
      if ((ret = gnutls_certificate_set_x509_system_trust(cred_)) < 0){
        FAIL_MSG("gnutls_certificate_set_x509_system_trust failed: %s", gnutls_strerror(ret));
      }else{
        INFO_MSG("Loaded %d Certificate Authorities from system auth store", ret);
      }
    }
    if (conf.getString("cert").size()){
      if ((ret = gnutls_certificate_set_x509_key_file(cred_, conf.getString("cert").c_str(), conf.getString("key").c_str(), GNUTLS_X509_FMT_PEM)) != 0){
        FAIL_MSG("gnutls_certificate_set_x509_key_file failed: %s", gnutls_strerror(ret));
        goto CLEAN_GNUTLS;
      }
    }
    if ((ret = gnutls_session_ticket_key_generate(&session_ticket_key_)) != 0){
      FAIL_MSG("gnutls_session_ticket_key_generate failed: %s", gnutls_strerror(ret));
      goto CLEAN_GNUTLS;
    }


    if (mistOut::listenMode(&conf)){
      {
        struct sigaction new_action;
        new_action.sa_sigaction = handleUSR1;
        sigemptyset(&new_action.sa_mask);
        new_action.sa_flags = 0;
        sigaction(SIGUSR1, &new_action, NULL);
      }

      Comms::defaultCommFlags = COMM_STATUS_NOKILL;

      servSock.bind(conf.getInteger("port"), conf.getString("interface"));
      std::set<QuicConn*> handlers;

      // Read packets from UDP socket
      while (conf.is_active){
        if (servSock.Receive()){
          // Skip empty packets
          if (!servSock.data.size()){continue;}

          // Try to parse QUIC headers
          ngtcp2_version_cid vc;
          int rVer = ngtcp2_pkt_decode_version_cid(&vc, (const uint8_t*)(char*)servSock.data, servSock.data.size(), 18);
          if (rVer == NGTCP2_ERR_VERSION_NEGOTIATION){
            // Respond with version negotiation packet
            static Util::ResizeablePointer vBuf;
            vBuf.allocate(NGTCP2_MAX_UDP_PAYLOAD_SIZE);
            vBuf.truncate(0);

            uint32_t prefVers[] = {NGTCP2_PROTO_VER_V1, NGTCP2_PROTO_VER_V2};
            uint8_t ran = rand() % 255;


            int nwrite = ngtcp2_pkt_write_version_negotiation((uint8_t*)(char*)vBuf, vBuf.rsize(), ran, vc.dcid, vc.dcidlen, vc.scid, vc.scidlen, prefVers, 2);
            if (nwrite < 0) {
              WARN_MSG("ngtcp2_pkt_write_version_negotiation: %s", ngtcp2_strerror(nwrite));
              continue;
            }

            vBuf.append(0, nwrite);
            // Send reply packet.
            // Our socket lib by default replies to the last received address, so this "just works".
            INFO_MSG("Writing version negotiation packet");
            servSock.SendNow(vBuf, nwrite);

            continue;
          }
          if (rVer != 0){
            WARN_MSG("Could not decode version/CID from QUIC packet header: %s", ngtcp2_strerror(rVer));
            continue;
          }

          // We now have a valid QUIC packet! Yay!
          std::string cid = std::string((char*)vc.dcid, vc.dcidlen);
          std::string conn = Encodings::Hex::encode(cid);

          if (!findHandler.count(cid)){

            ngtcp2_pkt_hd hd;
            if (ngtcp2_accept(&hd, (uint8_t*)(char*)servSock.data, servSock.data.size()) != 0){
              continue;
            }
            INFO_MSG("New QUIC connection! Dest conn ID = %s", conn.c_str());

            QuicConn * H = new QuicConn();
            handlers.insert(H);
            findHandler[cid] = H;
            H->S = createPipedThread();
            H->S.setBlocking(false);
            H->init(servSock, hd);
          }

          QuicConn * H = findHandler[cid];
          H->onPkt(servSock);

          for (std::set<QuicConn*>::iterator it = handlers.begin(); it != handlers.end(); ++it){
            if (!**it){
              INFO_MSG("Cleaning up handler %p", *it);
              handlers.erase(*it);
              delete *it;
              break;
            }
            if (!(*it)->needsTick()){(*it)->tick();}
            (*it)->spool();
          }
        }else{
          uint64_t nextTick = 5000000;
          for (std::set<QuicConn*>::iterator it = handlers.begin(); it != handlers.end(); ++it){
            if (!**it){
              INFO_MSG("Cleaning up handler %p", *it);
              handlers.erase(*it);
              delete *it;
              break;
            }
            (*it)->spool();
            uint64_t tickTime = (*it)->needsTick();
            if (!tickTime){
              (*it)->tick();
              tickTime = (*it)->needsTick();
            }
            if (tickTime && tickTime < nextTick){nextTick = tickTime;}
          }
          sleepNanos(nextTick);
        }
      }



      if (conf.is_restarting && Socket::checkTrueSocket(0)){
        INFO_MSG("Reloading input while re-using server socket");
        execvp(argv[0], argv);
        FAIL_MSG("Error reloading: %s", strerror(errno));
      }
    }else{
      HTTP::URL pushUrl(conf.getString("target"));
      if (pushUrl.protocol != "dtscquic"){
        FAIL_MSG("Can only push protocol 'dtscquic', not '%s'", pushUrl.protocol.c_str());
        return 1;
      }
      servSock.SetDestination(pushUrl.host, pushUrl.getPort());
      servSock.bind(0);
      QuicConn Q(false);
      Q.S = createPipedThread();
      Q.S.setBlocking(false);
      Q.initClient(servSock);
      while (Q && conf.is_active){
        if (servSock.Receive()){Q.onPkt(servSock);}
        Q.spool();
        uint64_t nextTick = 1000;
        uint64_t tickTime = Q.needsTick();
        if (!tickTime){
          Q.tick();
          tickTime = Q.needsTick();
        }else{
          //Q.tick(false);
        }
        if (tickTime && tickTime < nextTick){nextTick = tickTime;}
        sleepNanos(nextTick);
      }
      Q.S.close();
      Q.close();
    }
    // Clean up gnutls
CLEAN_GNUTLS:
    gnutls_anti_replay_deinit(anti_replay_);
    gnutls_free(session_ticket_key_.data);
    gnutls_certificate_free_credentials(cred_);
  }
  INFO_MSG("Exit reason: %s", Util::exitReason);
  return 0;
}
