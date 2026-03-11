#include <string.h>
#include <thread>
#include <set>
#include "foghorn.h"
#include "auth.h"
#include "json.h"
#include "timing.h"
#include "http_parser.h"

namespace Foghorn {

  /// Calculates a foghorn hash for a given packet.
  /// (Over)writes checksum into the pData argument, but only if it is at least 20 bytes long already.
  void calcHash(std::string & pData, const std::string & passphrase){
    if (pData.size() <= 20){return;}
    std::string dataWithPass(pData.data() + 20, pData.size() - 20);
    dataWithPass += passphrase;
    char hash[32];
    Secure::sha256bin(dataWithPass.data(), dataWithPass.size(), hash);
    memcpy((char*)pData.data() + 4, hash, 16);
  }

  /// Checks a foghorn hash for a given packet
  bool checkHash(const Util::ResizeablePointer & D, const std::string & passphrase){
    if (D.size() <= 20 || D[0] != 'F' || D[1] != 'O' || D[2] != 'G' || D[3] != 'H'){return false;}
    std::string dataWithPass(D + 20, D.size() - 20);
    dataWithPass += passphrase;
    char hash[32];
    Secure::sha256bin(dataWithPass.data(), dataWithPass.size(), hash);
    if (memcmp(hash, D + 4, 16)){return false;}
    return true;
  }

  bool isFHData(const Util::ResizeablePointer & D){
    return D.size() >= 21 && D[0] == 'F' && D[1] == 'O' && D[2] == 'G' && D[3] == 'H';
  }

  Instance::Instance(){
    lastHost4 = 0;
    lastHost6 = 0;
  }

  void Instance::init(const HTTP::URL & u){
    url = u;
    lastHost4 = 0;
    lastHost6 = 0;
    addrs = Socket::getAddrs(url.host, url.getPort());
  }

  bool Instance::hasAddr(const Socket::Address & a) const{
    if (!addrs.size()){return false;}
    for (const auto & addr : addrs){
      if (Socket::compareAddress(a, addr)){return true;}
    }
    return false;
  }

  List::List(){
    lastOpenNotice = 0;
    lastPublish = 0;
    portBytes[0] = portBytes[1] = 0;
    protocolString.assign("\000", 1);
  }

  size_t List::size() const{
    return instances.size();
  }

  void List::setPort(uint16_t port){
    portBytes[0] = (port >> 8);
    portBytes[1] = (port & 0x0ff);
  }

  void List::setProtocol(const std::string & p){
    protocolString.assign("\000", 1);
    protocolString[0] = p.size();
    protocolString += p;
  }

  void List::add(const std::string & u){
    HTTP::URL url(u);
    url.protocol = "fh";
    if (!url.getPort()){url.port = "7077";}
    std::string nUrl = url.getUrl();
    if (instances.count(nUrl)){return;}
    instances[nUrl].init(url);
  }

  void List::publish(Socket::UDPConnection & uSock, const uint64_t currTime){
    // Prevent running more than once per second
    if (lastPublish + 1 >= currTime){return;}
    lastPublish = currTime;

    // Check if we have a consistent port
    bool seenDouble4 = false, seenDouble6 = false; // Have we seen a port mentioned more than once?
    std::set<uint16_t> ports4; // IPv4 ports seen
    std::set<uint16_t> ports6; // IPv6 ports seen
    char bits = 0;
    // Set bit 1 if we received an OPEN_PORT message in the last 3 minutes
    if (lastOpenNotice + 180 >= currTime){bits |= 1;}

    std::map<std::string, Instance>::iterator it;
    for (it = instances.begin(); it != instances.end(); ++it){
      if (it->second.porthost4.size() && it->second.lastHost4 + 10 < currTime){
        it->second.porthost4.clear();
        INFO_MSG("IPv4 no longer published on %s:%" PRIu16 "!", it->second.url.host.c_str(), it->second.url.getPort());
      }
      if (it->second.porthost6.size() && it->second.lastHost6 + 10 < currTime){
        it->second.porthost6.clear();
        INFO_MSG("IPv6 no longer published on %s:%" PRIu16 "!", it->second.url.host.c_str(), it->second.url.getPort());
      }
      if (it->second.porthost4.size() >= 2){
        uint16_t p = (it->second.porthost4[0] << 8) | it->second.porthost4[1];
        if (p){
          if (ports4.count(p)){
            seenDouble4 = true;
          }else{
            ports4.insert(p);
          }
        }
      }
      if (it->second.porthost6.size() >= 2){
        uint16_t p = (it->second.porthost6[0] << 8) | it->second.porthost6[1];
        if (p){
          if (ports6.count(p)){
            seenDouble6 = true;
          }else{
            ports6.insert(p);
          }
        }
      }
    }
    if (seenDouble4 && ports4.size() == 1){bits |= 2;}
    if (seenDouble6 && ports6.size() == 1){bits |= 2;}

    uSock.setIgnoreSendErrors(true);
    for (it = instances.begin(); it != instances.end(); ++it){
      const HTTP::URL & U = it->second.url;
      std::string fogData("FOGH0123456789ABCDEF\000", 21);
      fogData += protocolString;
      fogData += (char)(U.path.size() >> 8);
      fogData += (char)(U.path.size() & 0xFF);
      fogData += U.path;
      fogData.append(portBytes, 2);
      fogData += bits;
      Foghorn::calcHash(fogData, U.user);
      for (const auto & jt : it->second.addrs) {
        if (uSock.setDestination(jt)) { uSock.SendNow(fogData); }
      }
    }
    uSock.setIgnoreSendErrors(false);


    std::set<std::string> toWipe;
    for (std::map<std::string, uint64_t>::iterator it = recentMap.begin(); it != recentMap.end(); ++it){
      if (it->second + 10 < currTime){toWipe.insert(it->first);}
    }
    for (std::set<std::string>::iterator it = toWipe.begin(); it != toWipe.end(); ++it){recentMap.erase(*it);}
  }

  static const char * stateToString(const uint8_t S){
    switch (S){
      case 0: return "open";
      case 1: return "consistent";
      case 2: return "predictable";
      case 0xFE: return "impenetrable";
      default: return "undetermined";
    }
  }

  /// Parses a Foghorn packet, if possible. Returns true if a punching request was received.
  bool List::parsePacket(Socket::UDPConnection & udpSrv, const uint64_t currTime){
    std::map<std::string, Instance>::iterator it;
    for (it = instances.begin(); it != instances.end(); ++it){
      // Ignore if the address doesn't match
      if (!it->second.hasAddr(udpSrv.getRemoteAddr())){continue;}
      // Ignore if the checksum doesn't match
      if (!Foghorn::checkHash(udpSrv.data, it->second.url.user)){continue;}
      // Valid foghorn packet
      if (udpSrv.data[20] == 1){ // Mapping response
        if (udpSrv.data.size() == 28){
          if (std::string(udpSrv.data+22, udpSrv.data.size()-22) != it->second.porthost4){
            it->second.porthost4.assign(udpSrv.data+22, udpSrv.data.size()-22);
            std::string host;
            uint16_t port = (udpSrv.data[22] << 8) | udpSrv.data[23];
            it->second.state4 = udpSrv.data[21];
            Socket::hostBytesToStr(udpSrv.data + 24, udpSrv.data.size()-24,host);
            if (!it->second.url.path.size()){
              INFO_MSG("Verified IPv4 with %s:%" PRIu16 " at %s:%" PRIu16,
                       it->second.url.host.c_str(), it->second.url.getPort(), 
                       host.c_str(), port);
            }else{
              INFO_MSG("Published IPv4 to %s:%" PRIu16 " as %s at %s:%" PRIu16 " (%s)",
                       it->second.url.host.c_str(), it->second.url.getPort(), 
                       it->second.url.path.c_str(), host.c_str(), port, stateToString(it->second.state4));
            }
          }
          if (it->second.state4 != udpSrv.data[21]){
            INFO_MSG("IPv4 at %s:%" PRIu16 " changed from %s to %s",
                     it->second.url.host.c_str(), it->second.url.getPort(),
                     stateToString(it->second.state4),
                     stateToString(udpSrv.data[21]));
            it->second.state4 = udpSrv.data[21];
          }
          it->second.lastHost4 = currTime;
        }
        if (udpSrv.data.size() == 40){
          if (std::string(udpSrv.data+22, udpSrv.data.size()-22) != it->second.porthost6){
            it->second.porthost6.assign(udpSrv.data+22, udpSrv.data.size()-22);
            std::string host;
            uint16_t port = (udpSrv.data[22] << 8) | udpSrv.data[23];
            it->second.state6 = udpSrv.data[21];
            Socket::hostBytesToStr(udpSrv.data + 24, udpSrv.data.size()-24,host);
            if (!it->second.url.path.size()){
              INFO_MSG("Verified IPv6 with %s:%" PRIu16 " at %s:%" PRIu16,
                       it->second.url.host.c_str(), it->second.url.getPort(), 
                       host.c_str(), port);
            }else{
              INFO_MSG("Published IPv6 to %s:%" PRIu16 " as %s at %s:%" PRIu16 " (%s)",
                       it->second.url.host.c_str(), it->second.url.getPort(), 
                       it->second.url.path.c_str(), host.c_str(), port, stateToString(it->second.state6));
            }
          }
          if (it->second.state6 != udpSrv.data[21]){
            INFO_MSG("IPv6 at %s:%" PRIu16 " changed from %s to %s",
                     it->second.url.host.c_str(), it->second.url.getPort(),
                     stateToString(it->second.state6),
                     stateToString(udpSrv.data[21]));
            it->second.state6 = udpSrv.data[21];
          }
          it->second.lastHost6 = currTime;
        }
        return false;
      }
      if (udpSrv.data[20] == 3){ // Request to connect to user
        std::string localIP = udpSrv.getLocalAddr().host();
        uint32_t localPort = udpSrv.getLocalAddr().port();

        std::string host;
        uint16_t port, addiLen;
        addiLen = (udpSrv.data[21] << 8) | udpSrv.data[22];
        std::string addi(udpSrv.data+23, addiLen);
        port = (udpSrv.data[23+addiLen] << 8) | udpSrv.data[24+addiLen];
        size_t remLen = udpSrv.data.size()-25-addiLen;
        if (remLen == 6 || remLen == 18){
          remLen -= 2;
          localPort = (udpSrv.data[25+addiLen+remLen] << 8) | udpSrv.data[25+addiLen+remLen+1];
        }
        Socket::hostBytesToStr(udpSrv.data + 25 + addiLen, remLen, host);

        std::string connIdent = localIP + ":" + JSON::Value(localPort).asString() + "/" + host + ":" + JSON::Value(port).asString();
        if (recentMap.count(connIdent)){return false;}
        recentMap[connIdent] = currTime;
        punchReq.host = host;
        punchReq.port = port;
        punchReq.additionalData = addi;
        punchReq.localHost = localIP;
        punchReq.localPort = localPort;
        return true;
      }
      if (udpSrv.data[20] == 4 && udpSrv.data.size() >= 23){ // Ping
        std::string dHost = udpSrv.getRemoteAddr().host();
        uint32_t dPort = udpSrv.getRemoteAddr().port();
        dPort = (udpSrv.data[21] << 8) | udpSrv.data[22];
        udpSrv.SetDestination(dHost, dPort);
        std::string fogData("FOGH0123456789ABCDEF\005", 21);
        fogData.append(udpSrv.data+23, udpSrv.data.size()-23);
        if (std::string(udpSrv.data+23, udpSrv.data.size()-23) == "OPEN_PORT"){
          lastOpenNotice = currTime;
        }
        Foghorn::calcHash(fogData, it->second.url.user);
        udpSrv.SendNow(fogData);
        return false;
      }
      if (udpSrv.data[20] == 8 && udpSrv.data.size() >= 21){ // Consistency checking address
        add(std::string(udpSrv.data + 21, udpSrv.data.size() - 21));
        return false;
      }
      // Not a packet type we care about, ignore
      return false;
    }
    // No match with any endpoint, ignore
    return false;
  }

  /// Returns the data of the last punching request received
  const PunchRequest & List::getPunchData() const{return punchReq;}

  bool List::insertRecent(const std::string & localIP, const uint16_t localPort,
                          const std::string & remoteIP, const uint16_t remotePort,
                          const uint64_t currTime){
    std::string connIdent = localIP + ":" + JSON::Value(localPort).asString() + "/" + remoteIP + ":" + JSON::Value(remotePort).asString();
    if (recentMap.count(connIdent)){return false;}
    recentMap[connIdent] = currTime;
    return true;
  }

  Puncher::~Puncher(){
    if (pData && !hasThread){
      delete pData;
      pData = 0;
    }
    if (pData){pData->connected = true;}
  }

  Puncher::Puncher(const HTTP::URL & target, const std::string & protocol, const std::string & additionalData){
    // Init variables
    hasThread = false;
    lPort = 0;
    pData = 0;

    // Copy target, abort if no target host, set port if missing
    trgt = target;
    if (!trgt.host.size()){return;}
    if (!trgt.getPort()){trgt.port = "7077";}

    // Allocate data
    pData = new PunchData();
    pData->sock = new Socket::UDPConnection();
    lPort = 0;
    if (trgt.args.size()){
      std::map<std::string, std::string> args;
      HTTP::parseVars(trgt.args, args);
      if (args.count("lport")){
        lPort = JSON::Value(args["lport"]).asInt();
      }
    }
    lPort = pData->sock->bind(lPort);

    // Prepare punch request string
    pData->sock->setIgnoreSendErrors(true);
    pData->sock->allocateDestination();
    pData->addrs = Socket::getAddrs(trgt.host, trgt.getPort());
    pData->reqStr = "FOGH0123456789ABCDEF\002";
    pData->reqStr += (char)protocol.size();
    pData->reqStr += protocol;
    pData->reqStr += (char)(trgt.path.size() << 8);
    pData->reqStr += (char)(trgt.path.size() & 0xff);
    pData->reqStr += trgt.path;
    pData->reqStr += (char)(additionalData.size() << 8);
    pData->reqStr += (char)(additionalData.size() & 0xFF);
    pData->reqStr += additionalData;
    pData->reqStr += (char)(lPort >> 8);
    pData->reqStr += (char)(lPort & 0xFF);
    pData->reqStr += (char)0;
    Foghorn::calcHash(pData->reqStr, trgt.user);
  }

  bool Puncher::isOpen(){
    return lPort == 0xFFFF;
  }

  Socket::UDPConnection * Puncher::getSocket(){
    if (!pData){return 0;}
    return pData->sock;
  }

  bool Puncher::start(){
    size_t attempt = 0;
    while (attempt < 32){

      // For second and further attempts, try "random" ports
      if (attempt){
        if (attempt == 1){lPort = 7750;}
        if (attempt == 2){lPort = 0;}
        uint16_t ran;
        Util::getRandomBytes(&ran, 2);
        lPort += (500+(ran % 500));

        // Re-create the socket
        pData->sock->close();
        delete pData->sock;
        pData->sock = new Socket::UDPConnection();
        if (!pData->sock->bind(lPort)){
          ++attempt;
          continue;
        }

        // Change data and recalculate hash
        pData->reqStr[pData->reqStr.size() - 3] = (char)(lPort >> 8);
        pData->reqStr[pData->reqStr.size() - 2] = (char)(lPort & 0xFF);
        pData->reqStr[pData->reqStr.size() - 1] = (char)0;
        Foghorn::calcHash(pData->reqStr, trgt.user);

      }

      // Prepare for select calls for efficient socket reading
      int maxFD = pData->sock->getSock();
      uint64_t startTime = Util::bootSecs();
      uint64_t currTime = 0;
      std::map<std::string, uint16_t> portMap4, portMap6;
      bool consistent4 = false, consistent6 = false;
      while (startTime + 6 > (currTime = Util::bootSecs())){
        if (currTime > startTime + 2){
          if (!(pData->reqStr[pData->reqStr.size() - 1] & 4)){
            INFO_MSG("Marking time-has-passed flag");
            // Set time-has-passed flag
            pData->reqStr[pData->reqStr.size() - 1] |= 4;
            Foghorn::calcHash(pData->reqStr, trgt.user);
          }
        }
        pData->sock->setIgnoreSendErrors(true);
        for (const auto & jt : pData->addrs) {
          if (pData->sock->setDestination(jt)) { pData->sock->SendNow(pData->reqStr); }
        }
        pData->sock->setIgnoreSendErrors(false);
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(maxFD, &rfds);

        struct timeval T;
        T.tv_sec = 0;
        T.tv_usec = 100000;
        int r = select(maxFD + 1, &rfds, NULL, NULL, &T);
        if (r){
          while (pData->sock->Receive()){
            if (Foghorn::checkHash(pData->sock->data, trgt.user)){
              // Hole punching instruction
              if (pData->sock->data[20] == 3 && pData->sock->data.size() >= 23){
                // First do some basic error checking
                uint16_t addiLen;
                addiLen = (pData->sock->data[21] << 8) | pData->sock->data[22];
                std::string addi(pData->sock->data+23, addiLen);
                if (pData->sock->data.size() < 25+addiLen+4){
                  INFO_MSG("Missing or invalid push instruction..?");
                  continue;
                }

                std::string localIP = pData->sock->getLocalAddr().host();

                // Extract the new target and apply it
                targetPort = (pData->sock->data[23+addiLen] << 8) | pData->sock->data[24+addiLen];
                size_t remainder = pData->sock->data.size()-25-addiLen;
                Socket::hostBytesToStr(pData->sock->data + 25 + addiLen, remainder >= 18?16:4, targetHost);
                if (remainder == 6 || remainder == 18){
                  lPort = (pData->sock->data[pData->sock->data.size() - 2] << 8) | pData->sock->data[pData->sock->data.size() - 1];
                }
                if (lPort == 0xFFFF){
                  INFO_MSG("Target: %s:%" PRIu16 ", port is open", targetHost.c_str(), targetPort);
                }else{
                  // Okay, we have a chance of success, but may need to keep punching. Start the thread.
                  pData->bgSock = pData->sock;
                  pData->sock = new Socket::UDPConnection();
                  pData->sock->data.assign(pData->bgSock->data, pData->bgSock->data.size());
                  pData->bgSock->connect();
                  hasThread = true;
                  std::thread punchThread([this](){
                    uint64_t startTime = Util::bootSecs();
                    pData->bgSock->setIgnoreSendErrors(true);
                    while (!pData->connected && startTime + 10 > Util::bootSecs()){
                      Util::sleep(100);
                      if (!pData->connected){
                        for (const auto & jt : pData->addrs) {
                          if (pData->bgSock->setDestination(jt)) { pData->bgSock->SendNow(pData->reqStr); }
                        }
                      }
                    }
                    pData->bgSock->setIgnoreSendErrors(false);
                    INFO_MSG("Exiting hole punching thread (%s)", pData->connected?"connected":"failed");
                    delete pData;
                    pData = 0;
                  });
                  punchThread.detach();
                  INFO_MSG("Target: %s:%" PRIu16 ", local port %" PRIu16, targetHost.c_str(), targetPort, lPort);
                  pData->sock->bind(lPort, localIP);
                }
                return true;
              }
              // Ping
              if (pData->sock->data[20] == 4 && pData->sock->data.size() >= 23){
                std::string dHost = pData->sock->getRemoteAddr().host();
                uint32_t dPort = pData->sock->getRemoteAddr().port();
                dPort = (pData->sock->data[21] << 8) | pData->sock->data[22];
                pData->sock->SetDestination(dHost, dPort);
                std::string fogData("FOGH0123456789ABCDEF\005", 21);
                fogData.append(pData->sock->data+23, pData->sock->data.size()-23);
                if (std::string(pData->sock->data+23, pData->sock->data.size()-23) == "OPEN_PORT"){
                  if (!(pData->reqStr[pData->reqStr.size() - 1] & 1)){
                    INFO_MSG("Marking port as open");
                    // Set port to open, recalculate hash
                    pData->reqStr[pData->reqStr.size() - 1] |= 1;
                    Foghorn::calcHash(pData->reqStr, trgt.user);
                  }
                }
                Foghorn::calcHash(fogData, trgt.user);
                pData->sock->SendNow(fogData);
                continue;
              }
              // Alternative Foghorn server; contact it for consistency check
              if (pData->sock->data[20] == 8 && pData->sock->data.size() >= 21){
                HTTP::URL altServ(std::string(pData->sock->data + 21, pData->sock->data.size() - 21));
                if (!altServ.getPort()){altServ.port = "7077";}
                if (!altServ.user.size()){altServ.user = trgt.user;}

                // Send mapping request with no protocol and no mapping name.
                std::string fogData("FOGH0123456789ABCDEF\000\000\000\000", 24);
                fogData += (char)(lPort >> 8);
                fogData += (char)(lPort & 0xFF);
                Foghorn::calcHash(fogData, altServ.user);

                // Send to all known addresses
                pData->sock->setIgnoreSendErrors(true);
                std::deque<Socket::Address> altAddrs = Socket::getAddrs(altServ.host, altServ.getPort());
                for (const auto & jt : altAddrs) {
                  if (pData->sock->setDestination(jt)) { pData->sock->SendNow(fogData); }
                }
                pData->sock->setIgnoreSendErrors(false);
                continue;
              }
              // Portscan request
              if (pData->sock->data[20] == 6 && pData->sock->data.size() >= 27){
                uint16_t startPort = (pData->sock->data[21] << 8) | pData->sock->data[22];
                uint16_t endPort = (pData->sock->data[23] << 8) | pData->sock->data[24];
                uint16_t portInterval = (pData->sock->data[25] << 8) | pData->sock->data[26];
                INFO_MSG("Received request to portscan from %" PRIu16 " to %" PRIu16 "with interval %" PRIu16, startPort, endPort, portInterval);
              }
              // Mapping response; check if we have consistency
              if (pData->sock->data[20] == 1){ // Mapping response
                std::string dHost = pData->sock->getRemoteAddr().host();

                std::map<std::string, uint16_t> * pMap = 0;
                if (pData->sock->data.size() == 28){
                  pMap = &portMap4;
                }else{
                  pMap = &portMap6;
                }

                uint16_t port = (pData->sock->data[22] << 8) | pData->sock->data[23];
                if (pMap->count(dHost) && (*pMap)[dHost] == port){continue;}
                (*pMap)[dHost] = port;
                INFO_MSG("According to %s our public port is %" PRIu16, dHost.c_str(), port);

                if (portMap4.size() > 1){
                  std::set<uint16_t> ports;
                  for (std::map<std::string, uint16_t>::iterator it = portMap4.begin(); it != portMap4.end(); ++it){
                    ports.insert(it->second);
                  }
                  consistent4 = (ports.size() == 1);
                }
                if (portMap6.size() > 1){
                  std::set<uint16_t> ports;
                  for (std::map<std::string, uint16_t>::iterator it = portMap6.begin(); it != portMap6.end(); ++it){
                    ports.insert(it->second);
                  }
                  consistent6 = (ports.size() == 1);
                }
                if (consistent4 || consistent6){
                  if (!(pData->reqStr[pData->reqStr.size() - 1] & 2)){
                    INFO_MSG("Marking port as consistent");
                    // Set port to consistent, recalculate hash
                    pData->reqStr[pData->reqStr.size() - 1] |= 2;
                    Foghorn::calcHash(pData->reqStr, trgt.user);
                  }
                }
                continue;
              }
            }
          }
        }
      }
      ++attempt;
    }
    return false;
  }

} // Foghorn

