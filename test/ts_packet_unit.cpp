#include <mist/ts_packet.h>

#include <cassert>
#include <cstdint>
#include <string>

int main() {
  std::string esInfo("\005\004Opus", 6);
  esInfo.append("\177\002\200", 3);
  esInfo.append(1, '\002');

  TS::ProgramDescriptors descriptors(esInfo.data(), esInfo.size());
  assert(descriptors.getRegistration() == "Opus");

  std::string ext = descriptors.getExtension();
  assert(ext.size() == 2);
  assert((uint8_t)ext[0] == 0x80);
  assert((uint8_t)ext[1] == 2);

  return 0;
}
