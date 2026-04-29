#include <stdint.h>
#include <string>
#include <sstream>
#include <cstdlib>
#include <mist/config.h>
#include <mist/defines.h>
#include <mist/downloader.h>
#include <mist/timing.h>
#include <mist/util.h>
#include <mist/auth.h>
#include <mist/foghorn.h>

Util::Config *cfg = 0;
std::string passphrase;
std::string altserver;
uint16_t myPort;
Socket::UDPConnection U; ///< Main listening (configured) UDP socket
Socket::UDPConnection openChecker; ///< UDP socket for checking open ports

std::string makePortHost(const std::string & binhost, uint16_t port){
  std::string porthost;
  if (binhost.size() == 16 && !memcmp(binhost.data(), "\000\000\000\000\000\000\000\000\000\000\377\377", 12)){
    // IPv4
    porthost.assign(binhost.data() + 10, 6);
  }else{
    // IPv6
    porthost = "XX";
    porthost += binhost;
  }
  porthost[0] = (port >> 8);
  porthost[1] = (port & 0xff);
  return porthost;
}

class binding{
public:
  // Member variables
  std::string prot;
  uint64_t lastActive4;
  uint64_t lastActive6;
  uint64_t lastConfirm4;
  uint64_t lastConfirm6;
  uint64_t lastOpenCheck;
  std::string porthost4;
  std::string porthost6;
  bool isOpen;
  bool predictablePort; ///< True if the public and private ports match
  bool consistentPort; ///< True if the public port is the same for all remote hosts
  // Functions
  binding(){
    lastActive4 = 0;
    lastActive6 = 0;
    lastConfirm4 = 0;
    lastConfirm6 = 0;
    lastOpenCheck = 0;
    predictablePort = false;
    consistentPort = false;
    isOpen = false;
  };
  ~binding(){};
  /// Returns the type of firewall this is
  uint8_t firewallType() const{
    if (isOpen){return FIRE_OPEN;}
    if (consistentPort){return FIRE_CONSISTANT;}
    if (predictablePort){return FIRE_PREDICTABLE;}
    return FIRE_UNDETERMINED;
  }
  const char * firewallName() const{
    if (isOpen){return "open";}
    if (consistentPort){return "consistent";}
    if (predictablePort){return "predictable";}
    return "undetermined";
  }
  /// Update binding, returns true if changed
  bool update(const std::string & name, const std::string & phost, const std::string & _prot, uint64_t currTime, Socket::UDPConnection & U){
    prot = _prot;
    bool ret = false;
    if (phost.size() == 6){
      ret = (phost != porthost4);
      porthost4 = phost;
      lastActive4 = currTime;
      if (lastConfirm4 + 2 <= currTime){
        std::string reply("FOGH0123456789ABCDEF\001\377", 22);
        reply[21] = firewallType();
        reply += phost;
        Foghorn::calcHash(reply, passphrase);
        U.SendNow(reply);
        lastConfirm4 = currTime;
      }
    }else if (phost.size() == 18){
      ret = (phost != porthost6);
      porthost6 = phost;
      lastActive6 = currTime;
      if (lastConfirm6 + 2 <= currTime){
        std::string reply("FOGH0123456789ABCDEF\001\377", 22);
        reply[21] = firewallType();
        reply += phost;
        Foghorn::calcHash(reply, passphrase);
        U.SendNow(reply);
        lastConfirm6 = currTime;
      }
    }
    if (ret){
      isOpen = false;
      lastOpenCheck = 0;
    }
    if (lastOpenCheck + 60 < currTime && phost.size() > 2){
      {
        // Tell the node about alternative server option
        if (altserver.size() && !consistentPort){
          std::string r("FOGH0123456789ABCDEF\010", 21);
          r += altserver;
          Foghorn::calcHash(r, passphrase);
          U.SendNow(r);
        }
        // Send message from different port to check if port is open
        std::string reply("FOGH0123456789ABCDEF\004XXOPEN_PORT", 32);
        reply[21] = (myPort >> 8);
        reply[22] = (myPort & 0xFF);
        Foghorn::calcHash(reply, passphrase);
        std::string host;
        Socket::hostBytesToStr(phost.data() + 2, phost.size()-2, host);
        uint32_t port = (phost[0] << 8) | phost[1];
        openChecker.SetDestination(host, port);
        openChecker.SendNow(reply);
      }
      lastOpenCheck = currTime;
    }
    return ret;
  }
  void setDestination6(Socket::UDPConnection & U){
    if (porthost6.size() == 18){
      std::string host;
      uint16_t port = (porthost6[0] << 8) | porthost6[1];
      Socket::hostBytesToStr(porthost6.data()+2, porthost6.size()-2, host);
      U.SetDestination(host, port);
      return;
    }
  }
  void setDestination4(Socket::UDPConnection & U){
    if (porthost4.size() == 6){
      std::string host;
      uint16_t port = (porthost4[0] << 8) | porthost4[1];
      Socket::hostBytesToStr(porthost4.data()+2, porthost4.size()-2, host);
      U.SetDestination(host, port);
      return;
    }
  }
  bool outdated(uint64_t t){
    if (lastActive4 + 10 < t){porthost4.clear();}
    if (lastActive6 + 10 < t){porthost6.clear();}
    return (lastActive4 + 10 < t && lastActive6 + 10 < t);
  }
};

class portResult{
public:
  char ports[0x10000];
  bool unset;
  portResult(){
    memset(ports, 0, 0x10000);
    unset = true;
  }
  void markPort(uint16_t portNo, bool predictable){
    ports[portNo] = predictable?1:2;
    unset = false;
  }
  std::string toString(){
    if (unset){return "all ports unknown";}
    std::stringstream ret;
    uint8_t prev = 0;
    size_t pNo = 0;
    for (size_t i = 0; i < 0x10000; ++i){

      if (i && ports[i] && ports[i] != prev){
        if (ret.str().size()){ret << ", ";}
        ret << "(" << pNo << "-" << (i-1) << ") = ";
        switch (prev){
          case 0: ret << "unknown"; break;
          case 1: ret << "predictable"; break;
          case 2: ret << "unpredictable"; break;
        }
        pNo = i;
      }
      if (ports[i]){prev = ports[i];}
    }
    return ret.str();
  }
  void fillNextRange(uint16_t & begin, uint16_t & end, uint16_t & step){
    if (unset){begin = 1; end = 0xffff; step = 65;}
    size_t maxGapLen = 0;
    size_t currGapStart = 0;
    size_t maxGapStart = 0;
    uint8_t currGapOpener = 0;
    bool inGap = false;
    for (size_t i = 0; i < 0x10000; ++i){
      if (inGap){
        if (!ports[i]){continue;}
        if (ports[i] == currGapOpener){
          inGap = false;
          currGapOpener = ports[i];
          continue;
        }
        if (i - currGapStart > maxGapLen){
          maxGapStart = currGapStart;
          maxGapLen = end - begin;
        }
      }else if (!ports[i]){
        currGapStart = i;
        inGap = true;
      }else{
        currGapOpener = ports[i];
      }
    }
    begin = maxGapStart;
    end = begin + maxGapLen;
    step = (maxGapLen / 10) + 1;
  }
};

void addListEntry(std::string & reply, const std::string & name, const binding & B, const std::string & phost, size_t & currEntry){
  if (reply.size() == 25){
    reply[21] = (currEntry << 8);
    reply[22] = (currEntry & 0xff);
  }
  size_t len = 3 + phost.size() + (name.size() > 100?100:name.size()) + (B.prot.size() > 10?10:B.prot.size());
  reply += (char)len;
  reply += (char)(name.size() > 100?100:name.size());
  reply += name.substr(0, 100);
  reply += (char)(B.prot.size() > 100?100:B.prot.size());
  reply += B.prot.substr(0, 10);
  reply += (char)B.firewallType();
  reply += phost;
}


int main(int argc, char **argv){
  Util::redirectLogsIfNeeded();
  Util::Config conf(argv[0]);
  cfg = &conf;

  JSON::Value opt;
  opt["arg"] = "integer";
  opt["short"] = "p";
  opt["long"] = "port";
  opt["help"] = "UDP port to listen on";
  opt["value"].append(7077u);
  conf.addOption("port", opt);

  opt.null();
  opt["arg"] = "string";
  opt["short"] = "P";
  opt["long"] = "passphrase";
  opt["help"] = "Passphrase to use for verification.";
  opt["value"][0u] = "";
  conf.addOption("passphrase", opt);

  opt.null();
  opt["arg"] = "string";
  opt["short"] = "i";
  opt["long"] = "interface";
  opt["help"] = "Network interface to listen on";
  opt["value"][0u] = "0.0.0.0";
  conf.addOption("interface", opt);

  opt.null();
  opt["arg"] = "string";
  opt["short"] = "b";
  opt["long"] = "altserver";
  opt["help"] = "Alternate server to use for consistency checks";
  opt["value"][0u] = "";
  conf.addOption("altserver", opt);

  opt.null();
  opt["arg"] = "string";
  opt["short"] = "r";
  opt["long"] = "range";
  opt["help"] = "Port range for randomized ports. Defaults to 1025-65535";
  opt["value"][0u] = "1025-65535";
  conf.addOption("range", opt);

  conf.parseArgs(argc, argv);
  conf.activate();
  passphrase = conf.getOption("passphrase").asStringRef();

  uint64_t rangeBegin = 1025;
  uint64_t rangeSize = 65535-1025;

  if (conf.getOption("range").asStringRef().size()){
    std::string range = conf.getOption("range").asStringRef();
    size_t dash = range.find('-');
    if (dash != std::string::npos){
      uint64_t numA = JSON::Value(range.substr(0, dash)).asInt();
      uint64_t numB = JSON::Value(range.substr(dash+1)).asInt();
      if (numB > 0xFFFF){numB = 0xFFFF;}
      if (numA && numB && numA <= numB){
        INFO_MSG("Setting range for randomized ports to %" PRIu64 " - %" PRIu64, numA, numB);
        rangeBegin = numA;
        rangeSize = numB-numA;
      }
    }
  }
  
  if (conf.getOption("altserver").asStringRef().size()){
    altserver = conf.getOption("altserver").asStringRef();
  }

  /// \TODO Complete basic uPnP IGD implementation...?
  if (false){
    Socket::UDPConnection upnp;
    upnp.bind(0);
    upnp.SetDestination("239.255.255.250", 1900);
    upnp.SendNow("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n\r\n");
    std::set<std::string> gateways;
    while (conf.is_active){
      uint64_t sleepTime = 3000000; // five second sleeps

      fd_set rfds;
      FD_ZERO(&rfds);
      FD_SET(upnp.getSock(), &rfds);

      struct timeval T;
      T.tv_sec = sleepTime / 1000000;
      T.tv_usec = sleepTime % 1000000;
      int r = select(upnp.getSock()+1, &rfds, NULL, NULL, &T);
      if (!r){break;}
      while (upnp.Receive()){
        if (upnp.data.size()){
          HTTP::Parser P;
          std::string str(upnp.data, upnp.data.size());
          if (P.Read(str)){
            gateways.insert(P.GetHeader("location"));
          }
        }
      }
    }
    for (std::set<std::string>::iterator it = gateways.begin(); it != gateways.end(); ++it){
      INFO_MSG("Found gateway: %s", it->c_str());
    }
    return 0;
  }

  myPort = U.bind(conf.getInteger("port"), conf.getString("interface"));
  if (!U){
    FAIL_MSG("Failed to bind to %s:%" PRId64, conf.getString("interface").c_str(), conf.getInteger("port"));
    return 1;
  }
  U.allocateDestination();

  int uSck = U.getSock();
  int maxSck = uSck;
  ++maxSck;
  std::map<std::string, binding> bindings;
  std::map<std::string, portResult> scanResults;
  uint64_t lastFoggy = 0;

  do{
    uint64_t sleepTime = 5000000; // five second sleeps

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(uSck, &rfds);

    struct timeval T;
    T.tv_sec = sleepTime / 1000000;
    T.tv_usec = sleepTime % 1000000;
    int r = select(maxSck, &rfds, NULL, NULL, &T);
    uint64_t currTime = Util::bootSecs();
    if (r > 0){
      while (U.Receive()){
        if (U.data.size() < 21 || memcmp(U.data, "FOGH", 4) || !Foghorn::checkHash(U.data, passphrase)){
          if (U.data.size() >= 21 && !memcmp(U.data, "FOGH", 4)){
            if (lastFoggy + 60 < currTime){
              INFO_MSG("🌫 Arr, the Mists, they be hard to read...");
              lastFoggy = currTime;
            }
          }
          continue;
        }
        if (U.data[20] == 0){ // Announce public port mapping
          if (U.data.size() < 22){continue;}
          size_t protLen = U.data[21];
          if (U.data.size() < 22 + protLen + 2){continue;}
          std::string prot(U.data + 22, protLen);
          size_t nameLen = (U.data[22+protLen] << 8) + U.data[22+protLen+1];
          if (U.data.size() < 22 + protLen + 2 + nameLen){continue;}
          std::string name(U.data + 22 + protLen + 2, nameLen);
          uint32_t port = U.getRemoteAddr().port();
          std::string ph = makePortHost(U.getRemoteAddr().binForm(), port);

          // No name? Reply always, do no port checks
          if (!name.size()){
            std::string reply("FOGH0123456789ABCDEF\001\377", 22);
            reply += ph;
            Foghorn::calcHash(reply, passphrase);
            U.SendNow(reply);
            continue;
          }

          binding & B = bindings[name];
          bool updated = false;
          if (B.update(name, ph, prot, currTime, U)){
            updated = true;
          }
          // Do we know the internal port?
          if (U.data.size() >= 22 + protLen + 2 + nameLen + 2){
            uint16_t intPort = ((U.data[22+protLen+2+nameLen] << 8) | U.data[22+protLen+2+nameLen+1]);
            B.predictablePort = (intPort == port);
          }
          if (U.data.size() >= 22 + protLen + 2 + nameLen + 3){
            B.isOpen = (U.data[22+protLen+2+nameLen+2] & 0x01);
            B.consistentPort = (U.data[22+protLen+2+nameLen+2] & 0x02);
          }
          if (updated){
            INFO_MSG("📣 Ahoy mate! Spotted %s %s over %s on %s", B.firewallName(), name.c_str(), prot.c_str(), U.getRemoteAddr().toString().c_str());
          }
        }else if (U.data[20] == 2){ // Hole punching request
          if (U.data.size() < 22){continue;}
          size_t protLen = U.data[21];
          if (U.data.size() < 22 + protLen + 2){continue;}
          std::string prot(U.data + 22, protLen);
          size_t nameLen = (U.data[22+protLen] << 8) + U.data[22+protLen+1];
          if (U.data.size() < 22 + protLen + 2 + nameLen){continue;}

          std::string name(U.data + 22 + protLen + 2, nameLen);
          std::string addi;
          bool reqIsPredictable = false;
          uint8_t fireType = 0;
          uint8_t bits = 0;
          if (U.data.size() >= 22+protLen+2+nameLen+2){
            size_t addiLen = (U.data[22+protLen+2+nameLen] << 8) + U.data[22+protLen+2+nameLen+1];
            if (U.data.size() >= 22 + protLen + 2 + nameLen + 2 + addiLen){
              addi.assign(U.data + 22 + protLen + 2 + nameLen + 2, addiLen);
            }
            if (U.data.size() >= 22+protLen+2+nameLen+2+addiLen+2){
              if (U.getRemoteAddr().port() == ((U.data[22+protLen+2+nameLen+2+addiLen] << 8) | U.data[22+protLen+2+nameLen+2+addiLen+1])){
                reqIsPredictable = true;
              }
            }
            if (U.data.size() >= 22+protLen+2+nameLen+3){
              bits = U.data[22+protLen+2+nameLen+2+addiLen+2];
              bits |= 0x80; // set MSB to indicate we have received bits at all
            }
          }
          if (!bindings.count(name)){
            if (addi.size()){
              INFO_MSG("🌊 Castaway! %s will not find %s (%s)!", U.getRemoteAddr().toString().c_str(), name.c_str(), addi.c_str());
            }else{
              INFO_MSG("🌊 Castaway! %s will not find %s!", U.getRemoteAddr().toString().c_str(), name.c_str());
            }
            continue;
          }
          binding & B = bindings[name];

          // If we have no bitflags, we cannot determine anything beyond predictable/undetermined
          if (!(bits & 0x80)){
            if (reqIsPredictable){fireType = FIRE_PREDICTABLE;}else{fireType = FIRE_UNDETERMINED;}
          }else{
            // We have bitflags; check them
            if (bits & 1){
              // Open port is the best case; no need to check anything else
              fireType = FIRE_OPEN;
            }else if (bits & 2){
              // Consistent is good enough; we can connect!
              fireType = FIRE_CONSISTANT;
            }else{
              fireType = FIRE_UNDETERMINED;
              if (reqIsPredictable && (bits & 4)){
                // TIME_HAS_PASSED is set, take predictable as the best result we have :-(
                fireType = FIRE_PREDICTABLE;
              }
            }
          }
          const char * hostType;
          switch (fireType){
            case FIRE_OPEN: hostType = "open"; break;
            case FIRE_CONSISTANT: hostType = "consistant"; break;
            case FIRE_PREDICTABLE: hostType = "predictable"; break;
            default: hostType = "undetermined";
          }
          // Okay, fireType contains our current and/or final answer now. Let's act on it!
          
          // First, check if we can connect at all to begin with
          std::string ph = makePortHost(U.getRemoteAddr().binForm(), U.getRemoteAddr().port());
          if (ph.size() > 6 && !B.porthost6.size()){
            if (addi.size()){
              INFO_MSG("🌊 Castaway! %s %s cannot ride the IPv6 waves toward %s (%s)!", hostType, U.getRemoteAddr().toString().c_str(), name.c_str(), addi.c_str());
            }else{
              INFO_MSG("🌊 Castaway! %s %s cannot ride the IPv6 waves toward %s!", hostType, U.getRemoteAddr().toString().c_str(), name.c_str());
            }
            continue;
          }else if (ph.size() == 6 && !B.porthost4.size()){
            if (addi.size()){
              INFO_MSG("🌊 Castaway! %s %s cannot ride the IPv4 waves toward %s (%s)!", hostType, U.getRemoteAddr().toString().c_str(), name.c_str(), addi.c_str());
            }else{
              INFO_MSG("🌊 Castaway! %s %s cannot ride the IPv4 waves toward %s!", hostType, U.getRemoteAddr().toString().c_str(), name.c_str());
            }
            continue;
          }

          if (fireType == FIRE_UNDETERMINED && !B.isOpen){
            {
              // Send message from different port to check if port is open
              std::string reply("FOGH0123456789ABCDEF\004XXOPEN_PORT", 32);
              reply[21] = (myPort >> 8);
              reply[22] = (myPort & 0xFF);
              Foghorn::calcHash(reply, passphrase);
              std::string host;
              Socket::hostBytesToStr(ph.data() + 2, ph.size()-2, host);
              uint32_t port = (ph[0] << 8) | ph[1];
              openChecker.SetDestination(host, port);
              openChecker.SendNow(reply);
            }
            // Tell the node about alternative server option
            if (altserver.size()){
              std::string r("FOGH0123456789ABCDEF\010", 21);
              r += altserver;
              Foghorn::calcHash(r, passphrase);
              U.SendNow(r);
            }
            { // Send mapping response, to tell the client what we found
              std::string reply("FOGH0123456789ABCDEF\001", 21);
              reply += (char)((fireType==FIRE_UNDETERMINED)?FIRE_IMPENETRABLE:fireType);
              reply += ph;
              Foghorn::calcHash(reply, passphrase);
              U.SendNow(reply);
            }
            continue;
          }

          uint32_t port = U.getRemoteAddr().port();
          if (fireType == FIRE_PREDICTABLE){
            uint64_t num = 0;
            Util::getRandomBytes(&num, sizeof(uint64_t));
            port = (num % rangeSize) + rangeBegin;
          }
          std::string usePortHost = (ph.size() > 6)?B.porthost6:B.porthost4;
          if (B.firewallType() == FIRE_PREDICTABLE){
            uint64_t num = 0;
            Util::getRandomBytes(&num, sizeof(uint64_t));
            uint16_t port = (num % rangeSize) + rangeBegin;
            usePortHost[0] = (port >> 8);
            usePortHost[1] = (port & 0xFF);
          }
          if (addi.size()){
            INFO_MSG("🛟 Castaway! %s %s is swimming toward %s %s (%s)!", hostType, U.getRemoteAddr().toString().c_str(), B.firewallName(), name.c_str(), addi.c_str());
          }else{
            INFO_MSG("🛟 Castaway! %s %s is swimming toward %s %s!", hostType, U.getRemoteAddr().toString().c_str(), B.firewallName(), name.c_str());
          }
          { // Send punch instruction to client side
            std::string reply("FOGH0123456789ABCDEF\003\000\000", 23);
            if (addi.size()){
              reply[21] = (addi.size() << 8);
              reply[22] = (addi.size() & 0xff);
              reply += addi;
            }
            reply += usePortHost;
            // Add random port number, since we might need it
            if (fireType == FIRE_PREDICTABLE || B.isOpen){
              if (B.isOpen){port = 0xFFFF;}
              reply += (char)(port >> 8);
              reply += (char)(port & 0xFF);
            }
            Foghorn::calcHash(reply, passphrase);
            U.SendNow(reply);
          }
          if (ph.size() > 6){
            B.setDestination6(U);
          }else{
            B.setDestination4(U);
          }
          if (!B.isOpen){ // Send punch instruction to server side, if not open port
            std::string reply("FOGH0123456789ABCDEF\003\000\000", 23);
            if (addi.size()){
              reply[21] = (addi.size() << 8);
              reply[22] = (addi.size() & 0xff);
              reply += addi;
            }
            reply += ph;
            // Add random port number, since we might need it
            if (B.firewallType() == FIRE_PREDICTABLE){
              reply.append(usePortHost.data(), 2);
            }
            Foghorn::calcHash(reply, passphrase);
            U.SendNow(reply);
          }
        }else if (U.data[20] == 4){ // Ping request
          if (U.data.size() < 23){continue;}
          uint16_t replyPort = (U.data[21] << 8) + U.data[22];
          if (replyPort){
            std::string host = U.getRemoteAddr().host();
            U.SetDestination(host, replyPort);
          }
          std::string reply("FOGH0123456789ABCDEF\005", 21);
          reply.append(U.data + 23, U.data.size() - 23);
          Foghorn::calcHash(reply, passphrase);
          U.SendNow(reply);
        }else if (U.data[20] == 7){ // Port scan probe
          if (U.data.size() < 23){continue;}
          size_t pubPort = (U.data[21] << 8) + U.data[22];
          std::string ph = makePortHost(U.getRemoteAddr().binForm(), U.getRemoteAddr().port());
          scanResults[ph.substr(2)].markPort(pubPort, pubPort == U.getRemoteAddr().port());
        }else if (U.data[20] == 0xFF){ // Get endpoint list
          if (U.data.size() < 23){continue;}
          size_t startEntry = (U.data[21] << 8) + U.data[22];
          size_t endEntry = startEntry;
          if (U.data.size() > 24){endEntry = (U.data[23] << 8) + U.data[24];}
          // Now startEntry/endEntry contain the list size we want
          size_t currEntry = 0;
          size_t totalEntry = 0;
          for (std::map<std::string, binding>::iterator it = bindings.begin(); it != bindings.end(); ++it){
            if (it->second.porthost4.size()){++totalEntry;}
            if (it->second.porthost6.size()){++totalEntry;}
          }
          std::string reply("FOGH0123456789ABCDEF\376\000\000\000\000", 25);
          reply[23] = (totalEntry << 8);
          reply[24] = (totalEntry & 0xff);
          for (std::map<std::string, binding>::iterator it = bindings.begin(); it != bindings.end(); ++it){
            if (it->second.porthost4.size()){
              if (currEntry >= startEntry && (reply.size() > 25 || currEntry <= endEntry)){
                addListEntry(reply, it->first, it->second, it->second.porthost4, currEntry);
              }
              ++currEntry;
            }
            if (it->second.porthost6.size()){
              if (currEntry >= startEntry && (reply.size() > 25 || currEntry <= endEntry)){
                addListEntry(reply, it->first, it->second, it->second.porthost6, currEntry);
              }
              ++currEntry;
            }
            if (reply.size() > 1100){
              Foghorn::calcHash(reply, passphrase);
              U.SendNow(reply);
              // Create a new one
              reply.assign("FOGH0123456789ABCDEF\376\000\000\000\000", 25);
              reply[23] = (totalEntry << 8);
              reply[24] = (totalEntry & 0xff);
            }
          }
          if (reply.size() > 25 || !totalEntry){
            Foghorn::calcHash(reply, passphrase);
            U.SendNow(reply);
          }
        }
      }
    }
    {
      std::set<std::string> toDelete;
      for (std::map<std::string, binding>::iterator it = bindings.begin(); it != bindings.end(); ++it){
        if (it->second.outdated(currTime)){
          toDelete.insert(it->first);
        }
      }
      for (std::set<std::string>::iterator it = toDelete.begin(); it != toDelete.end(); ++it){
        bindings.erase(*it);
        INFO_MSG("💀 Yarr.... That be the last we ever saw of %s", it->c_str());
      }
    }
  }while(U && conf.is_active);

  return 0;
}

