#include "socket.h"
#include "url.h"
#include "util.h"

#include <atomic>
#include <memory>

#define FIRE_OPEN 0x00
#define FIRE_CONSISTANT 0x01
#define FIRE_PREDICTABLE 0x02
#define FIRE_IMPENETRABLE 0xFE
#define FIRE_UNDETERMINED 0xFF

namespace Foghorn {
  void calcHash(std::string & pData, const std::string & passphrase);
  bool checkHash(const Util::ResizeablePointer & D, const std::string & passphrase);
  bool isFHData(const Util::ResizeablePointer & D);
  void appendUint16(std::string & data, uint16_t value);
  void writeUint16(std::string & data, size_t offset, uint16_t value);
  bool readUint16(const char *data, size_t size, size_t offset, uint16_t & value);

  bool makePunchRequest(std::string & data, const std::string & protocol, const std::string & path,
                        const std::string & additionalData, uint16_t localPort, uint8_t flags, const std::string & passphrase);

  class Announcement {
    public:
      std::string protocol;
      std::string path;
      bool hasLocalPort;
      uint16_t localPort;
      bool hasFlags;
      uint8_t flags;
  };

  bool parseAnnouncement(const char *data, size_t size, Announcement & announcement);

  class RelayRequest {
    public:
      std::string protocol;
      std::string path;
      std::string additionalData;
      uint16_t localPort;
      uint8_t flags;
  };

  bool parseRelayRequest(const char *data, size_t size, RelayRequest & request);

  class PunchInstruction {
    public:
      std::string additionalData;
      std::string host;
      uint16_t port;
      bool hasSuggestedPort;
      uint16_t suggestedPort;
  };

  bool parsePunchInstruction(const char *data, size_t size, PunchInstruction & instruction);

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

  class PunchRequest {
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
      bool insertRecent(const std::string & localIP, const uint16_t localPort, const std::string & remoteIP,
                        const uint16_t remotePort, const uint64_t currTime);
  };

  class PunchData {
    public:
      PunchData() : connected(false), sock(0), bgSock(0) {}
      ~PunchData() {
        delete bgSock;
        delete sock;
      }
      std::string reqStr;
      std::atomic<bool> connected;
      std::deque<Socket::Address> addrs;
      Socket::UDPConnection *sock;
      Socket::UDPConnection *bgSock;
  };

  class Puncher {
    private:
      HTTP::URL trgt;
      std::shared_ptr<PunchData> pData;
      uint16_t lPort;

    public:
      ~Puncher();
      Puncher(const HTTP::URL & target, const std::string & protocol, const std::string & additionalData = "");
      bool start();

      std::string targetHost;
      uint16_t targetPort;
      bool isOpen();
      Socket::UDPConnection *getSocket();
  };

} // namespace Foghorn
