#include <mist/foghorn.h>
#include <mist/timing.h>

#include <atomic>
#include <cassert>
#include <cstring>
#include <iostream>
#include <signal.h>
#include <string>
#include <sys/select.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

static bool receiveWithin(Socket::UDPConnection & socket, uint32_t timeoutMs) {
  fd_set readSet;
  FD_ZERO(&readSet);
  FD_SET(socket.getSock(), &readSet);
  struct timeval timeout;
  timeout.tv_sec = timeoutMs / 1000;
  timeout.tv_usec = (timeoutMs % 1000) * 1000;
  int result = select(socket.getSock() + 1, &readSet, 0, 0, &timeout);
  return result > 0 && socket.Receive();
}

static void testHash() {
  std::string packet("FOGH0123456789ABCDEF\005payload", 28);
  Foghorn::calcHash(packet, "secret");

  Util::ResizeablePointer data;
  data.assign(packet.data(), packet.size());
  assert(Foghorn::isFHData(data));
  assert(Foghorn::checkHash(data, "secret"));
  assert(!Foghorn::checkHash(data, "wrong-secret"));

  packet.back() ^= 1;
  data.assign(packet.data(), packet.size());
  assert(!Foghorn::checkHash(data, "secret"));

  data.assign("FOGH", 4);
  assert(!Foghorn::isFHData(data));
  assert(!Foghorn::checkHash(data, "secret"));
}

static void testIntegerEncoding() {
  std::string data;
  Foghorn::appendUint16(data, 0);
  Foghorn::appendUint16(data, 0x1234);
  Foghorn::appendUint16(data, 0xFFFF);
  assert(data.size() == 6);

  uint16_t value = 1;
  assert(Foghorn::readUint16(data.data(), data.size(), 0, value) && value == 0);
  assert(Foghorn::readUint16(data.data(), data.size(), 2, value) && value == 0x1234);
  assert(Foghorn::readUint16(data.data(), data.size(), 4, value) && value == 0xFFFF);
  assert(!Foghorn::readUint16(data.data(), data.size(), 5, value));
  assert(!Foghorn::readUint16(0, data.size(), 0, value));

  Foghorn::writeUint16(data, 2, 0xABCD);
  assert(Foghorn::readUint16(data.data(), data.size(), 2, value) && value == 0xABCD);
  std::string unchanged = data;
  Foghorn::writeUint16(data, data.size(), 1);
  assert(data == unchanged);
}

static void testPunchRequestEncoding() {
  std::string path(300, 'p');
  std::string additional(300, 'a');
  std::string request;
  assert(Foghorn::makePunchRequest(request, "SRT", path, additional, 0xCAFE, 5, "secret"));

  assert(request.size() == 21 + 1 + 3 + 2 + path.size() + 2 + additional.size() + 2 + 1);
  assert(request[20] == 2);
  assert((uint8_t)request[21] == 3);
  assert(request.substr(22, 3) == "SRT");
  size_t offset = 25;
  uint16_t length = 0;
  assert(Foghorn::readUint16(request.data(), request.size(), offset, length) && length == 300);
  offset += 2;
  assert(request.substr(offset, length) == path);
  offset += length;
  assert(Foghorn::readUint16(request.data(), request.size(), offset, length) && length == 300);
  offset += 2;
  assert(request.substr(offset, length) == additional);
  offset += length;
  uint16_t port = 0;
  assert(Foghorn::readUint16(request.data(), request.size(), offset, port) && port == 0xCAFE);
  assert((uint8_t)request[offset + 2] == 5);

  Util::ResizeablePointer data;
  data.assign(request.data(), request.size());
  assert(Foghorn::checkHash(data, "secret"));

  Foghorn::RelayRequest parsed;
  assert(Foghorn::parseRelayRequest(request.data(), request.size(), parsed));
  assert(parsed.protocol == "SRT");
  assert(parsed.path == path);
  assert(parsed.additionalData == additional);
  assert(parsed.localPort == 0xCAFE);
  assert(parsed.flags == 5);
  for (size_t size = 0; size < request.size(); ++size) {
    assert(!Foghorn::parseRelayRequest(request.data(), size, parsed));
  }
  request += 'x';
  assert(!Foghorn::parseRelayRequest(request.data(), request.size(), parsed));

  std::string sentinel = "unchanged";
  assert(!Foghorn::makePunchRequest(sentinel, std::string(256, 'x'), path, additional, 1, 0, ""));
  assert(sentinel == "unchanged");
  assert(!Foghorn::makePunchRequest(sentinel, "SRT", std::string(65536, 'x'), additional, 1, 0, ""));
  assert(sentinel == "unchanged");
  assert(!Foghorn::makePunchRequest(sentinel, "SRT", path, std::string(65536, 'x'), 1, 0, ""));
  assert(sentinel == "unchanged");
}

static void testAnnouncementParsing() {
  std::string packet("FOGH0123456789ABCDEF\000", 21);
  packet += (char)3;
  packet += "SRT";
  Foghorn::appendUint16(packet, 300);
  packet += std::string(300, 'p');
  const size_t requiredSize = packet.size();
  Foghorn::appendUint16(packet, 0xCAFE);
  const size_t portSize = packet.size();
  packet += (char)3;

  Foghorn::Announcement announcement;
  assert(Foghorn::parseAnnouncement(packet.data(), packet.size(), announcement));
  assert(announcement.protocol == "SRT");
  assert(announcement.path == std::string(300, 'p'));
  assert(announcement.hasLocalPort && announcement.localPort == 0xCAFE);
  assert(announcement.hasFlags && announcement.flags == 3);
  for (size_t size = 0; size < requiredSize; ++size) {
    assert(!Foghorn::parseAnnouncement(packet.data(), size, announcement));
  }
  assert(Foghorn::parseAnnouncement(packet.data(), requiredSize, announcement));
  assert(!announcement.hasLocalPort && !announcement.hasFlags);
  assert(!Foghorn::parseAnnouncement(packet.data(), requiredSize + 1, announcement));
  assert(Foghorn::parseAnnouncement(packet.data(), portSize, announcement));
  assert(announcement.hasLocalPort && !announcement.hasFlags);
  packet += 'x';
  assert(!Foghorn::parseAnnouncement(packet.data(), packet.size(), announcement));
}

static std::string makeInstruction(const std::string & additional, uint16_t port, const char *host, size_t hostLength,
                                   bool includeSuggestedPort, uint16_t suggestedPort) {
  std::string packet("FOGH0123456789ABCDEF\003", 21);
  Foghorn::appendUint16(packet, additional.size());
  packet += additional;
  Foghorn::appendUint16(packet, port);
  packet.append(host, hostLength);
  if (includeSuggestedPort) { Foghorn::appendUint16(packet, suggestedPort); }
  return packet;
}

static void testPunchInstructionParsing() {
  const char ipv4[] = {127, 0, 0, 1};
  std::string packet = makeInstruction(std::string(300, 'a'), 9000, ipv4, sizeof(ipv4), false, 0);
  Foghorn::PunchInstruction instruction;
  assert(Foghorn::parsePunchInstruction(packet.data(), packet.size(), instruction));
  assert(instruction.additionalData == std::string(300, 'a'));
  assert(instruction.host == "127.0.0.1");
  assert(instruction.port == 9000);
  assert(!instruction.hasSuggestedPort);

  const char ipv6[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, (char)0xFF, (char)0xFF, 10, 0, 0, 1};
  packet = makeInstruction("stream", 10000, ipv6, sizeof(ipv6), true, 12000);
  assert(Foghorn::parsePunchInstruction(packet.data(), packet.size(), instruction));
  assert(instruction.additionalData == "stream");
  assert(instruction.host == "::ffff:10.0.0.1" || instruction.host == "10.0.0.1");
  assert(instruction.port == 10000);
  assert(instruction.hasSuggestedPort);
  assert(instruction.suggestedPort == 12000);

  std::string malformed("FOGH0123456789ABCDEF\003\001\000tiny", 27);
  assert(!Foghorn::parsePunchInstruction(malformed.data(), malformed.size(), instruction));
  packet = makeInstruction("", 9000, ipv4, sizeof(ipv4), false, 0);
  packet += 'x';
  assert(!Foghorn::parsePunchInstruction(packet.data(), packet.size(), instruction));
  packet[20] = 4;
  assert(!Foghorn::parsePunchInstruction(packet.data(), packet.size(), instruction));
}

static void testPublishedAnnouncement() {
  Socket::UDPConnection receiver;
  uint16_t receiverPort = receiver.bind(0, "127.0.0.1");
  assert(receiverPort);
  receiver.allocateDestination();

  Socket::UDPConnection sender;
  assert(sender.bind(0, "127.0.0.1"));
  Foghorn::List list;
  list.setPort(0x1234);
  list.setProtocol("SRT");
  list.add("fh://secret@127.0.0.1:" + std::to_string(receiverPort) + "/camera");
  assert(list.size() == 1);
  list.publish(sender, 10);
  assert(receiveWithin(receiver, 1000));

  Util::ResizeablePointer & packet = receiver.data;
  assert(Foghorn::checkHash(packet, "secret"));
  assert(packet[20] == 0);
  assert((uint8_t)packet[21] == 3);
  assert(std::string(packet + 22, 3) == "SRT");
  size_t offset = 25;
  uint16_t pathLength = 0;
  assert(Foghorn::readUint16(packet, packet.size(), offset, pathLength));
  offset += 2;
  assert(std::string(packet + offset, pathLength) == "camera");
  offset += pathLength;
  uint16_t advertisedPort = 0;
  assert(Foghorn::readUint16(packet, packet.size(), offset, advertisedPort));
  assert(advertisedPort == 0x1234);
  assert((uint8_t)packet[offset + 2] == 0);

  Foghorn::Announcement announcement;
  assert(Foghorn::parseAnnouncement(packet, packet.size(), announcement));
  assert(announcement.protocol == "SRT");
  assert(announcement.path == "camera");
  assert(announcement.hasLocalPort && announcement.localPort == 0x1234);
  assert(announcement.hasFlags && announcement.flags == 0);

  list.publish(sender, 10);
  assert(!receiveWithin(receiver, 50));
  list.publish(sender, 11);
  assert(receiveWithin(receiver, 1000));

  const char targetHost[] = {10, 20, 30, 40};
  std::string instruction = makeInstruction("stream-id", 9000, targetHost, sizeof(targetHost), true, 12000);
  Foghorn::calcHash(instruction, "secret");
  receiver.SetDestination("127.0.0.1", sender.getBoundAddr().port());
  receiver.SendNow(instruction);
  assert(receiveWithin(sender, 1000));
  assert(list.parsePacket(sender, 12));
  const Foghorn::PunchRequest & request = list.getPunchData();
  assert(request.additionalData == "stream-id");
  assert(request.host == "10.20.30.40");
  assert(request.port == 9000);
  assert(request.localPort == 12000);

  receiver.SendNow(instruction);
  assert(receiveWithin(sender, 1000));
  assert(!list.parsePacket(sender, 12));

  std::string malformed("FOGH0123456789ABCDEF\003\001\000tiny", 27);
  Foghorn::calcHash(malformed, "secret");
  receiver.SendNow(malformed);
  assert(receiveWithin(sender, 1000));
  assert(!list.parsePacket(sender, 13));

  Foghorn::List invalid;
  invalid.add("fh://secret@127.0.0.1:" + std::to_string(receiverPort) + "/" + std::string(65536, 'x'));
  assert(invalid.size() == 0);
}

static void testLocalRelay(const char *relayPath) {
  const std::string protocol = "SRT-LONG-01";
  Socket::UDPConnection reservation;
  uint16_t relayPort = reservation.bind(0, "127.0.0.1");
  assert(relayPort);
  reservation.close();

  pid_t relayPid = fork();
  assert(relayPid >= 0);
  if (!relayPid) {
    std::string port = std::to_string(relayPort);
    execl(relayPath, relayPath, "--port", port.c_str(), "--interface", "127.0.0.1", "--passphrase", "secret", (char *)0);
    _exit(127);
  }

  Util::sleep(250);
  bool relayRunning = !kill(relayPid, 0);
  Socket::UDPConnection publisherSocket;
  uint16_t publisherPort = publisherSocket.bind(0, "127.0.0.1");
  publisherSocket.allocateDestination();
  Foghorn::List publisher;
  // Emulate a NAT mapping: the private port reported by the publisher differs from
  // the public source port observed by the relay. This exercises both relay
  // instructions rather than the direct-connect/open-port shortcut.
  publisher.setPort(publisherPort == 0xFFFF ? publisherPort - 1 : publisherPort + 1);
  publisher.setProtocol(protocol);
  std::string relayUrl = "fh://secret@127.0.0.1:" + std::to_string(relayPort) + "/camera";
  publisher.add(relayUrl);
  publisher.add("fh://secret@127.0.0.1:" + std::to_string(relayPort) + "/zebra");

  std::atomic<bool> stop(false);
  std::atomic<bool> gotMapping(false);
  std::atomic<bool> gotInstruction(false);
  std::string receivedAdditional;
  std::thread publisherThread([&]() {
    while (!stop.load()) {
      publisher.publish(publisherSocket, Util::bootSecs());
      if (receiveWithin(publisherSocket, 100) && Foghorn::isFHData(publisherSocket.data)) {
        if (publisherSocket.data[20] == 1) { gotMapping = true; }
        if (publisher.parsePacket(publisherSocket, Util::bootSecs())) {
          receivedAdditional = publisher.getPunchData().additionalData;
          gotInstruction = true;
        }
      }
    }
  });

  for (size_t attempt = 0; attempt < 20 && !gotMapping.load(); ++attempt) { Util::sleep(100); }

  Socket::UDPConnection discovery;
  bool discoveryBound = discovery.bind(0, "127.0.0.1");
  discovery.allocateDestination();
  discovery.SetDestination("127.0.0.1", relayPort);
  std::string listRequest("FOGH0123456789ABCDEF\377", 21);
  Foghorn::appendUint16(listRequest, 0);
  Foghorn::appendUint16(listRequest, 0);
  Foghorn::calcHash(listRequest, "secret");
  discovery.SendNow(listRequest);
  bool discovered = receiveWithin(discovery, 1000) && Foghorn::checkHash(discovery.data, "secret") &&
    discovery.data.size() >= 25 && discovery.data[20] == (char)0xFE;
  uint16_t discoveryTotal = 0;
  if (discovered) { discovered = Foghorn::readUint16(discovery.data, discovery.data.size(), 23, discoveryTotal); }
  std::string discoveredName;
  std::string discoveredProtocol;
  if (discovered && discovery.data.size() > 27) {
    const size_t entryLength = (uint8_t)discovery.data[25];
    size_t offset = 26;
    const size_t nameLength = (uint8_t)discovery.data[offset++];
    if (offset + nameLength + 1 <= discovery.data.size()) {
      discoveredName.assign(discovery.data + offset, nameLength);
      offset += nameLength;
      const size_t protocolLength = (uint8_t)discovery.data[offset++];
      if (offset + protocolLength <= discovery.data.size()) {
        discoveredProtocol.assign(discovery.data + offset, protocolLength);
      }
    }
    discovered = discovery.data.size() == 26 + entryLength;
  }

  Socket::UDPConnection wrongProtocol;
  bool wrongProtocolBound = wrongProtocol.bind(0, "127.0.0.1");
  wrongProtocol.allocateDestination();
  wrongProtocol.SetDestination("127.0.0.1", relayPort);
  std::string wrongRequest;
  bool wrongRequestBuilt =
    Foghorn::makePunchRequest(wrongRequest, "RIST", "camera", "stream-id", wrongProtocol.getBoundAddr().port(), 1, "secret");
  wrongProtocol.SendNow(wrongRequest);
  bool protocolMismatchRejected = !receiveWithin(wrongProtocol, 300);

  bool punched = false;
  std::string targetHost;
  uint16_t targetPort = 0;
  {
    Foghorn::Puncher puncher(HTTP::URL("srt-fh://secret@127.0.0.1:" + std::to_string(relayPort) + "/camera"), protocol, "stream-id");
    punched = puncher.start();
    targetHost = puncher.targetHost;
    targetPort = puncher.targetPort;
  }
  Util::sleep(250);
  stop = true;
  publisherThread.join();

  kill(relayPid, SIGTERM);
  int relayStatus = 0;
  waitpid(relayPid, &relayStatus, 0);

  assert(relayRunning);
  assert(gotMapping.load());
  assert(discoveryBound);
  assert(discovered && discoveryTotal == 2);
  assert(discoveredName == "camera");
  assert(discoveredProtocol == protocol);
  assert(wrongProtocolBound && wrongRequestBuilt && protocolMismatchRejected);
  assert(punched);
  assert(targetHost == "127.0.0.1" || targetHost == "::ffff:127.0.0.1");
  assert(targetPort == publisherPort);
  assert(gotInstruction.load());
  assert(receivedAdditional == "stream-id");
}

int main(int argc, char **argv) {
  if (argc == 3 && std::string(argv[1]) == "relay") {
    testLocalRelay(argv[2]);
    std::cout << "Foghorn local relay test passed" << std::endl;
    return 0;
  }
  testHash();
  testIntegerEncoding();
  testPunchRequestEncoding();
  testAnnouncementParsing();
  testPunchInstructionParsing();
  testPublishedAnnouncement();
  std::cout << "Foghorn protocol tests passed" << std::endl;
  return 0;
}
