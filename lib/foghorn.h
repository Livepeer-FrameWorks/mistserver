#include "util.h"
#include "url.h"
#include "socket.h"

#define FIRE_OPEN 0x00
#define FIRE_CONSISTANT 0x01
#define FIRE_PREDICTABLE 0x02
#define FIRE_IMPENETRABLE 0xFE
#define FIRE_UNDETERMINED 0xFF

namespace Foghorn{
  void calcHash(std::string & pData, const std::string & passphrase);
  bool checkHash(const Util::ResizeablePointer & D, const std::string & passphrase);
  bool isFHData(const Util::ResizeablePointer & D);

  class Instance {
  public:
    HTTP::URL url;
    std::string porthost4, porthost6;
    uint64_t lastHost4, lastHost6;
    std::deque<Socket::Address> addrs;
    uint16_t getPort4() const;
    uint16_t getPort6() const;
    uint8_t state4, state6;

    Instance();
    void init(const HTTP::URL & u);
    bool hasAddr(const Socket::Address & a) const;
  };

  class PunchRequest{
  public:
    std::string additionalData;
    std::string host;
    uint16_t port;
    std::string localHost;
    uint16_t localPort;
  };

  class List {
  private:
    std::map<std::string, Instance> instances;
    char portBytes[2];
    std::string protocolString;
    uint64_t lastOpenNotice;
    uint64_t lastPublish;
    PunchRequest punchReq;
    std::map<std::string, uint64_t> recentMap;
  public:
    List();
    void add(const std::string & u);
    void publish(Socket::UDPConnection & uSock, const uint64_t currTime);
    size_t size() const;
    void setPort(uint16_t port);
    void setProtocol(const std::string & p);
    bool parsePacket(Socket::UDPConnection & uSock, const uint64_t currTime);
    const PunchRequest & getPunchData() const;
    bool insertRecent(const std::string & localIP, const uint16_t localPort,
                      const std::string & remoteIP, const uint16_t remotePort,
                      const uint64_t currTime);
  };

  class PunchData{
  public:
    PunchData(){connected = false;}
    std::string reqStr;
    bool connected;
    std::deque<Socket::Address> addrs;
    Socket::UDPConnection * sock;
    Socket::UDPConnection * bgSock;
  };

  class Puncher{
  private:
    HTTP::URL trgt;
    PunchData * pData;
    uint16_t lPort;
    bool hasThread;
  public:
    ~Puncher();
    Puncher(const HTTP::URL & target, const std::string & protocol, const std::string & additionalData = "");
    bool start();

    std::string targetHost;
    uint16_t targetPort;
    bool isOpen();
    Socket::UDPConnection * getSocket();
  };

} // Foghorn

