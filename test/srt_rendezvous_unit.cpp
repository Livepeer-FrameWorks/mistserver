#include <mist/socket_srt.h>
#include <mist/timing.h>

#include <iostream>
#include <poll.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

static const std::string payload(1316, 'R');

static bool readBytes(int fd, char *buffer, size_t count) {
  size_t offset = 0;
  while (offset < count) {
    const ssize_t result = read(fd, buffer + offset, count - offset);
    if (result <= 0) { return false; }
    offset += result;
  }
  return true;
}

static int runPeer(const std::string & localHost, uint16_t localPort, const std::string & remoteHost, uint16_t remotePort,
                   const std::string & direction, int startFd, int readyFd, int transferFd, int acknowledgmentFd) {
  char start = 0;
  if (!readBytes(startFd, &start, 1)) { return 1; }
  close(startFd);

  if (!Socket::SRT::libraryInit()) {
    std::cerr << direction << " peer could not initialize libsrt" << std::endl;
    return 1;
  }

  Socket::UDPConnection udp(false, AF_INET);
  if (udp.bind(localPort, localHost) != localPort) {
    std::cerr << direction << " peer could not bind its UDP socket" << std::endl;
    return 1;
  }
  std::deque<Socket::Address> remote = Socket::getAddrs(remoteHost, remotePort, AF_INET, false);
  if (!remote.size()) {
    std::cerr << direction << " peer could not resolve its counterpart" << std::endl;
    return 1;
  }

  paramList parameters;
  parameters["messageapi"] = "true";
  parameters["timeout"] = "5000";
  parameters["tsbpd"] = "true";
  Socket::SRTConnection connection(udp, remote.front(), direction, parameters);
  if (!connection || connection.direction != direction || udp.getSock() != -1) {
    std::cerr << direction << " peer did not acquire its UDP socket as an SRT rendezvous connection" << std::endl;
    return 1;
  }
  bool sender = false;
  int senderSize = sizeof sender;
  if (srt_getsockopt(connection.getSocket(), 0, SRTO_SENDER, &sender, &senderSize) == SRT_ERROR || sender != (direction == "output")) {
    std::cerr << direction << " peer has the wrong SRT sender role" << std::endl;
    return 1;
  }

  if (write(readyFd, "r", 1) != 1) { return 1; }
  close(readyFd);
  if (!readBytes(transferFd, &start, 1)) { return 1; }
  close(transferFd);
  // Live-mode TSBPD needs one latency window to settle after a rendezvous
  // handshake; otherwise libsrt may legitimately drop the first timed packet.
  Util::sleep(250);

  if (direction == "output") {
    connection.SendNow(payload);
    pollfd acknowledgment = {acknowledgmentFd, POLLIN, 0};
    if (poll(&acknowledgment, 1, 10000) != 1 || !(acknowledgment.revents & POLLIN) || !readBytes(acknowledgmentFd, &start, 1)) {
      std::cerr << "output peer did not receive the input peer's acknowledgment" << std::endl;
      return 1;
    }
  } else {
    size_t received = 0;
    const uint64_t deadline = Util::bootMS() + 5000;
    while (!received && connection && Util::bootMS() < deadline) {
      if (connection.readable()) { received = connection.Recv(); }
      if (!received) { Util::sleep(10); }
    }
    if (received != payload.size() || std::string(connection.recvbuf, received) != payload) {
      std::cerr << "input peer did not receive the exact SRT payload in state " << connection.getStateStr()
                << ", bytes down " << connection.dataDown() << std::endl;
      return 1;
    }
    if (write(acknowledgmentFd, "a", 1) != 1) { return 1; }
  }
  close(acknowledgmentFd);
  connection.close();
  return 0;
}

int main() {
  Socket::UDPConnection inputReservation(false, AF_INET);
  Socket::UDPConnection outputReservation(false, AF_INET);
  const uint16_t inputPort = inputReservation.bind(0, "127.0.0.1");
  const uint16_t outputPort = outputReservation.bind(inputPort, "127.0.0.2");
  if (!inputPort || outputPort != inputPort) {
    std::cerr << "could not reserve the rendezvous port on two loopback addresses" << std::endl;
    return 1;
  }
  inputReservation.close();
  outputReservation.close();

  int startPipe[2];
  int readyPipe[2];
  int transferPipe[2];
  int acknowledgmentPipe[2];
  if (pipe(startPipe) || pipe(readyPipe) || pipe(transferPipe) || pipe(acknowledgmentPipe)) { return 1; }
  const pid_t inputPid = fork();
  if (!inputPid) {
    close(startPipe[1]);
    close(readyPipe[0]);
    close(transferPipe[1]);
    close(acknowledgmentPipe[0]);
    _exit(runPeer("127.0.0.1", inputPort, "127.0.0.2", outputPort, "input", startPipe[0], readyPipe[1], transferPipe[0],
                  acknowledgmentPipe[1]));
  }
  if (inputPid < 0) { return 1; }

  const pid_t outputPid = fork();
  if (!outputPid) {
    close(startPipe[1]);
    close(readyPipe[0]);
    close(transferPipe[1]);
    close(acknowledgmentPipe[1]);
    _exit(runPeer("127.0.0.2", outputPort, "127.0.0.1", inputPort, "output", startPipe[0], readyPipe[1],
                  transferPipe[0], acknowledgmentPipe[0]));
  }
  if (outputPid < 0) { return 1; }

  close(startPipe[0]);
  close(readyPipe[1]);
  close(transferPipe[0]);
  close(acknowledgmentPipe[0]);
  close(acknowledgmentPipe[1]);
  if (write(startPipe[1], "go", 2) != 2) { return 1; }
  close(startPipe[1]);
  char ready[2];
  if (!readBytes(readyPipe[0], ready, 2)) { return 1; }
  close(readyPipe[0]);
  if (write(transferPipe[1], "go", 2) != 2) { return 1; }
  close(transferPipe[1]);

  int inputStatus = 0;
  int outputStatus = 0;
  if (waitpid(inputPid, &inputStatus, 0) != inputPid || waitpid(outputPid, &outputStatus, 0) != outputPid) { return 1; }
  if (!WIFEXITED(inputStatus) || WEXITSTATUS(inputStatus) || !WIFEXITED(outputStatus) || WEXITSTATUS(outputStatus)) {
    std::cerr << "one or both SRT rendezvous peers failed" << std::endl;
    return 1;
  }
  return 0;
}
