#include <mist/defines.h>
#include <mist/socket.h>

#include <cstdio>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

static bool receiveWithin(Socket::UDPConnection & socket, uint32_t timeoutMs) {
  fd_set readSet;
  FD_ZERO(&readSet);
  FD_SET(socket.getSock(), &readSet);
  struct timeval timeout;
  timeout.tv_sec = timeoutMs / 1000;
  timeout.tv_usec = (timeoutMs % 1000) * 1000;
  return select(socket.getSock() + 1, &readSet, 0, 0, &timeout) > 0 && socket.Receive();
}

int main() {
  int captured[2];
  if (pipe(captured)) { return 1; }
  const int savedStderr = dup(STDERR_FILENO);
  if (savedStderr < 0 || dup2(captured[1], STDERR_FILENO) < 0) { return 1; }
  close(captured[1]);

  Util::printDebugLevel = DLVL_DEVEL;
  sockaddr_storage unknown = {};
  Socket::Address address(&unknown);
  fflush(stderr);

  if (dup2(savedStderr, STDERR_FILENO) < 0) { return 1; }
  close(savedStderr);

  char buffer[1024];
  const ssize_t readSize = read(captured[0], buffer, sizeof(buffer));
  close(captured[0]);
  const std::string logOutput = readSize > 0 ? std::string(buffer, readSize) : std::string();

  if (!address.binForm().empty()) {
    std::cerr << "unknown address family produced a binary address" << std::endl;
    return 1;
  }
  if (logOutput.find("FAIL|") != std::string::npos) {
    std::cerr << "routine empty address was reported as an operational failure" << std::endl;
    return 1;
  }

  Socket::UDPConnection receiver;
  const uint16_t receiverPort = receiver.bind(0, "127.0.0.1");
  if (!receiverPort || receiver.getBoundAddr().family() != AF_INET) {
    std::cerr << "an explicit IPv4 bind did not create a native IPv4 socket" << std::endl;
    return 1;
  }
  receiver.allocateDestination();

  Socket::UDPConnection sender;
  if (!sender.bind(0, "127.0.0.1") || sender.getBoundAddr().family() != AF_INET) {
    std::cerr << "the IPv4 sender did not create a native IPv4 socket" << std::endl;
    return 1;
  }
  sender.SetDestination("127.0.0.1", receiverPort);
  sender.SendNow("request");
  if (!receiveWithin(receiver, 1000) || std::string(receiver.data, receiver.data.size()) != "request") {
    std::cerr << "the native IPv4 socket did not receive the request" << std::endl;
    return 1;
  }
  if (!receiver.connect()) {
    std::cerr << "an already-bound UDP socket could not connect to its received peer" << std::endl;
    return 1;
  }
  receiver.SendNow("reply");
  if (!receiveWithin(sender, 1000) || std::string(sender.data, sender.data.size()) != "reply") {
    std::cerr << "the native IPv4 socket could not reply to the received peer address" << std::endl;
    return 1;
  }
  return 0;
}
