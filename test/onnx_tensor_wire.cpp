#include <mist/onnx.h>

#include <cstring>
#include <iostream>

static bool expect(bool value, const char *message) {
  if (!value) { std::cerr << message << std::endl; return false; }
  return true;
}

int main() {
  ONNX::TensorData image;
  image.name = "image";
  image.dtype = ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
  image.shape = {1, 2, 2};
  const float imageValues[] = {1.0f, 2.0f, 3.5f, -4.0f};
  image.bytes.resize(sizeof(imageValues));
  std::memcpy(image.bytes.data(), imageValues, sizeof(imageValues));

  ONNX::TensorData ids;
  ids.name = "ids";
  ids.dtype = ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64;
  ids.shape = {2};
  const int64_t idValues[] = {7, 9000000000LL};
  ids.bytes.resize(sizeof(idValues));
  std::memcpy(ids.bytes.data(), idValues, sizeof(idValues));

  std::string err;
  std::vector<uint8_t> packet;
  if (!expect(ONNX::TensorWire::encode({image, ids}, packet, err), err.c_str())) return 1;
  std::vector<ONNX::TensorData> decoded;
  if (!expect(ONNX::TensorWire::decode(packet.data(), packet.size(), decoded, err), err.c_str())) return 1;
  if (!expect(decoded.size() == 2, "tensor count changed")) return 1;
  if (!expect(decoded[0].name == image.name && decoded[0].dtype == image.dtype &&
              decoded[0].shape == image.shape && decoded[0].bytes == image.bytes,
              "float tensor did not round-trip")) return 1;
  if (!expect(decoded[1].name == ids.name && decoded[1].dtype == ids.dtype &&
              decoded[1].shape == ids.shape && decoded[1].bytes == ids.bytes,
              "int64 tensor did not round-trip")) return 1;

  std::vector<uint8_t> corrupt = packet;
  corrupt[0] = 'X';
  if (!expect(!ONNX::TensorWire::decode(corrupt.data(), corrupt.size(), decoded, err),
              "bad magic was accepted")) return 1;
  corrupt = packet;
  corrupt[5] = 1;
  if (!expect(!ONNX::TensorWire::decode(corrupt.data(), corrupt.size(), decoded, err),
              "reserved wire flags were accepted")) return 1;
  if (!expect(!ONNX::TensorWire::decode(packet.data(), packet.size() - 1, decoded, err),
              "truncated payload was accepted")) return 1;

  ONNX::TensorData bad = image;
  bad.shape = {1, 3, 2};
  if (!expect(!ONNX::TensorWire::encode({bad}, packet, err),
              "shape/byte mismatch was accepted")) return 1;
  return 0;
}
