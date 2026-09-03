// Manual test/dev tool (not a unit test): load a model through ModelFactory exactly
// like MistProcONNX does — auto-detection, sidecar labels and preprocessing applied —
// print the resolved configuration, and run one inference on a synthetic frame. Use it
// to sanity-check a new model/sidecar set without setting up a stream.
//
// Usage: onnxmodelprobe <model.onnx> [inputSize] [classification|detection|embedding] [image]
// The optional type override mirrors what MistProcONNX passes for curated registry
// models; without it the model goes through shape auto-detection. inputSize 0 (or
// omitted) uses the model's preprocessor sidecar size, like MistProcONNX does.
// An optional image file replaces the synthetic noise frame (for numeric comparisons).
#include "../lib/onnx.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>

int main(int argc, char **argv) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0]
              << " <model.onnx> [inputSize] [classification|detection|embedding] [image]\n"
              << "       " << argv[0] << " --ocr <det.onnx> <rec.onnx> <dict.txt> [image]\n"
              << "       " << argv[0] << " --tensor <model.onnx>"
              << std::endl;
    return 1;
  }
  const char *providerEnv = std::getenv("MIST_ONNX_TEST_PROVIDER");
  const std::string provider = providerEnv && *providerEnv ? providerEnv : "cpu";

  if (!strcmp(argv[1], "--tensor")) {
    if (argc < 3) return 1;
    ONNX::SessionRunner runner;
    std::string err;
    if (!runner.load(argv[2], 1, provider, err)) {
      std::cerr << "tensor session load failed: " << err << std::endl;
      return 1;
    }
    std::vector<ONNX::TensorData> inputs;
    for (const ONNX::TensorSpec &spec : runner.inputs()) {
      ONNX::TensorData tensor;
      tensor.name = spec.name;
      tensor.dtype = spec.dtype;
      tensor.shape = spec.shape;
      size_t count = 1;
      for (int64_t &dim : tensor.shape) {
        if (dim < 0) dim = 1;
        count *= (size_t)dim;
      }
      size_t elementSize = ONNX::TensorWire::elementSize(tensor.dtype);
      if (!elementSize || count > ONNX::TensorWire::DEFAULT_MAX_PACKET_BYTES / elementSize) {
        std::cerr << "unsupported or oversized input " << spec.name << std::endl;
        return 1;
      }
      tensor.bytes.assign(count * elementSize, 0);
      inputs.push_back(std::move(tensor));
    }
    std::vector<uint8_t> inputPacket, outputPacket;
    std::vector<ONNX::TensorData> decodedInputs, outputs, decodedOutputs;
    if (!ONNX::TensorWire::encode(inputs, inputPacket, err) ||
        !ONNX::TensorWire::decode(inputPacket.data(), inputPacket.size(), decodedInputs, err) ||
        !runner.runTensors(decodedInputs, outputs, err) ||
        !ONNX::TensorWire::encode(outputs, outputPacket, err) ||
        !ONNX::TensorWire::decode(outputPacket.data(), outputPacket.size(), decodedOutputs, err)) {
      std::cerr << "tensor round-trip/run failed: " << err << std::endl;
      return 1;
    }
    std::cout << "ONNXTENSOR: " << decodedInputs.size() << " input(s), "
              << decodedOutputs.size() << " output(s), wire " << inputPacket.size()
              << " -> " << outputPacket.size() << " bytes" << std::endl;
    for (const ONNX::TensorData &out : decodedOutputs) {
      std::cout << "  " << out.name << " " << ONNX::TensorWire::dtypeName(out.dtype) << " [";
      for (size_t i = 0; i < out.shape.size(); ++i) {
        if (i) std::cout << ",";
        std::cout << out.shape[i];
      }
      std::cout << "] " << out.bytes.size() << " bytes" << std::endl;
    }
    return 0;
  }

  if (!strcmp(argv[1], "--ocr-id")) {
    if (argc < 3) {
      std::cerr << "Usage: " << argv[0] << " --ocr-id <registry-id>" << std::endl;
      return 1;
    }
    // Registry-id path: resolve + auto-provision + create exactly like MistProcONNX's
    // isOCR branch — verifies the provision() success check for OCR (set MIST_MODEL_DIR).
    std::string id = argv[2];
    ONNX::OCRBundle bundle = ONNX::ModelRegistry::resolveOCRSet(id);
    if (!bundle.ok) {
      std::string hint;
      if (!ONNX::ModelRegistry::provision(id, hint)) {
        std::cerr << "provision failed: " << hint << std::endl;
        return 1;
      }
      bundle = ONNX::ModelRegistry::resolveOCRSet(id);
      if (!bundle.ok) {
        std::cerr << "OCR bundle did not resolve after provisioning" << std::endl;
        return 1;
      }
    }
    std::unique_ptr<ONNX::OCRModel> m = ONNX::ModelFactory::createOCRModel(bundle, 1, provider, false);
    std::cout << "provisioned+loaded OCR: " << (m ? "ok" : "FAIL") << " det=" << bundle.det << std::endl;
    return m ? 0 : 1;
  }

  if (!strcmp(argv[1], "--ocr")) {
    if (argc < 5) {
      std::cerr << "Usage: " << argv[0] << " --ocr <det.onnx> <rec.onnx> <dict.txt> [image]" << std::endl;
      return 1;
    }
    ONNX::OCRBundle bundle;
    bundle.det = argv[2];
    bundle.rec = argv[3];
    bundle.dict = argv[4];
    bundle.ok = true;
    std::unique_ptr<ONNX::OCRModel> ocr = ONNX::ModelFactory::createOCRModel(bundle, 1, provider, false);
    if (!ocr) {
      std::cerr << "createOCRModel failed" << std::endl;
      return 1;
    }
    cv::Mat frame;
    if (argc > 5) {
      frame = cv::imread(argv[5]);
    } else {
      // Render known text on a white canvas — round-trip proves det+rec+CTC end to end
      frame = cv::Mat(200, 700, CV_8UC3, cv::Scalar(255, 255, 255));
      cv::putText(frame, "HELLO WORLD 2026", cv::Point(30, 110), cv::FONT_HERSHEY_SIMPLEX, 2.0,
                  cv::Scalar(0, 0, 0), 4, cv::LINE_AA);
    }
    if (frame.empty()) {
      std::cerr << "Could not read/build the OCR input image" << std::endl;
      return 1;
    }
    // Type must be OCR so MistProcONNX's dispatch (getModelType() == OCR) is reachable —
    // a direct processOCRFrame call would pass even if the type were wrong.
    std::cout << "model type=" << (int)ocr->getModelType()
              << " (OCR=" << (int)ONNX::ModelType::OCR << ")" << std::endl;
    ONNX::OCRResult r = ocr->processOCRFrame(frame, 0.3f, 0);
    std::cout << "ocr ok=" << r.ok << " lines=" << r.lines.size() << std::endl;
    for (size_t i = 0; i < r.lines.size(); ++i) {
      std::cout << "  [" << i << "] \"" << r.lines[i].text << "\" conf=" << r.lines[i].confidence
                << std::endl;
    }
    std::cout << "text: " << r.text << std::endl;
    return 0;
  }
  // 0 = auto: the factory resolves the model-native size from the preprocessor
  // sidecar (same contract MistProcONNX and the secondary-model path use).
  int inputSize = (argc > 2) ? atoi(argv[2]) : 0;
  ONNX::ModelType typeOverride = ONNX::ModelType::UNKNOWN;
  if (argc > 3) {
    if (!strcmp(argv[3], "classification")) { typeOverride = ONNX::ModelType::YOLOV8_CLASSIFICATION; }
    else if (!strcmp(argv[3], "detection")) { typeOverride = ONNX::ModelType::YOLOV8_DETECTION; }
    else if (!strcmp(argv[3], "embedding")) { typeOverride = ONNX::ModelType::IMAGE_EMBEDDING; }
    else if (!strcmp(argv[3], "attribute")) { typeOverride = ONNX::ModelType::FACE_ATTRIBUTE; }
  }

  std::unique_ptr<ONNX::DetectionModel> model =
    ONNX::ModelFactory::createModel(argv[1], inputSize, 1, typeOverride, provider, false);
  if (!model) {
    std::cerr << "createModel failed for " << argv[1] << std::endl;
    return 1;
  }

  const ONNX::PreprocessConfig &cfg = model->getPreprocessConfig();
  const std::vector<std::string> &labels = model->getClassLabels();
  std::cout << "type: " << (int)model->getModelType() << std::endl;
  std::cout << "input: " << model->getInputWidth() << "x" << model->getInputHeight() << std::endl;
  std::cout << "resize: "
            << (cfg.resizeMode == ONNX::PreprocessConfig::LETTERBOX
                  ? "letterbox"
                  : (cfg.resizeMode == ONNX::PreprocessConfig::CENTER_CROP ? "center-crop" : "direct"))
            << std::endl;
  std::cout << "norm: "
            << (cfg.normMode == ONNX::PreprocessConfig::IMAGENET
                  ? "mean/std"
                  : (cfg.normMode == ONNX::PreprocessConfig::SCRFD_NORM ? "scrfd" : "scale01"))
            << " mean[0]=" << cfg.mean[0] << " std[0]=" << cfg.std[0] << std::endl;
  std::cout << "labels: " << labels.size() << std::endl;
  for (size_t i = 0; i < labels.size() && i < 4; ++i) {
    std::cout << "  [" << i << "] " << labels[i] << std::endl;
  }

  // One inference — proves the full preprocess+run+parse path. Synthetic noise by
  // default; a real image when given (for numeric comparison against references).
  cv::Mat frame;
  if (argc > 4) {
    frame = cv::imread(argv[4]);
    if (frame.empty()) {
      std::cerr << "Could not read image " << argv[4] << std::endl;
      return 1;
    }
  } else {
    frame = cv::Mat(480, 640, CV_8UC3);
    cv::randu(frame, cv::Scalar(0, 0, 0), cv::Scalar(255, 255, 255));
  }
  ONNX::ModelType t = model->getModelType();
  if (t == ONNX::ModelType::FACE_ATTRIBUTE) {
    ONNX::YOLOv8ClassificationModel *fa = static_cast<ONNX::YOLOv8ClassificationModel *>(model.get());
    ONNX::ClassificationResult r = fa->processClassificationFrame(frame, 0);
    float age = 0, gp = 0;
    for (const auto & s : r.top) {
      if (s.class_id == 0) age = s.confidence;
      else if (s.class_id == 1) gp = s.confidence;
    }
    std::cout << "face-attribute: age=" << age << " gender_prob=" << gp
              << " gender=" << (gp >= 0.5f ? "female" : "male") << std::endl;
  } else if (t == ONNX::ModelType::YOLOV8_CLASSIFICATION || t == ONNX::ModelType::YOLO11_CLASSIFICATION) {
    ONNX::YOLOv8ClassificationModel *cls = static_cast<ONNX::YOLOv8ClassificationModel *>(model.get());
    cls->setTopK(3); // exercise the ranked-list path like MistProcONNX's top_k option
    ONNX::ClassificationResult r = cls->processClassificationFrame(frame, 0);
    std::cout << "classification: id=" << r.class_id << " name=" << r.class_name
              << " conf=" << r.confidence << std::endl;
    for (size_t i = 0; i < r.top.size(); ++i) {
      std::cout << "  top[" << i << "] " << r.top[i].class_name << " " << r.top[i].confidence << std::endl;
    }
  } else if (t == ONNX::ModelType::IMAGE_EMBEDDING || t == ONNX::ModelType::FACE_RECOGNITION_ARCFACE) {
    ONNX::EmbeddingModel *emb = static_cast<ONNX::EmbeddingModel *>(model.get());
    ONNX::FaceEmbedding e = emb->processEmbeddingFrame(frame, 0);
    float norm = 0.0f;
    for (size_t i = 0; i < e.embedding.size(); ++i) { norm += e.embedding[i] * e.embedding[i]; }
    std::cout << "embedding: dim=" << e.embedding.size() << " l2norm=" << norm
              << " first=" << (e.embedding.empty() ? 0.0f : e.embedding[0]) << std::endl;
    if (emb->hasMatchSet()) {
      ONNX::ClassificationResult tags = emb->matchEmbedding(e.embedding, 5);
      std::cout << "zero-shot tags:" << std::endl;
      for (size_t i = 0; i < tags.top.size(); ++i) {
        std::cout << "  [" << i << "] " << tags.top[i].class_name << " cos=" << tags.top[i].confidence
                  << std::endl;
      }
    } else {
      std::cout << "embedding_values:";
      for (size_t i = 0; i < e.embedding.size(); ++i) { std::cout << " " << e.embedding[i]; }
      std::cout << std::endl;
    }
  } else {
    std::vector<ONNX::Detection> dets = model->processFrame(frame, 0.25f);
    std::cout << "detections: " << dets.size() << std::endl;
    for (size_t i = 0; i < dets.size() && i < 5; ++i) {
      std::cout << "  " << dets[i].class_name << " " << dets[i].confidence << std::endl;
    }

    // Exercise the streaming result contract and both output policies. The default
    // metadata-only path must not spend work on or emit JPEG; annotation is opt-in.
    std::vector<uchar> encoded;
    if (!cv::imencode(".jpg", frame, encoded)) {
      std::cerr << "Could not encode probe input" << std::endl;
      return 1;
    }
    ONNX::VideoPacket packet;
    packet.packetData.assign((const char *)encoded.data(), encoded.size());
    packet.timestamp = 5000000000ULL; // proves timestamps stay 64-bit
    packet.trackIdx = 7;
    packet.codec = "JPEG";
    packet.width = frame.cols;
    packet.height = frame.rows;
    ONNX::TemporalTracker tracker;
    ONNX::ProcessingStats stats;
    ONNX::Utils::SceneChangeDetector scene;
    auto metadataOnly = ONNX::Utils::processVideoPacketAuto(
      packet, *model, tracker, stats, scene, 0.25f, 0.4f, false, 80, false, false, false);
    if (metadataOnly.first["schema"].asString() != "mist.onnx.result/v1" ||
        metadataOnly.first["timestamp_ms"].asInt() != 5000000000ULL ||
        metadataOnly.second.jpegData.size() != 0) {
      std::cerr << "metadata-only result contract failed: " << metadataOnly.first.toString() << std::endl;
      return 1;
    }
    auto annotated = ONNX::Utils::processVideoPacketAuto(
      packet, *model, tracker, stats, scene, 0.25f, 0.4f, false, 80, true, false, false);
    if (annotated.second.jpegData.size() == 0) {
      std::cerr << "annotated-video opt-in produced no JPEG" << std::endl;
      return 1;
    }
    std::cout << "result schema: " << metadataOnly.first["schema"].asString()
              << " timestamp_ms=" << metadataOnly.first["timestamp_ms"].asInt()
              << " metadata_jpeg=0 annotated_jpeg=" << annotated.second.jpegData.size() << std::endl;
  }
  return 0;
}
