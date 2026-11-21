#include <mist/mp4_generic.h>

#include <iostream>
#include <string>

int main() {
  for (unsigned int objectType : {0x20u, 0x60u, 0x61u, 0x62u, 0x63u, 0x64u, 0x65u, 0x6Au, 0x40u, 0x69u, 0x6Bu, 0xFFu}) {
    std::string bytes(15, 0);
    bytes[0] = 4;
    bytes[1] = 13;
    bytes[2] = objectType;
    MP4::DCDescriptor descriptor(bytes.data(), bytes.size());
    const std::string expected = objectType == 0x20                        ? "MPEG4"
      : ((objectType >= 0x60 && objectType <= 0x65) || objectType == 0x6A) ? "UNKNOWN"
      : objectType == 0x40                                                 ? "AAC"
      : (objectType == 0x69 || objectType == 0x6B)                         ? "MP3"
                                                                           : "UNKNOWN";
    if (descriptor.getCodec() != expected) {
      std::cerr << "Object type " << objectType << " is " << descriptor.getCodec() << " rather than " << expected << std::endl;
      return 1;
    }
  }
  DTSC::Meta metadata;
  const size_t track = metadata.addTrack();
  metadata.setType(track, "audio");
  metadata.setCodec(track, "EAC-3");
  metadata.setRate(track, 48000);
  metadata.setChannels(track, 6);
  metadata.setSize(track, 16);
  metadata.setInit(track, std::string("\x02\x00\x20\x3f\x00", 5));
  MP4::AudioSampleEntry entry(metadata, track);
  if (!entry.isType("ec-3")) {
    std::cerr << "EAC-3 was written as " << entry.getType() << std::endl;
    return 1;
  }
  metadata.setCodec(track, "AAC");
  metadata.setInit(track, std::string("\x12\x10\x56\xe5\x00", 5));
  MP4::ESDS esds(metadata, track);
  if (esds.boxedSize() != 42 || esds.getInitData() != metadata.getInit(track)) {
    std::cerr << "AAC ESDS size disagrees with the progressive MP4 header" << std::endl;
    return 1;
  }
  std::cout << "PASS upstream MP4 object types and unchanged AAC ESDS sizing" << std::endl;
  return 0;
}
