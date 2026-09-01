#include <mist/onnx.h>

#include <iostream>

static bool common(const JSON::Value &value, const char *kind, uint64_t timestamp) {
  return value["schema"].asString() == "mist.onnx.result/v1" &&
         value["timestamp_ms"].asInt() == timestamp &&
         value["kind"].asString() == kind && value["status"].asString() == "ok" &&
         value["model"]["name"].asString() == "test-model" &&
         !value.isMember("timestamp") && !value.isMember("model_name") &&
         !value.isMember("model_type") && !value.isMember("inference_status");
}

int main() {
  const uint64_t largeTimestamp = 5000000000ULL;
  ONNX::InferenceMetrics metrics;
  ONNX::Detection detection;
  detection.x = 0.1f; detection.y = 0.2f; detection.w = 0.3f; detection.h = 0.4f;
  detection.confidence = 0.9f; detection.class_id = 0; detection.class_name = "person";
  detection.track_id = 0;
  JSON::Value det = ONNX::Utils::detectionsToJSON({detection}, largeTimestamp, metrics, "test-model");
  if (!common(det, "object_detection", largeTimestamp) ||
      !det["detections"].isArray() || det["detections"].size() != 1 ||
      !det["detections"][0u].isMember("bbox") || det["detections"][0u].isMember("x") ||
      det["detections"][0u].isMember("track_id")) {
    std::cerr << "detection schema mismatch: " << det.toString() << std::endl;
    return 1;
  }

  ONNX::ClassificationResult cls;
  cls.class_id = 1; cls.class_name = "yes"; cls.confidence = 0.75f; cls.timestamp = largeTimestamp;
  JSON::Value classified = ONNX::Utils::classificationToJSON(cls, metrics, "test-model");
  if (!common(classified, "classification", largeTimestamp)) {
    std::cerr << "classification schema mismatch: " << classified.toString() << std::endl;
    return 1;
  }

  ONNX::AudioResult audio;
  audio.startMs = largeTimestamp; audio.endMs = largeTimestamp + 5000; audio.ok = true;
  JSON::Value audioJson = ONNX::Utils::audioResultToJSON(audio, "test-model", "audio_tagging");
  if (!common(audioJson, "audio_tagging", largeTimestamp) ||
      audioJson["window"]["end_ms"].asInt() != largeTimestamp + 5000) {
    std::cerr << "audio schema mismatch: " << audioJson.toString() << std::endl;
    return 1;
  }

  JSON::Value event = ONNX::Utils::eventToJSON("speech", true, 0.8f, largeTimestamp, "test-model");
  if (!common(event, "event", largeTimestamp) || event["event"]["state"].asString() != "started") {
    std::cerr << "event schema mismatch: " << event.toString() << std::endl;
    return 1;
  }
  return 0;
}
