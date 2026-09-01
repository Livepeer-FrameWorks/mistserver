#include "onnx.h"

#include "config.h"
#include "defines.h"
#include "imagenet_classes.h"
#include "onnxruntime_c_api.h"
#include "procs.h"
#include "stream.h"
#include "timing.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <limits>
#include <mutex>
#include <numeric>
#include <unistd.h>

namespace ONNX {

  namespace TensorWire {
    size_t elementSize(ONNXTensorElementDataType dtype) {
      switch (dtype) {
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL:
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8:
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8: return 1;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16:
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_BFLOAT16:
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT16:
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT16: return 2;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT32:
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32: return 4;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE:
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT64:
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64: return 8;
        default: return 0;
      }
    }

    std::string dtypeName(ONNXTensorElementDataType dtype) {
      switch (dtype) {
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL: return "bool";
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8: return "uint8";
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8: return "int8";
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT16: return "uint16";
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT16: return "int16";
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT32: return "uint32";
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32: return "int32";
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT64: return "uint64";
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64: return "int64";
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16: return "float16";
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_BFLOAT16: return "bfloat16";
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT: return "float32";
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE: return "float64";
        default: return "";
      }
    }

    ONNXTensorElementDataType parseDtype(const std::string &name) {
      if (name == "bool") return ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL;
      if (name == "uint8") return ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8;
      if (name == "int8") return ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8;
      if (name == "uint16") return ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT16;
      if (name == "int16") return ONNX_TENSOR_ELEMENT_DATA_TYPE_INT16;
      if (name == "uint32") return ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT32;
      if (name == "int32") return ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32;
      if (name == "uint64") return ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT64;
      if (name == "int64") return ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64;
      if (name == "float16") return ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16;
      if (name == "bfloat16") return ONNX_TENSOR_ELEMENT_DATA_TYPE_BFLOAT16;
      if (name == "float32") return ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
      if (name == "float64") return ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE;
      return ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
    }

    namespace {
      bool littleEndianHost() {
        const uint16_t one = 1;
        return *(const uint8_t *)&one == 1;
      }

      void putU32(std::vector<uint8_t> &out, uint32_t value) {
        out.push_back((uint8_t)(value >> 24)); out.push_back((uint8_t)(value >> 16));
        out.push_back((uint8_t)(value >> 8)); out.push_back((uint8_t)value);
      }
      uint32_t getU32(const uint8_t *p) {
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
      }
      bool validateTensor(const TensorData &tensor, size_t &expected, std::string &err) {
        size_t elem = elementSize(tensor.dtype);
        if (!elem) { err = "unsupported tensor dtype for '" + tensor.name + "'"; return false; }
        size_t count = 1;
        for (int64_t dim : tensor.shape) {
          if (dim < 0) { err = "wire tensor has a dynamic/negative dimension"; return false; }
          if (dim && count > SIZE_MAX / (size_t)dim) { err = "tensor shape product overflow"; return false; }
          count *= (size_t)dim;
        }
        if (count > SIZE_MAX / elem) { err = "tensor byte size overflow"; return false; }
        expected = count * elem;
        if (expected != tensor.bytes.size()) { err = "tensor byte length does not match dtype and shape"; return false; }
        return true;
      }
    }

    bool encode(const std::vector<TensorData> &tensors, std::vector<uint8_t> &packet,
                std::string &err, size_t maxPacketBytes) {
      if (!littleEndianHost()) { err = "ONNXTENSOR v1 requires a little-endian host"; return false; }
      JSON::Value header;
      header["schema"] = "mist.onnx.tensor/v1";
      header["byte_order"] = "little";
      header["tensors"].append(); header["tensors"].shrink(0);
      size_t offset = 0;
      for (const TensorData &tensor : tensors) {
        size_t expected = 0;
        if (!validateTensor(tensor, expected, err)) return false;
        if (offset > SIZE_MAX - expected) { err = "tensor payload overflow"; return false; }
        JSON::Value item;
        item["name"] = tensor.name;
        item["dtype"] = dtypeName(tensor.dtype);
        item["shape"].append(); item["shape"].shrink(0);
        for (int64_t dim : tensor.shape) item["shape"].append(dim);
        item["offset"] = (uint64_t)offset;
        item["length"] = (uint64_t)expected;
        header["tensors"].append(item);
        offset += expected;
      }
      std::string headerText = header.toString();
      if (headerText.size() > UINT32_MAX || 12 + headerText.size() > maxPacketBytes ||
          offset > maxPacketBytes - 12 - headerText.size()) {
        err = "ONNXTENSOR packet exceeds configured maximum"; return false;
      }
      packet.clear(); packet.reserve(12 + headerText.size() + offset);
      packet.insert(packet.end(), {'M','S','T','T', VERSION, 0, 0, 0});
      putU32(packet, (uint32_t)headerText.size());
      packet.insert(packet.end(), headerText.begin(), headerText.end());
      for (const TensorData &tensor : tensors) packet.insert(packet.end(), tensor.bytes.begin(), tensor.bytes.end());
      return true;
    }

    bool decode(const void *packetData, size_t packetBytes, std::vector<TensorData> &tensors,
                std::string &err, size_t maxPacketBytes) {
      tensors.clear();
      if (!packetData || packetBytes < 12 || packetBytes > maxPacketBytes) { err = "invalid ONNXTENSOR packet size"; return false; }
      const uint8_t *packet = (const uint8_t *)packetData;
      if (std::memcmp(packet, "MSTT", 4) || packet[4] != VERSION || packet[5] || packet[6] || packet[7]) {
        err = "invalid ONNXTENSOR magic, version or reserved flags"; return false;
      }
      uint32_t headerLen = getU32(packet + 8);
      if (headerLen > packetBytes - 12) { err = "truncated ONNXTENSOR header"; return false; }
      JSON::Value header = JSON::fromString((const char *)packet + 12, headerLen);
      if (!littleEndianHost() || header["schema"].asString() != "mist.onnx.tensor/v1" ||
          header["byte_order"].asString() != "little" || !header["tensors"].isArray()) {
        err = "invalid ONNXTENSOR header"; return false;
      }
      const uint8_t *payload = packet + 12 + headerLen;
      size_t payloadBytes = packetBytes - 12 - headerLen;
      for (uint32_t i = 0; i < header["tensors"].size(); ++i) {
        const JSON::Value &item = header["tensors"][i];
        TensorData tensor;
        tensor.name = item["name"].asString(); tensor.dtype = parseDtype(item["dtype"].asString());
        if (!item["shape"].isArray() || !elementSize(tensor.dtype)) { err = "invalid tensor metadata"; return false; }
        for (uint32_t d = 0; d < item["shape"].size(); ++d) tensor.shape.push_back(item["shape"][d].asInt());
        int64_t offIn = item["offset"].asInt(), lenIn = item["length"].asInt();
        if (offIn < 0 || lenIn < 0) { err = "negative tensor offset or length"; return false; }
        size_t off = (size_t)offIn, len = (size_t)lenIn;
        if (off > payloadBytes || len > payloadBytes - off) { err = "tensor payload range is outside packet"; return false; }
        tensor.bytes.assign(payload + off, payload + off + len);
        size_t expected = 0;
        if (!validateTensor(tensor, expected, err)) return false;
        tensors.push_back(std::move(tensor));
      }
      return true;
    }
  }

  // Define static const member
  const size_t Detection::MAX_TRAIL_LENGTH;

  // ---- ORTHelpers implementations (C API helpers) ----
  namespace ORTHelpers {
    const OrtApi *api() {
      static const OrtApi *s_api = OrtGetApiBase()->GetApi(ORT_API_VERSION);
      return s_api;
    }

    OrtEnv *sharedEnv() {
      // Created once, never released (leaked on purpose). One OrtEnv per process; must
      // outlive all static/atexit teardown or ORT's LoggingManager destructor terminates.
      static OrtEnv *s_env = []() -> OrtEnv * {
        OrtEnv *e = nullptr;
        if (api()->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "ONNX", &e) != nullptr) { return nullptr; }
        return e;
      }();
      return s_env;
    }

    std::vector<int64_t> getTensorShape(const OrtTensorTypeAndShapeInfo *info) {
      std::vector<int64_t> dims;
      if (!info) { return dims; }
      size_t count = 0;
      if (api()->GetDimensionsCount(info, &count) != nullptr || count == 0) { return dims; }
      dims.resize(count);
      (void)api()->GetDimensions(info, dims.data(), count);
      return dims;
    }

    std::string shapeToString(const std::vector<int64_t> & dims) {
      std::string s = "[";
      for (size_t i = 0; i < dims.size(); ++i) {
        if (i) s += ", ";
        s += std::to_string(dims[i]);
      }
      s += "]";
      return s;
    }
    // Check an ORT status and log + release on error. Returns true on success.
    bool checkStatus(OrtStatus *status, const char *context) {
      if (status == nullptr) return true;
      const char *msg = api()->GetErrorMessage(status);
      ERROR_MSG("ORT error in %s: %s", context, msg ? msg : "(unknown)");
      api()->ReleaseStatus(status);
      return false;
    }
  } // namespace ORTHelpers

  // Clamp detection box to [0,1] with minimal size safeguarding
  static inline void clampDetection(Detection & d) {
    const float eps = 1e-6f;
    if (d.w < eps) d.w = eps;
    if (d.h < eps) d.h = eps;
    if (d.x < 0.0f) d.x = 0.0f;
    if (d.y < 0.0f) d.y = 0.0f;
    if (d.x + d.w > 1.0f) { d.w = std::max(eps, 1.0f - d.x); }
    if (d.y + d.h > 1.0f) { d.h = std::max(eps, 1.0f - d.y); }
  }

  static std::vector<Detection> trackIfEnabled(TemporalTracker &tracker,
                                                const std::vector<Detection> &detections,
                                                uint64_t timestamp, bool enabled) {
    if (enabled) return tracker.updateTracks(detections, timestamp);
    std::vector<Detection> stateless = detections;
    for (Detection &d : stateless) {
      d.track_id = 0;
      d.track_confidence = 0.0f;
      d.first_seen_time = 0;
      d.last_seen_time = 0;
      d.trail.clear();
      d.kalmanFilter.reset();
      d.kalmanInitialized = false;
    }
    return stateless;
  }

  // Clamp point to [0,1] range
  static inline void clampPoint(cv::Point2f & p) {
    if (p.x < 0.0f)
      p.x = 0.0f;
    else if (p.x > 1.0f)
      p.x = 1.0f;
    if (p.y < 0.0f)
      p.y = 0.0f;
    else if (p.y > 1.0f)
      p.y = 1.0f;
  }

  namespace Utils {
    const std::vector<std::string> COCO_CLASSES = {
      "person",         "bicycle",    "car",           "motorcycle",    "airplane",     "bus",           "train",
      "truck",          "boat",       "traffic light", "fire hydrant",  "stop sign",    "parking meter", "bench",
      "bird",           "cat",        "dog",           "horse",         "sheep",        "cow",           "elephant",
      "bear",           "zebra",      "giraffe",       "backpack",      "umbrella",     "handbag",       "tie",
      "suitcase",       "frisbee",    "skis",          "snowboard",     "sports ball",  "kite",          "baseball bat",
      "baseball glove", "skateboard", "surfboard",     "tennis racket", "bottle",       "wine glass",    "cup",
      "fork",           "knife",      "spoon",         "bowl",          "banana",       "apple",         "sandwich",
      "orange",         "broccoli",   "carrot",        "hot dog",       "pizza",        "donut",         "cake",
      "chair",          "couch",      "potted plant",  "bed",           "dining table", "toilet",        "tv",
      "laptop",         "mouse",      "remote",        "keyboard",      "cell phone",   "microwave",     "oven",
      "toaster",        "sink",       "refrigerator",  "book",          "clock",        "vase",          "scissors",
      "teddy bear",     "hair drier", "toothbrush"};

    // COCO pose keypoint names
    const std::vector<std::string> COCO_KEYPOINTS = {
      "nose",           "left_eye",   "right_eye",   "left_ear",   "right_ear",   "left_shoulder",
      "right_shoulder", "left_elbow", "right_elbow", "left_wrist", "right_wrist", "left_hip",
      "right_hip",      "left_knee",  "right_knee",  "left_ankle", "right_ankle"};

    // DOTA dataset class names (15 classes for oriented bounding box models)
    const std::vector<std::string> DOTA_CLASSES = {
      "plane",         "ship",            "storage-tank",      "baseball-diamond", "tennis-court",
      "basketball-court", "ground-track-field", "harbor",        "bridge",           "large-vehicle",
      "small-vehicle", "helicopter",      "roundabout",        "soccer-ball-field", "swimming-pool"};

    cv::Mat decodeVideoFrame(const char *data, size_t dataLen, const std::string & codec, uint64_t width, uint64_t height) {
      cv::Mat frame;

      // Validate input parameters
      if (!data || dataLen == 0 || width == 0 || height == 0) {
        ERROR_MSG("Invalid video frame parameters: data=%p, dataLen=%zu, width=%" PRIu64 ", height=%" PRIu64, data,
                  dataLen, (uint64_t)width, (uint64_t)height);
        return frame;
      }

      // Check for reasonable size limits to prevent memory issues
      if (width > 8192 || height > 8192) {
        ERROR_MSG("Video frame dimensions too large: %" PRIu64 "x%" PRIu64, (uint64_t)width, (uint64_t)height);
        return frame;
      }

      try {
        if (codec == "UYVY") {
          size_t expectedSize = width * height * 2;
          if (dataLen != expectedSize) {
            WARN_MSG("UYVY data size mismatch: got %zu, expected %zu", dataLen, expectedSize);
            return frame;
          }
          cv::Mat uyvy(height, width, CV_8UC2, const_cast<char *>(data));
          cv::cvtColor(uyvy, frame, cv::COLOR_YUV2BGR_UYVY);
        } else if (codec == "YUYV") {
          size_t expectedSize = width * height * 2;
          if (dataLen != expectedSize) {
            WARN_MSG("YUYV data size mismatch: got %zu, expected %zu", dataLen, expectedSize);
            return frame;
          }
          cv::Mat yuyv(height, width, CV_8UC2, const_cast<char *>(data));
          cv::cvtColor(yuyv, frame, cv::COLOR_YUV2BGR_YUYV);
        } else if (codec == "I420" || codec == "YUV420P") {
          size_t expectedSize = width * height * 3 / 2;
          if (dataLen != expectedSize) {
            WARN_MSG("I420 data size mismatch: got %zu, expected %zu", dataLen, expectedSize);
            return frame;
          }
          cv::Mat yuv420(height * 3 / 2, width, CV_8UC1, const_cast<char *>(data));
          cv::cvtColor(yuv420, frame, cv::COLOR_YUV2BGR_I420);
        } else if (codec == "NV12") {
          size_t expectedSize = width * height * 3 / 2;
          if (dataLen != expectedSize) {
            WARN_MSG("NV12 data size mismatch: got %zu, expected %zu", dataLen, expectedSize);
            return frame;
          }
          cv::Mat nv12(height * 3 / 2, width, CV_8UC1, const_cast<char *>(data));
          cv::cvtColor(nv12, frame, cv::COLOR_YUV2BGR_NV12);
        } else if (codec == "MJPEG" || codec == "JPEG") {
          std::vector<uchar> jpegData(data, data + dataLen);
          frame = cv::imdecode(jpegData, cv::IMREAD_COLOR);
          // JPEG decoding already produces BGR format, no conversion needed
        } else {
          ERROR_MSG("Unsupported video codec: %s", codec.c_str());
        }
      } catch (const cv::Exception & e) {
        ERROR_MSG("OpenCV error decoding %s frame: %s", codec.c_str(), e.what());
        frame = cv::Mat(); // Ensure empty frame is returned
      } catch (const std::exception & e) {
        ERROR_MSG("Error decoding %s frame: %s", codec.c_str(), e.what());
        frame = cv::Mat();
      }

      return frame;
    }

    // Template function to draw detections with optional tracking features
    template<typename DetectionType>
    cv::Mat drawDetectionsWithOptionalTracking(const cv::Mat & image, const std::vector<DetectionType> & detections,
                                               bool showTrackIds, bool showConfidence, bool withTracking) {
      cv::Mat result;
      image.copyTo(result);

      for (const auto & det : detections) {
        int x1 = static_cast<int>(det.x * image.cols);
        int y1 = static_cast<int>(det.y * image.rows);
        int x2 = static_cast<int>((det.x + det.w) * image.cols);
        int y2 = static_cast<int>((det.y + det.h) * image.rows);

        // Choose color based on mode
        cv::Scalar color;
        if (withTracking && det.track_id > 0) {
          // Color based on track confidence
          if (det.track_confidence > 0.8f) {
            color = cv::Scalar(0, 255, 0); // Green for stable tracks
          } else if (det.track_confidence > 0.5f) {
            color = cv::Scalar(0, 255, 255); // Yellow for medium confidence
          } else {
            color = cv::Scalar(0, 0, 255); // Red for new/unstable tracks
          }
        } else {
          // Color based on class ID
          switch (det.class_id % 6) {
            case 0: color = cv::Scalar(0, 0, 255); break; // Red
            case 1: color = cv::Scalar(0, 255, 0); break; // Green
            case 2: color = cv::Scalar(255, 0, 0); break; // Blue
            case 3: color = cv::Scalar(0, 255, 255); break; // Yellow
            case 4: color = cv::Scalar(255, 0, 255); break; // Magenta
            case 5: color = cv::Scalar(255, 255, 0); break; // Cyan
            default: color = cv::Scalar(255, 255, 255); break; // White
          }
        }

        // Draw bounding box
        int thickness = withTracking ? 2 : 3;
        cv::rectangle(result, cv::Point(x1, y1), cv::Point(x2, y2), color, thickness);

        // Draw trail if tracking mode and trail exists
        if (withTracking && det.trail.size() > 2) {
          int numSegments = std::min(static_cast<int>(det.trail.size()), 15);

          for (int i = 0; i < numSegments - 1; ++i) {
            float segmentPos = static_cast<float>(i) / (numSegments - 1);
            float alpha = (1.0f - segmentPos) * 0.7f;
            int trailThickness = static_cast<int>(6 * (1.0f - segmentPos) + 1);

            cv::Scalar segmentColor = color;
            float saturation = 1.0f - segmentPos * 0.6f;
            segmentColor *= saturation * alpha;

            int currentIndex = det.trail.size() - 1 - i;
            int nextIndex = det.trail.size() - 2 - i;

            if (currentIndex >= 0 && nextIndex >= 0 && currentIndex < det.trail.size() && nextIndex < det.trail.size()) {

              cv::Point2f currentPoint = det.trail[currentIndex];
              cv::Point2f nextPoint = det.trail[nextIndex];

              cv::Point currentPx(static_cast<int>(currentPoint.x * image.cols), static_cast<int>(currentPoint.y * image.rows));
              cv::Point nextPx(static_cast<int>(nextPoint.x * image.cols), static_cast<int>(nextPoint.y * image.rows));

              cv::line(result, currentPx, nextPx, segmentColor, trailThickness, cv::LINE_AA);
            }
          }
        }

        // Create label
        std::string label;
        if (std::is_same<DetectionType, Detection>::value) {
          label = det.class_name;
        } else {
          // For other detection types, use COCO classes or fallback
          if (det.class_id < COCO_CLASSES.size()) {
            label = COCO_CLASSES[det.class_id];
          } else {
            label = "obj" + std::to_string(det.class_id);
          }
        }

        if (showConfidence) { label += " " + std::to_string(static_cast<int>(det.confidence * 100)) + "%"; }

        if (showTrackIds && det.track_id > 0) {
          if (withTracking) {
            label += " ID:" + std::to_string(det.track_id);
            label += " TC:" + std::to_string(static_cast<int>(det.track_confidence * 100)) + "%";
            if (det.trail.size() > 0) { label += " T:" + std::to_string(det.trail.size()); }
          } else {
            label += " #" + std::to_string(det.track_id);
          }
        }

        // Draw label background and text
        int baseline = 0;
        float fontSize = withTracking ? 0.5f : 0.6f;
        int fontThickness = withTracking ? 1 : 2;
        cv::Size textSize = cv::getTextSize(label, cv::FONT_HERSHEY_SIMPLEX, fontSize, fontThickness, &baseline);
        cv::rectangle(result, cv::Point(x1, y1 - textSize.height - 10), cv::Point(x1 + textSize.width, y1), color, -1);
        cv::putText(result, label, cv::Point(x1, y1 - 5), cv::FONT_HERSHEY_SIMPLEX, fontSize, cv::Scalar(0, 0, 0), fontThickness);
      }

      return result;
    }

    JSON::Value detectionsToJSON(const std::vector<Detection> & detections, uint64_t timestamp,
                                 const InferenceMetrics & metrics, const std::string & modelName) {
      JSON::Value result;
      result.null();
      result["schema"] = "mist.onnx.result/v1";
      result["timestamp_ms"] = timestamp;
      result["model"]["name"] = modelName;
      result["kind"] = "object_detection";
      result["status"] = "ok";
      result["detections"].append(); result["detections"].shrink(0);

      for (const auto & det : detections) {
        JSON::Value detection;
        detection.null();

        // Basic detection data
        detection["bbox"]["x"] = det.x;
        detection["bbox"]["y"] = det.y;
        detection["bbox"]["w"] = det.w;
        detection["bbox"]["h"] = det.h;
        detection["confidence"] = det.confidence;
        detection["class_id"] = det.class_id;
        detection["class_name"] = det.class_name;

        // Rich tracking data (if tracked)
        if (det.track_id > 0) {
          detection["track_id"] = (uint64_t)det.track_id;

          // Temporal tracking information
          detection["tracking"]["track_confidence"] = det.track_confidence;
          detection["tracking"]["track_duration_ms"] = (uint64_t)det.getTrackDurationMs();
          detection["tracking"]["first_seen_ms"] = (uint64_t)det.first_seen_time;
          detection["tracking"]["last_seen_ms"] = (uint64_t)det.last_seen_time;
          detection["tracking"]["time_since_last_seen_ms"] = (uint64_t)det.getTimeSinceLastSeenMs(timestamp);

          // Trail/movement data
          detection["tracking"]["trail_length"] = static_cast<int>(det.trail.size());
          if (det.trail.size() > 0) {
            // Include recent trail points (last 5 for metadata efficiency)
            detection["tracking"]["recent_trail"].null();
            size_t startIdx = det.trail.size() > 5 ? det.trail.size() - 5 : 0;
            for (size_t i = startIdx; i < det.trail.size(); ++i) {
              JSON::Value point;
              point["x"] = det.trail[i].x;
              point["y"] = det.trail[i].y;
              detection["tracking"]["recent_trail"].append(point);
            }

            // Calculate movement metrics from trail
            if (det.trail.size() >= 2) {
              cv::Point2f current = det.trail.back();
              cv::Point2f previous = det.trail[det.trail.size() - 2];
              float movement = cv::norm(current - previous);
              detection["tracking"]["recent_movement"] = movement;

              // Calculate average speed over trail
              if (det.trail.size() >= 3) {
                float totalMovement = 0.0f;
                for (size_t i = 1; i < det.trail.size(); ++i) {
                  totalMovement += cv::norm(det.trail[i] - det.trail[i - 1]);
                }
                detection["tracking"]["avg_movement_speed"] = totalMovement / (det.trail.size() - 1);
              }
            }
          }

          // Kalman filter state
          detection["tracking"]["kalman_initialized"] = det.kalmanInitialized;
          if (det.kalmanInitialized && det.kalmanFilter) {
            // Get current Kalman state (position and velocity)
            cv::Mat state = det.kalmanFilter->statePost;
            if (state.rows >= 6) {
              detection["tracking"]["kalman_state"]["pos_x"] = state.at<float>(0);
              detection["tracking"]["kalman_state"]["pos_y"] = state.at<float>(1);
              detection["tracking"]["kalman_state"]["width"] = state.at<float>(2);
              detection["tracking"]["kalman_state"]["height"] = state.at<float>(3);
              detection["tracking"]["kalman_state"]["vel_x"] = state.at<float>(4);
              detection["tracking"]["kalman_state"]["vel_y"] = state.at<float>(5);

              // Calculate predicted velocity magnitude
              float vel_magnitude =
                std::sqrt(state.at<float>(4) * state.at<float>(4) + state.at<float>(5) * state.at<float>(5));
              detection["tracking"]["kalman_state"]["velocity_magnitude"] = vel_magnitude;
            }
          }

          // Object behavior analysis
          detection["tracking"]["is_stationary"] =
            (det.trail.size() > 3 && detection["tracking"]["avg_movement_speed"].asDouble() < 0.01);
          detection["tracking"]["is_new_track"] = (det.getTrackDurationMs() < 1000); // Less than 1 second
          detection["tracking"]["is_stable_track"] = (det.track_confidence > 0.8f && det.getTrackDurationMs() > 2000);
        }

        result["detections"].append(detection);
      }

      result["metrics"]["inference_ms"] = metrics.inferenceTimeMs;
      result["metrics"]["preprocess_ms"] = metrics.preprocessTimeMs;
      result["metrics"]["postprocess_ms"] = metrics.postprocessTimeMs;
      result["metrics"]["total_ms"] = metrics.totalTimeMs;

      // Add frame-level analytics
      result["frame_analytics"]["total_detections"] = static_cast<int>(detections.size());

      // Count tracked vs untracked
      int tracked_count = 0;
      int new_tracks = 0;
      int stable_tracks = 0;
      int stationary_objects = 0;

      for (const auto & det : detections) {
        if (det.track_id > 0) {
          tracked_count++;
          if (det.getTrackDurationMs() < 1000) new_tracks++;
          if (det.track_confidence > 0.8f && det.getTrackDurationMs() > 2000) stable_tracks++;
          if (det.trail.size() > 3) {
            // Calculate if stationary
            float totalMovement = 0.0f;
            for (size_t i = 1; i < det.trail.size(); ++i) {
              totalMovement += cv::norm(det.trail[i] - det.trail[i - 1]);
            }
            float avgMovement = totalMovement / (det.trail.size() - 1);
            if (avgMovement < 0.01) stationary_objects++;
          }
        }
      }

      result["frame_analytics"]["tracked_objects"] = tracked_count;
      result["frame_analytics"]["untracked_objects"] = static_cast<int>(detections.size()) - tracked_count;
      result["frame_analytics"]["new_tracks"] = new_tracks;
      result["frame_analytics"]["stable_tracks"] = stable_tracks;
      result["frame_analytics"]["stationary_objects"] = stationary_objects;

      return result;
    }

    // Template function to encode JPEG with detections and overlay
    template<typename DetectionType>
    std::vector<uchar> encodeJPEG(const cv::Mat & frame, const std::vector<DetectionType> & detections,
                                  const ProcessingStats & stats, int quality, InferenceMetrics *metrics) {
      auto jpegStart = std::chrono::high_resolution_clock::now();

      // Draw detections on frame with tracking information (trails, etc.)
      cv::Mat frameWithDetections;
      if (std::is_same<DetectionType, Detection>::value) {
        frameWithDetections = Utils::drawDetectionsWithOptionalTracking(
          reinterpret_cast<const cv::Mat &>(frame), reinterpret_cast<const std::vector<Detection> &>(detections), true, true, true);
      } else if (std::is_same<DetectionType, SegmentationDetection>::value) {
        frameWithDetections = Utils::drawSegmentationDetectionsWithTracking(
          reinterpret_cast<const cv::Mat &>(frame),
          reinterpret_cast<const std::vector<SegmentationDetection> &>(detections), true, true);
      } else {
        // Fallback for other detection types
        frame.copyTo(frameWithDetections);
      }

      // Add overlay with frame time, FPS, and processing stats
      {
        std::lock_guard<std::mutex> lock(stats.statsMutex);

        // Calculate current time and FPS
        auto now = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
        uint64_t currentTimeMs = duration.count();

        // Calculate FPS from recent frame times
        float currentFPS = 0.0f;
        if (stats.totalFrames > 0 && stats.totalInferenceTimeMs > 0) {
          currentFPS = (stats.totalFrames * 1000.0f) / stats.totalInferenceTimeMs;
        }

        // Prepare overlay text
        std::vector<std::string> overlayLines;
        overlayLines.push_back("Time: " + std::to_string(currentTimeMs) + "ms");
        overlayLines.push_back("FPS: " + std::to_string(static_cast<int>(currentFPS)));
        overlayLines.push_back("Detections: " + std::to_string(detections.size()));
        overlayLines.push_back("Total Frames: " + std::to_string(stats.totalFrames));

        if (metrics) {
          overlayLines.push_back("Inference: " + std::to_string(metrics->inferenceTimeMs) + "ms");
          overlayLines.push_back("Total: " + std::to_string(metrics->totalTimeMs) + "ms");
        }

        // Draw overlay background
        int lineHeight = 25;
        int padding = 10;
        int overlayHeight = overlayLines.size() * lineHeight + 2 * padding;
        int overlayWidth = 300;

        cv::Rect overlayRect(10, 10, overlayWidth, overlayHeight);
        Utils::drawFilledRectAlpha(frameWithDetections, overlayRect, cv::Scalar(0, 0, 0), 0.5);

        // Draw overlay text
        for (size_t i = 0; i < overlayLines.size(); ++i) {
          cv::Point textPos(20, 35 + i * lineHeight);
          cv::putText(frameWithDetections, overlayLines[i], textPos, cv::FONT_HERSHEY_SIMPLEX, 0.6,
                      cv::Scalar(255, 255, 255), 1, cv::LINE_AA);
        }
      }

      // Encode as JPEG
      std::vector<uchar> jpegData;
      std::vector<int> compressionParams = {cv::IMWRITE_JPEG_QUALITY, quality};
      bool success = cv::imencode(".jpg", frameWithDetections, jpegData, compressionParams);

      auto jpegEnd = std::chrono::high_resolution_clock::now();
      if (metrics) {
        metrics->jpegEncodeTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(jpegEnd - jpegStart).count();
      }

      if (!success) {
        ERROR_MSG("Failed to encode JPEG with detections");
        return {};
      }

      return jpegData;
    }

    JSON::Value genericResultToJSON(const GenericResult & result) {
      JSON::Value json;
      json.null();
      json["schema"] = "mist.onnx.result/v1";
      json["timestamp_ms"] = (uint64_t)result.timestamp;
      json["model"]["name"] = result.modelName;
      json["kind"] = result.modelType;
      json["status"] = "ok";
      json["raw_output"] = result.rawOutput;
      // Performance metrics removed - they belong in logs/monitoring, not metadata
      return json;
    }

    JSON::Value depthResultToJSON(const DepthResult & result, uint64_t timestamp, const std::string & modelName) {
      JSON::Value json;
      json.null();
      json["schema"] = "mist.onnx.result/v1";
      json["timestamp_ms"] = (uint64_t)timestamp;
      json["model"]["name"] = modelName;
      json["kind"] = "depth_estimation";
      json["status"] = "ok";
      json["depth_map_width"] = (uint64_t)result.depthMap.cols;
      json["depth_map_height"] = (uint64_t)result.depthMap.rows;
      json["metrics"]["inference_ms"] = (int64_t)result.metrics.inferenceTimeMs;
      json["metrics"]["total_ms"] = (int64_t)result.metrics.totalTimeMs;
      return json;
    }

    JSON::Value faceDetectionsToJSON(const std::vector<FaceDetection> & detections, uint64_t timestamp,
                                     const InferenceMetrics & metrics, const std::string & modelName) {
      JSON::Value json;
      json.null();
      json["schema"] = "mist.onnx.result/v1";
      json["timestamp_ms"] = (uint64_t)timestamp;
      json["model"]["name"] = modelName;
      json["kind"] = "face_detection";
      json["status"] = "ok";
      json["detection_count"] = (uint64_t)detections.size();
      json["metrics"]["inference_ms"] = (int64_t)metrics.inferenceTimeMs;
      json["metrics"]["total_ms"] = (int64_t)metrics.totalTimeMs;
      json["detections"].append(); json["detections"].shrink(0);
      for (size_t i = 0; i < detections.size(); ++i) {
        const auto & d = detections[i];
        JSON::Value det;
        det["bbox"]["x"] = d.x;
        det["bbox"]["y"] = d.y;
        det["bbox"]["w"] = d.w;
        det["bbox"]["h"] = d.h;
        det["confidence"] = d.confidence;
        det["class_name"] = d.class_name;
        if (d.track_id > 0) { det["track_id"] = (uint64_t)d.track_id; }
        JSON::Value lm;
        for (int k = 0; k < 5; ++k) {
          JSON::Value pt;
          pt["x"] = d.landmarks[k * 2];
          pt["y"] = d.landmarks[k * 2 + 1];
          lm.append(pt);
        }
        det["landmarks"] = lm;
        json["detections"].append(det);
      }
      return json;
    }

    JSON::Value faceEmbeddingToJSON(const FaceEmbedding & embedding, uint64_t timestamp, const std::string & modelName) {
      JSON::Value json;
      json.null();
      json["schema"] = "mist.onnx.result/v1";
      json["timestamp_ms"] = (uint64_t)timestamp;
      json["model"]["name"] = modelName;
      json["kind"] = "face_recognition";
      json["status"] = "ok";
      json["embedding_dim"] = (uint64_t)embedding.embedding.size();
      // Full vector: consumers do cosine matching from the metadata alone.
      for (size_t i = 0; i < embedding.embedding.size(); ++i) {
        json["embedding"].append(embedding.embedding[i]);
      }
      json["confidence"] = embedding.confidence;
      json["metrics"]["inference_ms"] = (int64_t)embedding.metrics.inferenceTimeMs;
      json["metrics"]["total_ms"] = (int64_t)embedding.metrics.totalTimeMs;
      return json;
    }

    JSON::Value ocrResultToJSON(const OCRResult & result, uint64_t timestamp, const std::string & modelName) {
      JSON::Value json;
      json["schema"] = "mist.onnx.result/v1";
      json["timestamp_ms"] = timestamp;
      json["model"]["name"] = modelName;
      json["kind"] = "ocr";
      json["status"] = result.ok ? "ok" : "failed";
      json["text"] = result.text;
      // Force an empty ARRAY (not null): null() alone serializes as JSON null, so a
      // no-text frame must still present lines:[] rather than lines:null. append+shrink
      // sets the ARRAY type with zero elements.
      json["lines"].append();
      json["lines"].shrink(0);
      for (size_t i = 0; i < result.lines.size(); ++i) {
        JSON::Value l;
        l["text"] = result.lines[i].text;
        l["confidence"] = result.lines[i].confidence;
        l["bbox"]["x"] = result.lines[i].x;
        l["bbox"]["y"] = result.lines[i].y;
        l["bbox"]["w"] = result.lines[i].w;
        l["bbox"]["h"] = result.lines[i].h;
        json["lines"].append(l);
      }
      json["metrics"]["inference_ms"] = result.metrics.inferenceTimeMs;
      json["metrics"]["total_ms"] = result.metrics.totalTimeMs;
      return json;
    }

    JSON::Value audioResultToJSON(const AudioResult & result, const std::string & modelName,
                                  const std::string & kind) {
      JSON::Value json;
      json["schema"] = "mist.onnx.result/v1";
      json["timestamp_ms"] = result.startMs;
      json["window"]["start_ms"] = result.startMs;
      json["window"]["end_ms"] = result.endMs;
      json["model"]["name"] = modelName;
      json["kind"] = kind;
      json["status"] = result.ok ? "ok" : "failed";
      for (size_t i = 0; i < result.scores.size(); ++i) {
        JSON::Value s;
        s["class_id"] = result.scores[i].class_id;
        s["class_name"] = result.scores[i].class_name;
        s["confidence"] = result.scores[i].confidence;
        json["scores"].append(s);
      }
      if (!result.embedding.empty()) {
        json["embedding_dim"] = (uint64_t)result.embedding.size();
        for (size_t i = 0; i < result.embedding.size(); ++i) {
          json["embedding"].append(result.embedding[i]);
        }
      }
      json["metrics"]["inference_ms"] = result.metrics.inferenceTimeMs;
      json["metrics"]["total_ms"] = result.metrics.totalTimeMs;
      return json;
    }

    JSON::Value eventToJSON(const std::string & label, bool started, float score,
                            uint64_t timestamp, const std::string & modelName) {
      JSON::Value json;
      json["schema"] = "mist.onnx.result/v1";
      json["timestamp_ms"] = timestamp;
      json["model"]["name"] = modelName;
      json["kind"] = "event";
      json["status"] = "ok";
      json["event"]["label"] = label;
      json["event"]["state"] = started ? "started" : "ended";
      json["event"]["score"] = score;
      return json;
    }

    JSON::Value sam2ResultToJSON(const SAM2Result & result, uint64_t timestamp, const std::string & modelName) {
      JSON::Value json;
      json.null();
      json["schema"] = "mist.onnx.result/v1";
      json["timestamp_ms"] = (uint64_t)timestamp;
      json["model"]["name"] = modelName;
      json["kind"] = "sam2";
      json["status"] = "ok";
      json["num_masks"] = (uint64_t)result.masks.size();
      JSON::Value scores;
      for (size_t i = 0; i < result.iouScores.size(); ++i) {
        scores.append(result.iouScores[i]);
      }
      json["iou_scores"] = scores;
      json["metrics"]["inference_ms"] = (int64_t)result.metrics.inferenceTimeMs;
      json["metrics"]["total_ms"] = (int64_t)result.metrics.totalTimeMs;
      return json;
    }

    cv::Mat drawDepthVisualization(const cv::Mat & frame, const DepthResult & result) {
      if (result.depthMap.empty()) return frame.clone();

      cv::Mat depthVis;
      cv::Mat depth8u;
      result.depthMap.convertTo(depth8u, CV_8UC1, 255.0);
      cv::applyColorMap(depth8u, depthVis, cv::COLORMAP_INFERNO);

      if (depthVis.size() != frame.size()) {
        cv::resize(depthVis, depthVis, frame.size());
      }

      // Blend original + depth colormap
      cv::Mat blended;
      cv::addWeighted(frame, 0.3, depthVis, 0.7, 0, blended);
      return blended;
    }

    cv::Mat drawFaceDetections(const cv::Mat & image, const std::vector<FaceDetection> & detections, bool showConfidence) {
      cv::Mat output = image.clone();
      for (const auto & d : detections) {
        int x = (int)(d.x * output.cols);
        int y = (int)(d.y * output.rows);
        int w = (int)(d.w * output.cols);
        int h = (int)(d.h * output.rows);

        cv::rectangle(output, cv::Rect(x, y, w, h), cv::Scalar(0, 255, 0), 2);

        if (showConfidence) {
          char buf[64];
          snprintf(buf, sizeof(buf), "face %.0f%%", d.confidence * 100);
          cv::putText(output, buf, cv::Point(x, y - 5), cv::FONT_HERSHEY_SIMPLEX, 0.5,
                      cv::Scalar(0, 255, 0), 1, cv::LINE_AA);
        }

        // Draw landmarks as small circles
        for (int k = 0; k < 5; ++k) {
          int lx = (int)(d.landmarks[k * 2] * output.cols);
          int ly = (int)(d.landmarks[k * 2 + 1] * output.rows);
          cv::circle(output, cv::Point(lx, ly), 3, cv::Scalar(0, 0, 255), -1);
        }
      }
      return output;
    }

    std::pair<JSON::Value, ProcessedVideoFrame>
      processVideoPacketAuto(const VideoPacket & packet, DetectionModel & model, TemporalTracker & tracker,
                             ProcessingStats & stats, Utils::SceneChangeDetector & sceneDetector, float confThreshold,
                             float nmsThreshold, bool enhanceImage, int jpegQuality, bool annotatedVideo,
                             bool trackingEnabled, bool sceneChangeEnabled) {
      JSON::Value metadata;
      ProcessedVideoFrame processedFrame;

      // Start timing for preprocessing (including video decoding)
      auto preprocessStart = std::chrono::high_resolution_clock::now();

      // Decode video frame
      cv::Mat frame =
        decodeVideoFrame((const char *)packet.packetData, packet.packetData.size(), packet.codec, packet.width, packet.height);

      if (frame.empty()) {
        WARN_MSG("Failed to decode video frame");
        metadata.null();
        return std::make_pair(metadata, processedFrame);
      }

      // Additional validation of decoded frame
      if (frame.channels() != 3) {
        ERROR_MSG("Decoded frame has wrong number of channels: %d (expected 3)", frame.channels());
        metadata.null();
        return std::make_pair(metadata, processedFrame);
      }

      if (frame.type() != CV_8UC3) {
        ERROR_MSG("Decoded frame has wrong type: %d (expected CV_8UC3=%d)", frame.type(), CV_8UC3);
        metadata.null();
        return std::make_pair(metadata, processedFrame);
      }

      if (frame.cols <= 0 || frame.rows <= 0) {
        ERROR_MSG("Decoded frame has invalid dimensions: %dx%d", frame.cols, frame.rows);
        metadata.null();
        return std::make_pair(metadata, processedFrame);
      }

      auto preprocessEnd = std::chrono::high_resolution_clock::now();
      int64_t videoDecodeTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(preprocessEnd - preprocessStart).count();

      VERYHIGH_MSG("Decoded frame: %dx%d, type=%d, channels=%d, codec=%s (decode time: %" PRId64 "ms)", frame.cols,
                   frame.rows, frame.type(), frame.channels(), packet.codec.c_str(), (int64_t)videoDecodeTimeMs);

      // Check model type and process accordingly
      ModelType modelType = model.getModelType();

      if (modelType == ModelType::YOLOV8_DETECTION || modelType == ModelType::YOLOV8_POSE ||
          modelType == ModelType::YOLOV8_SEGMENTATION || modelType == ModelType::YOLOV8_CLASSIFICATION ||
          modelType == ModelType::YOLOV8_OBB || modelType == ModelType::YOLO11_DETECTION ||
          modelType == ModelType::YOLO11_POSE || modelType == ModelType::YOLO11_SEGMENTATION ||
          modelType == ModelType::YOLO11_CLASSIFICATION || modelType == ModelType::YOLO11_OBB ||
          modelType == ModelType::YOLO_NMS_DETECTION || modelType == ModelType::YOLO_SPLIT_DETECTION) {

        // Handle segmentation models specially
        if (modelType == ModelType::YOLOV8_SEGMENTATION || modelType == ModelType::YOLO11_SEGMENTATION) {
          // Cast to segmentation model and use specialized processing
          YOLOv8SegmentationModel *segModel = dynamic_cast<YOLOv8SegmentationModel *>(&model);
          if (segModel) {
            InferenceMetrics metrics;
            std::vector<SegmentationDetection> segDetections =
              segModel->processSegmentationFrame(frame, confThreshold, nmsThreshold, &metrics);

            // Add video decode time to metrics
            metrics.videoDecodeTimeMs = videoDecodeTimeMs;

            // Convert segmentation detections to regular detections for tracking
            std::vector<Detection> detections;
            for (const auto & seg : segDetections) { detections.push_back(static_cast<Detection>(seg)); }

            // Time scene change detection
            auto sceneChangeStart = std::chrono::high_resolution_clock::now();
            bool sceneChanged = sceneChangeEnabled && Utils::detectSceneChange(sceneDetector, detections, packet.timestamp);
            auto sceneChangeEnd = std::chrono::high_resolution_clock::now();
            metrics.sceneChangeTimeMs =
              std::chrono::duration_cast<std::chrono::milliseconds>(sceneChangeEnd - sceneChangeStart).count();

            // Check for scene change BEFORE temporal tracking
            if (sceneChanged) {
              INFO_MSG("Scene change detected at timestamp %" PRIu64 ", performing soft reset", (uint64_t)packet.timestamp);
              tracker.softReset(detections, packet.timestamp);
              // Increment scene change counter in stats
              std::lock_guard<std::mutex> lock(stats.statsMutex);
              stats.sceneChangesDetected++;
            }

            // Apply temporal tracking
            auto trackingStart = std::chrono::high_resolution_clock::now();
            size_t tracksBefore = tracker.getTrackCount();
            std::vector<Detection> trackedDetections = trackIfEnabled(tracker, detections, packet.timestamp, trackingEnabled);
            size_t tracksAfter = tracker.getTrackCount();
            auto trackingEnd = std::chrono::high_resolution_clock::now();

            metrics.temporalTrackingTimeMs =
              std::chrono::duration_cast<std::chrono::milliseconds>(trackingEnd - trackingStart).count();
            metrics.trackedObjectCount = trackedDetections.size();

            // Calculate new and lost tracks
            if (tracksAfter > tracksBefore) { metrics.newTrackCount = tracksAfter - tracksBefore; }
            if (tracksBefore > tracksAfter) { metrics.lostTrackCount = tracksBefore - tracksAfter; }

            // Update segmentation detections with tracking info
            std::vector<SegmentationDetection> trackedSegDetections;
            for (size_t i = 0; i < segDetections.size() && i < trackedDetections.size(); ++i) {
              SegmentationDetection trackedSeg = segDetections[i];

              // Copy all tracking data from the tracked detection
              trackedSeg.track_id = trackedDetections[i].track_id;
              trackedSeg.first_seen_time = trackedDetections[i].first_seen_time;
              trackedSeg.last_seen_time = trackedDetections[i].last_seen_time;
              trackedSeg.track_confidence = trackedDetections[i].track_confidence;

              // Copy trail data for drawing trails
              trackedSeg.trail = trackedDetections[i].trail;

              // Copy Kalman filter state for prediction
              trackedSeg.kalmanFilter = trackedDetections[i].kalmanFilter;
              trackedSeg.kalmanInitialized = trackedDetections[i].kalmanInitialized;

              trackedSegDetections.push_back(trackedSeg);
            }

            // Create processed video frame with segmentation detections
            std::vector<uchar> jpegData;
            if (annotatedVideo) { jpegData = Utils::encodeJPEG(frame, trackedSegDetections, stats, jpegQuality, &metrics); }
            if (!jpegData.empty()) {
              processedFrame.jpegData.assign(jpegData.data(), jpegData.size());
              processedFrame.timestamp = packet.timestamp;
              processedFrame.width = packet.width;
              processedFrame.height = packet.height;
              processedFrame.detectionCount = trackedSegDetections.size();
            }

            // Update statistics
            stats.updateStats(metrics, trackedSegDetections.size(), packet.codec, packet.width, packet.height);

            // Create segmentation-specific metadata
            metadata =
              Utils::segmentationDetectionsToJSON(trackedSegDetections, packet.timestamp, metrics, model.getModelInfo().name);

          } else {
            ERROR_MSG("Failed to cast to YOLOv8SegmentationModel");
            metadata.null();
            return std::make_pair(metadata, processedFrame);
          }
        } else if (modelType == ModelType::YOLOV8_CLASSIFICATION || modelType == ModelType::YOLO11_CLASSIFICATION) {
          // Handle classification models specially
          YOLOv8ClassificationModel *clsModel = dynamic_cast<YOLOv8ClassificationModel *>(&model);
          if (clsModel) {
            InferenceMetrics metrics;
            ClassificationResult result = clsModel->processClassificationFrame(frame, &metrics);
            result.timestamp = packet.timestamp;

            // Add video decode time to metrics
            metrics.videoDecodeTimeMs = videoDecodeTimeMs;

            // Classification models don't need scene change detection or tracking
            metrics.temporalTrackingTimeMs = 0;
            metrics.sceneChangeTimeMs = 0;
            metrics.trackedObjectCount = 0;
            metrics.newTrackCount = 0;
            metrics.lostTrackCount = 0;

            // Create processed video frame with classification result
            cv::Mat frameWithResult;
            if (annotatedVideo) { frameWithResult = Utils::drawClassificationResult(frame, result); }

            // Add stats overlay (same approach as other encoding functions)
            if (annotatedVideo) {
              std::lock_guard<std::mutex> lock(stats.statsMutex);

              // Calculate current time and FPS
              auto now = std::chrono::steady_clock::now();
              auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
              uint64_t currentTimeMs = duration.count();

              // Calculate FPS from recent frame times
              float currentFPS = 0.0f;
              if (stats.totalFrames > 0 && stats.totalInferenceTimeMs > 0) {
                currentFPS = (stats.totalFrames * 1000.0f) / stats.totalInferenceTimeMs;
              }

              // Prepare overlay text
              std::vector<std::string> overlayLines;
              overlayLines.push_back("Time: " + std::to_string(currentTimeMs) + "ms");
              overlayLines.push_back("FPS: " + std::to_string(static_cast<int>(currentFPS)));
              overlayLines.push_back("Classification: " + result.class_name);
              overlayLines.push_back("Confidence: " + std::to_string(static_cast<int>(result.confidence * 100)) + "%");
              overlayLines.push_back("Total Frames: " + std::to_string(stats.totalFrames));
              overlayLines.push_back("Inference: " + std::to_string(metrics.inferenceTimeMs) + "ms");
              overlayLines.push_back("Total: " + std::to_string(metrics.totalTimeMs) + "ms");

              // Draw overlay background
              int lineHeight = 25;
              int padding = 10;
              int overlayHeight = overlayLines.size() * lineHeight + 2 * padding;
              int overlayWidth = 300;

              cv::Rect overlayRect(10, 10, overlayWidth, overlayHeight);
              Utils::drawFilledRectAlpha(frameWithResult, overlayRect, cv::Scalar(0, 0, 0), 0.5);

              // Draw overlay text
              for (size_t i = 0; i < overlayLines.size(); ++i) {
                cv::Point textPos(20, 35 + i * lineHeight);
                cv::putText(frameWithResult, overlayLines[i], textPos, cv::FONT_HERSHEY_SIMPLEX, 0.6,
                            cv::Scalar(255, 255, 255), 1, cv::LINE_AA);
              }
            }

            // Encode to JPEG
            auto jpegStart = std::chrono::high_resolution_clock::now();
            std::vector<int> jpegParams = {cv::IMWRITE_JPEG_QUALITY, jpegQuality};
            std::vector<uchar> jpegData;

            if (annotatedVideo && cv::imencode(".jpg", frameWithResult, jpegData, jpegParams)) {
              auto jpegEnd = std::chrono::high_resolution_clock::now();
              metrics.jpegEncodeTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(jpegEnd - jpegStart).count();

              processedFrame.jpegData.assign(jpegData.data(), jpegData.size());
              processedFrame.timestamp = packet.timestamp;
              processedFrame.width = packet.width;
              processedFrame.height = packet.height;
              processedFrame.detectionCount = 0; // Classification doesn't have detections
            }

            // Update statistics
            stats.updateStats(metrics, 0, packet.codec, packet.width, packet.height);

            // Create classification-specific metadata
            metadata = Utils::classificationToJSON(result, metrics, model.getModelInfo().name);

          } else {
            ERROR_MSG("Failed to cast to YOLOv8ClassificationModel");
            metadata.null();
            return std::make_pair(metadata, processedFrame);
          }
        } else if (modelType == ModelType::YOLOV8_POSE || modelType == ModelType::YOLO11_POSE) {
          // Handle pose models specially
          YOLOv8PoseModel *poseModel = dynamic_cast<YOLOv8PoseModel *>(&model);
          if (poseModel) {
            InferenceMetrics metrics;
            std::vector<PoseDetection> poseDetections = poseModel->processPoseFrame(frame, confThreshold, nmsThreshold, &metrics);

            // Add video decode time to metrics
            metrics.videoDecodeTimeMs = videoDecodeTimeMs;

            // Convert pose detections to regular detections for tracking
            std::vector<Detection> detections;
            for (const auto & pose : poseDetections) { detections.push_back(static_cast<Detection>(pose)); }

            // Time scene change detection
            auto sceneChangeStart = std::chrono::high_resolution_clock::now();
            bool sceneChanged = sceneChangeEnabled && Utils::detectSceneChange(sceneDetector, detections, packet.timestamp);
            auto sceneChangeEnd = std::chrono::high_resolution_clock::now();
            metrics.sceneChangeTimeMs =
              std::chrono::duration_cast<std::chrono::milliseconds>(sceneChangeEnd - sceneChangeStart).count();

            // Check for scene change BEFORE temporal tracking
            if (sceneChanged) {
              INFO_MSG("Scene change detected at timestamp %" PRIu64 ", performing soft reset", (uint64_t)packet.timestamp);
              tracker.softReset(detections, packet.timestamp);
              // Increment scene change counter in stats
              std::lock_guard<std::mutex> lock(stats.statsMutex);
              stats.sceneChangesDetected++;
            }

            // Apply temporal tracking
            auto trackingStart = std::chrono::high_resolution_clock::now();
            size_t tracksBefore = tracker.getTrackCount();
            std::vector<Detection> trackedDetections = trackIfEnabled(tracker, detections, packet.timestamp, trackingEnabled);
            size_t tracksAfter = tracker.getTrackCount();
            auto trackingEnd = std::chrono::high_resolution_clock::now();

            metrics.temporalTrackingTimeMs =
              std::chrono::duration_cast<std::chrono::milliseconds>(trackingEnd - trackingStart).count();
            metrics.trackedObjectCount = trackedDetections.size();

            // Calculate new and lost tracks
            if (tracksAfter > tracksBefore) { metrics.newTrackCount = tracksAfter - tracksBefore; }
            if (tracksBefore > tracksAfter) { metrics.lostTrackCount = tracksBefore - tracksAfter; }

            // Update pose detections with tracking info
            std::vector<PoseDetection> trackedPoseDetections;
            for (size_t i = 0; i < poseDetections.size() && i < trackedDetections.size(); ++i) {
              PoseDetection trackedPose = poseDetections[i];

              // Copy all tracking data from the tracked detection
              trackedPose.track_id = trackedDetections[i].track_id;
              trackedPose.first_seen_time = trackedDetections[i].first_seen_time;
              trackedPose.last_seen_time = trackedDetections[i].last_seen_time;
              trackedPose.track_confidence = trackedDetections[i].track_confidence;

              // Copy trail data for drawing trails
              trackedPose.trail = trackedDetections[i].trail;

              // Copy Kalman filter state for prediction
              trackedPose.kalmanFilter = trackedDetections[i].kalmanFilter;
              trackedPose.kalmanInitialized = trackedDetections[i].kalmanInitialized;

              trackedPoseDetections.push_back(trackedPose);
            }

            // Create processed video frame with pose detections
            cv::Mat frameWithPoses;
            if (annotatedVideo) {
              frameWithPoses = Utils::drawPoseDetectionsWithTracking(frame, trackedPoseDetections, true, true);
            }

            // Add stats overlay (same approach as other encoding functions)
            if (annotatedVideo) {
              std::lock_guard<std::mutex> lock(stats.statsMutex);

              // Calculate current time and FPS
              auto now = std::chrono::steady_clock::now();
              auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
              uint64_t currentTimeMs = duration.count();

              // Calculate FPS from recent frame times
              float currentFPS = 0.0f;
              if (stats.totalFrames > 0 && stats.totalInferenceTimeMs > 0) {
                currentFPS = (stats.totalFrames * 1000.0f) / stats.totalInferenceTimeMs;
              }

              // Prepare overlay text
              std::vector<std::string> overlayLines;
              overlayLines.push_back("Time: " + std::to_string(currentTimeMs) + "ms");
              overlayLines.push_back("FPS: " + std::to_string(static_cast<int>(currentFPS)));
              overlayLines.push_back("Poses: " + std::to_string(trackedPoseDetections.size()));
              overlayLines.push_back("Total Frames: " + std::to_string(stats.totalFrames));
              overlayLines.push_back("Inference: " + std::to_string(metrics.inferenceTimeMs) + "ms");
              overlayLines.push_back("Total: " + std::to_string(metrics.totalTimeMs) + "ms");

              // Draw overlay background
              int lineHeight = 25;
              int padding = 10;
              int overlayHeight = overlayLines.size() * lineHeight + 2 * padding;
              int overlayWidth = 300;

              cv::Rect overlayRect(10, 10, overlayWidth, overlayHeight);
              Utils::drawFilledRectAlpha(frameWithPoses, overlayRect, cv::Scalar(0, 0, 0), 0.5);

              // Draw overlay text
              for (size_t i = 0; i < overlayLines.size(); ++i) {
                cv::Point textPos(20, 35 + i * lineHeight);
                cv::putText(frameWithPoses, overlayLines[i], textPos, cv::FONT_HERSHEY_SIMPLEX, 0.6,
                            cv::Scalar(255, 255, 255), 1, cv::LINE_AA);
              }
            }

            // Encode to JPEG
            auto jpegStart = std::chrono::high_resolution_clock::now();
            std::vector<int> jpegParams = {cv::IMWRITE_JPEG_QUALITY, jpegQuality};
            std::vector<uchar> jpegData;

            if (annotatedVideo && cv::imencode(".jpg", frameWithPoses, jpegData, jpegParams)) {
              auto jpegEnd = std::chrono::high_resolution_clock::now();
              metrics.jpegEncodeTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(jpegEnd - jpegStart).count();

              processedFrame.jpegData.assign(jpegData.data(), jpegData.size());
              processedFrame.timestamp = packet.timestamp;
              processedFrame.width = packet.width;
              processedFrame.height = packet.height;
              processedFrame.detectionCount = trackedPoseDetections.size();
            }

            // Update statistics
            stats.updateStats(metrics, trackedPoseDetections.size(), packet.codec, packet.width, packet.height);

            // Create pose-specific metadata
            metadata =
              Utils::poseDetectionsToJSON(trackedPoseDetections, packet.timestamp, metrics, model.getModelInfo().name);

          } else {
            ERROR_MSG("Failed to cast to YOLOv8PoseModel");
            metadata.null();
            return std::make_pair(metadata, processedFrame);
          }
        } else if (modelType == ModelType::YOLOV8_OBB || modelType == ModelType::YOLO11_OBB) {
          // Handle OBB models specially
          YOLOv8OBBModel *obbModel = dynamic_cast<YOLOv8OBBModel *>(&model);
          if (obbModel) {
            InferenceMetrics metrics;
            std::vector<OBBDetection> obbDetections = obbModel->processOBBFrame(frame, confThreshold, nmsThreshold, &metrics);

            // Add video decode time to metrics
            metrics.videoDecodeTimeMs = videoDecodeTimeMs;

            // Convert OBB detections to regular detections for tracking
            std::vector<Detection> detections;
            for (const auto & obb : obbDetections) { detections.push_back(static_cast<Detection>(obb)); }

            // Time scene change detection
            auto sceneChangeStart = std::chrono::high_resolution_clock::now();
            bool sceneChanged = sceneChangeEnabled && Utils::detectSceneChange(sceneDetector, detections, packet.timestamp);
            auto sceneChangeEnd = std::chrono::high_resolution_clock::now();
            metrics.sceneChangeTimeMs =
              std::chrono::duration_cast<std::chrono::milliseconds>(sceneChangeEnd - sceneChangeStart).count();

            // Check for scene change BEFORE temporal tracking
            if (sceneChanged) {
              INFO_MSG("Scene change detected at timestamp %" PRIu64 ", performing soft reset", (uint64_t)packet.timestamp);
              tracker.softReset(detections, packet.timestamp);
              // Increment scene change counter in stats
              std::lock_guard<std::mutex> lock(stats.statsMutex);
              stats.sceneChangesDetected++;
            }

            // Apply temporal tracking
            auto trackingStart = std::chrono::high_resolution_clock::now();
            size_t tracksBefore = tracker.getTrackCount();
            std::vector<Detection> trackedDetections = trackIfEnabled(tracker, detections, packet.timestamp, trackingEnabled);
            size_t tracksAfter = tracker.getTrackCount();
            auto trackingEnd = std::chrono::high_resolution_clock::now();

            metrics.temporalTrackingTimeMs =
              std::chrono::duration_cast<std::chrono::milliseconds>(trackingEnd - trackingStart).count();
            metrics.trackedObjectCount = trackedDetections.size();

            // Calculate new and lost tracks
            if (tracksAfter > tracksBefore) { metrics.newTrackCount = tracksAfter - tracksBefore; }
            if (tracksBefore > tracksAfter) { metrics.lostTrackCount = tracksBefore - tracksAfter; }

            // Update OBB detections with tracking info
            std::vector<OBBDetection> trackedOBBDetections;
            for (size_t i = 0; i < obbDetections.size() && i < trackedDetections.size(); ++i) {
              OBBDetection trackedOBB = obbDetections[i];

              // Copy all tracking data from the tracked detection
              trackedOBB.track_id = trackedDetections[i].track_id;
              trackedOBB.first_seen_time = trackedDetections[i].first_seen_time;
              trackedOBB.last_seen_time = trackedDetections[i].last_seen_time;
              trackedOBB.track_confidence = trackedDetections[i].track_confidence;

              // Copy trail data for drawing trails
              trackedOBB.trail = trackedDetections[i].trail;

              // Copy Kalman filter state for prediction
              trackedOBB.kalmanFilter = trackedDetections[i].kalmanFilter;
              trackedOBB.kalmanInitialized = trackedDetections[i].kalmanInitialized;

              trackedOBBDetections.push_back(trackedOBB);
            }

            // Create processed video frame with OBB detections
            cv::Mat frameWithOBBs;
            if (annotatedVideo) {
              frameWithOBBs = Utils::drawOBBDetectionsWithTracking(frame, trackedOBBDetections, true, true);
            }

            // Add stats overlay (same approach as other encoding functions)
            if (annotatedVideo) {
              std::lock_guard<std::mutex> lock(stats.statsMutex);

              // Calculate current time and FPS
              auto now = std::chrono::steady_clock::now();
              auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
              uint64_t currentTimeMs = duration.count();

              // Calculate FPS from recent frame times
              float currentFPS = 0.0f;
              if (stats.totalFrames > 0 && stats.totalInferenceTimeMs > 0) {
                currentFPS = (stats.totalFrames * 1000.0f) / stats.totalInferenceTimeMs;
              }

              // Prepare overlay text
              std::vector<std::string> overlayLines;
              overlayLines.push_back("Time: " + std::to_string(currentTimeMs) + "ms");
              overlayLines.push_back("FPS: " + std::to_string(static_cast<int>(currentFPS)));
              overlayLines.push_back("OBBs: " + std::to_string(trackedOBBDetections.size()));
              overlayLines.push_back("Total Frames: " + std::to_string(stats.totalFrames));
              overlayLines.push_back("Inference: " + std::to_string(metrics.inferenceTimeMs) + "ms");
              overlayLines.push_back("Total: " + std::to_string(metrics.totalTimeMs) + "ms");

              // Draw overlay background
              int lineHeight = 25;
              int padding = 10;
              int overlayHeight = overlayLines.size() * lineHeight + 2 * padding;
              int overlayWidth = 300;

              cv::Rect overlayRect(10, 10, overlayWidth, overlayHeight);
              Utils::drawFilledRectAlpha(frameWithOBBs, overlayRect, cv::Scalar(0, 0, 0), 0.5);

              // Draw overlay text
              for (size_t i = 0; i < overlayLines.size(); ++i) {
                cv::Point textPos(20, 35 + i * lineHeight);
                cv::putText(frameWithOBBs, overlayLines[i], textPos, cv::FONT_HERSHEY_SIMPLEX, 0.6,
                            cv::Scalar(255, 255, 255), 1, cv::LINE_AA);
              }
            }

            // Encode to JPEG
            auto jpegStart = std::chrono::high_resolution_clock::now();
            std::vector<int> jpegParams = {cv::IMWRITE_JPEG_QUALITY, jpegQuality};
            std::vector<uchar> jpegData;

            if (annotatedVideo && cv::imencode(".jpg", frameWithOBBs, jpegData, jpegParams)) {
              auto jpegEnd = std::chrono::high_resolution_clock::now();
              metrics.jpegEncodeTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(jpegEnd - jpegStart).count();

              processedFrame.jpegData.assign(jpegData.data(), jpegData.size());
              processedFrame.timestamp = packet.timestamp;
              processedFrame.width = packet.width;
              processedFrame.height = packet.height;
              processedFrame.detectionCount = trackedOBBDetections.size();
            }

            // Update statistics
            stats.updateStats(metrics, trackedOBBDetections.size(), packet.codec, packet.width, packet.height);

            // Create OBB-specific metadata
            metadata = Utils::obbDetectionsToJSON(trackedOBBDetections, packet.timestamp, metrics, model.getModelInfo().name);

          } else {
            ERROR_MSG("Failed to cast to YOLOv8OBBModel");
            metadata.null();
            return std::make_pair(metadata, processedFrame);
          }
        } else {
          // Process with detection model (works for all other YOLO variants)
          InferenceMetrics metrics;
          std::vector<Detection> detections = model.processFrame(frame, confThreshold, nmsThreshold, &metrics);

          // Add video decode time to metrics
          metrics.videoDecodeTimeMs = videoDecodeTimeMs;

          // Time scene change detection
          auto sceneChangeStart = std::chrono::high_resolution_clock::now();
            bool sceneChanged = sceneChangeEnabled && Utils::detectSceneChange(sceneDetector, detections, packet.timestamp);
          auto sceneChangeEnd = std::chrono::high_resolution_clock::now();
          metrics.sceneChangeTimeMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(sceneChangeEnd - sceneChangeStart).count();

          // Check for scene change BEFORE temporal tracking (using raw detections)
          if (sceneChanged) {
            INFO_MSG("Scene change detected at timestamp %" PRIu64 ", performing soft reset", (uint64_t)packet.timestamp);
            tracker.softReset(detections, packet.timestamp);
            // Increment scene change counter in stats
            std::lock_guard<std::mutex> lock(stats.statsMutex);
            stats.sceneChangesDetected++;
          }

          // Apply temporal tracking (only for detection-based models)
          std::vector<Detection> trackedDetections;
          if (modelType == ModelType::YOLOV8_DETECTION || modelType == ModelType::YOLOV8_POSE ||
              modelType == ModelType::YOLO11_DETECTION || modelType == ModelType::YOLO11_POSE) {

            // Time temporal tracking
            auto trackingStart = std::chrono::high_resolution_clock::now();
            size_t tracksBefore = tracker.getTrackCount();
            trackedDetections = trackIfEnabled(tracker, detections, packet.timestamp, trackingEnabled);
            size_t tracksAfter = tracker.getTrackCount();
            auto trackingEnd = std::chrono::high_resolution_clock::now();

            metrics.temporalTrackingTimeMs =
              std::chrono::duration_cast<std::chrono::milliseconds>(trackingEnd - trackingStart).count();
            metrics.trackedObjectCount = trackedDetections.size();

            // Calculate new and lost tracks
            if (tracksAfter > tracksBefore) { metrics.newTrackCount = tracksAfter - tracksBefore; }
            if (tracksBefore > tracksAfter) { metrics.lostTrackCount = tracksBefore - tracksAfter; }

          } else {
            // Classification models don't need tracking
            trackedDetections = detections;
            metrics.trackedObjectCount = detections.size();
          }

          // Create processed video frame with detections (this captures JPEG encoding time)
          std::vector<uchar> jpegData;
          if (annotatedVideo) { jpegData = Utils::encodeJPEG(frame, trackedDetections, stats, jpegQuality, &metrics); }
          if (!jpegData.empty()) {
            processedFrame.jpegData.assign(jpegData.data(), jpegData.size());
            processedFrame.timestamp = packet.timestamp;
            processedFrame.width = packet.width;
            processedFrame.height = packet.height;
            processedFrame.detectionCount = trackedDetections.size();
          }

          // Update statistics AFTER all timing measurements are complete
          stats.updateStats(metrics, trackedDetections.size(), packet.codec, packet.width, packet.height);

          // Create metadata
          metadata = Utils::detectionsToJSON(trackedDetections, packet.timestamp, metrics, model.getModelInfo().name);
        }

      } else if (modelType == ModelType::RT_DETR_DETECTION) {
        RTDETRModel *rtdetrModel = dynamic_cast<RTDETRModel *>(&model);
        if (rtdetrModel) {
          InferenceMetrics metrics;
          std::vector<Detection> detections = rtdetrModel->processRTDETRFrame(frame, confThreshold, &metrics);
          metrics.videoDecodeTimeMs = videoDecodeTimeMs;

          auto sceneChangeStart = std::chrono::high_resolution_clock::now();
          bool sceneChanged = sceneChangeEnabled && Utils::detectSceneChange(sceneDetector, detections, packet.timestamp);
          auto sceneChangeEnd = std::chrono::high_resolution_clock::now();
          metrics.sceneChangeTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(sceneChangeEnd - sceneChangeStart).count();
          if (sceneChanged) {
            tracker.softReset(detections, packet.timestamp);
            std::lock_guard<std::mutex> lock(stats.statsMutex);
            stats.sceneChangesDetected++;
          }

          auto trackingStart = std::chrono::high_resolution_clock::now();
          std::vector<Detection> trackedDetections = trackIfEnabled(tracker, detections, packet.timestamp, trackingEnabled);
          auto trackingEnd = std::chrono::high_resolution_clock::now();
          metrics.temporalTrackingTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(trackingEnd - trackingStart).count();
          metrics.trackedObjectCount = trackedDetections.size();

          std::vector<uchar> jpegData;
          if (annotatedVideo) { jpegData = Utils::encodeJPEG(frame, trackedDetections, stats, jpegQuality, &metrics); }
          if (!jpegData.empty()) {
            processedFrame.jpegData.assign(jpegData.data(), jpegData.size());
            processedFrame.timestamp = packet.timestamp;
            processedFrame.width = packet.width;
            processedFrame.height = packet.height;
            processedFrame.detectionCount = trackedDetections.size();
          }

          stats.updateStats(metrics, trackedDetections.size(), packet.codec, packet.width, packet.height);
          metadata = Utils::detectionsToJSON(trackedDetections, packet.timestamp, metrics, model.getModelInfo().name);
        }

      } else if (modelType == ModelType::DEPTH_ESTIMATION) {
        DepthEstimationModel *depthModel = dynamic_cast<DepthEstimationModel *>(&model);
        if (depthModel) {
          InferenceMetrics metrics;
          DepthResult depthResult = depthModel->processDepthFrame(frame, &metrics);
          metrics.videoDecodeTimeMs = videoDecodeTimeMs;

          cv::Mat depthVis;
          if (annotatedVideo) { depthVis = Utils::drawDepthVisualization(frame, depthResult); }

          auto jpegStart = std::chrono::high_resolution_clock::now();
          std::vector<int> jpegParams = {cv::IMWRITE_JPEG_QUALITY, jpegQuality};
          std::vector<uchar> jpegData;
          if (annotatedVideo && cv::imencode(".jpg", depthVis, jpegData, jpegParams)) {
            auto jpegEnd = std::chrono::high_resolution_clock::now();
            metrics.jpegEncodeTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(jpegEnd - jpegStart).count();
            processedFrame.jpegData.assign(jpegData.data(), jpegData.size());
            processedFrame.timestamp = packet.timestamp;
            processedFrame.width = packet.width;
            processedFrame.height = packet.height;
            processedFrame.detectionCount = 0;
          }

          stats.updateStats(metrics, 0, packet.codec, packet.width, packet.height);
          metadata = Utils::depthResultToJSON(depthResult, packet.timestamp, model.getModelInfo().name);
        }

      } else if (modelType == ModelType::FACE_DETECTION_SCRFD) {
        SCRFDModel *scrfdModel = dynamic_cast<SCRFDModel *>(&model);
        if (scrfdModel) {
          InferenceMetrics metrics;
          std::vector<FaceDetection> faceDetections = scrfdModel->processFaceFrame(frame, confThreshold, nmsThreshold, &metrics);
          metrics.videoDecodeTimeMs = videoDecodeTimeMs;

          std::vector<Detection> baseDets(faceDetections.begin(), faceDetections.end());
          auto sceneChangeStart = std::chrono::high_resolution_clock::now();
          bool sceneChanged = sceneChangeEnabled && Utils::detectSceneChange(sceneDetector, baseDets, packet.timestamp);
          auto sceneChangeEnd = std::chrono::high_resolution_clock::now();
          metrics.sceneChangeTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(sceneChangeEnd - sceneChangeStart).count();
          if (sceneChanged) {
            tracker.softReset(baseDets, packet.timestamp);
            std::lock_guard<std::mutex> lock(stats.statsMutex);
            stats.sceneChangesDetected++;
          }

          auto trackingStart = std::chrono::high_resolution_clock::now();
          std::vector<Detection> trackedDetections = trackIfEnabled(tracker, baseDets, packet.timestamp, trackingEnabled);
          auto trackingEnd = std::chrono::high_resolution_clock::now();
          metrics.temporalTrackingTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(trackingEnd - trackingStart).count();
          metrics.trackedObjectCount = trackedDetections.size();

          // Merge tracking data back into face detections
          for (size_t i = 0; i < faceDetections.size() && i < trackedDetections.size(); ++i) {
            faceDetections[i].track_id = trackedDetections[i].track_id;
            faceDetections[i].first_seen_time = trackedDetections[i].first_seen_time;
            faceDetections[i].last_seen_time = trackedDetections[i].last_seen_time;
            faceDetections[i].track_confidence = trackedDetections[i].track_confidence;
            faceDetections[i].trail = trackedDetections[i].trail;
            faceDetections[i].kalmanFilter = trackedDetections[i].kalmanFilter;
            faceDetections[i].kalmanInitialized = trackedDetections[i].kalmanInitialized;
          }

          cv::Mat faceVis;
          if (annotatedVideo) { faceVis = Utils::drawFaceDetections(frame, faceDetections, true); }
          auto jpegStart = std::chrono::high_resolution_clock::now();
          std::vector<int> jpegParams = {cv::IMWRITE_JPEG_QUALITY, jpegQuality};
          std::vector<uchar> jpegData;
          if (annotatedVideo && cv::imencode(".jpg", faceVis, jpegData, jpegParams)) {
            auto jpegEnd = std::chrono::high_resolution_clock::now();
            metrics.jpegEncodeTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(jpegEnd - jpegStart).count();
            processedFrame.jpegData.assign(jpegData.data(), jpegData.size());
            processedFrame.timestamp = packet.timestamp;
            processedFrame.width = packet.width;
            processedFrame.height = packet.height;
            processedFrame.detectionCount = faceDetections.size();
          }

          stats.updateStats(metrics, faceDetections.size(), packet.codec, packet.width, packet.height);
          metadata = Utils::faceDetectionsToJSON(faceDetections, packet.timestamp, metrics, model.getModelInfo().name);
        }

      } else if (modelType == ModelType::FACE_RECOGNITION_ARCFACE || modelType == ModelType::IMAGE_EMBEDDING) {
        EmbeddingModel *embeddingModel = dynamic_cast<EmbeddingModel *>(&model);
        if (embeddingModel) {
          InferenceMetrics metrics;
          FaceEmbedding embedding = embeddingModel->processEmbeddingFrame(frame, &metrics);
          metrics.videoDecodeTimeMs = videoDecodeTimeMs;

          // Draw embedding info overlay
          cv::Mat vis;
          if (annotatedVideo) {
            vis = frame.clone();
            char buf[128];
            snprintf(buf, sizeof(buf), "%s: %zu-d embedding", model.getModelInfo().name.c_str(), embedding.embedding.size());
            cv::putText(vis, buf, cv::Point(20, 40), cv::FONT_HERSHEY_SIMPLEX, 0.8,
                        cv::Scalar(0, 255, 0), 2, cv::LINE_AA);
          }

          auto jpegStart = std::chrono::high_resolution_clock::now();
          std::vector<int> jpegParams = {cv::IMWRITE_JPEG_QUALITY, jpegQuality};
          std::vector<uchar> jpegData;
          if (annotatedVideo && cv::imencode(".jpg", vis, jpegData, jpegParams)) {
            auto jpegEnd = std::chrono::high_resolution_clock::now();
            metrics.jpegEncodeTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(jpegEnd - jpegStart).count();
            processedFrame.jpegData.assign(jpegData.data(), jpegData.size());
            processedFrame.timestamp = packet.timestamp;
            processedFrame.width = packet.width;
            processedFrame.height = packet.height;
            processedFrame.detectionCount = 0;
          }

          stats.updateStats(metrics, 0, packet.codec, packet.width, packet.height);
          metadata = Utils::faceEmbeddingToJSON(embedding, packet.timestamp, model.getModelInfo().name);
          if (modelType == ModelType::IMAGE_EMBEDDING) {
            metadata["kind"] = "image_embedding";
            // Zero-shot tagging: when a match set is loaded, rank the embedding against
            // it and attach a classification block (so event_class works on tags too).
            if (embeddingModel->hasMatchSet()) {
              ClassificationResult tags =
                embeddingModel->matchEmbedding(embedding.embedding, embeddingModel->getMatchTopK());
              metadata["classification"]["class_id"] = tags.class_id;
              metadata["classification"]["class_name"] = tags.class_name;
              metadata["classification"]["confidence"] = tags.confidence;
              for (size_t i = 0; i < tags.top.size(); ++i) {
                JSON::Value e;
                e["class_id"] = tags.top[i].class_id;
                e["class_name"] = tags.top[i].class_name;
                e["confidence"] = tags.top[i].confidence;
                metadata["classification"]["top"].append(e);
              }
            }
          }
        }

      } else if (modelType == ModelType::OCR) {
        OCRModel *ocrModel = dynamic_cast<OCRModel *>(&model);
        if (ocrModel) {
          InferenceMetrics metrics;
          OCRResult ocr = ocrModel->processOCRFrame(frame, confThreshold, &metrics);
          metrics.videoDecodeTimeMs = videoDecodeTimeMs;

          cv::Mat vis;
          if (annotatedVideo) {
            vis = frame.clone();
            for (const OCRLine & line : ocr.lines) {
              cv::Rect box((int)(line.x * frame.cols), (int)(line.y * frame.rows),
                           (int)(line.w * frame.cols), (int)(line.h * frame.rows));
              cv::rectangle(vis, box, cv::Scalar(0, 200, 0), 2);
              cv::putText(vis, line.text, cv::Point(box.x, std::max(0, box.y - 5)),
                          cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 200, 0), 2, cv::LINE_AA);
            }
          }
          auto jpegStart = std::chrono::high_resolution_clock::now();
          std::vector<int> jpegParams = {cv::IMWRITE_JPEG_QUALITY, jpegQuality};
          std::vector<uchar> jpegData;
          if (annotatedVideo && cv::imencode(".jpg", vis, jpegData, jpegParams)) {
            auto jpegEnd = std::chrono::high_resolution_clock::now();
            metrics.jpegEncodeTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(jpegEnd - jpegStart).count();
            processedFrame.jpegData.assign(jpegData.data(), jpegData.size());
            processedFrame.timestamp = packet.timestamp;
            processedFrame.width = packet.width;
            processedFrame.height = packet.height;
            processedFrame.detectionCount = ocr.lines.size();
          }
          stats.updateStats(metrics, ocr.lines.size(), packet.codec, packet.width, packet.height);
          metadata = Utils::ocrResultToJSON(ocr, packet.timestamp, model.getModelInfo().name);
        }

      } else if (modelType == ModelType::POSE_RTMO) {
        RTMOModel *rtmoModel = dynamic_cast<RTMOModel *>(&model);
        if (rtmoModel) {
          InferenceMetrics metrics;
          std::vector<PoseDetection> poseDetections = rtmoModel->processRTMOFrame(frame, confThreshold, &metrics);
          metrics.videoDecodeTimeMs = videoDecodeTimeMs;

          std::vector<Detection> baseDets(poseDetections.begin(), poseDetections.end());
          auto sceneChangeStart = std::chrono::high_resolution_clock::now();
          bool sceneChanged = sceneChangeEnabled && Utils::detectSceneChange(sceneDetector, baseDets, packet.timestamp);
          auto sceneChangeEnd = std::chrono::high_resolution_clock::now();
          metrics.sceneChangeTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(sceneChangeEnd - sceneChangeStart).count();
          if (sceneChanged) {
            tracker.softReset(baseDets, packet.timestamp);
            std::lock_guard<std::mutex> lock(stats.statsMutex);
            stats.sceneChangesDetected++;
          }

          auto trackingStart = std::chrono::high_resolution_clock::now();
          std::vector<Detection> trackedDetections = trackIfEnabled(tracker, baseDets, packet.timestamp, trackingEnabled);
          auto trackingEnd = std::chrono::high_resolution_clock::now();
          metrics.temporalTrackingTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(trackingEnd - trackingStart).count();
          metrics.trackedObjectCount = trackedDetections.size();

          // Merge tracking back into pose detections
          std::vector<PoseDetection> trackedPoses;
          for (size_t i = 0; i < poseDetections.size() && i < trackedDetections.size(); ++i) {
            PoseDetection tp = poseDetections[i];
            tp.track_id = trackedDetections[i].track_id;
            tp.first_seen_time = trackedDetections[i].first_seen_time;
            tp.last_seen_time = trackedDetections[i].last_seen_time;
            tp.track_confidence = trackedDetections[i].track_confidence;
            tp.trail = trackedDetections[i].trail;
            tp.kalmanFilter = trackedDetections[i].kalmanFilter;
            tp.kalmanInitialized = trackedDetections[i].kalmanInitialized;
            trackedPoses.push_back(tp);
          }

          cv::Mat poseVis;
          if (annotatedVideo) { poseVis = Utils::drawPoseDetectionsWithTracking(frame, trackedPoses, true, true); }
          auto jpegStart = std::chrono::high_resolution_clock::now();
          std::vector<int> jpegParams = {cv::IMWRITE_JPEG_QUALITY, jpegQuality};
          std::vector<uchar> jpegData;
          if (annotatedVideo && cv::imencode(".jpg", poseVis, jpegData, jpegParams)) {
            auto jpegEnd = std::chrono::high_resolution_clock::now();
            metrics.jpegEncodeTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(jpegEnd - jpegStart).count();
            processedFrame.jpegData.assign(jpegData.data(), jpegData.size());
            processedFrame.timestamp = packet.timestamp;
            processedFrame.width = packet.width;
            processedFrame.height = packet.height;
            processedFrame.detectionCount = trackedPoses.size();
          }

          stats.updateStats(metrics, trackedPoses.size(), packet.codec, packet.width, packet.height);
          metadata = Utils::poseDetectionsToJSON(trackedPoses, packet.timestamp, metrics, model.getModelInfo().name);
        }

      } else {
        // SAM2, generic, and other non-pipeline models use generic processing
        return processVideoPacketGeneric(packet, model, stats, enhanceImage, annotatedVideo, jpegQuality);
      }

      return std::make_pair(metadata, processedFrame);
    }

    std::pair<JSON::Value, ProcessedVideoFrame> processVideoPacketGeneric(const VideoPacket & packet, DetectionModel & model,
                                                                          ProcessingStats & stats, bool enhanceImage,
                                                                          bool annotatedVideo, int jpegQuality) {
      JSON::Value metadata;
      ProcessedVideoFrame processedFrame;

      // Decode video frame
      cv::Mat frame =
        decodeVideoFrame((const char *)packet.packetData, packet.packetData.size(), packet.codec, packet.width, packet.height);

      if (frame.empty()) {
        WARN_MSG("Failed to decode generic video frame");
        metadata.null();
        return std::make_pair(metadata, processedFrame);
      }

      // Additional validation of decoded frame
      if (frame.channels() != 3) {
        ERROR_MSG("Decoded generic frame has wrong number of channels: %d (expected 3)", frame.channels());
        metadata.null();
        return std::make_pair(metadata, processedFrame);
      }

      if (frame.type() != CV_8UC3) {
        ERROR_MSG("Decoded generic frame has wrong type: %d (expected CV_8UC3=%d)", frame.type(), CV_8UC3);
        metadata.null();
        return std::make_pair(metadata, processedFrame);
      }

      if (frame.cols <= 0 || frame.rows <= 0) {
        ERROR_MSG("Decoded generic frame has invalid dimensions: %dx%d", frame.cols, frame.rows);
        metadata.null();
        return std::make_pair(metadata, processedFrame);
      }

      VERYHIGH_MSG("Decoded generic frame: %dx%d, type=%d, channels=%d, codec=%s", frame.cols, frame.rows, frame.type(),
                   frame.channels(), packet.codec.c_str());

      // Process with generic model
      InferenceMetrics metrics;
      GenericResult result = model.processFrameGeneric(frame, &metrics);
      result.timestamp = packet.timestamp;

      // Update statistics (no detections for generic models)
      stats.updateStats(metrics, 0, packet.codec, packet.width, packet.height);

      // Create metadata from generic result
      metadata = Utils::genericResultToJSON(result);

      // Create simple processed video frame (no detections to draw)
      std::vector<int> jpegParams = {cv::IMWRITE_JPEG_QUALITY, jpegQuality};
      std::vector<uchar> jpegData;

      // Frame is already in BGR format, can encode directly
      if (annotatedVideo && cv::imencode(".jpg", frame, jpegData, jpegParams)) {
        processedFrame.jpegData.assign(jpegData.data(), jpegData.size());
        processedFrame.timestamp = packet.timestamp;
        processedFrame.width = packet.width;
        processedFrame.height = packet.height;
        processedFrame.detectionCount = 0;
      }

      return std::make_pair(metadata, processedFrame);
    }

    // Configuration validation functions
    bool validateModelPath(const std::string & modelPath) {
      if (modelPath.empty()) {
        ERROR_MSG("ONNX model_path is required and must be a non-empty string");
        return false;
      }

      // Check if file exists (basic validation)
      std::ifstream file(modelPath);
      if (!file.good()) {
        ERROR_MSG("ONNX model file does not exist: %s", modelPath.c_str());
        return false;
      }

      return true;
    }

    bool validateThreshold(float threshold, const std::string & name) {
      if (threshold < 0.0f || threshold > 1.0f) {
        WARN_MSG("%s should be between 0.0 and 1.0, got %.3f", name.c_str(), threshold);
        return false;
      }
      return true;
    }

    bool validateInputSize(int inputSize) {
      // Transformer models use sizes like 224/384/518; YOLO-family wants 320-1280 in
      // multiples of 32. Only reject clearly-broken values; warn about non-multiples
      // of 32 since letterboxed detectors usually require them.
      if (inputSize < 64 || inputSize > 4096) {
        WARN_MSG("input_size should be between 64-4096, got %d", inputSize);
        return false;
      }
      if (inputSize % 32 != 0) {
        WARN_MSG("input_size %d is not a multiple of 32 — fine for ViT/CLIP-style models, but "
                 "YOLO-family models require it and inference WILL fail on them at this size",
                 inputSize);
      }
      return true;
    }

    bool validateThreadCount(int threads) {
      if (threads < 1 || threads > 32) {
        WARN_MSG("threads should be between 1-32, got %d", threads);
        return false;
      }
      return true;
    }

    // Draw pose detections on image
    cv::Mat drawPoseDetections(const cv::Mat & image, const std::vector<PoseDetection> & detections, bool showTrackIds,
                               bool showConfidence) {
      cv::Mat result = image.clone();

      // COCO pose skeleton connections
      const std::vector<std::pair<int, int>> skeleton = {{0, 1},   {0, 2},   {1, 3},   {2, 4},  {5, 6},  {5, 7},
                                                         {7, 9},   {6, 8},   {8, 10},  {5, 11}, {6, 12}, {11, 12},
                                                         {11, 13}, {13, 15}, {12, 14}, {14, 16}};

      for (const auto & detection : detections) {
        // Draw bounding box (BGR format)
        cv::Scalar color(0, 255, 0); // Green
        int x = static_cast<int>(detection.x * image.cols);
        int y = static_cast<int>(detection.y * image.rows);
        int w = static_cast<int>(detection.w * image.cols);
        int h = static_cast<int>(detection.h * image.rows);

        cv::rectangle(result, cv::Rect(x, y, w, h), color, 2);

        // Draw keypoints (BGR format)
        for (size_t i = 0; i < detection.keypoints.size() && i < 17; ++i) {
          if (detection.keypoints[i].visible && detection.keypoints[i].confidence > 0.5f) {
            cv::Point2f kp(detection.keypoints[i].x * image.cols, detection.keypoints[i].y * image.rows);
            cv::circle(result, kp, 3, cv::Scalar(0, 0, 255), -1); // Red keypoints
          }
        }

        // Draw skeleton (BGR format)
        for (const auto & connection : skeleton) {
          if (connection.first < detection.keypoints.size() && connection.second < detection.keypoints.size()) {
            const auto & kp1 = detection.keypoints[connection.first];
            const auto & kp2 = detection.keypoints[connection.second];

            if (kp1.visible && kp2.visible && kp1.confidence > 0.5f && kp2.confidence > 0.5f) {
              cv::Point2f pt1(kp1.x * image.cols, kp1.y * image.rows);
              cv::Point2f pt2(kp2.x * image.cols, kp2.y * image.rows);
              cv::line(result, pt1, pt2, cv::Scalar(0, 0, 255), 2); // Red skeleton lines (BGR)
            }
          }
        }

        // Draw labels
        if (showConfidence || showTrackIds) {
          std::string label;
          if (showTrackIds && detection.track_id > 0) { label += "ID:" + std::to_string(detection.track_id) + " "; }
          if (showConfidence) { label += cv::format("%.2f", detection.confidence); }

          cv::putText(result, label, cv::Point(x, y - 10), cv::FONT_HERSHEY_SIMPLEX, 0.5, color, 2);
        }
      }

      return result;
    }

    // Unified drawing function that combines tracking with segmentation features
    cv::Mat drawSegmentationDetectionsWithTracking(const cv::Mat & image, const std::vector<SegmentationDetection> & detections,
                                                   bool showTrackIds, bool showConfidence) {
      // Step 1: Convert segmentation detections to base detections for tracking visualization
      std::vector<Detection> baseDetections;
      for (const auto & segDet : detections) { baseDetections.push_back(static_cast<Detection>(segDet)); }

      // Step 2: Draw base detections with tracking (trails, track confidence colors, etc.)
      cv::Mat result = Utils::drawDetectionsWithOptionalTracking(image, baseDetections, showTrackIds, showConfidence, true);

      // Step 3: Overlay segmentation-specific features (masks and contours)
      for (const auto & detection : detections) {
        // Local ROI blend using bounding rect of mask
        if (!detection.mask.empty()) {
          cv::Mat colorMask;
          cv::applyColorMap(detection.mask, colorMask, cv::COLORMAP_JET);
          cv::Mat maskRegion;
          cv::threshold(detection.mask, maskRegion, 0, 255, cv::THRESH_BINARY);
          cv::Rect roi = cv::boundingRect(maskRegion);
          if (roi.width > 0 && roi.height > 0) {
            cv::Mat blended;
            cv::addWeighted(result(roi), 0.7, colorMask(roi), 0.3, 0.0, blended);
            blended.copyTo(result(roi), maskRegion(roi));
          }
        }

        // Draw contour on top of everything
        if (!detection.contour.empty()) {
          std::vector<std::vector<cv::Point>> contours = {detection.contour};
          cv::drawContours(result, contours, -1, cv::Scalar(0, 255, 0), 2);
        }
      }

      return result;
    }

    cv::Mat drawSegmentationDetections(const cv::Mat & image, const std::vector<SegmentationDetection> & detections,
                                       bool showTrackIds, bool showConfidence) {
      cv::Mat result = image.clone();

      for (const auto & detection : detections) {
        // Local ROI blend using bounding rect of mask
        if (!detection.mask.empty()) {
          cv::Mat colorMask;
          cv::applyColorMap(detection.mask, colorMask, cv::COLORMAP_JET);
          cv::Mat maskRegion;
          cv::threshold(detection.mask, maskRegion, 0, 255, cv::THRESH_BINARY);
          cv::Rect roi = cv::boundingRect(maskRegion);
          if (roi.width > 0 && roi.height > 0) {
            cv::Mat blended;
            cv::addWeighted(result(roi), 0.7, colorMask(roi), 0.3, 0.0, blended);
            blended.copyTo(result(roi), maskRegion(roi));
          }
        }

        // Draw contour
        if (!detection.contour.empty()) {
          std::vector<std::vector<cv::Point>> contours = {detection.contour};
          cv::drawContours(result, contours, -1, cv::Scalar(0, 255, 0), 2);
        }

        // Draw bounding box
        cv::Scalar color(0, 255, 0);
        int x = static_cast<int>(detection.x * image.cols);
        int y = static_cast<int>(detection.y * image.rows);
        int w = static_cast<int>(detection.w * image.cols);
        int h = static_cast<int>(detection.h * image.rows);

        cv::rectangle(result, cv::Rect(x, y, w, h), color, 2);

        // Draw labels
        if (showConfidence || showTrackIds) {
          std::string label = detection.class_name;
          if (showTrackIds && detection.track_id > 0) { label += " ID:" + std::to_string(detection.track_id); }
          if (showConfidence) { label += cv::format(" %.2f", detection.confidence); }

          cv::putText(result, label, cv::Point(x, y - 10), cv::FONT_HERSHEY_SIMPLEX, 0.5, color, 2);
        }
      }

      return result;
    }

    // Draw OBB detections on image
    cv::Mat drawOBBDetections(const cv::Mat & image, const std::vector<OBBDetection> & detections, bool showTrackIds,
                              bool showConfidence) {
      cv::Mat result = image.clone();

      for (const auto & detection : detections) {
        // Draw oriented bounding box
        cv::Scalar color(0, 255, 0);

        if (detection.corners.size() == 4) {
          // Draw the 4 corners of the oriented box
          std::vector<cv::Point> corners;
          for (const auto & corner : detection.corners) {
            corners.push_back(cv::Point(corner.x * image.cols, corner.y * image.rows));
          }

          // Draw lines between corners
          for (size_t i = 0; i < 4; ++i) { cv::line(result, corners[i], corners[(i + 1) % 4], color, 2); }
        }

        // Draw center point
        cv::Point center(detection.center.x * image.cols, detection.center.y * image.rows);
        cv::circle(result, center, 3, cv::Scalar(0, 0, 255), -1);

        // Draw labels
        if (showConfidence || showTrackIds) {
          std::string label = detection.class_name;
          if (showTrackIds && detection.track_id > 0) { label += " ID:" + std::to_string(detection.track_id); }
          if (showConfidence) {
            // Safe confidence formatting to handle NaN/infinity
            if (std::isfinite(detection.confidence) && !std::isnan(detection.confidence)) {
              label += cv::format(" %.1f%%", detection.confidence * 100.0f);
            } else {
              label += " INVALID";
            }
          }

          cv::putText(result, label, cv::Point(center.x, center.y - 10), cv::FONT_HERSHEY_SIMPLEX, 0.5, color, 2);
        }
      }

      return result;
    }

    // Draw classification result on image
    cv::Mat drawClassificationResult(const cv::Mat & image, const ClassificationResult & result) {
      cv::Mat resultImage = image.clone();

      std::string label = result.class_name + cv::format(" (%.2f)", result.confidence);

      // Draw background rectangle for text
      cv::Size textSize = cv::getTextSize(label, cv::FONT_HERSHEY_SIMPLEX, 1.0, 2, nullptr);
      cv::rectangle(resultImage, cv::Point(10, 10), cv::Point(20 + textSize.width, 40 + textSize.height), cv::Scalar(0, 0, 0), -1);

      // Draw text
      cv::putText(resultImage, label, cv::Point(15, 35), cv::FONT_HERSHEY_SIMPLEX, 1.0, cv::Scalar(0, 255, 0), 2);

      return resultImage;
    }

    // Create JSON from pose detections
    JSON::Value poseDetectionsToJSON(const std::vector<PoseDetection> & detections, uint64_t timestamp,
                                     const InferenceMetrics & metrics, const std::string & modelName) {
      JSON::Value result;
      result["schema"] = "mist.onnx.result/v1";
      result["timestamp_ms"] = timestamp;
      result["model"]["name"] = modelName;
      result["kind"] = "pose";
      result["status"] = "ok";
      result["detections"].append(); result["detections"].shrink(0);

      for (const auto & detection : detections) {
        JSON::Value det;
        det["bbox"]["x"] = detection.x;
        det["bbox"]["y"] = detection.y;
        det["bbox"]["w"] = detection.w;
        det["bbox"]["h"] = detection.h;
        det["confidence"] = detection.confidence;
        det["pose_confidence"] = detection.pose_confidence;
        det["class_id"] = detection.class_id;
        det["class_name"] = detection.class_name;
        if (detection.track_id > 0) { det["track_id"] = detection.track_id; }

        det["keypoints"].append(); det["keypoints"].shrink(0);
        for (size_t i = 0; i < detection.keypoints.size() && i < COCO_KEYPOINTS.size(); ++i) {
          JSON::Value kp;
          kp["name"] = COCO_KEYPOINTS[i];
          kp["x"] = detection.keypoints[i].x;
          kp["y"] = detection.keypoints[i].y;
          kp["confidence"] = detection.keypoints[i].confidence;
          kp["visible"] = detection.keypoints[i].visible;
          det["keypoints"].append(kp);
        }

        result["detections"].append(det);
      }

      // Add performance metrics
      result["metrics"]["inference_ms"] = metrics.inferenceTimeMs;
      result["metrics"]["preprocess_ms"] = metrics.preprocessTimeMs;
      result["metrics"]["postprocess_ms"] = metrics.postprocessTimeMs;
      result["metrics"]["total_ms"] = metrics.totalTimeMs;

      return result;
    }

    // Create JSON from segmentation detections
    JSON::Value segmentationDetectionsToJSON(const std::vector<SegmentationDetection> & detections, uint64_t timestamp,
                                             const InferenceMetrics & metrics, const std::string & modelName) {
      JSON::Value result;
      result["schema"] = "mist.onnx.result/v1";
      result["timestamp_ms"] = timestamp;
      result["model"]["name"] = modelName;
      result["kind"] = "instance_segmentation";
      result["status"] = "ok";
      result["detections"].append(); result["detections"].shrink(0);

      for (const auto & detection : detections) {
        JSON::Value det;
        det["bbox"]["x"] = detection.x;
        det["bbox"]["y"] = detection.y;
        det["bbox"]["w"] = detection.w;
        det["bbox"]["h"] = detection.h;
        det["confidence"] = detection.confidence;
        det["mask_confidence"] = detection.mask_confidence;
        det["class_id"] = detection.class_id;
        det["class_name"] = detection.class_name;
        if (detection.track_id > 0) { det["track_id"] = detection.track_id; }

        // Add contour points
        det["contour"].append(); det["contour"].shrink(0);
        for (const auto & point : detection.contour) {
          JSON::Value pt;
          pt["x"] = point.x;
          pt["y"] = point.y;
          det["contour"].append(pt);
        }

        result["detections"].append(det);
      }

      // Add performance metrics
      result["metrics"]["inference_ms"] = metrics.inferenceTimeMs;
      result["metrics"]["preprocess_ms"] = metrics.preprocessTimeMs;
      result["metrics"]["postprocess_ms"] = metrics.postprocessTimeMs;
      result["metrics"]["total_ms"] = metrics.totalTimeMs;

      return result;
    }

    // Create JSON from OBB detections
    JSON::Value obbDetectionsToJSON(const std::vector<OBBDetection> & detections, uint64_t timestamp,
                                    const InferenceMetrics & metrics, const std::string & modelName) {
      JSON::Value result;
      result["schema"] = "mist.onnx.result/v1";
      result["timestamp_ms"] = timestamp;
      result["model"]["name"] = modelName;
      result["kind"] = "oriented_object_detection";
      result["status"] = "ok";
      result["detections"].append(); result["detections"].shrink(0);

      for (const auto & detection : detections) {
        JSON::Value det;
        det["bbox"]["x"] = detection.x;
        det["bbox"]["y"] = detection.y;
        det["bbox"]["w"] = detection.w;
        det["bbox"]["h"] = detection.h;
        det["confidence"] = detection.confidence;
        det["class_id"] = detection.class_id;
        det["class_name"] = detection.class_name;
        if (detection.track_id > 0) { det["track_id"] = detection.track_id; }

        // Add oriented bounding box specific data
        det["obb"]["center"]["x"] = detection.center.x;
        det["obb"]["center"]["y"] = detection.center.y;
        det["obb"]["size"]["width"] = detection.size.width;
        det["obb"]["size"]["height"] = detection.size.height;
        det["obb"]["angle"] = detection.angle;

        det["obb"]["corners"].append(); det["obb"]["corners"].shrink(0);
        for (const auto & corner : detection.corners) {
          JSON::Value pt;
          pt["x"] = corner.x;
          pt["y"] = corner.y;
          det["obb"]["corners"].append(pt);
        }

        result["detections"].append(det);
      }

      // Add performance metrics
      result["metrics"]["inference_ms"] = metrics.inferenceTimeMs;
      result["metrics"]["preprocess_ms"] = metrics.preprocessTimeMs;
      result["metrics"]["postprocess_ms"] = metrics.postprocessTimeMs;
      result["metrics"]["total_ms"] = metrics.totalTimeMs;

      return result;
    }

    // Create JSON from classification result
    JSON::Value classificationToJSON(const ClassificationResult & result, const InferenceMetrics & metrics,
                                     const std::string & modelName) {
      JSON::Value json;
      json["schema"] = "mist.onnx.result/v1";
      json["timestamp_ms"] = result.timestamp;
      json["model"]["name"] = modelName;
      json["kind"] = "classification";
      json["status"] = "ok";
      json["classification"]["class_id"] = result.class_id;
      json["classification"]["class_name"] = result.class_name;
      json["classification"]["confidence"] = result.confidence;
      if (result.top.size() > 1) {
        for (size_t i = 0; i < result.top.size(); ++i) {
          JSON::Value e;
          e["class_id"] = result.top[i].class_id;
          e["class_name"] = result.top[i].class_name;
          e["confidence"] = result.top[i].confidence;
          json["classification"]["top"].append(e);
        }
      }

      // Add performance metrics
      json["metrics"]["inference_ms"] = metrics.inferenceTimeMs;
      json["metrics"]["preprocess_ms"] = metrics.preprocessTimeMs;
      json["metrics"]["postprocess_ms"] = metrics.postprocessTimeMs;
      json["metrics"]["total_ms"] = metrics.totalTimeMs;

      return json;
    }

    size_t computeFbank(const float *samples, size_t count, int sampleRate, int numBins,
                        bool hanningWindow, std::vector<float> &out) {
      if (!samples || sampleRate <= 0 || numBins <= 0) { return 0; }
      const size_t frameLen = (size_t)(0.025 * sampleRate); // 25 ms
      const size_t frameShift = (size_t)(0.010 * sampleRate); // 10 ms
      if (count < frameLen || !frameLen || !frameShift) { return 0; }
      size_t fftLen = 1;
      while (fftLen < frameLen) { fftLen <<= 1; }
      const size_t numFft = fftLen / 2; // spectrum bins used (kaldi excludes Nyquist)
      const size_t numFrames = 1 + (count - frameLen) / frameShift; // snip_edges

      // Precompute the window function
      std::vector<float> window(frameLen);
      for (size_t i = 0; i < frameLen; ++i) {
        double hann = 0.5 - 0.5 * cos(2.0 * M_PI * i / (frameLen - 1));
        window[i] = hanningWindow ? (float)hann : (float)pow(hann, 0.85); // povey = hann^0.85
      }

      // Precompute the mel filterbank: triangular weights over fft bins, kaldi mel
      // scale 1127*ln(1+f/700), points evenly spaced in mel from 20 Hz to Nyquist.
      const double melLow = 1127.0 * log(1.0 + 20.0 / 700.0);
      const double melHigh = 1127.0 * log(1.0 + (sampleRate / 2.0) / 700.0);
      const double melStep = (melHigh - melLow) / (numBins + 1);
      std::vector<std::vector<float>> bank(numBins, std::vector<float>(numFft, 0.0f));
      for (int b = 0; b < numBins; ++b) {
        const double left = melLow + b * melStep;
        const double center = left + melStep;
        const double right = center + melStep;
        for (size_t k = 0; k < numFft; ++k) {
          const double freq = (double)k * sampleRate / (double)fftLen;
          const double mel = 1127.0 * log(1.0 + freq / 700.0);
          if (mel > left && mel < right) {
            bank[b][k] = (float)((mel <= center) ? (mel - left) / (center - left)
                                                 : (right - mel) / (right - center));
          }
        }
      }

      const size_t base = out.size();
      out.resize(base + numFrames * (size_t)numBins);
      cv::Mat frame(1, (int)fftLen, CV_32F);
      cv::Mat spectrum;
      for (size_t f = 0; f < numFrames; ++f) {
        float *buf = frame.ptr<float>(0);
        const float *src = samples + f * frameShift;
        // DC-offset removal (per frame), then preemphasis, then window
        double mean = 0.0;
        for (size_t i = 0; i < frameLen; ++i) { mean += src[i]; }
        mean /= frameLen;
        for (size_t i = 0; i < frameLen; ++i) { buf[i] = (float)(src[i] - mean); }
        for (size_t i = frameLen - 1; i > 0; --i) { buf[i] -= 0.97f * buf[i - 1]; }
        buf[0] -= 0.97f * buf[0];
        for (size_t i = 0; i < frameLen; ++i) { buf[i] *= window[i]; }
        for (size_t i = frameLen; i < fftLen; ++i) { buf[i] = 0.0f; }

        cv::dft(frame, spectrum); // CCS-packed: [re0, re1, im1, re2, im2, ..., reN/2]
        const float *sp = spectrum.ptr<float>(0);
        float *dst = out.data() + base + f * (size_t)numBins;
        // Power spectrum for bins 0..numFft-1
        std::vector<float> power(numFft);
        power[0] = sp[0] * sp[0];
        for (size_t k = 1; k < numFft; ++k) {
          power[k] = sp[2 * k - 1] * sp[2 * k - 1] + sp[2 * k] * sp[2 * k];
        }
        for (int b = 0; b < numBins; ++b) {
          float sum = 0.0f;
          const std::vector<float> & w = bank[b];
          for (size_t k = 0; k < numFft; ++k) { sum += w[k] * power[k]; }
          dst[b] = logf(sum > 1.19209290e-07f ? sum : 1.19209290e-07f); // FLT_EPSILON floor
        }
      }
      return numFrames;
    }

    size_t ctcGreedyDecode(const float *probs, size_t timesteps, size_t numClasses,
                           const std::vector<std::string> & charset, std::string & text,
                           float & confidence) {
      text.clear();
      confidence = 0.0f;
      if (!probs || !timesteps || !numClasses) { return 0; }
      size_t kept = 0;
      double confSum = 0.0;
      int prevClass = -1; // for collapsing runs of the same class
      for (size_t t = 0; t < timesteps; ++t) {
        const float *row = probs + t * numClasses;
        size_t best = 0;
        float bestP = row[0];
        for (size_t c = 1; c < numClasses; ++c) {
          if (row[c] > bestP) { bestP = row[c]; best = c; }
        }
        // CTC: skip blanks (class 0) and repeats of the previous emitted class
        if (best != 0 && (int)best != prevClass) {
          if (best < charset.size()) {
            text += charset[best];
            confSum += bestP;
            kept++;
          }
        }
        prevClass = (int)best;
      }
      if (kept) { confidence = (float)(confSum / kept); }
      return kept;
    }

    std::vector<std::string> loadLabelsFile(const std::string & path) {
      std::vector<std::string> labels;
      std::ifstream f(path.c_str());
      if (!f.is_open()) { return labels; }
      std::string line;
      while (std::getline(f, line)) {
        while (!line.empty() && (line[line.size() - 1] == '\r' || line[line.size() - 1] == ' ')) {
          line.erase(line.size() - 1);
        }
        labels.push_back(line);
      }
      // Trailing blank lines are file formatting; interior blanks stay (index = class id)
      while (!labels.empty() && labels.back().empty()) { labels.pop_back(); }
      return labels;
    }

    std::vector<std::string> loadLabelsFromHFConfig(const std::string & path) {
      std::vector<std::string> labels;
      if (access(path.c_str(), R_OK) != 0) { return labels; }
      JSON::Value cfg = JSON::fromFile(path);
      if (!cfg.isMember("id2label") || !cfg["id2label"].isObject()) { return labels; }
      jsonForEachConst(cfg["id2label"], it) {
        // Keys are external data: only all-digit keys are class ids (a stray
        // "LABEL_1"-style key would atoi() to 0 and clobber class 0's label), and
        // absurd ids must not blow up the vector.
        const std::string & key = it.key();
        if (key.empty() || key.find_first_not_of("0123456789") != std::string::npos) { continue; }
        int id = atoi(key.c_str());
        if (id < 0 || id > 65535) { continue; }
        if ((size_t)id >= labels.size()) { labels.resize(id + 1); }
        labels[id] = it->asString();
      }
      return labels;
    }

    SidecarConfig loadModelSidecars(const std::string & modelPath) {
      SidecarConfig sc;
      size_t slash = modelPath.find_last_of("/\\");
      std::string dir = (slash == std::string::npos) ? "" : modelPath.substr(0, slash + 1);
      std::string base = (slash == std::string::npos) ? modelPath : modelPath.substr(slash + 1);
      size_t ext = base.rfind(".onnx");
      std::string stem = (ext == std::string::npos) ? base : base.substr(0, ext);

      // The HF-named generic sidecars (config.json / preprocessor_config.json) only
      // apply when the model has its own directory — one .onnx per dir, the layout
      // provisioning creates. In a shared flat directory a stray HF download would
      // otherwise rewire every model's labels and preprocessing. labels.txt stays
      // shared (a dir can hold size variants of one model with one label set), and
      // <stem>-prefixed sidecars always apply.
      size_t onnxCount = 0;
      DIR *dp = opendir(dir.empty() ? "." : dir.c_str());
      if (dp) {
        struct dirent *de;
        while ((de = readdir(dp)) != 0) {
          size_t nameLen = strlen(de->d_name);
          if (nameLen > 5 && strcmp(de->d_name + nameLen - 5, ".onnx") == 0) { onnxCount++; }
        }
        closedir(dp);
      }
      const bool soleModelDir = onnxCount <= 1;

      sc.labels = loadLabelsFile(dir + stem + ".labels.txt");
      if (sc.labels.empty()) { sc.labels = loadLabelsFile(dir + "labels.txt"); }
      if (sc.labels.empty() && soleModelDir) { sc.labels = loadLabelsFromHFConfig(dir + "config.json"); }

      std::string prePath = dir + stem + ".preprocessor.json";
      if (access(prePath.c_str(), R_OK) != 0) {
        prePath = soleModelDir ? dir + "preprocessor_config.json" : "";
      }
      if (prePath.empty()) { return sc; }
      if (access(prePath.c_str(), R_OK) == 0) {
        JSON::Value cfg = JSON::fromFile(prePath);
        if (cfg.isObject() && cfg.isMember("sampling_rate")) {
          // Audio feature-extractor sidecar (wav2vec2 / AST / etc) — no image preprocessing.
          sc.samplingRate = (int)cfg["sampling_rate"].asInt();
          sc.audioNormalize = cfg.isMember("do_normalize") && cfg["do_normalize"].asBool();
          if (cfg.isMember("num_mel_bins")) { sc.numMelBins = (int)cfg["num_mel_bins"].asInt(); }
          if (cfg.isMember("max_length")) { sc.maxFrames = (int)cfg["max_length"].asInt(); }
          if (cfg.isMember("mean")) { sc.featMean = (float)cfg["mean"].asDouble(); }
          if (cfg.isMember("std")) { sc.featStd = (float)cfg["std"].asDouble(); }
        } else if (cfg.isObject()) {
          sc.hasPreproc = true;
          // HF image processors never letterbox: fixed height/width means a direct
          // resize; shortest_edge sizing (or an explicit do_center_crop) means scale
          // the short edge then center-crop (CLIP convention).
          bool centerCrop = (cfg.isMember("size") && cfg["size"].isMember("shortest_edge")) ||
                            (cfg.isMember("do_center_crop") && cfg["do_center_crop"].asBool());
          sc.preproc.resizeMode = centerCrop ? PreprocessConfig::CENTER_CROP
                                             : PreprocessConfig::DIRECT_RESIZE;
          if (cfg["image_mean"].size() >= 3 && cfg["image_std"].size() >= 3) {
            // Matches the HF pipeline: rescale 1/255, then (x - mean) / std per channel
            sc.preproc.normMode = PreprocessConfig::IMAGENET;
            for (uint32_t c = 0; c < 3; ++c) {
              sc.preproc.mean[c] = (float)cfg["image_mean"][c].asDouble();
              sc.preproc.std[c] = (float)cfg["image_std"][c].asDouble();
            }
          } else {
            sc.preproc.normMode = PreprocessConfig::SCALE_01;
          }
          if (cfg.isMember("size")) {
            if (cfg["size"].isMember("height")) {
              sc.inputSize = (int)cfg["size"]["height"].asInt();
            } else if (cfg["size"].isMember("shortest_edge")) {
              sc.inputSize = (int)cfg["size"]["shortest_edge"].asInt();
            }
          }
        }
      }
      return sc;
    }

    // Scene change detection (standalone, not part of TemporalTracker)
    bool detectSceneChange(SceneChangeDetector & detector, const std::vector<Detection> & newDetections, uint64_t timestamp) {
      if (!detector.enabled) return false;

      if (detector.previousDetections.empty()) {
        VERYHIGH_MSG("Scene change skipped: no previous detections at timestamp %" PRIu64, (uint64_t)timestamp);
        detector.previousDetections = newDetections;
        detector.lastTimestamp = timestamp;
        return false;
      }

      if (timestamp - detector.lastTimestamp > 10000) { // 10 seconds gap
        MEDIUM_MSG("Scene change skipped: timestamp gap too large (%" PRIu64 " - %" PRIu64 " = %" PRIu64 " > 10000ms)",
                   (uint64_t)timestamp, (uint64_t)detector.lastTimestamp, (uint64_t)(timestamp - detector.lastTimestamp));
        detector.previousDetections = newDetections;
        detector.lastTimestamp = timestamp;
        return false;
      }

      // Rate limiting: don't allow scene changes too frequently
      if (detector.lastSceneChangeTime > 0 && timestamp - detector.lastSceneChangeTime < detector.minMsBetweenChanges) {
        // Update state but don't trigger scene change
        detector.previousDetections = newDetections;
        detector.lastTimestamp = timestamp;
        return false;
      }

      // Calculate similarity between current and previous detections
      float similarity = calculateDetectionSimilarity(detector.previousDetections, newDetections);

      // Calculate change confidence (higher when similarity is low)
      float changeConfidence = 1.0f - similarity;

      // Scene change logic: trigger if change confidence is high enough
      bool sceneChanged = changeConfidence > detector.threshold;

      // Always log scene change checks for debugging
      if (sceneChanged) {
        INFO_MSG("Scene change detected: changeConf=%.3f, similarity=%.3f, prevCount=%zu, newCount=%zu",
                 changeConfidence, similarity, detector.previousDetections.size(), newDetections.size());
        detector.lastSceneChangeTime = timestamp; // Record when scene change occurred
      }

      // Update state for next frame
      detector.previousDetections = newDetections;
      detector.lastTimestamp = timestamp;

      return sceneChanged;
    }

    float calculateDetectionSimilarity(const std::vector<Detection> & dets1, const std::vector<Detection> & dets2) {
      if (dets1.empty() && dets2.empty()) return 1.0f; // Both empty = similar
      if (dets1.empty() || dets2.empty()) {
        // If one is empty but the other has only a few detections, don't consider it a major change
        size_t nonEmptySize = dets1.empty() ? dets2.size() : dets1.size();
        return nonEmptySize <= 3 ? 0.8f : 0.3f; // Allow up to 3 new/lost detections without scene change
      }

      // Calculate spatial distribution similarity using a more relaxed method
      float totalSimilarity = 0.0f;
      int matches = 0;

      // Use a larger distance threshold for matching (less sensitive)
      const float maxDistance = 0.4f;
      const float alpha = 0.6f; // blend centers and IoU

      for (const auto & det1 : dets1) {
        float bestMatch = 0.0f;
        cv::Point2f center1(det1.x + det1.w / 2, det1.y + det1.h / 2);

        for (const auto & det2 : dets2) {
          cv::Point2f center2(det2.x + det2.w / 2, det2.y + det2.h / 2);
          float distance = cv::norm(center1 - center2);

          // Only consider matches within reasonable distance
          if (distance < maxDistance) {
            float centerSim = std::exp(-distance * 1.5f);
            float iou = 0.0f;
            // IoU on normalized boxes
            Detection a = det1, b = det2;
            float x1 = std::max(a.x, b.x);
            float y1 = std::max(a.y, b.y);
            float x2 = std::min(a.x + a.w, b.x + b.w);
            float y2 = std::min(a.y + a.h, b.y + b.h);
            if (x2 > x1 && y2 > y1) {
              float inter = (x2 - x1) * (y2 - y1);
              float uni = a.w * a.h + b.w * b.h - inter;
              if (uni > 0.0f) iou = inter / uni;
            }
            float sim = alpha * centerSim + (1.0f - alpha) * iou;
            if (det1.class_id == det2.class_id) { sim *= 1.2f; }
            bestMatch = std::max(bestMatch, sim);
          }
        }

        totalSimilarity += bestMatch;
        matches++;
      }

      // Normalize by number of detections in first frame
      float avgSimilarity = matches > 0 ? totalSimilarity / matches : 0.0f;

      // Less aggressive penalty for detection count differences
      float countRatio = std::min(dets1.size(), dets2.size()) / static_cast<float>(std::max(dets1.size(), dets2.size()));

      // Apply a more forgiving count penalty
      countRatio = std::sqrt(countRatio);

      return avgSimilarity * countRatio;
    }

    // Unified drawing function that combines tracking with pose features
    cv::Mat drawPoseDetectionsWithTracking(const cv::Mat & image, const std::vector<PoseDetection> & detections,
                                           bool showTrackIds, bool showConfidence) {
      // Step 1: Convert pose detections to base detections for tracking visualization
      std::vector<Detection> baseDetections;
      for (const auto & poseDet : detections) { baseDetections.push_back(static_cast<Detection>(poseDet)); }

      // Step 2: Draw base detections with tracking (trails, track confidence colors, etc.)
      cv::Mat result = Utils::drawDetectionsWithOptionalTracking(image, baseDetections, showTrackIds, showConfidence, true);

      // Step 3: Overlay pose-specific features (keypoints and skeleton)
      // COCO pose skeleton connections
      const std::vector<std::pair<int, int>> skeleton = {{0, 1},   {0, 2},   {1, 3},   {2, 4},  {5, 6},  {5, 7},
                                                         {7, 9},   {6, 8},   {8, 10},  {5, 11}, {6, 12}, {11, 12},
                                                         {11, 13}, {13, 15}, {12, 14}, {14, 16}};

      for (const auto & detection : detections) {
        // Draw keypoints (BGR format)
        for (size_t i = 0; i < detection.keypoints.size() && i < 17; ++i) {
          if (detection.keypoints[i].visible && detection.keypoints[i].confidence > 0.5f) {
            cv::Point2f kp(detection.keypoints[i].x * image.cols, detection.keypoints[i].y * image.rows);
            cv::circle(result, kp, 4, cv::Scalar(0, 0, 255), -1); // Red keypoints
          }
        }

        // Draw skeleton (BGR format)
        for (const auto & connection : skeleton) {
          if (connection.first < detection.keypoints.size() && connection.second < detection.keypoints.size()) {
            const auto & kp1 = detection.keypoints[connection.first];
            const auto & kp2 = detection.keypoints[connection.second];

            if (kp1.visible && kp2.visible && kp1.confidence > 0.5f && kp2.confidence > 0.5f) {
              cv::Point2f pt1(kp1.x * image.cols, kp1.y * image.rows);
              cv::Point2f pt2(kp2.x * image.cols, kp2.y * image.rows);
              cv::line(result, pt1, pt2, cv::Scalar(0, 255, 255), 3); // Yellow skeleton lines (BGR)
            }
          }
        }
      }

      return result;
    }

    // Draw OBB detections with tracking trails and motion prediction
    cv::Mat drawOBBDetectionsWithTracking(const cv::Mat & image, const std::vector<OBBDetection> & detections,
                                          bool showTrackIds, bool showConfidence) {
      // Step 1: Convert OBB detections to base detections for tracking visualization
      std::vector<Detection> baseDetections;
      for (const auto & obbDet : detections) { baseDetections.push_back(static_cast<Detection>(obbDet)); }

      // Step 2: Draw base detections with tracking (trails, track confidence colors, etc.)
      cv::Mat result = Utils::drawDetectionsWithOptionalTracking(image, baseDetections, showTrackIds, showConfidence, true);

      // Step 3: Overlay OBB-specific features (oriented bounding boxes)
      for (const auto & detection : detections) {
        // Use track confidence to determine color intensity
        cv::Scalar color(0, 255, 0); // Default green
        if (detection.track_confidence > 0.0f) {
          // High confidence tracks get brighter colors
          int intensity = static_cast<int>(255 * std::min(1.0f, detection.track_confidence));
          color = cv::Scalar(0, intensity, 0); // Green with varying intensity
        }

        if (detection.corners.size() == 4) {
          // Draw the 4 corners of the oriented box
          std::vector<cv::Point> corners;
          for (const auto & corner : detection.corners) {
            corners.push_back(cv::Point(corner.x * image.cols, corner.y * image.rows));
          }

          // Draw lines between corners with thicker lines for tracked objects
          int lineThickness = (detection.track_id > 0) ? 3 : 2;
          for (size_t i = 0; i < 4; ++i) { cv::line(result, corners[i], corners[(i + 1) % 4], color, lineThickness); }
        }

        // Draw center point with track-aware styling
        cv::Point center(detection.center.x * image.cols, detection.center.y * image.rows);
        cv::Scalar centerColor =
          (detection.track_id > 0) ? cv::Scalar(0, 0, 255) : cv::Scalar(255, 0, 0); // Red for tracked, blue for new
        cv::circle(result, center, 4, centerColor, -1);

        // Draw angle indicator line from center
        if (detection.size.width > 0 && detection.size.height > 0) {
          float angleRad = detection.angle;
          float lineLength = std::min(detection.size.width, detection.size.height) * 0.3f * std::min(image.cols, image.rows);
          cv::Point endPoint(center.x + static_cast<int>(lineLength * cos(angleRad)),
                             center.y + static_cast<int>(lineLength * sin(angleRad)));
          cv::line(result, center, endPoint, cv::Scalar(255, 255, 0), 2); // Cyan angle indicator
        }

        // Enhanced labels with tracking info
        if (showConfidence || showTrackIds) {
          std::string label = detection.class_name;
          if (showTrackIds && detection.track_id > 0) {
            label += " ID:" + std::to_string(detection.track_id);
            if (detection.track_confidence > 0.0f) { label += cv::format(" (%.2f)", detection.track_confidence); }
          }
          if (showConfidence) {
            label += cv::format(" %.2f", detection.confidence);
            label += cv::format(" ∠%.1f°", detection.angle * 180.0f / M_PI);
          }

          // Position label above the center point
          cv::Point labelPos(center.x - 50, center.y - 15);

          // Draw label background for better readability
          cv::Size textSize = cv::getTextSize(label, cv::FONT_HERSHEY_SIMPLEX, 0.5, 1, nullptr);
          cv::Rect labelRect(labelPos.x - 2, labelPos.y - textSize.height - 2, textSize.width + 4, textSize.height + 4);
          drawFilledRectAlpha(result, labelRect, cv::Scalar(0, 0, 0), 0.5);

          cv::putText(result, label, labelPos, cv::FONT_HERSHEY_SIMPLEX, 0.5, color, 1);
        }
      }

      return result;
    }

    void drawFilledRectAlpha(cv::Mat &image, const cv::Rect &rect,
                             const cv::Scalar &color, double alpha) {
      cv::Rect safe = rect & cv::Rect(0, 0, image.cols, image.rows);
      if (safe.width <= 0 || safe.height <= 0) return;
      cv::Mat roi = image(safe);
      cv::Mat overlay(safe.size(), image.type(), color);
      cv::addWeighted(overlay, alpha, roi, 1.0 - alpha, 0.0, roi);
    }

  } // namespace Utils

  // ProcessingStats implementation
  void ProcessingStats::updateStats(const InferenceMetrics & metrics, int detectionCount, const std::string & codec,
                                    uint64_t width, uint64_t height) {
    std::lock_guard<std::mutex> lock(statsMutex);

    totalFrames++;
    totalDetections += detectionCount;
    totalInferenceTimeMs += metrics.inferenceTimeMs;
    totalPreprocessTimeMs += metrics.preprocessTimeMs;
    totalPostprocessTimeMs += metrics.postprocessTimeMs;
    totalJpegEncodeTimeMs += metrics.jpegEncodeTimeMs;

    // Enhanced bottleneck tracking
    totalVideoDecodeTimeMs += metrics.videoDecodeTimeMs;
    totalTemporalTrackingTimeMs += metrics.temporalTrackingTimeMs;
    totalSceneChangeTimeMs += metrics.sceneChangeTimeMs;
    totalTensorCreationTimeMs += metrics.tensorCreationTimeMs;
    totalTensorCopyTimeMs += metrics.tensorCopyTimeMs;
    totalNmsTimeMs += metrics.nmsTimeMs;
    totalKalmanFilterTimeMs += metrics.kalmanFilterTimeMs;

    // ONNX inference timing analysis
    if (metrics.inferenceTimeMs > 0) {
      // Track max inference time
      if (metrics.inferenceTimeMs > maxInferenceTimeMs) { maxInferenceTimeMs = metrics.inferenceTimeMs; }

      // Track min inference time
      if (metrics.inferenceTimeMs < minInferenceTimeMs) { minInferenceTimeMs = metrics.inferenceTimeMs; }

      // Calculate rolling average (exponential moving average)
      inferenceTimesCount++;
      if (inferenceTimesCount == 1) {
        rollingAvgInferenceMs = static_cast<double>(metrics.inferenceTimeMs);
      } else {
        // Use exponential moving average: 90% old, 10% new for stability
        rollingAvgInferenceMs = 0.9 * rollingAvgInferenceMs + 0.1 * metrics.inferenceTimeMs;
      }
    }

    // Track object counts
    totalTracksCreated += metrics.newTrackCount;
    totalTracksLost += metrics.lostTrackCount;

    if (detectionCount > 0) { framesWithDetections++; }

    lastCodec = codec;
    lastWidth = width;
    lastHeight = height;

    calculateAverages();
  }

  void ProcessingStats::calculateAverages() {
    if (totalFrames > 0) {
      avgInferenceMs = static_cast<double>(totalInferenceTimeMs) / totalFrames;
      avgPreprocessMs = static_cast<double>(totalPreprocessTimeMs) / totalFrames;
      avgPostprocessMs = static_cast<double>(totalPostprocessTimeMs) / totalFrames;
      avgJpegEncodeMs = static_cast<double>(totalJpegEncodeTimeMs) / totalFrames;
      avgVideoDecodeMs = static_cast<double>(totalVideoDecodeTimeMs) / totalFrames;
      avgTemporalTrackingMs = static_cast<double>(totalTemporalTrackingTimeMs) / totalFrames;
      avgSceneChangeMs = static_cast<double>(totalSceneChangeTimeMs) / totalFrames;
      avgTensorCreationMs = static_cast<double>(totalTensorCreationTimeMs) / totalFrames;
      avgTensorCopyMs = static_cast<double>(totalTensorCopyTimeMs) / totalFrames;
      avgNmsMs = static_cast<double>(totalNmsTimeMs) / totalFrames;
      avgKalmanFilterMs = static_cast<double>(totalKalmanFilterTimeMs) / totalFrames;
      avgDetectionsPerFrame = static_cast<double>(totalDetections) / totalFrames;

      // Calculate proper FPS using moving average over recent frames
      uint64_t currentTime = Util::bootSecs();
      if (lastStatsTime > 0) {
        uint64_t timeDiff = currentTime - lastStatsTime;
        if (timeDiff > 0) {
          // Calculate instantaneous FPS for this period
          double instantFps = 1.0; // Default to 1 FPS if only 1 frame processed
          if (totalFrames > 1) {
            // Use a moving window approach - calculate FPS based on recent processing
            double totalTimeMs = avgPreprocessMs + avgInferenceMs + avgPostprocessMs + avgJpegEncodeMs;
            if (totalTimeMs > 0) {
              instantFps = 1000.0 / totalTimeMs; // Convert ms to FPS
            }
          }

          // Apply exponential moving average for smooth FPS calculation
          if (fps == 0.0) {
            fps = instantFps; // First measurement
          } else {
            fps = 0.8 * fps + 0.2 * instantFps; // 80% old, 20% new
          }
        }
      }
      lastStatsTime = currentTime;
    }
  }

  void ProcessingStats::logStats() const {
    if (totalFrames == 0) {
      MEDIUM_MSG("ONNX Processing Stats: No frames processed yet");
      return;
    }

    double detectionRate = (framesWithDetections * 100.0) / totalFrames;

    MEDIUM_MSG("=== ONNX Processing Statistics ===");
    MEDIUM_MSG("Total frames processed: %" PRIu64, (uint64_t)totalFrames);
    MEDIUM_MSG("Total detections found: %" PRIu64, (uint64_t)totalDetections);
    MEDIUM_MSG("Frames with detections: %" PRIu64 " (%.1f%%)", (uint64_t)framesWithDetections, detectionRate);
    MEDIUM_MSG("Average detections per frame: %.2f", avgDetectionsPerFrame);
    MEDIUM_MSG("Current FPS: %.2f", fps);
    MEDIUM_MSG("Last video format: %s %" PRIu64 "x%" PRIu64, lastCodec.c_str(), (uint64_t)lastWidth, (uint64_t)lastHeight);
    MEDIUM_MSG("Performance averages:");
    MEDIUM_MSG("  - Video decoding: %.2fms", avgVideoDecodeMs);
    MEDIUM_MSG("  - Preprocessing: %.2fms", avgPreprocessMs);
    MEDIUM_MSG("  - ONNX inference: %.2fms", avgInferenceMs);
    MEDIUM_MSG("  - Postprocessing: %.2fms", avgPostprocessMs);
    MEDIUM_MSG("  - Temporal tracking: %.2fms", avgTemporalTrackingMs);
    MEDIUM_MSG("  - Scene change detection: %.2fms", avgSceneChangeMs);
    MEDIUM_MSG("  - JPEG encoding: %.2fms", avgJpegEncodeMs);
    MEDIUM_MSG("  - Total per frame: %.2fms",
               avgPreprocessMs + avgInferenceMs + avgPostprocessMs + avgJpegEncodeMs + avgTemporalTrackingMs + avgSceneChangeMs);

    // ONNX inference timing analysis
    if (inferenceTimesCount > 0) {
      MEDIUM_MSG("ONNX Inference Timing Analysis:");
      MEDIUM_MSG("  - Average inference time: %.2fms", avgInferenceMs);
      MEDIUM_MSG("  - Rolling average (recent): %.2fms", rollingAvgInferenceMs);
      MEDIUM_MSG("  - Maximum inference time: %" PRId64 "ms", (int64_t)maxInferenceTimeMs);
      if (minInferenceTimeMs != INT64_MAX) {
        MEDIUM_MSG("  - Minimum inference time: %" PRId64 "ms", (int64_t)minInferenceTimeMs);
      }
      MEDIUM_MSG("  - Inference time variance: %.2fms (max-min)",
                 static_cast<double>(maxInferenceTimeMs - (minInferenceTimeMs == INT64_MAX ? 0 : minInferenceTimeMs)));

      // Performance consistency analysis
      double varianceRatio = (maxInferenceTimeMs > 0) ? static_cast<double>(maxInferenceTimeMs) / rollingAvgInferenceMs : 1.0;
      if (varianceRatio > 2.0) {
        MEDIUM_MSG("  - WARNING: High inference time variance (%.1fx), inconsistent performance!", varianceRatio);
      } else if (varianceRatio > 1.5) {
        MEDIUM_MSG("  - NOTICE: Moderate inference time variance (%.1fx)", varianceRatio);
      } else {
        MEDIUM_MSG("  - Good: Consistent inference timing (%.1fx variance)", varianceRatio);
      }
    }

    // Tracking statistics
    if (totalTracksCreated > 0 || totalTracksLost > 0) {
      MEDIUM_MSG("Tracking statistics:");
      MEDIUM_MSG("  - Total tracks created: %" PRIu64, (uint64_t)totalTracksCreated);
      MEDIUM_MSG("  - Total tracks lost: %" PRIu64, (uint64_t)totalTracksLost);
      MEDIUM_MSG("  - Scene changes detected: %" PRIu64, (uint64_t)sceneChangesDetected);
    }

    MEDIUM_MSG("================================");
  }

  // ---- SessionRunner: modality-agnostic ONNX session wrapper ----
  SessionRunner::SessionRunner() {
    const OrtApi *api = ORTHelpers::api();
    // Use the shared, process-global env (never created/owned per instance) — see
    // ORTHelpers::sharedEnv(). Multiple OrtEnv per process + teardown = ORT abort.
    env_ = ORTHelpers::sharedEnv();
    if (!env_) {
      throw std::runtime_error("Failed to create OrtEnv");
    }
    if (OrtStatus *st = api->CreateSessionOptions(&opts_)) {
      std::string message = api->GetErrorMessage(st);
      api->ReleaseStatus(st);
      throw std::runtime_error("Failed to create OrtSessionOptions: " + message);
    }
    if (OrtStatus *st = api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memInfo_)) {
      std::string message = api->GetErrorMessage(st);
      api->ReleaseStatus(st);
      api->ReleaseSessionOptions(opts_); opts_ = nullptr;
      throw std::runtime_error("Failed to create OrtMemoryInfo: " + message);
    }
    if (OrtStatus *st = api->GetAllocatorWithDefaultOptions(&allocator_)) {
      std::string message = api->GetErrorMessage(st);
      api->ReleaseStatus(st);
      api->ReleaseMemoryInfo(memInfo_); memInfo_ = nullptr;
      api->ReleaseSessionOptions(opts_); opts_ = nullptr;
      throw std::runtime_error("Failed to get default allocator: " + message);
    }
  }

  SessionRunner::~SessionRunner() {
    const OrtApi *api = ORTHelpers::api();
    for (auto *s : inputNameStrings_) { if (s && allocator_) allocator_->Free(allocator_, s); }
    for (auto *s : outputNameStrings_) { if (s && allocator_) allocator_->Free(allocator_, s); }
    if (session_) api->ReleaseSession(session_);
    if (opts_) api->ReleaseSessionOptions(opts_);
    if (memInfo_) api->ReleaseMemoryInfo(memInfo_);
    // env_ is the shared process-global env (ORTHelpers::sharedEnv); intentionally NOT
    // released here — releasing it (esp. at static teardown) is what aborted.
  }

  bool SessionRunner::load(const std::string &modelPath, int numThreads,
                           const std::string &requestedEP, std::string &err, bool lowLatency) {
    const OrtApi *api = ORTHelpers::api();
#ifdef MIST_ONNX_PROFILE
    const std::string buildProfile = MIST_ONNX_PROFILE;
#else
    const std::string buildProfile = "cpu";
#endif
    bool providerInProfile = requestedEP == buildProfile ||
                             (buildProfile == "tensorrt" && requestedEP == "cuda");
    if (!requestedEP.empty() && requestedEP != "cpu" && !providerInProfile) {
      err = "Execution provider '" + requestedEP + "' is not included in this '" +
            buildProfile + "' ONNX build profile";
      return false;
    }

    // Intra-op thread count. Always set an explicit value (>=1); ORT's default of 0 means
    // "use every core", which oversubscribes MistServer's one-process-per-stream model.
    // The caller clamps to a sane range; we only guard the lower bound here.
    if (numThreads < 1) { numThreads = 1; }
    if (!ORTHelpers::checkStatus(api->SetIntraOpNumThreads(opts_, numThreads), "SetIntraOpNumThreads")) {
      WARN_MSG("Failed to set intra-op thread count to %d", numThreads);
    }

    // Session recipe (mirrors the upstream Parakeet service, adapted for our process model):
    // full graph optimisation, sequential execution, a single inter-op thread, and flush
    // denormals to zero (avoids AVX2/FMA slowdowns on quantised weights). Intra-op
    // hot-spinning is OFF by default — it pins a core per session, which is fine for a single
    // dedicated server but oversubscribes a host running many stream processes; enable it via
    // lowLatency for single-stream latency-critical use. Each call is best-effort but its
    // OrtStatus is released to avoid leaking on runtimes that reject a key.
    if (OrtStatus *st = api->SetSessionGraphOptimizationLevel(opts_, ORT_ENABLE_ALL)) { api->ReleaseStatus(st); }
    if (OrtStatus *st = api->SetSessionExecutionMode(opts_, ORT_SEQUENTIAL)) { api->ReleaseStatus(st); }
    if (!ORTHelpers::checkStatus(api->SetInterOpNumThreads(opts_, 1), "SetInterOpNumThreads")) {
      WARN_MSG("Failed to set inter-op thread count to 1");
    }
    if (OrtStatus *st = api->AddSessionConfigEntry(opts_, "session.set_denormal_as_zero", "1")) { api->ReleaseStatus(st); }
    if (OrtStatus *st = api->AddSessionConfigEntry(opts_, "session.intra_op.allow_spinning", lowLatency ? "1" : "0")) { api->ReleaseStatus(st); }
    if (OrtStatus *st = api->AddSessionConfigEntry(opts_, "session.inter_op.allow_spinning", "0")) { api->ReleaseStatus(st); }

    // Register execution providers.
    // CUDA/TensorRT require the V2 opaque-options API; other EPs use the generic API.
    if (requestedEP != "cpu") {
      bool epExplicit = !requestedEP.empty();
      bool epRegistered = false;

      auto tryRegisterCUDA = [&]() -> bool {
        OrtCUDAProviderOptionsV2 *cudaOpts = nullptr;
        OrtStatus *st = api->CreateCUDAProviderOptions(&cudaOpts);
        if (st) {
          const char *msg = api->GetErrorMessage(st);
          MEDIUM_MSG("CUDA EP not available: %s", msg ? msg : "(unknown)");
          api->ReleaseStatus(st);
          return false;
        }
        // device_id plus conv-autotuning knobs from the upstream Parakeet service:
        // exhaustive cuDNN conv search + max workspace trade startup time for the
        // fastest kernels; copy in the default stream avoids extra sync points.
        const char *keys[] = {"device_id", "cudnn_conv_algo_search",
                              "cudnn_conv_use_max_workspace", "do_copy_in_default_stream"};
        const char *vals[] = {"0", "EXHAUSTIVE", "1", "1"};
        st = api->UpdateCUDAProviderOptions(cudaOpts, keys, vals, 4);
        if (st) {
          const char *msg = api->GetErrorMessage(st);
          MEDIUM_MSG("CUDA EP option update failed: %s", msg ? msg : "(unknown)");
          api->ReleaseStatus(st);
          api->ReleaseCUDAProviderOptions(cudaOpts);
          return false;
        }
        st = api->SessionOptionsAppendExecutionProvider_CUDA_V2(opts_, cudaOpts);
        api->ReleaseCUDAProviderOptions(cudaOpts);
        if (st) {
          const char *msg = api->GetErrorMessage(st);
          MEDIUM_MSG("CUDA EP registration failed: %s", msg ? msg : "(unknown)");
          api->ReleaseStatus(st);
          return false;
        }
        activeEP_ = "CUDA";
        INFO_MSG("Execution provider enabled: CUDA");
        return true;
      };

      auto tryRegisterTensorRT = [&]() -> bool {
        OrtTensorRTProviderOptionsV2 *trtOpts = nullptr;
        OrtStatus *st = api->CreateTensorRTProviderOptions(&trtOpts);
        if (st) {
          const char *msg = api->GetErrorMessage(st);
          MEDIUM_MSG("TensorRT EP not available: %s", msg ? msg : "(unknown)");
          api->ReleaseStatus(st);
          return false;
        }
        const char *keys[] = {"device_id"};
        const char *vals[] = {"0"};
        st = api->UpdateTensorRTProviderOptions(trtOpts, keys, vals, 1);
        if (st) {
          const char *msg = api->GetErrorMessage(st);
          MEDIUM_MSG("TensorRT EP option update failed: %s", msg ? msg : "(unknown)");
          api->ReleaseStatus(st);
          api->ReleaseTensorRTProviderOptions(trtOpts);
          return false;
        }
        st = api->SessionOptionsAppendExecutionProvider_TensorRT_V2(opts_, trtOpts);
        api->ReleaseTensorRTProviderOptions(trtOpts);
        if (st) {
          const char *msg = api->GetErrorMessage(st);
          MEDIUM_MSG("TensorRT EP registration failed: %s", msg ? msg : "(unknown)");
          api->ReleaseStatus(st);
          return false;
        }
        activeEP_ = "TensorRT";
        INFO_MSG("Execution provider enabled: TensorRT");
        return true;
      };

      auto tryRegisterGenericEP = [&](const char *name, const char *const *keys,
                                      const char *const *values, size_t numOpts) -> bool {
        OrtStatus *st = api->SessionOptionsAppendExecutionProvider(opts_, name, keys, values, numOpts);
        if (st) {
          const char *msg = api->GetErrorMessage(st);
          MEDIUM_MSG("%s EP not available: %s", name, msg ? msg : "(unknown)");
          api->ReleaseStatus(st);
          return false;
        }
        activeEP_ = name;
        INFO_MSG("Execution provider enabled: %s", name);
        return true;
      };

      const char *oviKeys[] = {"device_type"};
      const char *oviVals[] = {"AUTO"};

      if (!epExplicit) {
        if (buildProfile == "cuda") { epRegistered = tryRegisterCUDA(); }
        else if (buildProfile == "tensorrt") {
          epRegistered = tryRegisterTensorRT();
          if (epRegistered) {
            bool cudaFallback = tryRegisterCUDA();
            activeEP_ = cudaFallback ? "TensorRT+CUDA" : "TensorRT";
          }
        }
        else if (buildProfile == "coreml") { epRegistered = tryRegisterGenericEP("CoreML", nullptr, nullptr, 0); }
        else if (buildProfile == "openvino") { epRegistered = tryRegisterGenericEP("OpenVINO", oviKeys, oviVals, 1); }
        if (!epRegistered) {
          MEDIUM_MSG("ONNX build profile '%s' is using CPU", buildProfile.c_str());
        }
      } else {
        if (requestedEP == "cuda") { epRegistered = tryRegisterCUDA(); }
        else if (requestedEP == "tensorrt") {
          epRegistered = tryRegisterTensorRT();
          if (epRegistered) {
            bool cudaFallback = tryRegisterCUDA();
            activeEP_ = cudaFallback ? "TensorRT+CUDA" : "TensorRT";
          }
        }
        else if (requestedEP == "coreml") { epRegistered = tryRegisterGenericEP("CoreML", nullptr, nullptr, 0); }
        else if (requestedEP == "openvino") { epRegistered = tryRegisterGenericEP("OpenVINO", oviKeys, oviVals, 1); }
        else {
          err = "Unknown execution provider: " + requestedEP;
          return false;
        }
        if (!epRegistered) {
          err = "Requested execution provider '" + requestedEP + "' could not be registered";
          return false;
        }
      }
    }

    // Create session
    if (OrtStatus *st = api->CreateSession(env_, modelPath.c_str(), opts_, &session_)) {
      err = "Failed to create ONNX session for model " + modelPath + ": " + api->GetErrorMessage(st);
      api->ReleaseStatus(st);
      return false;
    }
    // Report the EP that was registered for this session. NOTE: this is the requested/
    // registered provider, not proof of per-node assignment — ORT can still fall back to
    // CPU for unsupported nodes, and the C API exposes no simple per-node EP query.
    MEDIUM_MSG("ONNX session ready (registered EP=%s): %s", activeEP_.c_str(), modelPath.c_str());

    // Enumerate input ports (names + shapes/dtypes), making no assumptions about them.
    size_t inCount = 0;
    if (!ORTHelpers::checkStatus(api->SessionGetInputCount(session_, &inCount), "SessionGetInputCount")) {
      err = "SessionGetInputCount failed";
      return false;
    }
    for (size_t i = 0; i < inCount; ++i) {
      char *name = nullptr;
      if (!ORTHelpers::checkStatus(api->SessionGetInputName(session_, i, allocator_, &name), "SessionGetInputName")) {
        err = "SessionGetInputName failed";
        return false;
      }
      inputNameStrings_.push_back(name);
      inputNames_.push_back(name);

      TensorSpec spec;
      spec.name = name;
      OrtTypeInfo *ti = nullptr;
      if (ORTHelpers::checkStatus(api->SessionGetInputTypeInfo(session_, i, &ti), "SessionGetInputTypeInfo")) {
        const OrtTensorTypeAndShapeInfo *ttsi = nullptr;
        if (ORTHelpers::checkStatus(api->CastTypeInfoToTensorInfo(ti, &ttsi), "CastTypeInfoToTensorInfo") && ttsi) {
          spec.shape = ORTHelpers::getTensorShape(ttsi);
          (void)api->GetTensorElementType(ttsi, &spec.dtype);
        }
        if (ti) api->ReleaseTypeInfo(ti);
      }
      inputSpecs_.push_back(spec);
    }

    // Enumerate output ports.
    size_t outCount = 0;
    if (!ORTHelpers::checkStatus(api->SessionGetOutputCount(session_, &outCount), "SessionGetOutputCount")) {
      err = "SessionGetOutputCount failed";
      return false;
    }
    for (size_t i = 0; i < outCount; ++i) {
      char *name = nullptr;
      if (!ORTHelpers::checkStatus(api->SessionGetOutputName(session_, i, allocator_, &name), "SessionGetOutputName")) {
        err = "SessionGetOutputName failed";
        return false;
      }
      outputNameStrings_.push_back(name);
      outputNames_.push_back(name);

      TensorSpec spec;
      spec.name = name;
      OrtTypeInfo *ti = nullptr;
      if (ORTHelpers::checkStatus(api->SessionGetOutputTypeInfo(session_, i, &ti), "SessionGetOutputTypeInfo")) {
        const OrtTensorTypeAndShapeInfo *ttsi = nullptr;
        if (ORTHelpers::checkStatus(api->CastTypeInfoToTensorInfo(ti, &ttsi), "CastTypeInfoToTensorInfo") && ttsi) {
          spec.shape = ORTHelpers::getTensorShape(ttsi);
          (void)api->GetTensorElementType(ttsi, &spec.dtype);
        }
        if (ti) api->ReleaseTypeInfo(ti);
      }
      outputSpecs_.push_back(spec);
    }

    if (inputSpecs_.empty()) { err = "Model has no inputs"; return false; }
    if (outputSpecs_.empty()) { err = "Model has no outputs"; return false; }
    return true;
  }

  bool SessionRunner::run(const std::vector<OrtValue *> &inputs, std::vector<OrtValue *> &outputs,
                          std::string &err) {
    if (!session_) { err = "Session not loaded"; return false; }
    const OrtApi *api = ORTHelpers::api();

    // Substitute owned state tensors at bound state inputs left as nullptr
    std::vector<OrtValue *> effective = inputs;
    std::vector<OrtValue *> stateTensors; // owned for the duration of this call
    if (!stateLoops_.empty()) {
      if (effective.size() < inputSpecs_.size()) { effective.resize(inputSpecs_.size(), nullptr); }
      for (StateLoop & loop : stateLoops_) {
        if (loop.inIdx < effective.size() && !effective[loop.inIdx]) {
          OrtValue *sv = createFloatTensor(loop.buffer.data(), loop.buffer.size(), loop.shape, err);
          if (!sv) {
            for (OrtValue *v : stateTensors) { api->ReleaseValue(v); }
            return false;
          }
          stateTensors.push_back(sv);
          effective[loop.inIdx] = sv;
        }
      }
    }

    outputs.assign(outputNames_.size(), nullptr);
    OrtStatus *st = api->Run(session_, nullptr, inputNames_.data(),
                             const_cast<const OrtValue *const *>(effective.data()), effective.size(),
                             outputNames_.data(), outputs.size(), outputs.data());
    for (OrtValue *v : stateTensors) { api->ReleaseValue(v); }
    if (st != nullptr) {
      const char *msg = api->GetErrorMessage(st);
      err = msg ? msg : "(unknown)";
      api->ReleaseStatus(st);
      return false;
    }

    // Copy each bound output back into its owned state buffer for the next call
    for (StateLoop & loop : stateLoops_) {
      if (loop.outIdx >= outputs.size() || !outputs[loop.outIdx]) { continue; }
      float *data = nullptr;
      OrtStatus *ds = api->GetTensorMutableData(outputs[loop.outIdx], (void **)&data);
      if (ds) { api->ReleaseStatus(ds); continue; }
      if (!data) { continue; }
      OrtTensorTypeAndShapeInfo *info = nullptr;
      OrtStatus *is = api->GetTensorTypeAndShape(outputs[loop.outIdx], &info);
      if (is) { api->ReleaseStatus(is); continue; }
      size_t count = 0;
      (void)api->GetTensorShapeElementCount(info, &count);
      if (count > 0) {
        if (count != loop.buffer.size()) {
          // Adopt the model's actual state shape (seed guessed dynamic dims as 1)
          loop.shape = ORTHelpers::getTensorShape(info);
          loop.buffer.resize(count);
        }
        std::memcpy(loop.buffer.data(), data, count * sizeof(float));
      }
      api->ReleaseTensorTypeAndShapeInfo(info);
    }
    return true;
  }

  bool SessionRunner::runTensors(const std::vector<TensorData> &inputs,
                                 std::vector<TensorData> &outputs, std::string &err) {
    outputs.clear();
    if (!session_) { err = "Session not loaded"; return false; }
    if (inputs.size() != inputSpecs_.size()) {
      err = "Expected " + std::to_string(inputSpecs_.size()) + " input tensors, got " +
            std::to_string(inputs.size());
      return false;
    }
    std::vector<const TensorData *> ordered(inputSpecs_.size(), nullptr);
    for (size_t supplied = 0; supplied < inputs.size(); ++supplied) {
      size_t target = supplied;
      if (!inputs[supplied].name.empty()) {
        target = inputSpecs_.size();
        for (size_t i = 0; i < inputSpecs_.size(); ++i) {
          if (inputSpecs_[i].name == inputs[supplied].name) { target = i; break; }
        }
        if (target == inputSpecs_.size()) { err = "Unknown model input '" + inputs[supplied].name + "'"; return false; }
      }
      if (ordered[target]) { err = "Duplicate model input '" + inputSpecs_[target].name + "'"; return false; }
      ordered[target] = &inputs[supplied];
    }

    const OrtApi *api = ORTHelpers::api();
    std::vector<OrtValue *> ortInputs(inputSpecs_.size(), nullptr);
    auto releaseInputs = [&]() { for (OrtValue *v : ortInputs) if (v) api->ReleaseValue(v); };
    for (size_t i = 0; i < inputSpecs_.size(); ++i) {
      if (!ordered[i]) { err = "Missing model input '" + inputSpecs_[i].name + "'"; releaseInputs(); return false; }
      const TensorData &tensor = *ordered[i];
      if (tensor.dtype != inputSpecs_[i].dtype) {
        err = "Input '" + inputSpecs_[i].name + "' dtype mismatch: expected " +
              TensorWire::dtypeName(inputSpecs_[i].dtype) + ", got " + TensorWire::dtypeName(tensor.dtype);
        releaseInputs(); return false;
      }
      if (tensor.shape.size() != inputSpecs_[i].shape.size()) {
        err = "Input '" + inputSpecs_[i].name + "' rank mismatch"; releaseInputs(); return false;
      }
      for (size_t d = 0; d < tensor.shape.size(); ++d) {
        if (tensor.shape[d] < 0 || (inputSpecs_[i].shape[d] >= 0 && tensor.shape[d] != inputSpecs_[i].shape[d])) {
          err = "Input '" + inputSpecs_[i].name + "' shape mismatch"; releaseInputs(); return false;
        }
      }
      size_t elem = TensorWire::elementSize(tensor.dtype), count = 1;
      if (!elem) { err = "Unsupported input dtype"; releaseInputs(); return false; }
      for (int64_t d : tensor.shape) {
        if (d && count > SIZE_MAX / (size_t)d) { err = "Input shape overflow"; releaseInputs(); return false; }
        count *= (size_t)d;
      }
      if (count > SIZE_MAX / elem || count * elem != tensor.bytes.size()) {
        err = "Input byte length does not match shape"; releaseInputs(); return false;
      }
      OrtStatus *st = api->CreateTensorWithDataAsOrtValue(
        memInfo_, const_cast<uint8_t *>(tensor.bytes.data()), tensor.bytes.size(),
        tensor.shape.data(), tensor.shape.size(), tensor.dtype, &ortInputs[i]);
      if (st) {
        err = api->GetErrorMessage(st); api->ReleaseStatus(st); releaseInputs(); return false;
      }
    }

    std::vector<OrtValue *> ortOutputs;
    bool ok = run(ortInputs, ortOutputs, err);
    releaseInputs();
    if (!ok) { for (OrtValue *v : ortOutputs) if (v) api->ReleaseValue(v); return false; }
    ORTHelpers::OrtOutputsGuard outputGuard(ortOutputs);
    outputs.reserve(ortOutputs.size());
    for (size_t i = 0; i < ortOutputs.size(); ++i) {
      OrtTensorTypeAndShapeInfo *info = nullptr;
      OrtStatus *st = api->GetTensorTypeAndShape(ortOutputs[i], &info);
      if (st) { err = api->GetErrorMessage(st); api->ReleaseStatus(st); return false; }
      TensorData tensor;
      tensor.name = outputSpecs_[i].name;
      tensor.shape = ORTHelpers::getTensorShape(info);
      st = api->GetTensorElementType(info, &tensor.dtype);
      if (st) {
        err = api->GetErrorMessage(st); api->ReleaseStatus(st);
        api->ReleaseTensorTypeAndShapeInfo(info); return false;
      }
      size_t count = 0;
      st = api->GetTensorShapeElementCount(info, &count);
      if (st) {
        err = api->GetErrorMessage(st); api->ReleaseStatus(st);
        api->ReleaseTensorTypeAndShapeInfo(info); return false;
      }
      api->ReleaseTensorTypeAndShapeInfo(info);
      size_t elem = TensorWire::elementSize(tensor.dtype);
      if (!elem || (count && count > SIZE_MAX / elem)) { err = "Unsupported or oversized output tensor"; return false; }
      void *data = nullptr;
      st = api->GetTensorMutableData(ortOutputs[i], &data);
      if (st) { err = api->GetErrorMessage(st); api->ReleaseStatus(st); return false; }
      tensor.bytes.resize(count * elem);
      if (!tensor.bytes.empty()) std::memcpy(tensor.bytes.data(), data, tensor.bytes.size());
      outputs.push_back(std::move(tensor));
    }
    return true;
  }

  bool SessionRunner::bindStateLoop(const std::string &outputName, const std::string &inputName,
                                    std::string &err) {
    if (!session_) { err = "Session not loaded"; return false; }
    size_t outIdx = outputSpecs_.size(), inIdx = inputSpecs_.size();
    for (size_t i = 0; i < outputSpecs_.size(); ++i) {
      if (outputSpecs_[i].name == outputName) { outIdx = i; break; }
    }
    for (size_t i = 0; i < inputSpecs_.size(); ++i) {
      if (inputSpecs_[i].name == inputName) { inIdx = i; break; }
    }
    if (outIdx == outputSpecs_.size() || inIdx == inputSpecs_.size()) {
      err = "State ports not found: " + outputName + " -> " + inputName;
      return false;
    }
    if (inputSpecs_[inIdx].dtype != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
      err = "State loop supports float32 ports only (input '" + inputName + "')";
      return false;
    }
    if (outputSpecs_[outIdx].dtype != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
      err = "State loop supports float32 ports only (output '" + outputName + "')";
      return false;
    }
    StateLoop loop;
    loop.outIdx = outIdx;
    loop.inIdx = inIdx;
    loop.shape = inputSpecs_[inIdx].shape;
    size_t count = 1;
    for (size_t i = 0; i < loop.shape.size(); ++i) {
      if (loop.shape[i] < 1) { loop.shape[i] = 1; }
      count *= (size_t)loop.shape[i];
    }
    loop.buffer.assign(count, 0.0f);
    stateLoops_.push_back(loop);
    return true;
  }

  void SessionRunner::resetState() {
    for (StateLoop & loop : stateLoops_) {
      std::fill(loop.buffer.begin(), loop.buffer.end(), 0.0f);
    }
  }

  namespace {
    // IEEE-754 half (float16) <-> float32. ORT's C API exposes no conversion helper, so
    // we convert at the tensor boundary for FP16 model variants. Denormals/inf/nan handled.
    float halfToFloat(uint16_t h) {
      uint32_t sign = (uint32_t)(h & 0x8000) << 16;
      uint32_t exp = (h >> 10) & 0x1F;
      uint32_t mant = h & 0x3FF;
      uint32_t f;
      if (exp == 0) {
        if (mant == 0) { f = sign; }
        else {                                   // subnormal -> normalised float32
          uint32_t e = 0;
          while ((mant & 0x400) == 0) { mant <<= 1; e++; }
          mant &= 0x3FF;
          f = sign | ((127 - 15 - e + 1) << 23) | (mant << 13);
        }
      } else if (exp == 0x1F) {
        f = sign | 0x7F800000u | (mant << 13);   // inf / nan
      } else {
        f = sign | ((exp - 15 + 127) << 23) | (mant << 13);
      }
      float out; std::memcpy(&out, &f, sizeof(out)); return out;
    }

    uint16_t floatToHalf(float value) {
      uint32_t f; std::memcpy(&f, &value, sizeof(f));
      uint32_t sign = (f >> 16) & 0x8000;
      int32_t exp = (int32_t)((f >> 23) & 0xFF) - 127 + 15;
      uint32_t mant = f & 0x7FFFFF;
      if (((f >> 23) & 0xFF) == 0xFF) {           // inf / nan
        return (uint16_t)(sign | 0x7C00 | (mant ? 0x200 : 0));
      }
      if (exp >= 0x1F) { return (uint16_t)(sign | 0x7C00); }  // overflow -> inf
      if (exp <= 0) {                              // subnormal / underflow
        if (exp < -10) { return (uint16_t)sign; }
        mant |= 0x800000;
        uint32_t shift = (uint32_t)(14 - exp);
        uint32_t half = mant >> shift;
        uint32_t rem = mant & ((1u << shift) - 1);
        if (rem > (1u << (shift - 1)) || (rem == (1u << (shift - 1)) && (half & 1))) { half++; }
        return (uint16_t)(sign | half);
      }
      uint16_t half = (uint16_t)(sign | (exp << 10) | (mant >> 13));
      if ((mant & 0x1000) && ((mant & 0x0FFF) || (half & 1))) { half++; }  // round to nearest even
      return half;
    }
  } // namespace

  OrtValue *SessionRunner::createFloatTensor(const float *data, size_t count,
                                             const std::vector<int64_t> &shape, std::string &err) {
    const OrtApi *api = ORTHelpers::api();
    OrtValue *val = nullptr;
    OrtStatus *st = api->CreateTensorWithDataAsOrtValue(
      memInfo_, const_cast<float *>(data), count * sizeof(float), shape.data(), shape.size(),
      ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &val);
    if (st != nullptr) {
      const char *msg = api->GetErrorMessage(st);
      err = msg ? msg : "(unknown)";
      api->ReleaseStatus(st);
      return nullptr;
    }
    return val;
  }

  OrtValue *SessionRunner::createRealTensor(const float *data, size_t count,
                                            const std::vector<int64_t> &shape,
                                            ONNXTensorElementDataType dtype, std::string &err) {
    // FLOAT (and any non-half type) uses the zero-copy borrow path; the caller's buffer
    // must outlive the tensor. FLOAT16 allocates an ORT-owned buffer and converts into it,
    // so the source buffer need not outlive the returned tensor.
    if (dtype != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16) {
      return createFloatTensor(data, count, shape, err);
    }
    // The FLOAT path gets count-vs-shape validation from ORT (byte length checked against the
    // shape); the FP16 path allocates by shape and writes count halfs, so enforce the same
    // invariant here rather than risk a silent overflow/uninitialised tail on a mismatch.
    size_t shapeCount = 1;
    for (int64_t d : shape) { shapeCount *= (d > 0 ? (size_t)d : 0); }
    if (shapeCount != count) {
      err = "createRealTensor: element count does not match shape product";
      return nullptr;
    }
    const OrtApi *api = ORTHelpers::api();
    OrtValue *val = nullptr;
    OrtStatus *st = api->CreateTensorAsOrtValue(allocator_, shape.data(), shape.size(),
                                                ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16, &val);
    if (st != nullptr) {
      const char *msg = api->GetErrorMessage(st);
      err = msg ? msg : "(unknown)";
      api->ReleaseStatus(st);
      return nullptr;
    }
    void *raw = nullptr;
    if ((st = api->GetTensorMutableData(val, &raw)) != nullptr) {
      const char *msg = api->GetErrorMessage(st);
      err = msg ? msg : "(unknown)";
      api->ReleaseStatus(st);
      api->ReleaseValue(val);
      return nullptr;
    }
    uint16_t *h = (uint16_t *)raw;
    for (size_t i = 0; i < count; ++i) { h[i] = floatToHalf(data[i]); }
    return val;
  }

  OrtValue *SessionRunner::createInt32Tensor(const int32_t *data, size_t count,
                                             const std::vector<int64_t> &shape, std::string &err) {
    const OrtApi *api = ORTHelpers::api();
    OrtValue *val = nullptr;
    OrtStatus *st = api->CreateTensorWithDataAsOrtValue(
      memInfo_, const_cast<int32_t *>(data), count * sizeof(int32_t), shape.data(), shape.size(),
      ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32, &val);
    if (st != nullptr) {
      const char *msg = api->GetErrorMessage(st);
      err = msg ? msg : "(unknown)";
      api->ReleaseStatus(st);
      return nullptr;
    }
    return val;
  }

  OrtValue *SessionRunner::createInt64Tensor(const int64_t *data, size_t count,
                                             const std::vector<int64_t> &shape, std::string &err) {
    const OrtApi *api = ORTHelpers::api();
    OrtValue *val = nullptr;
    OrtStatus *st = api->CreateTensorWithDataAsOrtValue(
      memInfo_, const_cast<int64_t *>(data), count * sizeof(int64_t), shape.data(), shape.size(),
      ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &val);
    if (st != nullptr) {
      const char *msg = api->GetErrorMessage(st);
      err = msg ? msg : "(unknown)";
      api->ReleaseStatus(st);
      return nullptr;
    }
    return val;
  }

  // DetectionModel implementation
  DetectionModel::DetectionModel(const std::string & modelPath, int inputSize)
    : modelPath_(modelPath), inputWidth_(inputSize), inputHeight_(inputSize), initialized_(false), enhanceImage_(false), softNmsSigma_(0.5f) {
  }

  DetectionModel::~DetectionModel() {}

  void DetectionModel::runSession(const std::vector<OrtValue *> & inputs, std::vector<OrtValue *> & outputs,
                                  const char *ctx) {
    std::string err;
    if (!runner_.run(inputs, outputs, err)) {
      throw std::runtime_error(std::string(ctx) + " Run failed: " + err);
    }
  }

  bool DetectionModel::initialize(int numThreads) {
    try {
      // Load the model through the neutral runtime layer (owns session + I/O).
      std::string loadErr;
      if (!runner_.load(modelPath_, numThreads, requestedEP_, loadErr, lowLatency_)) {
        ERROR_MSG("Failed to load ONNX model %s: %s", modelPath_.c_str(), loadErr.c_str());
        return false;
      }
      activeEP_ = runner_.activeEP();

      // Vision adapter input validation: expect a single 4D NCHW RGB image input.
      // This assumption lives HERE (in the vision adapter), not in SessionRunner, so
      // a future non-image adapter can load models that this check would reject.
      const std::vector<TensorSpec> &inSpecs = runner_.inputs();
      const std::vector<int64_t> &inputShape = inSpecs[0].shape;
      if (inputShape.size() != 4) {
        ERROR_MSG("Expected 4D input tensor, got %zu dimensions", inputShape.size());
        return false;
      }
      INFO_MSG("Model input shape: [%" PRId64 ", %" PRId64 ", %" PRId64 ", %" PRId64 "]",
               inputShape[0], inputShape[1], inputShape[2], inputShape[3]);
      if (inputShape[2] == -1 || inputShape[3] == -1) {
        WARN_MSG("Model has dynamic spatial dimensions (%" PRId64 "x%" PRId64 "), using configured %dx%d",
                 inputShape[2], inputShape[3], inputHeight_, inputWidth_);
      } else {
        if (inputShape[2] != inputHeight_ || inputShape[3] != inputWidth_) {
          WARN_MSG("Model expects input %" PRId64 "x%" PRId64 " (HxW), configured for %dx%d. Adjusting...",
                   inputShape[2], inputShape[3], inputHeight_, inputWidth_);
        }
        inputHeight_ = (int)inputShape[2];
        inputWidth_ = (int)inputShape[3];
      }
      // Accept 3 channels, or a dynamic channel dim (-1): the vision preprocessor
      // always feeds a 3-channel tensor, which a dynamic-channel model accepts. This
      // matches the modality gate (classifyModality), which treats 3/-1 as vision.
      if (inputShape[1] != 3 && inputShape[1] != -1) {
        ERROR_MSG("Expected 3 channels (RGB), got %" PRId64 " channels", (int64_t)inputShape[1]);
        return false;
      }

      // Determine model type: use override if set, otherwise auto-detect
      if (modelTypeOverride_ != ModelType::UNKNOWN) {
        modelInfo_.type = modelTypeOverride_;
        INFO_MSG("Using model type override: %d", (int)modelTypeOverride_);
      } else {
        modelInfo_.type = detectModelType();
      }

      // Populate model info based on detected type
      switch (modelInfo_.type) {
        case ModelType::YOLOV8_DETECTION:
        case ModelType::YOLO11_DETECTION:
          modelInfo_.name = "YOLOv8/YOLO11 Detection";
          modelInfo_.numClasses = 80; // COCO dataset
          break;
        case ModelType::YOLOV8_POSE:
        case ModelType::YOLO11_POSE:
          modelInfo_.name = "YOLOv8/YOLO11 Pose";
          modelInfo_.numClasses = 1; // Person class only
          break;
        case ModelType::YOLOV8_SEGMENTATION:
        case ModelType::YOLO11_SEGMENTATION:
          modelInfo_.name = "YOLOv8/YOLO11 Segmentation";
          modelInfo_.numClasses = 80; // COCO dataset
          break;
        case ModelType::YOLOV8_CLASSIFICATION:
        case ModelType::YOLO11_CLASSIFICATION:
          modelInfo_.name = "YOLOv8/YOLO11 Classification";
          // Number of classes determined from output shape
          if (!modelInfo_.outputShapes.empty() && !modelInfo_.outputShapes[0].empty()) {
            modelInfo_.numClasses = static_cast<int>(modelInfo_.outputShapes[0].back());
          }
          break;
        case ModelType::YOLOV8_OBB:
        case ModelType::YOLO11_OBB:
          modelInfo_.name = "YOLOv8/YOLO11 Oriented Bounding Boxes";
          modelInfo_.numClasses = 15; // DOTA dataset classes
          break;
        case ModelType::YOLO_NMS_DETECTION: modelInfo_.name = "YOLO NMS Detection"; modelInfo_.numClasses = 80; break;
        case ModelType::YOLO_SPLIT_DETECTION: modelInfo_.name = "End-to-end Split-output Detection"; modelInfo_.numClasses = 80; break;
        case ModelType::RT_DETR_DETECTION: modelInfo_.name = "RT-DETR Detection"; modelInfo_.numClasses = 80; break;
        case ModelType::DEPTH_ESTIMATION: modelInfo_.name = "Depth Estimation"; break;
        case ModelType::FACE_DETECTION_SCRFD: modelInfo_.name = "SCRFD Face Detection"; break;
        case ModelType::FACE_RECOGNITION_ARCFACE: modelInfo_.name = "ArcFace Recognition"; break;
        case ModelType::IMAGE_EMBEDDING: modelInfo_.name = "Image Embedding"; break;
        case ModelType::OCR: modelInfo_.name = "OCR"; break;
        case ModelType::FACE_ATTRIBUTE: modelInfo_.name = "Face Attribute (age/gender)"; break;
        case ModelType::POSE_RTMO: modelInfo_.name = "RTMO Pose"; break;
        case ModelType::SAM2_ENCODER: modelInfo_.name = "SAM2 Encoder"; break;
        case ModelType::SAM2_DECODER: modelInfo_.name = "SAM2 Decoder"; break;
        case ModelType::GENERIC_CLASSIFICATION:
          modelInfo_.name = "Generic Classification";
          {
            const std::vector<TensorSpec> &outs = runner_.outputs();
            if (!outs.empty() && outs[0].shape.size() == 2) {
              modelInfo_.numClasses = (int)outs[0].shape[1];
            }
          }
          break;
        case ModelType::GENERIC_DETECTION: modelInfo_.name = "Generic Detection"; break;
        default: modelInfo_.name = "Unknown Model"; break;
      }

      initialized_ = true;
      INFO_MSG("ONNX model initialized: %s (input: %dx%d)", modelPath_.c_str(), inputWidth_, inputHeight_);

      // Perform a test inference to validate ONNX Runtime stability
      try {
        INFO_MSG("Performing test inference to validate ONNX Runtime...");
        cv::Mat testFrame = cv::Mat::zeros(inputHeight_, inputWidth_, CV_8UC3);
        testFrame.setTo(cv::Scalar(128, 128, 128)); // Gray test image

        InferenceMetrics testMetrics;
        std::vector<Detection> testResult = processFrame(testFrame, 0.5f, 0.4f, &testMetrics);

        INFO_MSG("Test inference completed successfully: %zu detections, %" PRId64 " ms", testResult.size(),
                 (int64_t)testMetrics.totalTimeMs);
      } catch (const std::exception & e) {
        ERROR_MSG("Test inference failed: %s", e.what());
        initialized_ = false;
        return false;
      } catch (...) {
        ERROR_MSG("Test inference failed with unknown exception");
        initialized_ = false;
        return false;
      }

      return true;

    } catch (const std::exception & e) {
      ERROR_MSG("Failed to initialize ONNX model %s: %s", modelPath_.c_str(), e.what());
      return false;
    }
  }

  static const char *modalityName(ModelModality m) {
    switch (m) {
      case ModelModality::VISION: return "vision";
      case ModelModality::AUDIO: return "audio";
      case ModelModality::TENSOR: return "tensor";
      default: return "unknown";
    }
  }

  // Coarse input-modality gate: decide from the FIRST input port whether this is a
  // vision model the DetectionModel adapter can drive. The vision adapter requires a
  // 4D NCHW float image with 3 (or dynamic) channels; anything else is not force-fit.
  // Audio/other models are recognised (best-effort) so they can be reported and, later,
  // routed to a dedicated adapter instead of crashing the vision pipeline.
  static ModelModality classifyModality(const std::vector<TensorSpec> & inputs) {
    if (inputs.empty()) return ModelModality::UNKNOWN;
    const TensorSpec & in0 = inputs[0];
    const bool isFloat = in0.dtype == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
                         in0.dtype == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16 ||
                         in0.dtype == ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED; // tolerate unknown dtype

    // Vision: single 4D NCHW image tensor, channels 3 or dynamic.
    if (in0.shape.size() == 4 && isFloat) {
      int64_t ch = in0.shape[1];
      if (ch == 3 || ch == -1) return ModelModality::VISION;
    }

    // Audio heuristics: speech models take log-mel [1,80,T], features, or raw waveform.
    std::string n = in0.name;
    for (auto & c : n) { c = (char)std::tolower((unsigned char)c); }
    const bool audioName = n.find("mel") != std::string::npos || n.find("audio") != std::string::npos ||
                           n.find("feature") != std::string::npos || n.find("wave") != std::string::npos ||
                           n.find("speech") != std::string::npos;
    if (isFloat && (audioName || in0.shape.size() == 2 || in0.shape.size() == 3)) {
      return ModelModality::AUDIO;
    }
    return ModelModality::UNKNOWN;
  }

  // Shared pattern-based model type classification from output shapes/names/counts.
  // Works for any input size and class count (no hardcoded 8400/84/56/116).
  static ModelType classifyModelOutputs(const std::vector<std::vector<int64_t>> & outputShapesIn,
                                         const std::vector<std::string> & outputNames, int inputH, int inputW) {
    size_t outCount = outputShapesIn.size();
    if (outCount == 0) return ModelType::GENERIC_UNKNOWN;

    // Transformer exports (ViT classifiers, CLIP, etc) routinely leave the batch
    // dimension dynamic (-1). We always run batch 1, so treat a dynamic leading dim as
    // 1 for shape classification — otherwise e.g. a [-1,2] classifier head fails the
    // [1,N] check and falls through to GENERIC_UNKNOWN.
    std::vector<std::vector<int64_t>> outputShapes = outputShapesIn;
    for (size_t i = 0; i < outputShapes.size(); ++i) {
      if (!outputShapes[i].empty() && outputShapes[i][0] < 0) { outputShapes[i][0] = 1; }
    }
    const auto & shape0 = outputShapes[0];

    // RT-DETR: 3 named outputs (labels, boxes, scores)
    if (outCount == 3) {
      bool hasLabels = false, hasBoxes = false, hasScores = false;
      for (const auto & n : outputNames) {
        if (n.find("label") != std::string::npos) hasLabels = true;
        if (n.find("box") != std::string::npos) hasBoxes = true;
        if (n.find("score") != std::string::npos) hasScores = true;
      }
      if (hasLabels && hasBoxes && hasScores) {
        INFO_MSG("Model type detected: RT-DETR Detection (labels+boxes+scores outputs)");
        return ModelType::RT_DETR_DETECTION;
      }
    }

    // SCRFD: 6 or 9 outputs at 3 scales (score_8/16/32, bbox_8/16/32, [kps_8/16/32])
    if (outCount == 6 || outCount == 9) {
      int scoreCount = 0, bboxCount = 0;
      for (const auto & n : outputNames) {
        if (n.find("score") != std::string::npos) scoreCount++;
        if (n.find("bbox") != std::string::npos) bboxCount++;
      }
      if (scoreCount == 3 && bboxCount == 3) {
        INFO_MSG("Model type detected: SCRFD Face Detection (%zu outputs)", outCount);
        return ModelType::FACE_DETECTION_SCRFD;
      }
    }

    // SAM2 encoder: output named image_embed with shape [1, 256, 64, 64]
    if (outCount >= 2) {
      for (size_t i = 0; i < outCount; ++i) {
        if (outputNames[i].find("image_embed") != std::string::npos && outputShapes[i].size() == 4) {
          INFO_MSG("Model type detected: SAM2 Encoder");
          return ModelType::SAM2_ENCODER;
        }
        if (outputNames[i].find("masks") != std::string::npos && outputNames[i].find("iou") == std::string::npos) {
          for (size_t j = 0; j < outCount; ++j) {
            if (outputNames[j].find("iou") != std::string::npos) {
              INFO_MSG("Model type detected: SAM2 Decoder");
              return ModelType::SAM2_DECODER;
            }
          }
        }
      }
    }

    // ArcFace: single output [1, 512] (face embedding)
    if (outCount == 1 && shape0.size() == 2 && shape0[0] == 1 && shape0[1] == 512 && inputH == 112 && inputW == 112) {
      INFO_MSG("Model type detected: ArcFace Recognition (112x112, 512-d embedding)");
      return ModelType::FACE_RECOGNITION_ARCFACE;
    }

    // Split detection export: one [1,N,C] class-logit tensor and one [1,N,4]
    // normalized cxcywh box tensor. Match by shape so output names may vary.
    if (outCount == 2 && outputShapes[0].size() == 3 && outputShapes[1].size() == 3) {
      const auto &a = outputShapes[0];
      const auto &b = outputShapes[1];
      bool aScores = a[1] > 0 && a[2] > 4 && b[1] == a[1] && b[2] == 4;
      bool bScores = b[1] > 0 && b[2] > 4 && a[1] == b[1] && a[2] == 4;
      if (aScores || bScores) {
        int64_t queries = aScores ? a[1] : b[1];
        int64_t classes = aScores ? a[2] : b[2];
        INFO_MSG("Model type detected: split-output detection (%" PRId64 " queries, %" PRId64 " classes)",
                 queries, classes);
        return ModelType::YOLO_SPLIT_DETECTION;
      }
    }

    // 3D output [1, features, anchors] — YOLO family and NMS-embedded
    // Must be checked before depth estimation, since YOLO [1, 84, 8400] would match [1, H, W] depth pattern
    if (shape0.size() == 3) {
      int64_t dim1 = shape0[1];
      int64_t dim2 = shape0[2];

      // NMS-embedded YOLO: [1, max_det, 6+] where max_det is small (<=300) and dim1 < dim2 normally
      // but NMS output has dim2 small (6-7) and dim1 is max_det (~300)
      if (dim2 >= 5 && dim2 <= 57 && dim1 > 0 && dim1 <= 300) {
        INFO_MSG("Model type detected: YOLO NMS Detection ([1, %" PRId64 ", %" PRId64 "])", dim1, dim2);
        return ModelType::YOLO_NMS_DETECTION;
      }

      // Standard YOLO: [1, features, anchors] where features is small, anchors is large
      if (dim1 > 4 && dim2 > 100) {
        // Segmentation: 2 outputs, second is mask prototypes [1, 32, H, W]
        if (outCount >= 2 && outputShapes[1].size() == 4 && outputShapes[1][1] == 32) {
          int64_t nc = dim1 - 4 - 32; // features = 4 + nc + 32 mask coefficients
          INFO_MSG("Model type detected: YOLOv8 Segmentation (%" PRId64 " features, %" PRId64 " anchors, nc=%" PRId64 ")", dim1, dim2, nc);
          return ModelType::YOLOV8_SEGMENTATION;
        }

        // Pose: features == 56 (4 bbox + 1 conf + 17*3 keypoints), always person-only
        if (dim1 == 56) {
          INFO_MSG("Model type detected: YOLOv8 Pose (56 features, %" PRId64 " anchors)", dim2);
          return ModelType::YOLOV8_POSE;
        }

        // RTMO: 2 outputs (simcc_x and simcc_y) both 3D, or check output names
        if (outCount == 2 && outputShapes[1].size() == 3) {
          bool isSimCC = false;
          for (const auto & n : outputNames) {
            if (n.find("simcc") != std::string::npos) { isSimCC = true; break; }
          }
          if (isSimCC) {
            INFO_MSG("Model type detected: RTMO Pose (SimCC outputs)");
            return ModelType::POSE_RTMO;
          }
        }

        // OBB vs Detection: features = 4 + nc + 1 (OBB) or 4 + nc (detection)
        // OBB uses DOTA (15 classes) so features=20, or custom nc with features = nc + 5
        // Detection uses COCO (80 classes) so features=84, or custom nc with features = nc + 4
        // Heuristic: if (features - 4) matches known class counts, prefer detection
        int64_t ncDet = dim1 - 4;
        int64_t ncOBB = dim1 - 5;
        if (ncOBB == 15) {
          INFO_MSG("Model type detected: YOLOv8 OBB (%" PRId64 " features, %" PRId64 " anchors, nc=15 DOTA)", dim1, dim2);
          return ModelType::YOLOV8_OBB;
        }
        if (ncDet > 0) {
          INFO_MSG("Model type detected: YOLOv8 Detection (%" PRId64 " features, %" PRId64 " anchors, nc=%" PRId64 ")", dim1, dim2, ncDet);
          return ModelType::YOLOV8_DETECTION;
        }

        INFO_MSG("Model type detected: Generic Detection (%" PRId64 " features, %" PRId64 " anchors)", dim1, dim2);
        return ModelType::GENERIC_DETECTION;
      }
    }

    // Depth estimation: single spatial output [1, H, W] or [1, 1, H, W]
    // Checked after YOLO to avoid misclassifying e.g. [1, 84, 8400] as depth
    if (outCount == 1) {
      if (shape0.size() == 3 && shape0[0] == 1 && shape0[1] > 32 && shape0[2] > 32) {
        INFO_MSG("Model type detected: Depth Estimation (output [1, %" PRId64 ", %" PRId64 "])", shape0[1], shape0[2]);
        return ModelType::DEPTH_ESTIMATION;
      }
      if (shape0.size() == 4 && shape0[0] == 1 && shape0[1] == 1 && shape0[2] > 32 && shape0[3] > 32) {
        INFO_MSG("Model type detected: Depth Estimation (output [1, 1, %" PRId64 ", %" PRId64 "])", shape0[2], shape0[3]);
        return ModelType::DEPTH_ESTIMATION;
      }
    }

    // 2D output [1, num_classes] — Classification
    if (shape0.size() == 2 && shape0[0] == 1 && shape0[1] > 1) {
      INFO_MSG("Model type detected: Classification (%" PRId64 " classes)", shape0[1]);
      return ModelType::YOLOV8_CLASSIFICATION;
    }

    INFO_MSG("Could not auto-detect model type from output shapes");
    return ModelType::GENERIC_UNKNOWN;
  }

  ModelType DetectionModel::detectModelType() {
    if (!runner_.loaded()) return ModelType::UNKNOWN;

    try {
      const std::vector<TensorSpec> &outs = runner_.outputs();
      INFO_MSG("Model has %zu output(s)", outs.size());

      std::vector<std::vector<int64_t>> outputShapes;
      std::vector<std::string> outputNames;
      for (size_t i = 0; i < outs.size(); ++i) {
        outputShapes.push_back(outs[i].shape);
        INFO_MSG("Output %zu shape: %s", i, ORTHelpers::shapeToString(outs[i].shape).c_str());
        outputNames.push_back(outs[i].name.empty() ? ("output" + std::to_string(i)) : outs[i].name);
      }

      return classifyModelOutputs(outputShapes, outputNames, inputHeight_, inputWidth_);
    } catch (const std::exception & e) {
      ERROR_MSG("Error detecting model type %s: %s", modelPath_.c_str(), e.what());
      return ModelType::UNKNOWN;
    }
  }

  std::vector<Detection> DetectionModel::processFrame(const cv::Mat & frame, float confThreshold, float nmsThreshold,
                                                      InferenceMetrics *metrics) {
    std::vector<Detection> detections;

    // Validate input frame
    if (frame.empty()) {
      ERROR_MSG("Input frame is empty");
      return {};
    }

    if (frame.channels() != 3) {
      ERROR_MSG("Input frame must have 3 channels (BGR), got %d channels", frame.channels());
      return {};
    }

    if (frame.type() != CV_8UC3) {
      ERROR_MSG("Input frame must be CV_8UC3 format, got type %d", frame.type());
      return {};
    }

    VERYHIGH_MSG("Processing frame: %dx%d, type=%d, channels=%d", frame.cols, frame.rows, frame.type(), frame.channels());

    auto start = std::chrono::high_resolution_clock::now();
    InferenceMetrics localMetrics;
    if (!metrics) metrics = &localMetrics;

    try {
      // Preprocess image
      auto preprocessStart = std::chrono::high_resolution_clock::now();
      cv::Mat processedFrame = preprocessImage(frame);

      // Validate preprocessed frame
      if (processedFrame.empty()) {
        ERROR_MSG("Preprocessed frame is empty");
        return {};
      }

      if (processedFrame.channels() != 3) {
        ERROR_MSG("Preprocessed frame must have 3 channels, got %d", processedFrame.channels());
        return {};
      }

      if (enhanceImage_) {
        processedFrame = enhanceImage(processedFrame);
        // Validate enhanced frame
        if (processedFrame.empty()) {
          ERROR_MSG("Enhanced frame is empty");
          return {};
        }
      }
      auto preprocessEnd = std::chrono::high_resolution_clock::now();
      metrics->preprocessTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(preprocessEnd - preprocessStart).count();

      // Prepare input tensor
      TensorData tensorData = createInputTensor(processedFrame);
      ORTHelpers::OrtValueGuard inputGuard(tensorData.inputTensor);

      // Run inference
      auto inferenceStart = std::chrono::high_resolution_clock::now();
      const OrtApi *api = ORTHelpers::api();
      std::vector<OrtValue *> outputs;
      ORTHelpers::OrtOutputsGuard outputsGuard(outputs);
      runSession({tensorData.inputTensor}, outputs, "ONNX Runtime");
      auto inferenceEnd = std::chrono::high_resolution_clock::now();

      // Parse output using the derived class's parseOutput method
      float *outputData = nullptr;
      if (!ORTHelpers::checkStatus(api->GetTensorMutableData(outputs[0], (void **)&outputData), "GetTensorMutableData")) {
        throw std::runtime_error("Failed to get output tensor data");
      }
      OrtTensorTypeAndShapeInfo *ttsi = nullptr;
      if (!ORTHelpers::checkStatus(api->GetTensorTypeAndShape(outputs[0], &ttsi), "GetTensorTypeAndShape")) {
        throw std::runtime_error("Failed to get output tensor shape");
      }
      auto outputShape = ORTHelpers::getTensorShape(ttsi);
      if (ttsi) api->ReleaseTensorTypeAndShapeInfo(const_cast<OrtTensorTypeAndShapeInfo *>(ttsi));

      auto parseStart = std::chrono::high_resolution_clock::now();
      detections = parseOutput(outputData, outputShape, confThreshold, cv::Size(frame.cols, frame.rows));
      auto parseEnd = std::chrono::high_resolution_clock::now();

      // Apply NMS
      auto nmsStart = std::chrono::high_resolution_clock::now();
      if (softNmsSigma_ > 0.0f) {
        detections = applySoftNMS(detections, nmsThreshold, confThreshold, softNmsSigma_);
      } else {
        detections = applyNMS(detections, nmsThreshold);
      }
      auto nmsEnd = std::chrono::high_resolution_clock::now();

      auto endTime = std::chrono::high_resolution_clock::now();

      // Update metrics
      metrics->inferenceTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(inferenceEnd - inferenceStart).count();
      metrics->postprocessTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(parseEnd - parseStart).count();
      metrics->nmsTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(nmsEnd - nmsStart).count();
      metrics->totalTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - start).count();
      metrics->inputWidth = frame.cols;
      metrics->inputHeight = frame.rows;
      metrics->detectionCount = detections.size();

      VERYHIGH_MSG("Inference completed: %zu detections, total time: %" PRId64 "ms", detections.size(), (int64_t)metrics->totalTimeMs);

    } catch (const std::exception & e) {
      ERROR_MSG("Inference error: %s", e.what());
      return {};
    }

    return detections;
  }

  GenericResult DetectionModel::processFrameGeneric(const cv::Mat & frame, InferenceMetrics *metrics) {
    GenericResult result;
    result.timestamp = 0; // Will be set by caller
    result.modelName = modelPath_;
    result.modelType = "generic";

    if (!initialized_) {
      ERROR_MSG("Model not initialized");
      return result;
    }

    auto start = std::chrono::high_resolution_clock::now();
    InferenceMetrics localMetrics;
    if (!metrics) metrics = &localMetrics;

    try {
      // Preprocess image
      auto preprocessStart = std::chrono::high_resolution_clock::now();
      cv::Mat processedFrame = preprocessImage(frame);
      if (enhanceImage_) { processedFrame = enhanceImage(processedFrame); }
      auto preprocessEnd = std::chrono::high_resolution_clock::now();
      metrics->preprocessTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(preprocessEnd - preprocessStart).count();

      // Prepare input tensor
      TensorData tensorData = createInputTensor(processedFrame);
      ORTHelpers::OrtValueGuard inputGuard(tensorData.inputTensor);
      const OrtApi *api = ORTHelpers::api();

      // Run inference
      auto inferenceStart = std::chrono::high_resolution_clock::now();
      std::vector<OrtValue *> outputs;
      ORTHelpers::OrtOutputsGuard outputsGuard(outputs);
      runSession({tensorData.inputTensor}, outputs, "ONNX Runtime");
      auto inferenceEnd = std::chrono::high_resolution_clock::now();
      metrics->inferenceTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(inferenceEnd - inferenceStart).count();

      // Convert all outputs to JSON
      auto postprocessStart = std::chrono::high_resolution_clock::now();
      result.rawOutput.null();
      result.rawOutput["outputs"].null();

      for (size_t i = 0; i < outputs.size(); ++i) {
        float *outputData = nullptr;
        if (!ORTHelpers::checkStatus(api->GetTensorMutableData(outputs[i], (void **)&outputData), "GetTensorMutableData")) {
          throw std::runtime_error("Failed to get generic output tensor data for output " + std::to_string(i));
        }
        OrtTensorTypeAndShapeInfo *ttsi = nullptr;
        if (!ORTHelpers::checkStatus(api->GetTensorTypeAndShape(outputs[i], &ttsi), "GetTensorTypeAndShape")) {
          throw std::runtime_error("Failed to get generic output tensor shape for output " + std::to_string(i));
        }
        auto outputShape = ORTHelpers::getTensorShape(ttsi);
        if (ttsi) api->ReleaseTensorTypeAndShapeInfo(ttsi);
        std::string outputName = runner_.outputs()[i].name;

        result.rawOutput["outputs"][outputName] = tensorToJSON(outputData, outputShape, outputName);
      }

      auto postprocessEnd = std::chrono::high_resolution_clock::now();
      metrics->postprocessTimeMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(postprocessEnd - postprocessStart).count();

      auto end = std::chrono::high_resolution_clock::now();
      metrics->totalTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
      metrics->inputWidth = frame.cols;
      metrics->inputHeight = frame.rows;
      metrics->detectionCount = 0;

      result.metrics = *metrics;
      return result;

    } catch (const std::exception & e) {
      ERROR_MSG("Error during generic inference: %s", e.what());
      return result;
    }
  }

  cv::Mat DetectionModel::preprocessImage(const cv::Mat & image) {
    if (image.empty()) {
      ERROR_MSG("Input image is empty in preprocessImage");
      return cv::Mat();
    }

    if (image.channels() != 3) {
      ERROR_MSG("Input image must have 3 channels in preprocessImage, got %d", image.channels());
      return cv::Mat();
    }

    cv::Mat resized;
    try {
      bool doLetterbox = (preprocessConfig_.resizeMode == PreprocessConfig::LETTERBOX) && useLetterbox_;
      if (doLetterbox) {
        float scaleW = (float)inputWidth_ / image.cols;
        float scaleH = (float)inputHeight_ / image.rows;
        letterboxScale_ = std::min(scaleW, scaleH);
        int newW = (int)(image.cols * letterboxScale_);
        int newH = (int)(image.rows * letterboxScale_);
        letterboxPadX_ = (inputWidth_ - newW) / 2;
        letterboxPadY_ = (inputHeight_ - newH) / 2;

        cv::Mat scaled;
        cv::resize(image, scaled, cv::Size(newW, newH));
        const float *pad = preprocessConfig_.letterboxPadColor;
        resized = cv::Mat(inputHeight_, inputWidth_, image.type(), cv::Scalar(pad[0], pad[1], pad[2]));
        scaled.copyTo(resized(cv::Rect(letterboxPadX_, letterboxPadY_, newW, newH)));
      } else if (preprocessConfig_.resizeMode == PreprocessConfig::CENTER_CROP) {
        // HF-style: scale the SHORT edge to the target size, then crop the center
        letterboxScale_ = 1.0f;
        letterboxPadX_ = 0;
        letterboxPadY_ = 0;
        float scale = std::max((float)inputWidth_ / image.cols, (float)inputHeight_ / image.rows);
        int newW = (int)lround(image.cols * scale);
        int newH = (int)lround(image.rows * scale);
        if (newW < inputWidth_) { newW = inputWidth_; }
        if (newH < inputHeight_) { newH = inputHeight_; }
        cv::Mat scaled;
        // AREA for downscale approximates PIL's antialiased resize (the HF reference
        // pipeline); CUBIC for upscale matches its bicubic resample.
        cv::resize(image, scaled, cv::Size(newW, newH), 0, 0,
                   scale < 1.0f ? cv::INTER_AREA : cv::INTER_CUBIC);
        int offX = (newW - inputWidth_) / 2;
        int offY = (newH - inputHeight_) / 2;
        resized = scaled(cv::Rect(offX, offY, inputWidth_, inputHeight_)).clone();
      } else {
        letterboxScale_ = 1.0f;
        letterboxPadX_ = 0;
        letterboxPadY_ = 0;
        cv::resize(image, resized, cv::Size(inputWidth_, inputHeight_));
      }

      if (resized.empty()) {
        ERROR_MSG("Resize operation resulted in empty image");
        return cv::Mat();
      }

      if (resized.channels() != 3) {
        ERROR_MSG("Resized image has wrong number of channels: %d", resized.channels());
        return cv::Mat();
      }

      VERYHIGH_MSG("Preprocessed image: %dx%d -> %dx%d (letterbox=%s, scale=%.3f, pad=%d,%d)",
                   image.cols, image.rows, resized.cols, resized.rows,
                   useLetterbox_ ? "yes" : "no", letterboxScale_, letterboxPadX_, letterboxPadY_);

    } catch (const cv::Exception & e) {
      ERROR_MSG("OpenCV error during resize: %s", e.what());
      return cv::Mat();
    }

    return resized;
  }

  void DetectionModel::remapLetterboxCoords(Detection & det) {
    if (!useLetterbox_ || (letterboxPadX_ == 0 && letterboxPadY_ == 0)) return;
    float activeW = (float)(inputWidth_ - 2 * letterboxPadX_);
    float activeH = (float)(inputHeight_ - 2 * letterboxPadY_);
    if (activeW <= 0.0f || activeH <= 0.0f) return;
    det.x = (det.x * inputWidth_ - letterboxPadX_) / activeW;
    det.y = (det.y * inputHeight_ - letterboxPadY_) / activeH;
    det.w = det.w * inputWidth_ / activeW;
    det.h = det.h * inputHeight_ / activeH;
  }

  cv::Mat DetectionModel::enhanceImage(const cv::Mat & image) {
    cv::Mat enhanced;

    // Convert to LAB color space for better histogram equalization
    cv::Mat lab;
    cv::cvtColor(image, lab, cv::COLOR_BGR2Lab);

    // Split channels
    std::vector<cv::Mat> labChannels;
    cv::split(lab, labChannels);

    // Apply CLAHE to L channel
    cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE(2.0, cv::Size(8, 8));
    clahe->apply(labChannels[0], labChannels[0]);

    // Merge channels back
    cv::merge(labChannels, lab);

    // Convert back to BGR
    cv::cvtColor(lab, enhanced, cv::COLOR_Lab2BGR);

    return enhanced;
  }

  std::vector<Detection> DetectionModel::applyNMS(const std::vector<Detection> & detections, float nmsThreshold) {
    if (detections.empty()) return detections;

    INSANE_MSG("NMS input: %zu detections, threshold=%.3f", detections.size(), nmsThreshold);

    std::vector<size_t> indices(detections.size());
    std::iota(indices.begin(), indices.end(), 0);

    // Sort by confidence (highest first)
    std::sort(indices.begin(), indices.end(),
              [&detections](size_t a, size_t b) { return detections[a].confidence > detections[b].confidence; });

    std::vector<bool> suppressed(detections.size(), false);
    std::vector<Detection> result;

    for (size_t i = 0; i < indices.size(); ++i) {
      if (suppressed[indices[i]]) continue;

      result.push_back(detections[indices[i]]);

      // Suppress overlapping detections
      for (size_t j = i + 1; j < indices.size(); ++j) {
        if (suppressed[indices[j]]) continue;

        float iou = calculateIoU(detections[indices[i]], detections[indices[j]]);
        if (iou > nmsThreshold) { suppressed[indices[j]] = true; }
      }
    }

    INSANE_MSG("NMS output: %zu detections (removed %zu)", result.size(), detections.size() - result.size());
    return result;
  }

  float DetectionModel::calculateIoU(const Detection & a, const Detection & b) {
    float x1 = std::max(a.x, b.x);
    float y1 = std::max(a.y, b.y);
    float x2 = std::min(a.x + a.w, b.x + b.w);
    float y2 = std::min(a.y + a.h, b.y + b.h);

    if (x2 <= x1 || y2 <= y1) return 0.0f;

    float intersection = (x2 - x1) * (y2 - y1);
    float unionArea = a.w * a.h + b.w * b.h - intersection;

    return intersection / unionArea;
  }

  float DetectionModel::calculateDIoU(const Detection & a, const Detection & b) {
    // Calculate standard IoU first
    float iou = calculateIoU(a, b);

    // Calculate center points
    float cx_a = a.x + a.w / 2.0f;
    float cy_a = a.y + a.h / 2.0f;
    float cx_b = b.x + b.w / 2.0f;
    float cy_b = b.y + b.h / 2.0f;

    // Distance between centers squared
    float center_distance_sq = (cx_a - cx_b) * (cx_a - cx_b) + (cy_a - cy_b) * (cy_a - cy_b);

    // Diagonal of smallest enclosing box
    float x1 = std::min(a.x, b.x);
    float y1 = std::min(a.y, b.y);
    float x2 = std::max(a.x + a.w, b.x + b.w);
    float y2 = std::max(a.y + a.h, b.y + b.h);
    float diagonal_sq = (x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1);

    // Avoid division by zero
    if (diagonal_sq < 1e-7f) return iou;

    // DIoU = IoU - (distance²/diagonal²)
    float diou = iou - (center_distance_sq / diagonal_sq);

    return diou;
  }

  std::vector<Detection> DetectionModel::applySoftNMS(const std::vector<Detection> & detections, float nmsThreshold,
                                                      float confThreshold, float sigma) {
    if (detections.empty()) return detections;

    INSANE_MSG("Soft-NMS input: %zu detections, nms_threshold=%.3f, conf_threshold=%.3f, sigma=%.3f", detections.size(),
               nmsThreshold, confThreshold, sigma);

    // Copy detections so we can modify confidence scores
    std::vector<Detection> dets = detections;

    // Sort by confidence (highest first)
    std::sort(dets.begin(), dets.end(),
              [](const Detection & a, const Detection & b) { return a.confidence > b.confidence; });

    // Apply soft-NMS
    for (size_t i = 0; i < dets.size(); ++i) {
      if (dets[i].confidence < confThreshold) continue;

      for (size_t j = i + 1; j < dets.size(); ++j) {
        if (dets[j].confidence < confThreshold) continue;

        // Only apply NMS to same class detections
        if (dets[i].class_id != dets[j].class_id) continue;

        float diou = calculateDIoU(dets[i], dets[j]);

        if (diou > nmsThreshold) {
          // Soft-NMS: decay confidence instead of hard suppression
          // Using Gaussian decay: conf *= exp(-diou²/sigma)
          dets[j].confidence *= std::exp(-diou * diou / sigma);
          if (dets[j].confidence < 0.0f) dets[j].confidence = 0.0f;
          if (dets[j].confidence > 1.0f) dets[j].confidence = 1.0f;
        }
      }
    }

    // Filter out detections below confidence threshold and resort
    std::vector<Detection> result;
    for (const auto & det : dets) {
      if (det.confidence >= confThreshold) { result.push_back(det); }
    }

    // Sort final results by confidence
    std::sort(result.begin(), result.end(),
              [](const Detection & a, const Detection & b) { return a.confidence > b.confidence; });

    INSANE_MSG("Soft-NMS output: %zu detections (filtered %zu below threshold)", result.size(),
               detections.size() - result.size());
    return result;
  }

  JSON::Value DetectionModel::tensorToJSON(float *data, const std::vector<int64_t> & shape, const std::string & name) {
    JSON::Value tensor;
    tensor.null();
    tensor["name"] = name;
    tensor["shape"].null();

    // Add shape information
    for (auto dim : shape) { tensor["shape"].append(static_cast<int>(dim)); }

    // Calculate total elements
    int64_t totalElements = 1;
    for (auto dim : shape) { totalElements *= dim; }

    // Limit output size to prevent overwhelming metadata (max 1000 elements)
    int64_t maxElements = std::min(totalElements, static_cast<int64_t>(1000));

    tensor["data"].null();
    for (int64_t i = 0; i < maxElements; ++i) { tensor["data"].append(data[i]); }

    if (totalElements > maxElements) {
      tensor["truncated"] = true;
      tensor["total_elements"] = static_cast<int>(totalElements);
    }

    return tensor;
  }

  // YOLOv8Model implementation (handles detection)
  YOLOv8Model::YOLOv8Model(const std::string & modelPath, int inputSize) : DetectionModel(modelPath, inputSize) {}

  std::vector<Detection> YOLOv8Model::parseOutput(float *outputData, const std::vector<int64_t> & outputShape,
                                                  float confThreshold, const cv::Size & originalSize) {
    std::vector<Detection> detections;

    if (outputShape.size() != 3) {
      ERROR_MSG("Invalid output shape for YOLOv8 model");
      return detections;
    }

    int64_t numDetections = outputShape[2];
    int64_t numFeatures = outputShape[1];

    VERYHIGH_MSG("YOLOv8 parsing: shape=[%" PRId64 ", %" PRId64 ", %" PRId64 "], confThreshold=%.3f",
                 (int64_t)outputShape[0], (int64_t)outputShape[1], (int64_t)outputShape[2], confThreshold);

    // Sample a few detections to see confidence values
    float maxConfSeen = 0.0f;
    int detectionsAboveThreshold = 0;

    // Determine if this is a segmentation model (116 features) or detection model (84 features)
    bool isSegmentationModel = (numFeatures == 116);
    int64_t classChannels = isSegmentationModel ? 84 : numFeatures; // Only use first 84 channels for classes in segmentation

    // YOLOv8 format: [cx, cy, w, h, class0_conf, class1_conf, ...]
    // For segmentation: [cx, cy, w, h, class0_conf, ..., class79_conf, mask_coeff0, ..., mask_coeff31]
    for (int64_t i = 0; i < numDetections; ++i) {
      float cx = outputData[i];
      float cy = outputData[numDetections + i];
      float w = outputData[2 * numDetections + i];
      float h = outputData[3 * numDetections + i];

      // Find best class (only check class channels, not mask coefficients)
      float maxConf = 0.0f;
      int bestClass = -1;
      for (int64_t c = 4; c < classChannels; ++c) {
        float conf = outputData[c * numDetections + i];
        if (conf > maxConf) {
          maxConf = conf;
          bestClass = c - 4;
        }
      }

      // Track max confidence seen across all detections
      if (maxConf > maxConfSeen) { maxConfSeen = maxConf; }

      if (maxConf >= confThreshold) {
        detectionsAboveThreshold++;
        Detection det;
        // Convert from center format to top-left format and normalize
        det.x = (cx - w / 2.0f) / inputWidth_;
        det.y = (cy - h / 2.0f) / inputHeight_;
        det.w = w / inputWidth_;
        det.h = h / inputHeight_;
        det.confidence = maxConf;
        det.class_id = bestClass;
        det.class_name = className(bestClass, Utils::COCO_CLASSES);
        det.track_id = i;

        remapLetterboxCoords(det);
        clampDetection(det);
        detections.push_back(det);
      }
    }

    VERYHIGH_MSG("YOLOv8 parsing results: %d detections above threshold %.3f, max confidence seen: "
                 "%.6f (segmentation: %s)",
                 detectionsAboveThreshold, confThreshold, maxConfSeen, isSegmentationModel ? "yes" : "no");

    return detections;
  }

  // YOLOv8PoseModel implementation
  YOLOv8PoseModel::YOLOv8PoseModel(const std::string & modelPath, int inputSize)
    : DetectionModel(modelPath, inputSize) {}

  std::vector<Detection> YOLOv8PoseModel::parseOutput(float *outputData, const std::vector<int64_t> & outputShape,
                                                      float confThreshold, const cv::Size & originalSize) {
    // Convert pose detections to regular detections for base class compatibility
    std::vector<PoseDetection> poseDetections = parsePoseOutput(outputData, outputShape, confThreshold, originalSize);
    std::vector<Detection> detections;

    for (const auto & pose : poseDetections) { detections.push_back(static_cast<Detection>(pose)); }

    return detections;
  }

  std::vector<PoseDetection> YOLOv8PoseModel::parsePoseOutput(float *outputData, const std::vector<int64_t> & outputShape,
                                                              float confThreshold, const cv::Size & originalSize) {
    std::vector<PoseDetection> detections;

    if (outputShape.size() != 3 || outputShape[1] != 56) {
      ERROR_MSG("Invalid output shape for YOLOv8 pose model");
      return detections;
    }

    int64_t numDetections = outputShape[2];

    // YOLOv8 Pose format: [cx, cy, w, h, conf, kp0_x, kp0_y, kp0_conf, ..., kp16_x, kp16_y, kp16_conf]
    for (int64_t i = 0; i < numDetections; ++i) {
      float cx = outputData[i];
      float cy = outputData[numDetections + i];
      float w = outputData[2 * numDetections + i];
      float h = outputData[3 * numDetections + i];
      float conf = outputData[4 * numDetections + i];

      if (conf >= confThreshold) {
        PoseDetection det;
        // Convert from center format to top-left format and normalize
        det.x = (cx - w / 2.0f) / inputWidth_;
        det.y = (cy - h / 2.0f) / inputHeight_;
        det.w = w / inputWidth_;
        det.h = h / inputHeight_;
        det.confidence = conf;
        det.pose_confidence = conf;
        det.class_id = 0; // Person class
        det.class_name = "person";

        det.track_id = i;

        // Parse keypoints (17 keypoints, 3 values each: x, y, confidence)
        det.keypoints.resize(17);
        for (int kp = 0; kp < 17; ++kp) {
          int baseIdx = 5 + kp * 3;
          det.keypoints[kp].x = outputData[(baseIdx)*numDetections + i] / inputWidth_;
          det.keypoints[kp].y = outputData[(baseIdx + 1) * numDetections + i] / inputHeight_;
          det.keypoints[kp].confidence = outputData[(baseIdx + 2) * numDetections + i];
          det.keypoints[kp].visible = det.keypoints[kp].confidence > 0.5f;
        }

        remapLetterboxCoords(static_cast<Detection &>(det));
        // Remap keypoints from letterbox space to original image space
        if (useLetterbox_ && (letterboxPadX_ != 0 || letterboxPadY_ != 0)) {
          float activeW = (float)(inputWidth_ - 2 * letterboxPadX_);
          float activeH = (float)(inputHeight_ - 2 * letterboxPadY_);
          if (activeW > 0.0f && activeH > 0.0f) {
            for (auto & kp : det.keypoints) {
              kp.x = (kp.x * inputWidth_ - letterboxPadX_) / activeW;
              kp.y = (kp.y * inputHeight_ - letterboxPadY_) / activeH;
            }
          }
        }
        clampDetection(static_cast<Detection &>(det));
        detections.push_back(det);
      }
    }

    return detections;
  }

  std::vector<PoseDetection> YOLOv8PoseModel::processPoseFrame(const cv::Mat & frame, float confThreshold,
                                                               float nmsThreshold, InferenceMetrics *metrics) {
    std::vector<PoseDetection> detections;

    try {
      auto startTime = std::chrono::high_resolution_clock::now();

      // Preprocess
      cv::Mat processedFrame = preprocessImage(frame);
      if (enhanceImage_) { processedFrame = enhanceImage(processedFrame); }

      auto preprocessEnd = std::chrono::high_resolution_clock::now();

      // Create input tensor using the helper function
      TensorData tensorData = createInputTensor(processedFrame);
      ORTHelpers::OrtValueGuard inputGuard(tensorData.inputTensor);

      // Run inference (C API)
      auto inferenceStart = std::chrono::high_resolution_clock::now();
      const OrtApi *api = ORTHelpers::api();
      std::vector<OrtValue *> outputs;
      ORTHelpers::OrtOutputsGuard outputsGuard(outputs);
      runSession({tensorData.inputTensor}, outputs, "ONNX Runtime");

      auto inferenceEnd = std::chrono::high_resolution_clock::now();

      // Parse pose output directly to get keypoints
      float *outputData = nullptr;
      if (!ORTHelpers::checkStatus(api->GetTensorMutableData(outputs[0], (void **)&outputData), "GetTensorMutableData")) {
        throw std::runtime_error("Failed to get pose output tensor data");
      }
      OrtTensorTypeAndShapeInfo *ttsi = nullptr;
      if (!ORTHelpers::checkStatus(api->GetTensorTypeAndShape(outputs[0], &ttsi), "GetTensorTypeAndShape")) {
        throw std::runtime_error("Failed to get pose output tensor shape");
      }
      auto outputShape = ORTHelpers::getTensorShape(ttsi);
      if (ttsi) api->ReleaseTensorTypeAndShapeInfo(ttsi);

      detections = parsePoseOutput(outputData, outputShape, confThreshold, cv::Size(frame.cols, frame.rows));

      auto postprocessEnd = std::chrono::high_resolution_clock::now();

      // Apply NMS
      if (nmsThreshold > 0.0f && !detections.empty()) {
        // Convert to base detections for NMS, carrying vector index in track_id
        std::vector<Detection> baseDetections;
        for (size_t i = 0; i < detections.size(); ++i) {
          Detection d = static_cast<Detection>(detections[i]);
          d.track_id = i;
          baseDetections.push_back(d);
        }

        baseDetections = applyNMS(baseDetections, nmsThreshold);

        // Convert back to pose detections using carried index
        std::vector<PoseDetection> nmsDetections;
        for (size_t i = 0; i < baseDetections.size(); ++i) {
          size_t origIdx = (size_t)baseDetections[i].track_id;
          if (origIdx < detections.size()) {
            nmsDetections.push_back(detections[origIdx]);
          }
        }
        detections = nmsDetections;
      }

      // Update metrics
      if (metrics) {
        metrics->preprocessTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(preprocessEnd - startTime).count();
        metrics->inferenceTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(inferenceEnd - inferenceStart).count();
        metrics->postprocessTimeMs =
          std::chrono::duration_cast<std::chrono::milliseconds>(postprocessEnd - inferenceEnd).count();
        metrics->totalTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(postprocessEnd - startTime).count();
        metrics->detectionCount = detections.size();
      }

    } catch (const std::exception & e) { ERROR_MSG("Error in pose frame processing: %s", e.what()); }

    return detections;
  }

  // YOLOv8SegmentationModel implementation
  YOLOv8SegmentationModel::YOLOv8SegmentationModel(const std::string & modelPath, int inputSize)
    : DetectionModel(modelPath, inputSize) {}

  std::vector<Detection> YOLOv8SegmentationModel::parseOutput(float *outputData, const std::vector<int64_t> & outputShape,
                                                              float confThreshold, const cv::Size & originalSize) {
    // Use the same parsing logic as YOLOv8Model, which already handles segmentation models correctly
    std::vector<Detection> detections;

    if (outputShape.size() != 3) {
      ERROR_MSG("Invalid output shape for YOLOv8 segmentation model");
      return detections;
    }

    int64_t numDetections = outputShape[2];
    int64_t numFeatures = outputShape[1];

    VERYHIGH_MSG("YOLOv8 segmentation parsing: shape=[%" PRId64 ", %" PRId64 ", %" PRId64 "], confThreshold=%.3f",
                 outputShape[0], outputShape[1], outputShape[2], confThreshold);

    // Sample a few detections to see confidence values
    float maxConfSeen = 0.0f;
    int detectionsAboveThreshold = 0;

    // Determine if this is a segmentation model (116 features) or detection model (84 features)
    bool isSegmentationModel = (numFeatures == 116);
    int64_t classChannels = isSegmentationModel ? 84 : numFeatures; // Only use first 84 channels for classes in segmentation

    // YOLOv8 format: [cx, cy, w, h, class0_conf, class1_conf, ...]
    // For segmentation: [cx, cy, w, h, class0_conf, ..., class79_conf, mask_coeff0, ..., mask_coeff31]
    for (int64_t i = 0; i < numDetections; ++i) {
      float cx = outputData[i];
      float cy = outputData[numDetections + i];
      float w = outputData[2 * numDetections + i];
      float h = outputData[3 * numDetections + i];

      // Find best class (only check class channels, not mask coefficients)
      float maxConf = 0.0f;
      int bestClass = -1;
      for (int64_t c = 4; c < classChannels; ++c) {
        float conf = outputData[c * numDetections + i];
        if (conf > maxConf) {
          maxConf = conf;
          bestClass = c - 4;
        }
      }

      // Track max confidence seen across all detections
      if (maxConf > maxConfSeen) { maxConfSeen = maxConf; }

      if (maxConf >= confThreshold) {
        detectionsAboveThreshold++;
        Detection det;
        // Convert from center format to top-left format and normalize
        det.x = (cx - w / 2.0f) / inputWidth_;
        det.y = (cy - h / 2.0f) / inputHeight_;
        det.w = w / inputWidth_;
        det.h = h / inputHeight_;
        det.confidence = maxConf;
        det.class_id = bestClass;
        det.class_name = className(bestClass, Utils::COCO_CLASSES);
        det.track_id = i;

        remapLetterboxCoords(det);
        clampDetection(det);
        detections.push_back(det);
      }
    }

    VERYHIGH_MSG("YOLOv8 segmentation parsing results: %d detections above threshold %.3f, max "
                 "confidence seen: %.6f",
                 detectionsAboveThreshold, confThreshold, maxConfSeen);

    return detections;
  }

  std::vector<SegmentationDetection> YOLOv8SegmentationModel::processSegmentationFrame(const cv::Mat & frame,
                                                                                       float confThreshold, float nmsThreshold,
                                                                                       InferenceMetrics *metrics) {
    std::vector<SegmentationDetection> detections;

    try {
      auto startTime = std::chrono::high_resolution_clock::now();

      // Preprocess
      cv::Mat processedFrame = preprocessImage(frame);
      if (enhanceImage_) { processedFrame = enhanceImage(processedFrame); }

      auto preprocessEnd = std::chrono::high_resolution_clock::now();

      // Create input tensor
      TensorData tensorData = createInputTensor(processedFrame);
      ORTHelpers::OrtValueGuard inputGuard(tensorData.inputTensor);

      // Run inference
      auto inferenceStart = std::chrono::high_resolution_clock::now();
      const OrtApi *api = ORTHelpers::api();
      std::vector<OrtValue *> outputs;
      ORTHelpers::OrtOutputsGuard outputsGuard(outputs);
      runSession({tensorData.inputTensor}, outputs, "ONNX Runtime");

      auto inferenceEnd = std::chrono::high_resolution_clock::now();

      // Verify we have both outputs for segmentation
      if (outputs.size() < 2) {
        ERROR_MSG("Segmentation model requires 2 outputs, got %zu", outputs.size());
        return detections;
      }

      // Get detection output [1, 116, 8400] - detections + mask coefficients
      float *detectionData = nullptr;
      if (!ORTHelpers::checkStatus(api->GetTensorMutableData(outputs[0], (void **)&detectionData), "GetTensorMutableData")) {
        throw std::runtime_error("Failed to get segmentation detection tensor data");
      }
      OrtTensorTypeAndShapeInfo *ttsi0 = nullptr;
      if (!ORTHelpers::checkStatus(api->GetTensorTypeAndShape(outputs[0], &ttsi0), "GetTensorTypeAndShape")) {
        throw std::runtime_error("Failed to get segmentation detection tensor shape");
      }
      auto detectionShape = ORTHelpers::getTensorShape(ttsi0);
      if (ttsi0) api->ReleaseTensorTypeAndShapeInfo(ttsi0);

      // Get mask prototype output [1, 32, 160, 160] - mask prototypes
      float *prototypeData = nullptr;
      if (!ORTHelpers::checkStatus(api->GetTensorMutableData(outputs[1], (void **)&prototypeData), "GetTensorMutableData")) {
        throw std::runtime_error("Failed to get segmentation prototype tensor data");
      }
      OrtTensorTypeAndShapeInfo *ttsi1 = nullptr;
      if (!ORTHelpers::checkStatus(api->GetTensorTypeAndShape(outputs[1], &ttsi1), "GetTensorTypeAndShape")) {
        throw std::runtime_error("Failed to get segmentation prototype tensor shape");
      }
      auto prototypeShape = ORTHelpers::getTensorShape(ttsi1);
      if (ttsi1) api->ReleaseTensorTypeAndShapeInfo(ttsi1);

      // Validate output shapes
      if (detectionShape.size() != 3 || detectionShape[1] != 116) {
        ERROR_MSG("Invalid detection output shape for segmentation model: [%" PRId64 ", %" PRId64 ", %" PRId64 "]",
                  detectionShape[0], detectionShape[1], detectionShape[2]);
        return detections;
      }

      if (prototypeShape.size() != 4 || prototypeShape[1] != 32) {
        ERROR_MSG("Invalid prototype output shape for segmentation model: [%" PRId64 ", %" PRId64 ", %" PRId64
                  ", %" PRId64 "]",
                  prototypeShape[0], prototypeShape[1], prototypeShape[2], prototypeShape[3]);
        return detections;
      }

      // Step 1: Use the working YOLOv8Model parseOutput logic to get detections
      // This will automatically handle the 116-channel segmentation format correctly
      std::vector<Detection> regularDetections =
        parseOutput(detectionData, detectionShape, confThreshold, cv::Size(frame.cols, frame.rows));

      // Step 2: For each detection, generate proper segmentation masks
      for (const auto & detection : regularDetections) {
        // Use the raw tensor index carried through track_id during parsing
        int originalIndex = (int)detection.track_id;
        int64_t numDetections = detectionShape[2];

        if (originalIndex < 0 || originalIndex >= (int)numDetections) {
          WARN_MSG("Invalid original index %d for detection, skipping mask generation", originalIndex);
          continue;
        }

        // Extract mask coefficients (channels 84-115)
        std::vector<float> maskCoefficients(32);
        for (int c = 0; c < 32; ++c) {
          int channelIndex = 84 + c; // Channels 84-115
          maskCoefficients[c] = detectionData[channelIndex * numDetections + originalIndex];
        }

        // Generate mask from coefficients and prototypes
        cv::Rect bbox(detection.x * frame.cols, detection.y * frame.rows, detection.w * frame.cols, detection.h * frame.rows);
        cv::Mat mask = generateMask(maskCoefficients, prototypeData, prototypeShape, bbox, cv::Size(frame.cols, frame.rows));

        // Extract contour from mask
        std::vector<cv::Point> contour = extractContour(mask);

        // Create SegmentationDetection with real mask
        SegmentationDetection segDet;
        segDet.x = detection.x;
        segDet.y = detection.y;
        segDet.w = detection.w;
        segDet.h = detection.h;
        segDet.confidence = detection.confidence;
        segDet.class_id = detection.class_id;
        segDet.class_name = detection.class_name;
        segDet.track_id = detection.track_id;
        segDet.mask_confidence = detection.confidence;
        segDet.mask = mask;
        segDet.contour = contour;

        detections.push_back(segDet);
      }

      // Apply NMS to segmentation detections
      if (!detections.empty() && nmsThreshold > 0.0f) { detections = applySegmentationNMS(detections, nmsThreshold); }

      auto endTime = std::chrono::high_resolution_clock::now();

      // Update metrics
      if (metrics) {
        metrics->preprocessTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(preprocessEnd - startTime).count();
        metrics->inferenceTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(inferenceEnd - inferenceStart).count();
        metrics->postprocessTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - inferenceEnd).count();
        metrics->totalTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
        metrics->inputWidth = frame.cols;
        metrics->inputHeight = frame.rows;
        metrics->detectionCount = detections.size();
      }
    } catch (const std::exception & e) { ERROR_MSG("Segmentation inference error: %s", e.what()); }

    return detections;
  }

  // New implementation for proper mask generation
  std::vector<SegmentationDetection>
    YOLOv8SegmentationModel::parseSegmentationOutputWithMasks(float *detectionData, const std::vector<int64_t> & detectionShape,
                                                              float *prototypeData, const std::vector<int64_t> & prototypeShape,
                                                              float confThreshold, const cv::Size & originalSize) {
    std::vector<SegmentationDetection> segmentationDetections;

    if (detectionShape.size() != 3 || detectionShape[1] != 116) {
      ERROR_MSG("Invalid detection output shape for segmentation model: expected [1, 116, N], got "
                "[%" PRId64 ", %" PRId64 ", %" PRId64 "]",
                detectionShape[0], detectionShape[1], detectionShape[2]);
      return segmentationDetections;
    }

    if (prototypeShape.size() != 4 || prototypeShape[1] != 32) {
      ERROR_MSG("Invalid prototype output shape for segmentation model: expected [1, 32, H, W], "
                "got [%" PRId64 ", %" PRId64 ", %" PRId64 ", %" PRId64 "]",
                prototypeShape[0], prototypeShape[1], prototypeShape[2], prototypeShape[3]);
      return segmentationDetections;
    }

    int64_t numDetections = detectionShape[2];

    // Step 1: Parse detections using the exact same logic as YOLOv8Model::parseOutput
    // but applied to only the first 84 channels (detection part)
    std::vector<Detection> regularDetections;

    // Sample a few detections to see confidence values
    float maxConfSeen = 0.0f;
    int detectionsAboveThreshold = 0;

    // YOLOv8 format: [cx, cy, w, h, class0_conf, class1_conf, ...]
    for (int64_t i = 0; i < numDetections; ++i) {
      float cx = detectionData[i];
      float cy = detectionData[numDetections + i];
      float w = detectionData[2 * numDetections + i];
      float h = detectionData[3 * numDetections + i];

      // Find best class (classes 4-83, which are the first 80 classes)
      float maxConf = 0.0f;
      int bestClass = -1;
      for (int64_t c = 4; c < 84; ++c) { // Only check first 84 channels (detection part)
        float conf = detectionData[c * numDetections + i];
        if (conf > maxConf) {
          maxConf = conf;
          bestClass = c - 4;
        }
      }

      // Track max confidence seen across all detections
      if (maxConf > maxConfSeen) { maxConfSeen = maxConf; }

      // Debug: Log first few detections to see what we're getting
      if (i < 5) {
        VERYHIGH_MSG("Detection %" PRId64 ": cx=%.3f, cy=%.3f, w=%.3f, h=%.3f, maxConf=%.3f, bestClass=%d", i, cx, cy,
                     w, h, maxConf, bestClass);
      }

      if (maxConf >= confThreshold) {
        detectionsAboveThreshold++;
        Detection det;
        // Convert from center format to top-left format and normalize
        det.x = (cx - w / 2.0f) / inputWidth_;
        det.y = (cy - h / 2.0f) / inputHeight_;
        det.w = w / inputWidth_;
        det.h = h / inputHeight_;
        det.confidence = maxConf;
        det.class_id = bestClass;
        det.class_name = className(bestClass, Utils::COCO_CLASSES);
        det.track_id = i;

        remapLetterboxCoords(det);
        regularDetections.push_back(det);
      }
    }

    INFO_MSG("Found %d regular detections above threshold %.3f (max confidence seen: %.3f)", detectionsAboveThreshold,
             confThreshold, maxConfSeen);

    // Step 2: For each valid detection, generate the corresponding mask
    for (const auto & detection : regularDetections) {
      // Use the raw tensor index carried through track_id during parsing
      int originalIndex = (int)detection.track_id;

      if (originalIndex < 0 || originalIndex >= (int)numDetections) {
        WARN_MSG("Invalid original index %d for detection", originalIndex);
        continue;
      }

      // Step 3: Extract mask coefficients (channels 84-115)
      std::vector<float> maskCoefficients(32);
      for (int c = 0; c < 32; ++c) {
        int channelIndex = 84 + c; // Channels 84-115
        maskCoefficients[c] = detectionData[channelIndex * numDetections + originalIndex];
      }

      // Step 4: Generate mask from coefficients and prototypes
      cv::Rect bbox(detection.x * originalSize.width, detection.y * originalSize.height,
                    detection.w * originalSize.width, detection.h * originalSize.height);
      cv::Mat mask = generateMask(maskCoefficients, prototypeData, prototypeShape, bbox, originalSize);

      // Step 5: Extract contour from mask
      std::vector<cv::Point> contour = extractContour(mask);

      // Step 6: Create SegmentationDetection
      SegmentationDetection segDet;
      segDet.x = detection.x;
      segDet.y = detection.y;
      segDet.w = detection.w;
      segDet.h = detection.h;
      segDet.confidence = detection.confidence;
      segDet.class_id = detection.class_id;
      segDet.class_name = detection.class_name;
      segDet.track_id = detection.track_id;
      segDet.mask = mask;
      segDet.mask_confidence = detection.confidence;
      segDet.contour = contour;

      segmentationDetections.push_back(segDet);
    }

    INFO_MSG("Generated %zu segmentation detections", segmentationDetections.size());
    return segmentationDetections;
  }

  cv::Mat YOLOv8SegmentationModel::generateMask(const std::vector<float> & maskCoeffs, float *prototypeData,
                                                const std::vector<int64_t> & prototypeShape, const cv::Rect & bbox,
                                                const cv::Size & originalSize) {
    if (maskCoeffs.size() != 32) {
      ERROR_MSG("Invalid mask coefficients size: %zu", maskCoeffs.size());
      return cv::Mat();
    }

    int64_t prototypeHeight = prototypeShape[2]; // 160
    int64_t prototypeWidth = prototypeShape[3]; // 160

    // Compose mask using matrix multiply: coeff(1x32) * proto(32x(H*W)) -> (1xH*W)
    cv::Mat proto(32, (int)(prototypeHeight * prototypeWidth), CV_32F, prototypeData);
    cv::Mat coeff(1, 32, CV_32F, const_cast<float *>(maskCoeffs.data()));
    cv::Mat flat = coeff * proto;
    cv::Mat mask = flat.reshape(1, (int)prototypeHeight).clone();

    // Apply sigmoid activation to get probabilities
    cv::Mat sigmoidMask;
    cv::exp(-mask, sigmoidMask);
    sigmoidMask = 1.0 / (1.0 + sigmoidMask);

    // Threshold to binary mask
    cv::Mat binaryMask;
    cv::threshold(sigmoidMask, binaryMask, 0.5, 255, cv::THRESH_BINARY);
    binaryMask.convertTo(binaryMask, CV_8UC1);

    // Resize mask to original image size
    cv::Mat resizedMask;
    cv::resize(binaryMask, resizedMask, originalSize, 0, 0, cv::INTER_LINEAR);

    // Crop mask to bounding box region (optional optimization)
    if (bbox.x >= 0 && bbox.y >= 0 && bbox.x + bbox.width <= originalSize.width && bbox.y + bbox.height <= originalSize.height) {
      cv::Mat croppedMask = cv::Mat::zeros(originalSize, CV_8UC1);
      resizedMask(bbox).copyTo(croppedMask(bbox));
      return croppedMask;
    }

    return resizedMask;
  }

  std::vector<cv::Point> YOLOv8SegmentationModel::extractContour(const cv::Mat & mask) {
    std::vector<cv::Point> contour;

    if (mask.empty()) { return contour; }

    // Find contours in the mask
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    if (!contours.empty()) {
      // Find the largest contour (main object)
      size_t largestIdx = 0;
      double largestArea = 0;
      for (size_t i = 0; i < contours.size(); ++i) {
        double area = cv::contourArea(contours[i]);
        if (area > largestArea) {
          largestArea = area;
          largestIdx = i;
        }
      }

      // Simplify contour to reduce point count
      std::vector<cv::Point> approxContour;
      double epsilon = 0.002 * cv::arcLength(contours[largestIdx], true); // 0.2% of perimeter
      cv::approxPolyDP(contours[largestIdx], approxContour, epsilon, true);

      contour = approxContour;
    }

    return contour;
  }

  std::vector<SegmentationDetection>
    YOLOv8SegmentationModel::applySegmentationNMS(const std::vector<SegmentationDetection> & detections, float nmsThreshold) {
    if (detections.empty()) return detections;

    // Convert to base Detection objects for NMS, carrying vector index in track_id
    std::vector<Detection> baseDetections;
    for (size_t i = 0; i < detections.size(); ++i) {
      Detection d = static_cast<Detection>(detections[i]);
      d.track_id = i;
      baseDetections.push_back(d);
    }

    // Apply standard NMS
    std::vector<Detection> nmsDetections = applyNMS(baseDetections, nmsThreshold);

    // Convert back to SegmentationDetection using carried index
    std::vector<SegmentationDetection> result;
    for (const auto & nmsDet : nmsDetections) {
      size_t origIdx = (size_t)nmsDet.track_id;
      if (origIdx < detections.size()) {
        result.push_back(detections[origIdx]);
      }
    }

    return result;
  }

  std::string DetectionModel::className(int classId, const std::vector<std::string> & fallback) const {
    if (classId >= 0) {
      if (!classLabels_.empty()) {
        if ((size_t)classId < classLabels_.size() && !classLabels_[classId].empty()) { return classLabels_[classId]; }
      } else if ((size_t)classId < fallback.size()) {
        return fallback[classId];
      }
    }
    return "class_" + std::to_string(classId);
  }

  // YOLOv8ClassificationModel implementation
  YOLOv8ClassificationModel::YOLOv8ClassificationModel(const std::string & modelPath, int inputSize)
    : DetectionModel(modelPath, inputSize) {}

  std::vector<Detection> YOLOv8ClassificationModel::parseOutput(float *outputData, const std::vector<int64_t> & outputShape,
                                                                float confThreshold, const cv::Size & originalSize) {
    // Classification models don't produce detections in the traditional sense
    return {};
  }

  ClassificationResult YOLOv8ClassificationModel::parseClassificationOutput(float *outputData, const std::vector<int64_t> & outputShape) {
    ClassificationResult result;
    result.class_id = -1;
    result.class_name = "unknown";
    result.confidence = 0.0f;

    if (outputShape.size() != 2) {
      ERROR_MSG("Invalid output shape for YOLOv8 classification model");
      return result;
    }

    int64_t numClasses = outputShape[1];
    if (numClasses < 1) {
      ERROR_MSG("Classification model reports %" PRId64 " classes", numClasses);
      return result;
    }

    // Convert raw output values to confidences (see OutputMode)
    std::vector<float> conf(numClasses);
    switch (outputMode_) {
      case SIGMOID:
        for (int64_t i = 0; i < numClasses; ++i) { conf[i] = 1.0f / (1.0f + std::exp(-outputData[i])); }
        break;
      case RAW:
        for (int64_t i = 0; i < numClasses; ++i) { conf[i] = outputData[i]; }
        break;
      case SOFTMAX:
      default: {
        float maxLogit = outputData[0];
        for (int64_t i = 1; i < numClasses; ++i) {
          if (outputData[i] > maxLogit) maxLogit = outputData[i];
        }
        float sumExp = 0.0f;
        for (int64_t i = 0; i < numClasses; ++i) {
          conf[i] = std::exp(outputData[i] - maxLogit);
          sumExp += conf[i];
        }
        for (int64_t i = 0; i < numClasses; ++i) { conf[i] /= sumExp; }
        break;
      }
    }

    // Rank classes by confidence, best first
    unsigned k = topK_;
    if ((int64_t)k > numClasses) { k = (unsigned)numClasses; }
    std::vector<int> order(numClasses);
    std::iota(order.begin(), order.end(), 0);
    std::partial_sort(order.begin(), order.begin() + k, order.end(),
                      [&conf](int a, int b) { return conf[a] > conf[b]; });

    // The built-in ImageNet fallback only makes sense when the head actually has
    // ImageNet's class count — a 2-class NSFW/violence model must not report
    // "goldfish", it reports "class_1" until a labels sidecar provides names.
    static const std::vector<std::string> noFallback;
    const std::vector<std::string> & fallback =
      (numClasses == (int64_t)IMAGENET_CLASSES.size()) ? IMAGENET_CLASSES : noFallback;

    result.class_id = order[0];
    result.confidence = conf[order[0]];
    result.class_name = className(order[0], fallback);
    if (k > 1) {
      result.top.reserve(k);
      for (unsigned i = 0; i < k; ++i) {
        ClassScore cs;
        cs.class_id = order[i];
        cs.class_name = className(order[i], fallback);
        cs.confidence = conf[order[i]];
        result.top.push_back(cs);
      }
    }

    return result;
  }

  ClassificationResult YOLOv8ClassificationModel::processClassificationFrame(const cv::Mat & frame, InferenceMetrics *metrics) {
    ClassificationResult result;
    result.timestamp = 0; // Will be set by caller

    try {
      auto startTime = std::chrono::high_resolution_clock::now();

      // Preprocess
      cv::Mat processedFrame = preprocessImage(frame);
      if (enhanceImage_) { processedFrame = enhanceImage(processedFrame); }

      auto preprocessEnd = std::chrono::high_resolution_clock::now();

      // Create input tensor
      TensorData tensorData = createInputTensor(processedFrame);
      ORTHelpers::OrtValueGuard inputGuard(tensorData.inputTensor);

      // Run inference
      auto inferenceStart = std::chrono::high_resolution_clock::now();
      const OrtApi *api = ORTHelpers::api();
      std::vector<OrtValue *> outputs;
      ORTHelpers::OrtOutputsGuard outputsGuard(outputs);
      runSession({tensorData.inputTensor}, outputs, "ONNX Runtime");

      auto inferenceEnd = std::chrono::high_resolution_clock::now();

      // Parse output
      float *outputData = nullptr;
      if (!ORTHelpers::checkStatus(api->GetTensorMutableData(outputs[0], (void **)&outputData), "GetTensorMutableData")) {
        throw std::runtime_error("Failed to get classification output tensor data");
      }
      OrtTensorTypeAndShapeInfo *ttsi = nullptr;
      if (!ORTHelpers::checkStatus(api->GetTensorTypeAndShape(outputs[0], &ttsi), "GetTensorTypeAndShape")) {
        throw std::runtime_error("Failed to get classification output tensor shape");
      }
      auto outputShape = ORTHelpers::getTensorShape(ttsi);
      if (ttsi) api->ReleaseTensorTypeAndShapeInfo(ttsi);

      result = parseClassificationOutput(outputData, outputShape);

      auto endTime = std::chrono::high_resolution_clock::now();

      // Update metrics
      if (metrics) {
        metrics->preprocessTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(preprocessEnd - startTime).count();
        metrics->inferenceTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(inferenceEnd - inferenceStart).count();
        metrics->postprocessTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - inferenceEnd).count();
        metrics->totalTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
        metrics->inputWidth = frame.cols;
        metrics->inputHeight = frame.rows;
        metrics->detectionCount = 1;
      }

    } catch (const std::exception & e) { ERROR_MSG("Classification inference error: %s", e.what()); }

    return result;
  }

  // YOLOv8OBBModel implementation
  YOLOv8OBBModel::YOLOv8OBBModel(const std::string & modelPath, int inputSize) : DetectionModel(modelPath, inputSize) {}

  std::vector<Detection> YOLOv8OBBModel::parseOutput(float *outputData, const std::vector<int64_t> & outputShape,
                                                     float confThreshold, const cv::Size & originalSize) {
    // Convert OBB detections to regular detections for base class compatibility
    std::vector<OBBDetection> obbDetections = parseOBBOutput(outputData, outputShape, confThreshold, originalSize);
    std::vector<Detection> detections;

    for (const auto & obb : obbDetections) { detections.push_back(static_cast<Detection>(obb)); }

    return detections;
  }

  std::vector<OBBDetection> YOLOv8OBBModel::parseOBBOutput(float *outputData, const std::vector<int64_t> & outputShape,
                                                           float confThreshold, const cv::Size & originalSize) {
    std::vector<OBBDetection> detections;

    if (outputShape.size() != 3) {
      ERROR_MSG("Invalid output shape for YOLOv8 OBB model: expected 3D tensor");
      return detections;
    }

    int64_t numDetections = outputShape[2];
    int64_t numFeatures = outputShape[1];

    // Support both standard (85/89 features) and compact (20 features) OBB formats
    bool isCompactFormat = (numFeatures == 20);
    bool isStandardFormat = (numFeatures == 85 || numFeatures == 89);

    if (!isCompactFormat && !isStandardFormat) {
      ERROR_MSG("Invalid output shape for YOLOv8 OBB model: expected 20, 85, or 89 features, got %" PRId64, (int64_t)numFeatures);
      return detections;
    }

    // YOLOv8 OBB format: [cx, cy, w, h, angle, class0_conf, ..., classN_conf]
    // Compact format: 15 classes (features 5-19)
    // Standard format: 80 classes (features 5-84)
    int64_t numClasses = isCompactFormat ? 15 : 80;

    VERYHIGH_MSG(
      "OBB parsing: shape=[%" PRId64 ", %" PRId64 ", %" PRId64 "], format=%s, classes=%" PRId64 ", confThreshold=%.3f",
      outputShape[0], outputShape[1], outputShape[2], isCompactFormat ? "compact" : "standard", numClasses, confThreshold);

    // Sample a few detections to see raw confidence values and determine normalization
    float maxRawConfSeen = -1000.0f;
    float minRawConfSeen = 1000.0f;
    int detectionsAboveThreshold = 0;
    int rawDetectionsChecked = 0;
    int earlyFilteredOut = 0;

    // Helper function for safe sigmoid calculation
    auto safeSigmoid = [](float x) -> float {
      // Clamp extreme values to prevent overflow/underflow
      if (x > 88.0f) return 1.0f; // exp(-88) is essentially 0
      if (x < -88.0f) return 0.0f; // exp(88) is essentially infinity

      float result = 1.0f / (1.0f + std::exp(-x));

      // Validate result for NaN/infinity
      if (!std::isfinite(result) || std::isnan(result)) {
        return 0.0f; // Return 0 confidence for invalid values
      }

      return result;
    };

    // First pass: collect all sigmoid-activated confidences to determine normalization range
    std::vector<float> allConfidences;
    allConfidences.reserve(numDetections);

    for (int64_t i = 0; i < numDetections; ++i) {
      // Find best class confidence (starting from index 5)
      float maxRawConf = -1000.0f;
      for (int64_t c = 5; c < 5 + numClasses; ++c) {
        float rawConf = outputData[c * numDetections + i];

        // Validate raw confidence value
        if (!std::isfinite(rawConf) || std::isnan(rawConf)) {
          continue; // Skip invalid values
        }

        if (rawConf > maxRawConf) { maxRawConf = rawConf; }
      }

      // Skip if no valid class found
      if (maxRawConf == -1000.0f) { continue; }

      // Track raw confidence range for debugging
      if (maxRawConf > maxRawConfSeen) { maxRawConfSeen = maxRawConf; }
      if (maxRawConf < minRawConfSeen) { minRawConfSeen = maxRawConf; }

      // Apply sigmoid to convert logits to probabilities
      float sigmoidConf = safeSigmoid(maxRawConf);
      allConfidences.push_back(sigmoidConf);
    }

    if (allConfidences.empty()) {
      WARN_MSG("No valid confidence values found in OBB output");
      return detections;
    }

    // Calculate normalization parameters from the sigmoid outputs
    float minSigmoidConf = *std::min_element(allConfidences.begin(), allConfidences.end());
    float maxSigmoidConf = *std::max_element(allConfidences.begin(), allConfidences.end());

    // Model-based normalization for YOLO11 OBB logits
    // YOLO models typically output logits in a meaningful range of roughly [-6, +6]
    // sigmoid(-6) ≈ 0.0025 (very low confidence, background)
    // sigmoid(+6) ≈ 0.9975 (very high confidence, strong detection)
    // We map this theoretical sigmoid range [0.0025, 0.9975] to [0.0, 1.0]
    const float modelMinSigmoid = 0.0025f; // sigmoid(-6) - theoretical minimum
    const float modelMaxSigmoid = 0.9975f; // sigmoid(+6) - theoretical maximum

    // Normalize sigmoid outputs to 0-1 range using model-based bounds
    auto normalizeConfidence = [modelMinSigmoid, modelMaxSigmoid](float sigmoidConf) -> float {
      // Map sigmoid range [0.0025, 0.9975] to [0.0, 1.0]
      // This preserves the full resolution of the model's output
      return std::max(0.0f, std::min(1.0f, (sigmoidConf - modelMinSigmoid) / (modelMaxSigmoid - modelMinSigmoid)));
    };

    VERYHIGH_MSG("OBB confidence normalization: raw range [%.6f, %.6f], sigmoid range [%.6f, "
                 "%.6f], model bounds [%.6f, %.6f]",
                 minRawConfSeen, maxRawConfSeen, minSigmoidConf, maxSigmoidConf, modelMinSigmoid, modelMaxSigmoid);

    // Second pass: create detections with normalized confidences
    size_t confIndex = 0;
    for (int64_t i = 0; i < numDetections; ++i) {
      // Find best class confidence (starting from index 5)
      float maxRawConf = -1000.0f;
      int bestClass = -1;
      for (int64_t c = 5; c < 5 + numClasses; ++c) {
        float rawConf = outputData[c * numDetections + i];

        // Validate raw confidence value
        if (!std::isfinite(rawConf) || std::isnan(rawConf)) {
          continue; // Skip invalid values
        }

        if (rawConf > maxRawConf) {
          maxRawConf = rawConf;
          bestClass = c - 5;
        }
      }

      // Skip if no valid class found
      if (bestClass == -1 || confIndex >= allConfidences.size()) { continue; }

      rawDetectionsChecked++;

      // Get the pre-computed sigmoid confidence and normalize it
      float sigmoidConf = allConfidences[confIndex++];
      float normalizedConf = normalizeConfidence(sigmoidConf);

      // Apply user's confidence threshold to normalized values
      if (normalizedConf < confThreshold) {
        earlyFilteredOut++;
        continue;
      }

      detectionsAboveThreshold++;

      // Get bounding box coordinates
      float cx = outputData[i];
      float cy = outputData[numDetections + i];
      float w = outputData[2 * numDetections + i];
      float h = outputData[3 * numDetections + i];
      float angle = outputData[4 * numDetections + i];

      // Validate bounding box coordinates
      if (!std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(w) || !std::isfinite(h) || !std::isfinite(angle) ||
          w <= 0 || h <= 0) {
        continue; // Skip invalid bounding boxes
      }

      OBBDetection det;
      // Convert from center format to top-left format and normalize
      det.x = (cx - w / 2.0f) / inputWidth_;
      det.y = (cy - h / 2.0f) / inputHeight_;
      det.w = w / inputWidth_;
      det.h = h / inputHeight_;
      det.confidence = normalizedConf; // Use the normalized confidence
      det.class_id = bestClass;
      det.track_id = i;
      // Select class names based on output class count (custom labels take precedence)
      if (!getClassLabels().empty()) {
        det.class_name = className(bestClass, Utils::COCO_CLASSES);
      } else if (numClasses == 15 && bestClass >= 0 && (size_t)bestClass < Utils::DOTA_CLASSES.size()) {
        det.class_name = Utils::DOTA_CLASSES[bestClass];
      } else if (bestClass >= 0 && (size_t)bestClass < Utils::COCO_CLASSES.size()) {
        det.class_name = Utils::COCO_CLASSES[bestClass];
      } else {
        det.class_name = "class_" + std::to_string(bestClass);
      }
      // Normalize angle to [-pi, pi]
      const float pi = 3.14159265358979323846f;
      while (angle > pi) angle -= 2.0f * pi;
      while (angle < -pi) angle += 2.0f * pi;
      det.angle = angle;
      det.center = cv::Point2f(cx / inputWidth_, cy / inputHeight_);
      det.size = cv::Size2f(w / inputWidth_, h / inputHeight_);

      // Calculate corner points for oriented bounding box
      float cos_a = std::cos(angle);
      float sin_a = std::sin(angle);
      float hw = w / 2.0f;
      float hh = h / 2.0f;

      det.corners.resize(4);
      det.corners[0] = cv::Point2f((cx + (-hw * cos_a - -hh * sin_a)) / inputWidth_, (cy + (-hw * sin_a + -hh * cos_a)) / inputHeight_);
      det.corners[1] = cv::Point2f((cx + (hw * cos_a - -hh * sin_a)) / inputWidth_, (cy + (hw * sin_a + -hh * cos_a)) / inputHeight_);
      det.corners[2] = cv::Point2f((cx + (hw * cos_a - hh * sin_a)) / inputWidth_, (cy + (hw * sin_a + hh * cos_a)) / inputHeight_);
      det.corners[3] = cv::Point2f((cx + (-hw * cos_a - hh * sin_a)) / inputWidth_, (cy + (-hw * sin_a + hh * cos_a)) / inputHeight_);

      // Remap all coordinates from letterbox space to original image space
      remapLetterboxCoords(static_cast<Detection &>(det));
      if (useLetterbox_ && (letterboxPadX_ != 0 || letterboxPadY_ != 0)) {
        float activeW = (float)(inputWidth_ - 2 * letterboxPadX_);
        float activeH = (float)(inputHeight_ - 2 * letterboxPadY_);
        if (activeW > 0.0f && activeH > 0.0f) {
          det.center.x = (det.center.x * inputWidth_ - letterboxPadX_) / activeW;
          det.center.y = (det.center.y * inputHeight_ - letterboxPadY_) / activeH;
          det.size.width = det.size.width * inputWidth_ / activeW;
          det.size.height = det.size.height * inputHeight_ / activeH;
          for (auto & c : det.corners) {
            c.x = (c.x * inputWidth_ - letterboxPadX_) / activeW;
            c.y = (c.y * inputHeight_ - letterboxPadY_) / activeH;
          }
        }
      }

      // Clamp corner points to [0,1]
      for (size_t k = 0; k < det.corners.size(); ++k) { clampPoint(det.corners[k]); }
      clampDetection(static_cast<Detection &>(det));
      detections.push_back(det);
    }

    VERYHIGH_MSG("OBB parsing results: %d detections above threshold %.3f, %d raw detections "
                 "checked, %d early filtered out",
                 detectionsAboveThreshold, confThreshold, rawDetectionsChecked, earlyFilteredOut);

    // Debug: Print first few detections to see what we're getting
    if (detections.size() > 0) {
      VERYHIGH_MSG("First few OBB detections:");
      for (size_t i = 0; i < std::min(detections.size(), size_t(5)); ++i) {
        const auto & det = detections[i];
        VERYHIGH_MSG("  [%zu] class=%d (%s), conf=%.6f, bbox=(%.3f,%.3f,%.3f,%.3f), angle=%.3f", i, det.class_id,
                     det.class_name.c_str(), det.confidence, det.x, det.y, det.w, det.h, det.angle);
      }
    }

    // If we still have too many detections, warn but don't error
    if (detections.size() > 100) {
      WARN_MSG("OBB model detected %zu objects after confidence filtering (threshold=%.3f). "
               "Sigmoid range: [%.6f, %.6f]",
               detections.size(), confThreshold, minSigmoidConf, maxSigmoidConf);

      // Count detections by confidence range
      int veryHighConf = 0, highConf = 0, medConf = 0, lowConf = 0;
      for (const auto & det : detections) {
        if (det.confidence > 0.9f)
          veryHighConf++;
        else if (det.confidence > 0.8f)
          highConf++;
        else if (det.confidence > 0.7f)
          medConf++;
        else
          lowConf++;
      }
      WARN_MSG("Confidence distribution: >90%%: %d, 80-90%%: %d, 70-80%%: %d, <70%%: %d", veryHighConf, highConf, medConf, lowConf);
    }

    // Debug output for confidence processing
    if (rawDetectionsChecked > 0) {
      INFO_MSG("OBB Confidence Analysis: Raw range [%.6f, %.6f], Sigmoid range [%.6f, %.6f], "
               "Normalized to [0.0, 1.0], %d/%d detections above threshold %.3f, %d early filtered",
               minRawConfSeen, maxRawConfSeen, minSigmoidConf, maxSigmoidConf, detectionsAboveThreshold,
               rawDetectionsChecked, confThreshold, earlyFilteredOut);
    }

    return detections;
  }
  std::vector<OBBDetection> YOLOv8OBBModel::processOBBFrame(const cv::Mat & frame, float confThreshold,
                                                            float nmsThreshold, InferenceMetrics *metrics) {
    std::vector<OBBDetection> detections;

    try {
      auto startTime = std::chrono::high_resolution_clock::now();

      // Preprocess
      cv::Mat processedFrame = preprocessImage(frame);
      if (enhanceImage_) { processedFrame = enhanceImage(processedFrame); }

      auto preprocessEnd = std::chrono::high_resolution_clock::now();

      // Create input tensor using the helper function
      TensorData tensorData = createInputTensor(processedFrame);
      ORTHelpers::OrtValueGuard inputGuard(tensorData.inputTensor);

      // Run inference (C API)
      auto inferenceStart = std::chrono::high_resolution_clock::now();
      const OrtApi *api = ORTHelpers::api();
      std::vector<OrtValue *> outputs;
      ORTHelpers::OrtOutputsGuard outputsGuard(outputs);
      runSession({tensorData.inputTensor}, outputs, "ONNX Runtime");

      auto inferenceEnd = std::chrono::high_resolution_clock::now();

      // Parse OBB output directly to get oriented bounding boxes
      float *outputData = nullptr;
      if (!ORTHelpers::checkStatus(api->GetTensorMutableData(outputs[0], (void **)&outputData), "GetTensorMutableData")) {
        throw std::runtime_error("Failed to get OBB output tensor data");
      }
      OrtTensorTypeAndShapeInfo *ttsi = nullptr;
      if (!ORTHelpers::checkStatus(api->GetTensorTypeAndShape(outputs[0], &ttsi), "GetTensorTypeAndShape")) {
        throw std::runtime_error("Failed to get OBB output tensor shape");
      }
      auto outputShape = ORTHelpers::getTensorShape(ttsi);
      if (ttsi) api->ReleaseTensorTypeAndShapeInfo(ttsi);

      detections = parseOBBOutput(outputData, outputShape, confThreshold, cv::Size(frame.cols, frame.rows));

      auto postprocessEnd = std::chrono::high_resolution_clock::now();

      // Apply NMS
      if (nmsThreshold > 0.0f && !detections.empty()) {
        // Convert to base detections for NMS, carrying vector index in track_id
        std::vector<Detection> baseDetections;
        for (size_t i = 0; i < detections.size(); ++i) {
          Detection d = static_cast<Detection>(detections[i]);
          d.track_id = i;
          baseDetections.push_back(d);
        }

        baseDetections = applyNMS(baseDetections, nmsThreshold);

        // Convert back to OBB detections using carried index
        std::vector<OBBDetection> nmsDetections;
        for (size_t i = 0; i < baseDetections.size(); ++i) {
          size_t origIdx = (size_t)baseDetections[i].track_id;
          if (origIdx < detections.size()) {
            nmsDetections.push_back(detections[origIdx]);
          }
        }
        detections = nmsDetections;
      }

      // Update metrics
      if (metrics) {
        metrics->preprocessTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(preprocessEnd - startTime).count();
        metrics->inferenceTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(inferenceEnd - inferenceStart).count();
        metrics->postprocessTimeMs =
          std::chrono::duration_cast<std::chrono::milliseconds>(postprocessEnd - inferenceEnd).count();
        metrics->totalTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(postprocessEnd - startTime).count();
        metrics->detectionCount = detections.size();
      }

    } catch (const std::exception & e) { ERROR_MSG("Error in OBB frame processing: %s", e.what()); }

    return detections;
  }
  // TemporalTracker implementation
  TemporalTracker::TemporalTracker(float iouThreshold, int minConsecutiveMs, int maxMissingMs)
    : nextTrackId_(1), iouThreshold_(iouThreshold), minConsecutiveMs_(minConsecutiveMs), maxMissingMs_(maxMissingMs) {}

  std::vector<Detection> TemporalTracker::updateTracks(const std::vector<Detection> & newDetections, uint64_t timestamp) {
    INSANE_MSG("Tracker input: %zu detections, timestamp %" PRIu64, newDetections.size(), (uint64_t)timestamp);

    std::vector<Detection> smoothedDetections;

    // Match new detections with existing tracks (Hungarian assignment with gating)
    std::vector<bool> detectionMatched(newDetections.size(), false);
    std::vector<bool> trackMatched(tracks_.size(), false);
    size_t T = tracks_.size();
    size_t D = newDetections.size();
    if (T > 0 && D > 0) {
      // Pre-compute Kalman predictions for all tracks to avoid mutating state during scoring
      std::vector<Detection> predictedTracks(T);
      for (size_t i = 0; i < T; ++i) {
        if (useKalmanFilter_ && tracks_[i].kalmanInitialized) {
          predictedTracks[i] = predictKalmanState(tracks_[i], timestamp);
        } else {
          predictedTracks[i] = tracks_[i];
        }
      }

      size_t N = std::max(T, D);
      const float BIG = 1e6f;
      std::vector<float> cost(N * N, BIG);
      for (size_t i = 0; i < T; ++i) {
        for (size_t j = 0; j < D; ++j) {
          float iou = calculateIoU(predictedTracks[i], newDetections[j]);
          cv::Point2f c1(predictedTracks[i].x + predictedTracks[i].w / 2.0f, predictedTracks[i].y + predictedTracks[i].h / 2.0f);
          cv::Point2f c2(newDetections[j].x + newDetections[j].w / 2.0f, newDetections[j].y + newDetections[j].h / 2.0f);
          float dist = cv::norm(c1 - c2);
          bool pass = (iou >= iouThreshold_) || (dist < 0.2f);
          cost[i * N + j] = pass ? (1.0f - iou + 0.1f * dist) : BIG;
        }
      }
      std::vector<float> u(N + 1, 0), v(N + 1, 0);
      std::vector<int> p(N + 1, 0), way(N + 1, 0);
      for (size_t i = 1; i <= N; ++i) {
        p[0] = (int)i;
        int j0 = 0;
        std::vector<float> minv(N + 1, BIG);
        std::vector<char> used(N + 1, 0);
        do {
          used[j0] = 1;
          int i0 = p[j0], j1 = 0;
          float delta = BIG;
          for (size_t j = 1; j <= N; ++j)
            if (!used[j]) {
              float cur = cost[(i0 - 1) * N + (j - 1)] - u[i0] - v[j];
              if (cur < minv[j]) {
                minv[j] = cur;
                way[j] = j0;
              }
              if (minv[j] < delta) {
                delta = minv[j];
                j1 = (int)j;
              }
            }
          for (size_t j = 0; j <= N; ++j) {
            if (used[j]) {
              u[p[j]] += delta;
              v[j] -= delta;
            } else {
              minv[j] -= delta;
            }
          }
          j0 = j1;
        } while (p[j0] != 0);
        do {
          int j1 = way[j0];
          p[j0] = p[j1];
          j0 = j1;
        } while (j0);
      }
      std::vector<int> matchRow(N + 1, 0);
      for (size_t j = 1; j <= N; ++j) matchRow[p[j]] = (int)j;
      for (size_t i = 0; i < T; ++i) {
        int j = matchRow[i + 1] - 1;
        if (j >= 0 && (size_t)j < D && cost[i * N + j] < BIG / 2) {
          trackMatched[i] = true;
          detectionMatched[j] = true;
          Detection & track = tracks_[i];
          track.x = newDetections[j].x;
          track.y = newDetections[j].y;
          track.w = newDetections[j].w;
          track.h = newDetections[j].h;
          track.confidence = newDetections[j].confidence;
          track.class_id = newDetections[j].class_id;
          track.class_name = newDetections[j].class_name;
          track.addTrailPoint();
          if (useKalmanFilter_) {
            if (!track.kalmanInitialized) { initializeKalmanFilter(track); }
            updateKalmanFilter(track, newDetections[j], timestamp);
          }
          track.last_seen_time = timestamp;
          float dt = 1.0f / 30.0f;
          track.track_confidence = std::min(1.0f, track.track_confidence + 0.3f * dt);
          uint64_t duration = track.getTrackDurationMs();
          uint64_t requiredMs = (duration >= minConsecutiveMs_)
            ? std::max<uint64_t>(100, static_cast<uint64_t>(minConsecutiveMs_ * 0.7))
            : minConsecutiveMs_;
          if (duration >= requiredMs) { smoothedDetections.push_back(track); }
        }
      }
    }

    // Second pass: handle unmatched tracks (missing detections)
    for (size_t j = 0; j < tracks_.size(); ++j) {
      if (trackMatched[j]) continue;

      Detection & track = tracks_[j];

      // Use Kalman filter prediction for missing detections
      if (useKalmanFilter_ && track.kalmanInitialized) {
        Detection predicted = predictKalmanState(track, timestamp);
        track.x = predicted.x;
        track.y = predicted.y;
        track.w = predicted.w;
        track.h = predicted.h;

        // Note: No trail point added for predictions - trails only show actual detections
      }

      // Reduce confidence using time-proportional decay
      float dt = 0.0f;
      if (track.last_seen_time > 0 && timestamp >= track.last_seen_time) {
        dt = (timestamp - track.last_seen_time) / 1000.0f;
      }
      if (dt <= 0.0f) dt = 1.0f / 30.0f;
      track.track_confidence = std::max(0.0f, track.track_confidence - 0.5f * dt);

      // Keep track alive if it hasn't been missing for too long
      uint64_t timeSinceLastSeen = track.getTimeSinceLastSeenMs(timestamp);
      if (timeSinceLastSeen <= maxMissingMs_) {
        // Use hysteresis for predicted tracks too
        uint64_t trackDuration = track.getTrackDurationMs();
        uint64_t requiredMs = (trackDuration >= minConsecutiveMs_)
          ? std::max<uint64_t>(100, static_cast<uint64_t>(minConsecutiveMs_ * 0.7))
          : minConsecutiveMs_;

        if (trackDuration >= requiredMs) { smoothedDetections.push_back(track); }
      }
    }

    // Third pass: create new tracks for unmatched detections
    for (size_t i = 0; i < newDetections.size(); ++i) {
      if (detectionMatched[i]) continue;

      // Check for potential duplicate tracks before creating new one
      bool isDuplicate = false;
      for (const auto & existingTrack : tracks_) {
        float iou = calculateIoU(existingTrack, newDetections[i]);
        // Use a lower threshold to detect potential duplicates
        if (iou > iouThreshold_ * 0.7f) {
          isDuplicate = true;
          VERYHIGH_MSG("Preventing duplicate track creation: IoU %.3f with existing track %" PRIu64, iou,
                       (uint64_t)existingTrack.track_id);
          break;
        }
      }

      // Also check against recently output detections to prevent duplicates
      for (const auto & outputDetection : smoothedDetections) {
        float iou = calculateIoU(outputDetection, newDetections[i]);
        if (iou > iouThreshold_ * 0.8f) {
          isDuplicate = true;
          VERYHIGH_MSG("Preventing duplicate track creation: IoU %.3f with output detection %" PRIu64, iou,
                       (uint64_t)outputDetection.track_id);
          break;
        }
      }

      if (isDuplicate) {
        continue; // Skip creating this track
      }

      Detection newTrack = newDetections[i];
      newTrack.track_id = nextTrackId_++;
      newTrack.first_seen_time = timestamp;
      newTrack.last_seen_time = timestamp;
      newTrack.track_confidence = 0.1f; // Start with low confidence
      newTrack.clearTrail(); // Initialize empty trail
      newTrack.addTrailPoint(); // Add initial position

      // Initialize Kalman filter for new track
      if (useKalmanFilter_) { initializeKalmanFilter(newTrack); }

      tracks_.push_back(newTrack);

      // Add to output only if it meets the minimum consecutive time requirement
      // New tracks start with 0 duration, so they won't appear immediately
      uint64_t trackDuration = newTrack.getTrackDurationMs();
      if (trackDuration >= minConsecutiveMs_) { smoothedDetections.push_back(newTrack); }
    }

    // Remove tracks that have been missing for too long
    tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(),
                                 [timestamp, this](const Detection & track) {
      return track.getTimeSinceLastSeenMs(timestamp) > maxMissingMs_;
    }),
                  tracks_.end());

    INSANE_MSG("Tracker output: %zu detections (filtered out %zu)", smoothedDetections.size(),
               newDetections.size() - smoothedDetections.size());
    return smoothedDetections;
  }

  void TemporalTracker::softReset(const std::vector<Detection> & currentDetections, uint64_t timestamp) {
    if (tracks_.empty()) {
      // No existing tracks, nothing to preserve
      return;
    }

    size_t originalTrackCount = tracks_.size();
    std::vector<Detection> preservedTracks;

    // Try to match current detections with existing tracks
    for (const auto & track : tracks_) {
      float bestIoU = 0.0f;
      bool foundMatch = false;

      for (const auto & detection : currentDetections) {
        float iou = calculateIoU(track, detection);
        if (iou > bestIoU && iou > iouThreshold_) {
          bestIoU = iou;
          foundMatch = true;
        }
      }

      // Preserve tracks that still have matching detections
      if (foundMatch) {
        Detection preservedTrack = track;
        // Keep track ID and timing but reset some properties
        preservedTrack.last_seen_time = timestamp;
        // Reduce track duration slightly but don't reset completely
        uint64_t currentDuration = preservedTrack.getTrackDurationMs();
        preservedTrack.first_seen_time = timestamp - std::max<uint64_t>(100, (uint64_t)(currentDuration / 2));
        // Reduce track confidence but don't reset completely
        preservedTrack.track_confidence = std::max(0.3f, preservedTrack.track_confidence * 0.7f);
        // Keep trail for continuity but reduce its length
        if (preservedTrack.trail.size() > 5) {
          // Keep only the last 5 trail points for continuity
          preservedTrack.trail.erase(preservedTrack.trail.begin(), preservedTrack.trail.end() - 5);
        }

        preservedTracks.push_back(preservedTrack);
      }
    }

    // Replace tracks with preserved ones
    tracks_ = preservedTracks;

    INFO_MSG("Scene change soft reset: preserved %zu out of %zu tracks", preservedTracks.size(), originalTrackCount);
  }

  float TemporalTracker::calculateIoU(const Detection & a, const Detection & b) {
    float x1 = std::max(a.x, b.x);
    float y1 = std::max(a.y, b.y);
    float x2 = std::min(a.x + a.w, b.x + b.w);
    float y2 = std::min(a.y + a.h, b.y + b.h);

    if (x2 <= x1 || y2 <= y1) return 0.0f;

    float intersection = (x2 - x1) * (y2 - y1);
    float areaA = a.w * a.h;
    float areaB = b.w * b.h;
    float unionArea = areaA + areaB - intersection;

    return intersection / unionArea;
  }

  float TemporalTracker::calculateIoUWithPrediction(Detection & track, const Detection & detection, uint64_t timestamp) {
    // Use Kalman filter prediction if available
    if (useKalmanFilter_ && track.kalmanInitialized) {
      Detection predicted = predictKalmanState(track, timestamp);
      return calculateIoU(predicted, detection);
    }

    // No prediction available, use current position
    return calculateIoU(track, detection);
  }

  void TemporalTracker::initializeKalmanFilter(Detection & detection) {
    if (!useKalmanFilter_) return;

    // Create 6-state Kalman filter: [x, y, w, h, vx, vy]
    detection.kalmanFilter = std::make_shared<cv::KalmanFilter>(6, 4, 0);

    // State transition matrix (constant velocity model)
    // x' = x + vx*dt, y' = y + vy*dt, w' = w, h' = h, vx' = vx, vy' = vy
    detection.kalmanFilter->transitionMatrix = (cv::Mat_<float>(6, 6) << 1, 0, 0, 0, 1, 0, // x = x + vx*dt (dt=1 for simplicity)
                                                0, 1, 0, 0, 0, 1, // y = y + vy*dt
                                                0, 0, 1, 0, 0, 0, // w = w (constant)
                                                0, 0, 0, 1, 0, 0, // h = h (constant)
                                                0, 0, 0, 0, 1, 0, // vx = vx (constant velocity)
                                                0, 0, 0, 0, 0, 1 // vy = vy (constant velocity)
    );

    // Measurement matrix (we observe x, y, w, h)
    detection.kalmanFilter->measurementMatrix = (cv::Mat_<float>(4, 6) << 1, 0, 0, 0, 0, 0, // observe x
                                                 0, 1, 0, 0, 0, 0, // observe y
                                                 0, 0, 1, 0, 0, 0, // observe w
                                                 0, 0, 0, 1, 0, 0 // observe h
    );

    // Process noise covariance (how much we trust the model) - more conservative
    cv::setIdentity(detection.kalmanFilter->processNoiseCov, cv::Scalar::all(kalmanProcessNoise_));
    // Much more conservative velocity noise to prevent wild predictions
    detection.kalmanFilter->processNoiseCov.at<float>(4, 4) = kalmanProcessNoise_ * 5; // Reduced from 10
    detection.kalmanFilter->processNoiseCov.at<float>(5, 5) = kalmanProcessNoise_ * 5; // Reduced from 10

    // Measurement noise covariance (how much we trust the measurements)
    cv::setIdentity(detection.kalmanFilter->measurementNoiseCov, cv::Scalar::all(kalmanMeasurementNoise_));

    // Error covariance matrix (initial uncertainty) - more conservative
    cv::setIdentity(detection.kalmanFilter->errorCovPost, cv::Scalar::all(0.1f)); // Reduced from 1.0f
    // Higher uncertainty for velocity components initially
    detection.kalmanFilter->errorCovPost.at<float>(4, 4) = 0.01f; // Very low initial velocity uncertainty
    detection.kalmanFilter->errorCovPost.at<float>(5, 5) = 0.01f; // Very low initial velocity uncertainty

    // Initialize state with current detection (zero initial velocity)
    detection.kalmanFilter->statePre.at<float>(0) = detection.x;
    detection.kalmanFilter->statePre.at<float>(1) = detection.y;
    detection.kalmanFilter->statePre.at<float>(2) = detection.w;
    detection.kalmanFilter->statePre.at<float>(3) = detection.h;
    detection.kalmanFilter->statePre.at<float>(4) = 0.0f; // vx - start with zero velocity
    detection.kalmanFilter->statePre.at<float>(5) = 0.0f; // vy - start with zero velocity

    detection.kalmanFilter->statePost = detection.kalmanFilter->statePre.clone();
    detection.kalmanInitialized = true;

    VERYHIGH_MSG("Initialized conservative Kalman filter for track %" PRIu64 " at (%.3f, %.3f, %.3f, %.3f)",
                 detection.track_id, detection.x, detection.y, detection.w, detection.h);
  }

  void TemporalTracker::updateKalmanFilter(Detection & track, const Detection & measurement, uint64_t timestamp) {
    if (!useKalmanFilter_ || !track.kalmanFilter || !track.kalmanInitialized) return;
    // Update transition matrix with real dt (seconds)
    float dt = 0.0f;
    if (track.last_seen_time > 0 && timestamp >= track.last_seen_time) {
      dt = (timestamp - track.last_seen_time) / 1000.0f;
    }
    if (dt <= 0.0f) dt = 1.0f / 30.0f; // fallback ~30 FPS
    track.kalmanFilter->transitionMatrix.at<float>(0, 4) = dt;
    track.kalmanFilter->transitionMatrix.at<float>(1, 5) = dt;

    // Predict step
    cv::Mat prediction = track.kalmanFilter->predict();

    // Create measurement vector [x, y, w, h]
    cv::Mat measurementVec = (cv::Mat_<float>(4, 1) << measurement.x, measurement.y, measurement.w, measurement.h);

    // Correct step
    cv::Mat corrected = track.kalmanFilter->correct(measurementVec);

    // Update track with corrected state
    track.x = corrected.at<float>(0);
    track.y = corrected.at<float>(1);
    track.w = corrected.at<float>(2);
    track.h = corrected.at<float>(3);
    clampDetection(track);

    // Update velocity from Kalman state - removed since velocity field doesn't exist

    VERYHIGH_MSG("Updated Kalman filter for track %" PRIu64 ": pos(%.3f, %.3f) size(%.3f, %.3f)", track.track_id,
                 track.x, track.y, track.w, track.h);
  }

  Detection TemporalTracker::predictKalmanState(Detection & track, uint64_t timestamp) {
    if (!useKalmanFilter_ || !track.kalmanInitialized) {
      // No Kalman filter available, return current state
      return track;
    }

    // Predict next state using Kalman filter
    // Update transition matrix with real dt (seconds)
    float dt = 0.0f;
    if (track.last_seen_time > 0 && timestamp >= track.last_seen_time) {
      dt = (timestamp - track.last_seen_time) / 1000.0f;
    }
    if (dt <= 0.0f) dt = 1.0f / 30.0f; // fallback ~30 FPS
    track.kalmanFilter->transitionMatrix.at<float>(0, 4) = dt;
    track.kalmanFilter->transitionMatrix.at<float>(1, 5) = dt;

    cv::Mat prediction = track.kalmanFilter->predict();

    Detection predicted = track;
    predicted.x = prediction.at<float>(0);
    predicted.y = prediction.at<float>(1);
    predicted.w = prediction.at<float>(2);
    predicted.h = prediction.at<float>(3);
    // Note: velocity fields removed since Detection struct no longer has them
    clampDetection(predicted);

    // Validate Kalman prediction - prevent "flying boxes"
    if (!isValidPrediction(predicted, track)) {
      VERYHIGH_MSG("Invalid Kalman prediction for track %" PRIu64 ", using current position", (uint64_t)track.track_id);

      // Reset Kalman filter if prediction is invalid
      if (track.kalmanInitialized) {
        // Reinitialize with current position
        Detection resetTrack = track;
        // Note: velocity field removed
        track.kalmanInitialized = false;
        initializeKalmanFilter(track);
      }

      return track; // Return current position
    }

    VERYHIGH_MSG("Kalman prediction for track %" PRIu64 ": pos(%.3f, %.3f) size(%.3f, %.3f)", track.track_id,
                 predicted.x, predicted.y, predicted.w, predicted.h);

    return predicted;
  }

  // Validate prediction to prevent "flying boxes"
  bool TemporalTracker::isValidPrediction(const Detection & predicted, const Detection & original) {
    // Check if prediction is within reasonable bounds (0-1 normalized coordinates)
    if (predicted.x < -0.5f || predicted.x > 1.5f || predicted.y < -0.5f || predicted.y > 1.5f) {
      VERYHIGH_MSG("Invalid prediction: position out of bounds (%.3f, %.3f)", predicted.x, predicted.y);
      return false;
    }

    // Check if predicted size is reasonable (not too small or too large)
    if (predicted.w < 0.001f || predicted.w > 2.0f || predicted.h < 0.001f || predicted.h > 2.0f) {
      VERYHIGH_MSG("Invalid prediction: size out of bounds (%.3f, %.3f)", predicted.w, predicted.h);
      return false;
    }

    // Check if prediction hasn't moved too far from original position
    float centerX = predicted.x + predicted.w / 2;
    float centerY = predicted.y + predicted.h / 2;
    float originalCenterX = original.x + original.w / 2;
    float originalCenterY = original.y + original.h / 2;

    float distance = std::sqrt(std::pow(centerX - originalCenterX, 2) + std::pow(centerY - originalCenterY, 2));

    // Maximum allowed movement is 50% of image diagonal per prediction
    float maxDistance = 0.5f; // 50% of normalized image diagonal
    if (distance > maxDistance) {
      VERYHIGH_MSG("Invalid prediction: moved too far %.3f > %.3f", distance, maxDistance);
      return false;
    }

    // Check velocity magnitude (prevent unrealistic speeds) - removed since velocity field doesn't
    // exist Note: Velocity validation removed since Detection struct no longer has velocity field

    return true; // All checks passed
  }

  // GenericModel implementation
  // ---- YOLONMSModel (baked-in NMS, output [1, max_det, 6]) ----

  YOLONMSModel::YOLONMSModel(const std::string & modelPath, int inputSize) : DetectionModel(modelPath, inputSize) {}

  std::vector<Detection> YOLONMSModel::parseOutput(float *outputData, const std::vector<int64_t> & outputShape,
                                                    float confThreshold, const cv::Size & originalSize) {
    std::vector<Detection> detections;
    if (outputShape.size() != 3 || outputShape[2] < 6) return detections;

    int64_t maxDet = outputShape[1];
    int64_t cols = outputShape[2];

    for (int64_t i = 0; i < maxDet; ++i) {
      float *row = outputData + i * cols;
      float conf = row[4];
      if (conf < confThreshold) continue;

      int classId = static_cast<int>(row[5]);
      float x1 = row[0] / inputWidth_;
      float y1 = row[1] / inputHeight_;
      float x2 = row[2] / inputWidth_;
      float y2 = row[3] / inputHeight_;

      Detection det;
      det.x = x1;
      det.y = y1;
      det.w = x2 - x1;
      det.h = y2 - y1;
      det.confidence = conf;
      det.class_id = classId;
      det.class_name = className(classId, Utils::COCO_CLASSES);
      det.track_id = i;

      remapLetterboxCoords(det);
      clampDetection(det);
      detections.push_back(det);
    }
    return detections;
  }

  // ---- YOLOSplitModel (class logits [1,N,C] + normalized cxcywh boxes [1,N,4]) ----

  YOLOSplitModel::YOLOSplitModel(const std::string & modelPath, int inputSize)
    : DetectionModel(modelPath, inputSize) {
    preprocessConfig_.resizeMode = PreprocessConfig::DIRECT_RESIZE;
    preprocessConfig_.normMode = PreprocessConfig::SCALE_01;
    useLetterbox_ = false;
  }

  std::vector<Detection> YOLOSplitModel::parseOutput(float *, const std::vector<int64_t> &,
                                                      float, const cv::Size &) {
    return {};
  }

  std::vector<Detection> YOLOSplitModel::processFrame(const cv::Mat &frame, float confThreshold,
                                                       float nmsThreshold, InferenceMetrics *metrics) {
    (void)nmsThreshold; // End-to-end query outputs are already duplicate-free by design.
    std::vector<Detection> detections;
    if (frame.empty() || !initialized_) return detections;
    auto start = std::chrono::high_resolution_clock::now();
    InferenceMetrics localMetrics;
    if (!metrics) metrics = &localMetrics;

    try {
      auto ppStart = std::chrono::high_resolution_clock::now();
      cv::Mat processed = preprocessImage(frame);
      if (processed.empty()) return detections;
      if (enhanceImage_) processed = enhanceImage(processed);
      auto ppEnd = std::chrono::high_resolution_clock::now();
      metrics->preprocessTimeMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(ppEnd - ppStart).count();

      TensorData td = createInputTensor(processed);
      ORTHelpers::OrtValueGuard inputGuard(td.inputTensor);
      auto infStart = std::chrono::high_resolution_clock::now();
      const OrtApi *api = ORTHelpers::api();
      std::vector<OrtValue *> outputs;
      ORTHelpers::OrtOutputsGuard outputsGuard(outputs);
      runSession({td.inputTensor}, outputs, "split-output detection");
      auto infEnd = std::chrono::high_resolution_clock::now();
      metrics->inferenceTimeMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(infEnd - infStart).count();

      float *scores = nullptr, *boxes = nullptr;
      int64_t queries = 0, classes = 0;
      bool logits = false;
      for (size_t i = 0; i < outputs.size(); ++i) {
        void *raw = nullptr;
        if (!ORTHelpers::checkStatus(api->GetTensorMutableData(outputs[i], &raw), "split output data")) {
          throw std::runtime_error("Could not read split detection output");
        }
        OrtTensorTypeAndShapeInfo *info = nullptr;
        if (!ORTHelpers::checkStatus(api->GetTensorTypeAndShape(outputs[i], &info), "split output shape")) {
          throw std::runtime_error("Could not read split detection shape");
        }
        std::vector<int64_t> shape = ORTHelpers::getTensorShape(info);
        ONNXTensorElementDataType dtype = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
        OrtStatus *typeStatus = api->GetTensorElementType(info, &dtype);
        if (typeStatus) { api->ReleaseStatus(typeStatus); }
        api->ReleaseTensorTypeAndShapeInfo(info);
        if (dtype != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT || shape.size() != 3 || shape[1] <= 0) continue;
        if (shape[2] == 4) {
          boxes = static_cast<float *>(raw);
          queries = shape[1];
        } else if (shape[2] > 4) {
          scores = static_cast<float *>(raw);
          queries = shape[1];
          classes = shape[2];
          std::string name = runner_.outputs()[i].name;
          std::transform(name.begin(), name.end(), name.begin(),
                         [](unsigned char c) { return (char)std::tolower(c); });
          logits = name.find("logit") != std::string::npos;
        }
      }
      if (!scores || !boxes || queries <= 0 || classes <= 0) {
        throw std::runtime_error("Expected [1,N,C] scores/logits and [1,N,4] boxes");
      }

      // HF object-detection exports expose raw independent class logits. For unnamed
      // exports, values outside [0,1] identify the same representation.
      if (!logits) {
        size_t inspect = (size_t)std::min<int64_t>(queries * classes, 1024);
        for (size_t i = 0; i < inspect; ++i) {
          if (scores[i] < 0.0f || scores[i] > 1.0f) { logits = true; break; }
        }
      }
      auto postStart = std::chrono::high_resolution_clock::now();
      for (int64_t q = 0; q < queries; ++q) {
        int bestClass = 0;
        float best = -std::numeric_limits<float>::infinity();
        for (int64_t c = 0; c < classes; ++c) {
          float value = scores[q * classes + c];
          if (value > best) { best = value; bestClass = (int)c; }
        }
        float confidence = logits ? 1.0f / (1.0f + std::exp(-best)) : best;
        if (!std::isfinite(confidence) || confidence < confThreshold) continue;
        float cx = boxes[q * 4 + 0], cy = boxes[q * 4 + 1];
        float bw = boxes[q * 4 + 2], bh = boxes[q * 4 + 3];
        Detection det;
        det.x = cx - bw * 0.5f;
        det.y = cy - bh * 0.5f;
        det.w = bw;
        det.h = bh;
        det.confidence = confidence;
        det.class_id = bestClass;
        det.class_name = className(bestClass, Utils::COCO_CLASSES);
        det.track_id = 0;
        clampDetection(det);
        detections.push_back(det);
      }
      auto postEnd = std::chrono::high_resolution_clock::now();
      metrics->postprocessTimeMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(postEnd - postStart).count();
      metrics->totalTimeMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(postEnd - start).count();
      metrics->inputWidth = frame.cols;
      metrics->inputHeight = frame.rows;
      metrics->detectionCount = detections.size();
    } catch (const std::exception &e) {
      ERROR_MSG("Split-output detection error: %s", e.what());
      return {};
    }
    return detections;
  }

  // ---- RTDETRModel (NMS-free transformer detection) ----

  RTDETRModel::RTDETRModel(const std::string & modelPath, int inputSize) : DetectionModel(modelPath, inputSize) {
    preprocessConfig_.resizeMode = PreprocessConfig::DIRECT_RESIZE;
    preprocessConfig_.normMode = PreprocessConfig::SCALE_01;
    useLetterbox_ = false;
  }

  std::vector<Detection> RTDETRModel::parseOutput(float * /*outputData*/, const std::vector<int64_t> & /*outputShape*/,
                                                   float /*confThreshold*/, const cv::Size & /*originalSize*/) {
    return {};
  }

  std::vector<Detection> RTDETRModel::processRTDETRFrame(const cv::Mat & frame, float confThreshold,
                                                          InferenceMetrics *metrics) {
    std::vector<Detection> detections;
    if (frame.empty() || !initialized_) return detections;

    auto start = std::chrono::high_resolution_clock::now();
    InferenceMetrics localMetrics;
    if (!metrics) metrics = &localMetrics;

    try {
      auto ppStart = std::chrono::high_resolution_clock::now();
      cv::Mat processed = preprocessImage(frame);
      auto ppEnd = std::chrono::high_resolution_clock::now();
      metrics->preprocessTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(ppEnd - ppStart).count();

      TensorData td = createInputTensor(processed);
      ORTHelpers::OrtValueGuard inputGuard(td.inputTensor);

      auto infStart = std::chrono::high_resolution_clock::now();
      const OrtApi *api = ORTHelpers::api();
      std::vector<OrtValue *> outputs;
      ORTHelpers::OrtOutputsGuard outputsGuard(outputs);
      runSession({td.inputTensor}, outputs, "RT-DETR");
      auto infEnd = std::chrono::high_resolution_clock::now();
      metrics->inferenceTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(infEnd - infStart).count();

      auto postStart = std::chrono::high_resolution_clock::now();

      // RT-DETR outputs: labels [1,300], boxes [1,300,4], scores [1,300]
      // Labels may be float or int64 depending on the export
      void *labelsRaw = nullptr;
      float *boxesData = nullptr, *scoresData = nullptr;
      bool labelsAreInt64 = false;
      int64_t numDet = 0;

      for (size_t i = 0; i < outputs.size(); ++i) {
        void *data = nullptr;
        (void)api->GetTensorMutableData(outputs[i], &data);
        OrtTensorTypeAndShapeInfo *info = nullptr;
        (void)api->GetTensorTypeAndShape(outputs[i], &info);
        auto shape = ORTHelpers::getTensorShape(info);

        std::string name = runner_.outputs()[i].name;
        if (name.find("label") != std::string::npos) {
          labelsRaw = data;
          if (shape.size() >= 2) numDet = shape[1];
          ONNXTensorElementDataType elemType = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
          if (info) (void)api->GetTensorElementType(info, &elemType);
          labelsAreInt64 = (elemType == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64);
        } else if (name.find("box") != std::string::npos) {
          boxesData = static_cast<float *>(data);
          if (shape.size() >= 2 && numDet == 0) numDet = shape[1];
        } else if (name.find("score") != std::string::npos) {
          scoresData = static_cast<float *>(data);
        }
        if (info) api->ReleaseTensorTypeAndShapeInfo(info);
      }

      if (!labelsRaw || !boxesData || !scoresData || numDet == 0) {
        ERROR_MSG("RT-DETR: could not find expected output tensors");
        return detections;
      }

      // RT-DETR boxes are cx,cy,w,h in normalized [0,1] coordinates
      for (int64_t i = 0; i < numDet; ++i) {
        float score = scoresData[i];
        if (score < confThreshold) continue;

        int classId = labelsAreInt64 ? static_cast<int>(static_cast<int64_t *>(labelsRaw)[i])
                                     : static_cast<int>(static_cast<float *>(labelsRaw)[i]);
        float cx = boxesData[i * 4 + 0];
        float cy = boxesData[i * 4 + 1];
        float bw = boxesData[i * 4 + 2];
        float bh = boxesData[i * 4 + 3];

        Detection det;
        det.x = cx - bw / 2.0f;
        det.y = cy - bh / 2.0f;
        det.w = bw;
        det.h = bh;
        det.confidence = score;
        det.class_id = classId;
        det.class_name = className(classId, Utils::COCO_CLASSES);
        det.track_id = i;
        clampDetection(det);
        detections.push_back(det);
      }

      auto postEnd = std::chrono::high_resolution_clock::now();
      metrics->postprocessTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(postEnd - postStart).count();

      auto end = std::chrono::high_resolution_clock::now();
      metrics->totalTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
      metrics->detectionCount = detections.size();

    } catch (const std::exception & e) {
      ERROR_MSG("RT-DETR processing error: %s", e.what());
    }
    return detections;
  }

  // ---- DepthEstimationModel ----

  DepthEstimationModel::DepthEstimationModel(const std::string & modelPath, int inputSize)
    : DetectionModel(modelPath, inputSize) {
    preprocessConfig_.resizeMode = PreprocessConfig::DIRECT_RESIZE;
    preprocessConfig_.normMode = PreprocessConfig::IMAGENET;
    useLetterbox_ = false;
  }

  std::vector<Detection> DepthEstimationModel::parseOutput(float * /*outputData*/, const std::vector<int64_t> & /*outputShape*/,
                                                            float /*confThreshold*/, const cv::Size & /*originalSize*/) {
    return {};
  }

  DepthResult DepthEstimationModel::processDepthFrame(const cv::Mat & frame, InferenceMetrics *metrics) {
    DepthResult result;
    if (frame.empty() || !initialized_) return result;

    auto start = std::chrono::high_resolution_clock::now();
    InferenceMetrics localMetrics;
    if (!metrics) metrics = &localMetrics;

    try {
      auto ppStart = std::chrono::high_resolution_clock::now();
      cv::Mat processed = preprocessImage(frame);
      auto ppEnd = std::chrono::high_resolution_clock::now();
      metrics->preprocessTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(ppEnd - ppStart).count();

      TensorData td = createInputTensor(processed);
      ORTHelpers::OrtValueGuard inputGuard(td.inputTensor);

      auto infStart = std::chrono::high_resolution_clock::now();
      const OrtApi *api = ORTHelpers::api();
      std::vector<OrtValue *> outputs;
      ORTHelpers::OrtOutputsGuard outputsGuard(outputs);
      runSession({td.inputTensor}, outputs, "Depth");
      auto infEnd = std::chrono::high_resolution_clock::now();
      metrics->inferenceTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(infEnd - infStart).count();

      auto postStart = std::chrono::high_resolution_clock::now();

      float *outData = nullptr;
      (void)api->GetTensorMutableData(outputs[0], (void **)&outData);
      OrtTensorTypeAndShapeInfo *info = nullptr;
      (void)api->GetTensorTypeAndShape(outputs[0], &info);
      auto shape = ORTHelpers::getTensorShape(info);
      if (info) api->ReleaseTensorTypeAndShapeInfo(info);

      int depthH = 0, depthW = 0;
      if (shape.size() == 3) {
        depthH = (int)shape[1]; depthW = (int)shape[2];
      } else if (shape.size() == 4) {
        depthH = (int)shape[2]; depthW = (int)shape[3];
      }

      if (depthH > 0 && depthW > 0 && outData) {
        cv::Mat rawDepth(depthH, depthW, CV_32FC1, outData);
        // Normalize to [0,1] for display (relative inverse depth)
        double minVal, maxVal;
        cv::minMaxLoc(rawDepth, &minVal, &maxVal);
        if (maxVal > minVal) {
          result.depthMap = (rawDepth - minVal) / (maxVal - minVal);
        } else {
          result.depthMap = rawDepth.clone();
        }
        // Resize to original frame size
        cv::resize(result.depthMap, result.depthMap, frame.size(), 0, 0, cv::INTER_LINEAR);
      }

      auto postEnd = std::chrono::high_resolution_clock::now();
      metrics->postprocessTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(postEnd - postStart).count();

      auto end = std::chrono::high_resolution_clock::now();
      metrics->totalTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
      result.metrics = *metrics;

    } catch (const std::exception & e) {
      ERROR_MSG("Depth processing error: %s", e.what());
    }
    return result;
  }

  // ---- SCRFDModel (multi-scale face detection with landmarks) ----

  SCRFDModel::SCRFDModel(const std::string & modelPath, int inputSize) : DetectionModel(modelPath, inputSize) {
    preprocessConfig_.resizeMode = PreprocessConfig::LETTERBOX;
    preprocessConfig_.normMode = PreprocessConfig::SCRFD_NORM;
  }

  std::vector<Detection> SCRFDModel::parseOutput(float * /*outputData*/, const std::vector<int64_t> & /*outputShape*/,
                                                  float /*confThreshold*/, const cv::Size & /*originalSize*/) {
    return {};
  }

  std::vector<FaceDetection> SCRFDModel::processFaceFrame(const cv::Mat & frame, float confThreshold,
                                                           float nmsThreshold, InferenceMetrics *metrics) {
    std::vector<FaceDetection> detections;
    if (frame.empty() || !initialized_) return detections;

    auto start = std::chrono::high_resolution_clock::now();
    InferenceMetrics localMetrics;
    if (!metrics) metrics = &localMetrics;

    try {
      auto ppStart = std::chrono::high_resolution_clock::now();
      cv::Mat processed = preprocessImage(frame);
      auto ppEnd = std::chrono::high_resolution_clock::now();
      metrics->preprocessTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(ppEnd - ppStart).count();

      TensorData td = createInputTensor(processed);
      ORTHelpers::OrtValueGuard inputGuard(td.inputTensor);

      auto infStart = std::chrono::high_resolution_clock::now();
      const OrtApi *api = ORTHelpers::api();
      std::vector<OrtValue *> outputs;
      ORTHelpers::OrtOutputsGuard outputsGuard(outputs);
      runSession({td.inputTensor}, outputs, "SCRFD");
      auto infEnd = std::chrono::high_resolution_clock::now();
      metrics->inferenceTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(infEnd - infStart).count();

      auto postStart = std::chrono::high_resolution_clock::now();

      // SCRFD outputs at 3 strides (8, 16, 32).
      // For each stride: score tensor [1, num_anchors, 1], bbox tensor [1, num_anchors, 4],
      //   optionally kps tensor [1, num_anchors, 10]
      // Tensors are ordered by name: score_stride, bbox_stride, kps_stride (sorted)
      const int strides[] = {8, 16, 32};
      bool hasKeypoints = (runner_.numOutputs() == 9);

      for (int s = 0; s < 3; ++s) {
        int stride = strides[s];
        int fmapH = inputHeight_ / stride;
        int fmapW = inputWidth_ / stride;
        int numAnchors = fmapH * fmapW * 2; // 2 anchors per location

        // Find tensors for this stride by index: scores at s*3 (or s*2), bbox at s*3+1, kps at s*3+2
        int scoreIdx = hasKeypoints ? s * 3 : s * 2;
        int bboxIdx = scoreIdx + 1;
        int kpsIdx = hasKeypoints ? scoreIdx + 2 : -1;

        if (scoreIdx >= (int)outputs.size() || bboxIdx >= (int)outputs.size()) continue;

        float *scoreData = nullptr, *bboxData = nullptr, *kpsData = nullptr;
        (void)api->GetTensorMutableData(outputs[scoreIdx], (void **)&scoreData);
        (void)api->GetTensorMutableData(outputs[bboxIdx], (void **)&bboxData);
        if (kpsIdx >= 0 && kpsIdx < (int)outputs.size()) {
          (void)api->GetTensorMutableData(outputs[kpsIdx], (void **)&kpsData);
        }

        if (!scoreData || !bboxData) continue;

        for (int anchorIdx = 0; anchorIdx < numAnchors; ++anchorIdx) {
          float score = scoreData[anchorIdx];
          if (score < confThreshold) continue;

          // Decode anchor position
          int anchorPerLoc = 2;
          int locIdx = anchorIdx / anchorPerLoc;
          int ay = locIdx / fmapW;
          int ax = locIdx % fmapW;
          float anchorCx = (ax + 0.5f) * stride;
          float anchorCy = (ay + 0.5f) * stride;

          // Decode bbox: distance from anchor to edges (left, top, right, bottom)
          float dl = bboxData[anchorIdx * 4 + 0] * stride;
          float dt = bboxData[anchorIdx * 4 + 1] * stride;
          float dr = bboxData[anchorIdx * 4 + 2] * stride;
          float db = bboxData[anchorIdx * 4 + 3] * stride;

          float x1 = anchorCx - dl;
          float y1 = anchorCy - dt;
          float x2 = anchorCx + dr;
          float y2 = anchorCy + db;

          FaceDetection det;
          det.x = x1 / inputWidth_;
          det.y = y1 / inputHeight_;
          det.w = (x2 - x1) / inputWidth_;
          det.h = (y2 - y1) / inputHeight_;
          det.confidence = score;
          det.class_id = 0;
          det.class_name = "face";
          det.track_id = detections.size();
          memset(det.landmarks, 0, sizeof(det.landmarks));

          if (kpsData) {
            for (int k = 0; k < 5; ++k) {
              float kpx = (kpsData[anchorIdx * 10 + k * 2] * stride + anchorCx) / inputWidth_;
              float kpy = (kpsData[anchorIdx * 10 + k * 2 + 1] * stride + anchorCy) / inputHeight_;
              det.landmarks[k * 2] = kpx;
              det.landmarks[k * 2 + 1] = kpy;
            }
          }

          remapLetterboxCoords(det);
          clampDetection(det);
          // Remap landmarks through letterbox
          if (useLetterbox_ && letterboxScale_ > 0.0f) {
            for (int k = 0; k < 5; ++k) {
              det.landmarks[k * 2] = (det.landmarks[k * 2] * inputWidth_ - letterboxPadX_) /
                                       (inputWidth_ - 2 * letterboxPadX_);
              det.landmarks[k * 2 + 1] = (det.landmarks[k * 2 + 1] * inputHeight_ - letterboxPadY_) /
                                            (inputHeight_ - 2 * letterboxPadY_);
              det.landmarks[k * 2] = std::max(0.0f, std::min(1.0f, det.landmarks[k * 2]));
              det.landmarks[k * 2 + 1] = std::max(0.0f, std::min(1.0f, det.landmarks[k * 2 + 1]));
            }
          }

          detections.push_back(det);
        }
      }

      // Apply NMS
      if (nmsThreshold > 0.0f && detections.size() > 1) {
        std::vector<Detection> baseDets(detections.begin(), detections.end());
        auto nmsResult = applyNMS(baseDets, nmsThreshold);
        std::vector<FaceDetection> filtered;
        for (const auto & d : nmsResult) {
          for (const auto & fd : detections) {
            if (std::abs(fd.x - d.x) < 1e-5f && std::abs(fd.y - d.y) < 1e-5f) {
              filtered.push_back(fd);
              break;
            }
          }
        }
        detections = filtered;
      }

      auto postEnd = std::chrono::high_resolution_clock::now();
      metrics->postprocessTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(postEnd - postStart).count();
      auto end = std::chrono::high_resolution_clock::now();
      metrics->totalTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
      metrics->detectionCount = detections.size();

    } catch (const std::exception & e) {
      ERROR_MSG("SCRFD processing error: %s", e.what());
    }
    return detections;
  }

  // ---- ArcFaceModel (face recognition embedding) ----

  EmbeddingModel::EmbeddingModel(const std::string & modelPath, int inputSize) : DetectionModel(modelPath, inputSize) {
    preprocessConfig_.resizeMode = PreprocessConfig::DIRECT_RESIZE;
    preprocessConfig_.normMode = PreprocessConfig::SCALE_01;
    useLetterbox_ = false;
  }

  ArcFaceModel::ArcFaceModel(const std::string & modelPath, int inputSize) : EmbeddingModel(modelPath, inputSize) {
    preprocessConfig_.normMode = PreprocessConfig::SCRFD_NORM;
  }

  OCRModel::OCRModel(const std::string & detPath, const std::string & recPath, const std::string & dictPath)
    : DetectionModel(detPath, 960), recPath_(recPath), dictPath_(dictPath) {
    // Detection input is dynamic; the base uses these only to size a default input.
    preprocessConfig_.resizeMode = PreprocessConfig::DIRECT_RESIZE;
    preprocessConfig_.normMode = PreprocessConfig::IMAGENET;
    useLetterbox_ = false;
  }

  std::vector<Detection> OCRModel::parseOutput(float * /*outputData*/, const std::vector<int64_t> & /*outputShape*/,
                                                float /*confThreshold*/, const cv::Size & /*originalSize*/) {
    return {};
  }

  bool OCRModel::initializeRec(int numThreads, const std::string & requestedEP, bool lowLatency,
                               std::string & err) {
    if (!recRunner_.load(recPath_, numThreads, requestedEP, err, lowLatency)) { return false; }
    // Charset: PP-OCR dict.txt is one token per line; index 0 is the CTC blank and the
    // trailing space token (index dict+1) is implicit, matching the model's class count.
    std::vector<std::string> dict = Utils::loadLabelsFile(dictPath_);
    if (dict.empty()) {
      err = "OCR charset (dict) is empty or unreadable: " + dictPath_;
      return false;
    }
    charset_.clear();
    charset_.push_back(""); // CTC blank at index 0
    for (const std::string & tok : dict) { charset_.push_back(tok); }
    charset_.push_back(" "); // PP-OCR appends a space class after the dict
    recReady_ = true;
    INFO_MSG("OCR recognition ready: %s (%zu symbols incl. blank)", recPath_.c_str(), charset_.size());
    return true;
  }

  std::vector<cv::RotatedRect> OCRModel::extractBoxes(const cv::Mat & probMap, float binThresh) {
    std::vector<cv::RotatedRect> boxes;
    cv::Mat bin;
    cv::threshold(probMap, bin, binThresh, 255.0, cv::THRESH_BINARY);
    bin.convertTo(bin, CV_8UC1);
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(bin, contours, cv::RETR_LIST, cv::CHAIN_APPROX_SIMPLE);
    for (const auto & contour : contours) {
      if (contour.size() < 4) { continue; }
      double area = cv::contourArea(contour);
      if (area < 16.0) { continue; } // drop specks
      cv::RotatedRect box = cv::minAreaRect(contour);
      // DB unclip: expand the box outward by (area * ratio / perimeter). The detector is
      // trained on shrunk text regions, so recognition needs the box grown back out.
      double perim = cv::arcLength(contour, true);
      if (perim < 1.0) { continue; }
      float dist = (float)(area * 1.6 / perim);
      box.size.width += 2.0f * dist;
      box.size.height += 2.0f * dist;
      boxes.push_back(box);
    }
    return boxes;
  }

  std::string OCRModel::recognizeLine(const cv::Mat & lineImg, float & conf) {
    conf = 0.0f;
    if (lineImg.empty() || !recReady_) { return ""; }
    // PP-OCR rec input: 3x48xW, width scaled to preserve aspect (min 16). Normalize
    // (x/255 - 0.5) / 0.5 per the PaddleOCR rec preprocessing.
    const int targetH = 48;
    int targetW = (int)std::lround((double)targetH * lineImg.cols / std::max(1, lineImg.rows));
    if (targetW < 16) { targetW = 16; }
    cv::Mat resized;
    cv::resize(lineImg, resized, cv::Size(targetW, targetH));
    cv::Mat rgb;
    cv::cvtColor(resized, rgb, cv::COLOR_BGR2RGB);
    rgb.convertTo(rgb, CV_32F);
    rgb = (rgb / 255.0f - 0.5f) / 0.5f;

    std::vector<float> chw((size_t)3 * targetH * targetW);
    std::vector<cv::Mat> ch(3);
    cv::split(rgb, ch);
    for (int c = 0; c < 3; ++c) {
      std::memcpy(chw.data() + (size_t)c * targetH * targetW, ch[c].data,
                  (size_t)targetH * targetW * sizeof(float));
    }
    std::vector<int64_t> shape = {1, 3, targetH, targetW};
    std::string err;
    OrtValue *in = recRunner_.createFloatTensor(chw.data(), chw.size(), shape, err);
    if (!in) { return ""; }
    ORTHelpers::OrtValueGuard inGuard(in);
    std::vector<OrtValue *> outputs;
    ORTHelpers::OrtOutputsGuard outGuard(outputs);
    if (!recRunner_.run({in}, outputs, err)) { return ""; }

    const OrtApi *api = ORTHelpers::api();
    float *data = nullptr;
    if (api->GetTensorMutableData(outputs[0], (void **)&data) != nullptr || !data) { return ""; }
    OrtTensorTypeAndShapeInfo *info = nullptr;
    if (api->GetTensorTypeAndShape(outputs[0], &info) != nullptr) { return ""; }
    auto oshape = ORTHelpers::getTensorShape(info);
    api->ReleaseTensorTypeAndShapeInfo(info);
    if (oshape.size() != 3) { return ""; } // [1, T, num_classes]
    size_t T = (size_t)oshape[1];
    size_t numClasses = (size_t)oshape[2];
    std::string text;
    Utils::ctcGreedyDecode(data, T, numClasses, charset_, text, conf);
    return text;
  }

  OCRResult OCRModel::processOCRFrame(const cv::Mat & frame, float confThreshold, InferenceMetrics *metrics) {
    OCRResult result;
    if (frame.empty() || !initialized_ || !recReady_) { return result; }
    InferenceMetrics local;
    if (!metrics) { metrics = &local; }
    auto start = std::chrono::high_resolution_clock::now();

    try {
      // Detection preprocessing: scale so the long side <= 960, both dims rounded to a
      // multiple of 32 (the model's stride), IMAGENET normalization; input dims vary per frame.
      const int maxSide = 960;
      float scale = std::min(1.0f, (float)maxSide / std::max(frame.cols, frame.rows));
      int rw = std::max(32, (int)std::lround(frame.cols * scale / 32.0) * 32);
      int rh = std::max(32, (int)std::lround(frame.rows * scale / 32.0) * 32);
      cv::Mat resized;
      cv::resize(frame, resized, cv::Size(rw, rh));
      cv::Mat rgb;
      cv::cvtColor(resized, rgb, cv::COLOR_BGR2RGB);
      rgb.convertTo(rgb, CV_32F);
      rgb /= 255.0f;
      {
        std::vector<cv::Mat> c(3);
        cv::split(rgb, c);
        for (int i = 0; i < 3; ++i) { c[i] = (c[i] - preprocessConfig_.mean[i]) / preprocessConfig_.std[i]; }
        cv::merge(c, rgb);
      }
      std::vector<float> chw((size_t)3 * rh * rw);
      std::vector<cv::Mat> ch(3);
      cv::split(rgb, ch);
      for (int i = 0; i < 3; ++i) {
        std::memcpy(chw.data() + (size_t)i * rh * rw, ch[i].data, (size_t)rh * rw * sizeof(float));
      }
      auto ppEnd = std::chrono::high_resolution_clock::now();
      metrics->preprocessTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(ppEnd - start).count();

      std::vector<int64_t> shape = {1, 3, rh, rw};
      std::string err;
      OrtValue *in = runner_.createFloatTensor(chw.data(), chw.size(), shape, err);
      if (!in) { return result; }
      ORTHelpers::OrtValueGuard inGuard(in);
      std::vector<OrtValue *> outputs;
      ORTHelpers::OrtOutputsGuard outGuard(outputs);
      if (!runner_.run({in}, outputs, err)) {
        ERROR_MSG("OCR detection failed: %s", err.c_str());
        return result;
      }
      auto infEnd = std::chrono::high_resolution_clock::now();
      metrics->inferenceTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(infEnd - ppEnd).count();

      const OrtApi *api = ORTHelpers::api();
      float *prob = nullptr;
      if (api->GetTensorMutableData(outputs[0], (void **)&prob) != nullptr || !prob) { return result; }
      OrtTensorTypeAndShapeInfo *info = nullptr;
      if (api->GetTensorTypeAndShape(outputs[0], &info) != nullptr) { return result; }
      auto oshape = ORTHelpers::getTensorShape(info);
      api->ReleaseTensorTypeAndShapeInfo(info);
      // Output is [1,1,H,W] probability map
      if (oshape.size() != 4) { return result; }
      int ph = (int)oshape[2], pw = (int)oshape[3];
      cv::Mat probMap(ph, pw, CV_32F, prob);

      std::vector<cv::RotatedRect> boxes = extractBoxes(probMap, 0.3f);
      for (const cv::RotatedRect & box : boxes) {
        // Map the box from prob-map space to the ORIGINAL frame and warp the line upright
        float sx = (float)frame.cols / pw;
        float sy = (float)frame.rows / ph;
        cv::Point2f pts[4];
        box.points(pts);
        for (int i = 0; i < 4; ++i) { pts[i].x *= sx; pts[i].y *= sy; }
        float bw = box.size.width * sx;
        float bh = box.size.height * sy;
        if (bw < 4 || bh < 4) { continue; }
        float wRect = std::max(bw, bh), hRect = std::min(bw, bh); // text lines are wider than tall
        cv::Point2f dst[4] = {{0, 0}, {wRect, 0}, {wRect, hRect}, {0, hRect}};
        // Assign corners TL,TR,BR,BL by sum/diff (robust for any tilt, unlike an angular
        // sort which rotates the assignment once an elongated box tilts past atan(bh/bw)):
        // TL has the smallest x+y, BR the largest; TR the largest x-y, BL the smallest.
        cv::Point2f src[4];
        {
          int tl = 0, br = 0, tr = 0, bl = 0;
          for (int i = 1; i < 4; ++i) {
            if (pts[i].x + pts[i].y < pts[tl].x + pts[tl].y) { tl = i; }
            if (pts[i].x + pts[i].y > pts[br].x + pts[br].y) { br = i; }
            if (pts[i].x - pts[i].y > pts[tr].x - pts[tr].y) { tr = i; }
            if (pts[i].x - pts[i].y < pts[bl].x - pts[bl].y) { bl = i; }
          }
          src[0] = pts[tl];
          src[1] = pts[tr];
          src[2] = pts[br];
          src[3] = pts[bl];
        }
        cv::Mat M = cv::getPerspectiveTransform(src, dst);
        cv::Mat lineImg;
        cv::warpPerspective(frame, lineImg, M, cv::Size((int)wRect, (int)hRect));
        if (lineImg.empty()) { continue; }

        float conf = 0.0f;
        std::string text = recognizeLine(lineImg, conf);
        if (text.empty() || conf < confThreshold) { continue; }
        OCRLine line;
        line.text = text;
        line.confidence = conf;
        cv::Rect2f br = box.boundingRect2f();
        line.x = (br.x * sx) / frame.cols;
        line.y = (br.y * sy) / frame.rows;
        line.w = (br.width * sx) / frame.cols;
        line.h = (br.height * sy) / frame.rows;
        result.lines.push_back(line);
      }

      // Reading order: top-to-bottom, then left-to-right. Quantize y into line bands
      // BEFORE comparing so the ordering is transitive (a raw |a.y-b.y|<band tolerance
      // is intransitive → UB in std::sort).
      std::sort(result.lines.begin(), result.lines.end(), [](const OCRLine & a, const OCRLine & b) {
        int ba = (int)(a.y / 0.02f), bb = (int)(b.y / 0.02f);
        if (ba != bb) { return ba < bb; }
        return a.x < b.x;
      });
      for (size_t i = 0; i < result.lines.size(); ++i) {
        if (i) { result.text += "\n"; }
        result.text += result.lines[i].text;
      }
      result.ok = true;
      auto end = std::chrono::high_resolution_clock::now();
      metrics->postprocessTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(end - infEnd).count();
      metrics->totalTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
      result.metrics = *metrics;
    } catch (const cv::Exception & e) {
      ERROR_MSG("OCR processing error: %s", e.what());
    }
    return result;
  }

  std::vector<Detection> EmbeddingModel::parseOutput(float * /*outputData*/, const std::vector<int64_t> & /*outputShape*/,
                                                      float /*confThreshold*/, const cv::Size & /*originalSize*/) {
    return {};
  }

  bool EmbeddingModel::loadMatchSet(const std::string & path) {
    matchLabels_.clear();
    matchEmbeds_.clear();
    if (access(path.c_str(), R_OK) != 0) { return false; }
    JSON::Value j = JSON::fromFile(path);
    if (!j.isMember("labels") || !j.isMember("embeddings")) { return false; }
    JSON::Value & labels = j["labels"];
    JSON::Value & embeds = j["embeddings"];
    if (labels.size() != embeds.size() || labels.size() == 0) { return false; }
    for (unsigned i = 0; i < labels.size(); ++i) {
      std::vector<float> row;
      row.reserve(embeds[i].size());
      double norm = 0.0;
      for (unsigned k = 0; k < embeds[i].size(); ++k) {
        float v = (float)embeds[i][k].asDouble();
        row.push_back(v);
        norm += (double)v * v;
      }
      // Normalize defensively so cosine == dot even if the asset wasn't pre-normalized
      norm = std::sqrt(norm);
      if (norm > 1e-10) {
        for (float & v : row) { v = (float)(v / norm); }
      }
      matchLabels_.push_back(labels[i].asString());
      matchEmbeds_.push_back(std::move(row));
    }
    INFO_MSG("Zero-shot match set: %zu labels", matchLabels_.size());
    return true;
  }

  ClassificationResult EmbeddingModel::matchEmbedding(const std::vector<float> & embedding, unsigned topK) const {
    ClassificationResult result;
    result.class_id = -1;
    result.class_name = "unknown";
    result.confidence = 0.0f;
    if (matchEmbeds_.empty() || embedding.empty()) { return result; }
    // Dimension mismatch (wrong text tower / stale asset) makes every cosine 0; report
    // it as no-match rather than silently naming the first label at confidence 0.
    if (matchEmbeds_[0].size() != embedding.size()) {
      static bool warned = false;
      if (!warned) {
        WARN_MSG("Zero-shot match set dim %zu != image embedding dim %zu; ignoring tags. "
                 "Regenerate text_embeddings.json from the matching text tower.",
                 matchEmbeds_[0].size(), embedding.size());
        warned = true;
      }
      return result;
    }
    // embedding is already L2-normalized by processEmbeddingFrame, match rows by load
    std::vector<float> sims(matchEmbeds_.size(), 0.0f);
    for (size_t i = 0; i < matchEmbeds_.size(); ++i) {
      if (matchEmbeds_[i].size() != embedding.size()) { continue; }
      float dot = 0.0f;
      for (size_t k = 0; k < embedding.size(); ++k) { dot += embedding[k] * matchEmbeds_[i][k]; }
      sims[i] = dot;
    }
    unsigned k = topK ? topK : 1;
    if (k > sims.size()) { k = (unsigned)sims.size(); }
    std::vector<int> order(sims.size());
    std::iota(order.begin(), order.end(), 0);
    std::partial_sort(order.begin(), order.begin() + k, order.end(),
                      [&sims](int a, int b) { return sims[a] > sims[b]; });
    result.class_id = order[0];
    result.class_name = matchLabels_[order[0]];
    result.confidence = sims[order[0]];
    for (unsigned i = 0; i < k; ++i) {
      ClassScore cs;
      cs.class_id = order[i];
      cs.class_name = matchLabels_[order[i]];
      cs.confidence = sims[order[i]];
      result.top.push_back(cs);
    }
    return result;
  }

  FaceEmbedding EmbeddingModel::processEmbeddingFrame(const cv::Mat & alignedFace, InferenceMetrics *metrics) {
    FaceEmbedding result;
    result.confidence = 0.0f;
    if (alignedFace.empty() || !initialized_) return result;

    auto start = std::chrono::high_resolution_clock::now();
    InferenceMetrics localMetrics;
    if (!metrics) metrics = &localMetrics;

    try {
      auto ppStart = std::chrono::high_resolution_clock::now();
      cv::Mat processed = preprocessImage(alignedFace);
      auto ppEnd = std::chrono::high_resolution_clock::now();
      metrics->preprocessTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(ppEnd - ppStart).count();

      TensorData td = createInputTensor(processed);
      ORTHelpers::OrtValueGuard inputGuard(td.inputTensor);

      auto infStart = std::chrono::high_resolution_clock::now();
      const OrtApi *api = ORTHelpers::api();
      std::vector<OrtValue *> outputs;
      ORTHelpers::OrtOutputsGuard outputsGuard(outputs);
      runSession({td.inputTensor}, outputs, "Embedding");
      auto infEnd = std::chrono::high_resolution_clock::now();
      metrics->inferenceTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(infEnd - infStart).count();

      auto postStart = std::chrono::high_resolution_clock::now();

      float *outData = nullptr;
      (void)api->GetTensorMutableData(outputs[0], (void **)&outData);
      OrtTensorTypeAndShapeInfo *info = nullptr;
      (void)api->GetTensorTypeAndShape(outputs[0], &info);
      auto shape = ORTHelpers::getTensorShape(info);
      if (info) api->ReleaseTensorTypeAndShapeInfo(info);

      int embeddingDim = (shape.size() == 2) ? (int)shape[1] : 512;
      result.embedding.resize(embeddingDim);
      std::memcpy(result.embedding.data(), outData, embeddingDim * sizeof(float));

      // L2-normalize the embedding
      float norm = 0.0f;
      for (int i = 0; i < embeddingDim; ++i) norm += result.embedding[i] * result.embedding[i];
      norm = std::sqrt(norm);
      if (norm > 1e-10f) {
        for (int i = 0; i < embeddingDim; ++i) result.embedding[i] /= norm;
      }
      result.confidence = 1.0f;

      auto postEnd = std::chrono::high_resolution_clock::now();
      metrics->postprocessTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(postEnd - postStart).count();
      auto end = std::chrono::high_resolution_clock::now();
      metrics->totalTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
      result.metrics = *metrics;

    } catch (const std::exception & e) {
      ERROR_MSG("Embedding processing error: %s", e.what());
    }
    return result;
  }

  // ---- RTMOModel (one-stage multi-person pose with SimCC) ----

  RTMOModel::RTMOModel(const std::string & modelPath, int inputSize) : DetectionModel(modelPath, inputSize) {
    preprocessConfig_.resizeMode = PreprocessConfig::LETTERBOX;
    preprocessConfig_.normMode = PreprocessConfig::SCALE_01;
  }

  std::vector<Detection> RTMOModel::parseOutput(float * /*outputData*/, const std::vector<int64_t> & /*outputShape*/,
                                                 float /*confThreshold*/, const cv::Size & /*originalSize*/) {
    return {};
  }

  std::vector<PoseDetection> RTMOModel::processRTMOFrame(const cv::Mat & frame, float confThreshold,
                                                          InferenceMetrics *metrics) {
    std::vector<PoseDetection> detections;
    if (frame.empty() || !initialized_) return detections;

    auto start = std::chrono::high_resolution_clock::now();
    InferenceMetrics localMetrics;
    if (!metrics) metrics = &localMetrics;

    try {
      auto ppStart = std::chrono::high_resolution_clock::now();
      cv::Mat processed = preprocessImage(frame);
      auto ppEnd = std::chrono::high_resolution_clock::now();
      metrics->preprocessTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(ppEnd - ppStart).count();

      TensorData td = createInputTensor(processed);
      ORTHelpers::OrtValueGuard inputGuard(td.inputTensor);

      auto infStart = std::chrono::high_resolution_clock::now();
      const OrtApi *api = ORTHelpers::api();
      std::vector<OrtValue *> outputs;
      ORTHelpers::OrtOutputsGuard outputsGuard(outputs);
      runSession({td.inputTensor}, outputs, "RTMO");
      auto infEnd = std::chrono::high_resolution_clock::now();
      metrics->inferenceTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(infEnd - infStart).count();

      auto postStart = std::chrono::high_resolution_clock::now();

      // RTMO outputs: dets [1, N, 5] (x1,y1,x2,y2,score) + keypoints [1, N, K, 3] (x,y,score)
      // or SimCC format: simcc_x [1, N, K, Wx], simcc_y [1, N, K, Wy]
      // Detect format from output shapes
      if (outputs.size() >= 2) {
        float *detsData = nullptr, *kpsData = nullptr;
        std::vector<int64_t> detsShape, kpsShape;

        for (size_t i = 0; i < outputs.size(); ++i) {
          float *data = nullptr;
          (void)api->GetTensorMutableData(outputs[i], (void **)&data);
          OrtTensorTypeAndShapeInfo *tinfo = nullptr;
          (void)api->GetTensorTypeAndShape(outputs[i], &tinfo);
          auto s = ORTHelpers::getTensorShape(tinfo);
          if (tinfo) api->ReleaseTensorTypeAndShapeInfo(tinfo);

          // Detection tensor: shape [1, N, 5] or [1, N, 6]
          if (s.size() == 3 && (s[2] == 5 || s[2] == 6)) {
            detsData = data;
            detsShape = s;
          }
          // Keypoints tensor: shape [1, N, K, 3]
          if (s.size() == 4 && s[3] == 3) {
            kpsData = data;
            kpsShape = s;
          }
        }

        if (detsData && detsShape.size() == 3) {
          int64_t numPeople = detsShape[1];
          int64_t detCols = detsShape[2];
          int numKeypoints = (kpsData && kpsShape.size() == 4) ? (int)kpsShape[2] : 0;

          for (int64_t p = 0; p < numPeople; ++p) {
            float *row = detsData + p * detCols;
            float score = row[4];
            if (score < confThreshold) continue;

            float x1 = row[0] / inputWidth_;
            float y1 = row[1] / inputHeight_;
            float x2 = row[2] / inputWidth_;
            float y2 = row[3] / inputHeight_;

            PoseDetection pd;
            pd.x = x1;
            pd.y = y1;
            pd.w = x2 - x1;
            pd.h = y2 - y1;
            pd.confidence = score;
            pd.pose_confidence = score;
            pd.class_id = 0;
            pd.class_name = "person";
            pd.track_id = detections.size();

            if (kpsData && numKeypoints > 0) {
              int stride = numKeypoints * 3;
              for (int k = 0; k < numKeypoints && k < 17; ++k) {
                float *kp = kpsData + p * stride + k * 3;
                Keypoint keypoint;
                keypoint.x = kp[0] / inputWidth_;
                keypoint.y = kp[1] / inputHeight_;
                keypoint.confidence = kp[2];
                keypoint.visible = (kp[2] > 0.3f);
                pd.keypoints.push_back(keypoint);
              }
            }

            remapLetterboxCoords(pd);
            clampDetection(pd);
            if (useLetterbox_ && letterboxScale_ > 0.0f) {
              for (auto & kp : pd.keypoints) {
                float rawX = kp.x * inputWidth_;
                float rawY = kp.y * inputHeight_;
                kp.x = (rawX - letterboxPadX_) / (inputWidth_ - 2 * letterboxPadX_);
                kp.y = (rawY - letterboxPadY_) / (inputHeight_ - 2 * letterboxPadY_);
                kp.x = std::max(0.0f, std::min(1.0f, kp.x));
                kp.y = std::max(0.0f, std::min(1.0f, kp.y));
              }
            }

            detections.push_back(pd);
          }
        }
      }

      auto postEnd = std::chrono::high_resolution_clock::now();
      metrics->postprocessTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(postEnd - postStart).count();
      auto end = std::chrono::high_resolution_clock::now();
      metrics->totalTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
      metrics->detectionCount = detections.size();

    } catch (const std::exception & e) {
      ERROR_MSG("RTMO processing error: %s", e.what());
    }
    return detections;
  }

  // ---- SAM2EncoderModel (image embedding for promptable segmentation) ----

  SAM2EncoderModel::SAM2EncoderModel(const std::string & modelPath, int inputSize) : DetectionModel(modelPath, inputSize) {
    preprocessConfig_.resizeMode = PreprocessConfig::DIRECT_RESIZE;
    preprocessConfig_.normMode = PreprocessConfig::IMAGENET;
    useLetterbox_ = false;
  }

  std::vector<Detection> SAM2EncoderModel::parseOutput(float * /*outputData*/, const std::vector<int64_t> & /*outputShape*/,
                                                        float /*confThreshold*/, const cv::Size & /*originalSize*/) {
    return {};
  }

  std::vector<cv::Mat> SAM2EncoderModel::encodeImage(const cv::Mat & frame, InferenceMetrics *metrics) {
    std::vector<cv::Mat> embeddings;
    if (frame.empty() || !initialized_) return embeddings;

    auto start = std::chrono::high_resolution_clock::now();
    InferenceMetrics localMetrics;
    if (!metrics) metrics = &localMetrics;

    try {
      auto ppStart = std::chrono::high_resolution_clock::now();
      cv::Mat processed = preprocessImage(frame);
      auto ppEnd = std::chrono::high_resolution_clock::now();
      metrics->preprocessTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(ppEnd - ppStart).count();

      TensorData td = createInputTensor(processed);
      ORTHelpers::OrtValueGuard inputGuard(td.inputTensor);

      auto infStart = std::chrono::high_resolution_clock::now();
      const OrtApi *api = ORTHelpers::api();
      std::vector<OrtValue *> outputs;
      ORTHelpers::OrtOutputsGuard outputsGuard(outputs);
      runSession({td.inputTensor}, outputs, "SAM2 Encoder");
      auto infEnd = std::chrono::high_resolution_clock::now();
      metrics->inferenceTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(infEnd - infStart).count();

      // Pack each output tensor as a cv::Mat for downstream decoder use
      for (size_t i = 0; i < outputs.size(); ++i) {
        float *data = nullptr;
        (void)api->GetTensorMutableData(outputs[i], (void **)&data);
        OrtTensorTypeAndShapeInfo *info = nullptr;
        (void)api->GetTensorTypeAndShape(outputs[i], &info);
        auto shape = ORTHelpers::getTensorShape(info);
        if (info) api->ReleaseTensorTypeAndShapeInfo(info);

        if (!data || shape.empty()) continue;

        size_t totalElements = 1;
        for (auto d : shape) totalElements *= (d > 0 ? d : 1);

        // Store as 1D float Mat (consumer interprets shape from modelInfo_)
        cv::Mat tensor(1, (int)totalElements, CV_32FC1);
        std::memcpy(tensor.data, data, totalElements * sizeof(float));
        embeddings.push_back(tensor);
      }

      auto end = std::chrono::high_resolution_clock::now();
      metrics->totalTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    } catch (const std::exception & e) {
      ERROR_MSG("SAM2 Encoder error: %s", e.what());
    }
    return embeddings;
  }

  // ---- SAM2DecoderModel (mask decoder with point/box prompts) ----

  SAM2DecoderModel::SAM2DecoderModel(const std::string & modelPath, int inputSize) : DetectionModel(modelPath, inputSize) {
    preprocessConfig_.resizeMode = PreprocessConfig::DIRECT_RESIZE;
    preprocessConfig_.normMode = PreprocessConfig::SCALE_01;
    useLetterbox_ = false;
  }

  std::vector<Detection> SAM2DecoderModel::parseOutput(float * /*outputData*/, const std::vector<int64_t> & /*outputShape*/,
                                                        float /*confThreshold*/, const cv::Size & /*originalSize*/) {
    return {};
  }

  SAM2Result SAM2DecoderModel::decodeMasks(const std::vector<cv::Mat> & imageEmbeddings,
                                            const std::vector<cv::Point2f> & pointPrompts,
                                            const std::vector<int> & pointLabels,
                                            const cv::Size & originalSize,
                                            InferenceMetrics *metrics) {
    SAM2Result result;
    if (!initialized_ || imageEmbeddings.empty()) return result;

    auto start = std::chrono::high_resolution_clock::now();
    InferenceMetrics localMetrics;
    if (!metrics) metrics = &localMetrics;

    try {
      const OrtApi *api = ORTHelpers::api();

      // SAM2 decoder takes multiple inputs: image_embed, point_coords, point_labels, mask_input, has_mask_input
      // Build input tensors manually since this model has non-standard inputs
      const std::vector<TensorSpec> &inSpecs = runner_.inputs();
      size_t numInputs = inSpecs.size();
      std::vector<OrtValue *> inputTensors(numInputs, nullptr);

      // Backing buffers must outlive the ORT tensors that reference them (until after Run)
      std::vector<std::vector<float>> inputBuffers(numInputs);

      for (size_t i = 0; i < numInputs && i < imageEmbeddings.size(); ++i) {
        const std::vector<int64_t> &shape = inSpecs[i].shape;
        size_t numElements = 1;
        for (auto d : shape) numElements *= (d > 0 ? d : 1);

        const cv::Mat & emb = imageEmbeddings[i];
        size_t embElements = emb.total();
        inputBuffers[i].resize(numElements, 0.0f);

        if (embElements == numElements) {
          std::memcpy(inputBuffers[i].data(), emb.ptr<float>(), numElements * sizeof(float));
        }

        std::string tErr;
        inputTensors[i] = runner_.createFloatTensor(inputBuffers[i].data(), inputBuffers[i].size(), shape, tErr);
      }

      // Run decoder
      auto infStart = std::chrono::high_resolution_clock::now();
      std::vector<OrtValue *> outputs;
      ORTHelpers::OrtOutputsGuard outputsGuard(outputs);

      std::string runErr;
      bool runOk = runner_.run(inputTensors, outputs, runErr);

      // Clean up input tensors
      for (auto *t : inputTensors) { if (t) api->ReleaseValue(t); }

      if (!runOk) {
        throw std::runtime_error("SAM2 Decoder Run failed: " + runErr);
      }
      auto infEnd = std::chrono::high_resolution_clock::now();
      metrics->inferenceTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(infEnd - infStart).count();

      // Parse masks and IoU scores
      // Output 0: masks [1, num_masks, H, W], Output 1: iou_scores [1, num_masks]
      for (size_t i = 0; i < outputs.size(); ++i) {
        float *data = nullptr;
        (void)api->GetTensorMutableData(outputs[i], (void **)&data);
        OrtTensorTypeAndShapeInfo *info = nullptr;
        (void)api->GetTensorTypeAndShape(outputs[i], &info);
        auto shape = ORTHelpers::getTensorShape(info);
        if (info) api->ReleaseTensorTypeAndShapeInfo(info);

        if (!data) continue;

        std::string name = runner_.outputs()[i].name;
        if (name.find("mask") != std::string::npos && shape.size() == 4) {
          int numMasks = (int)shape[1];
          int maskH = (int)shape[2];
          int maskW = (int)shape[3];
          for (int m = 0; m < numMasks; ++m) {
            cv::Mat maskLogits(maskH, maskW, CV_32FC1, data + m * maskH * maskW);
            cv::Mat mask;
            cv::threshold(maskLogits, mask, 0.0f, 1.0f, cv::THRESH_BINARY);
            mask.convertTo(mask, CV_8UC1, 255.0);
            cv::resize(mask, mask, originalSize, 0, 0, cv::INTER_LINEAR);
            result.masks.push_back(mask);
          }
        } else if (name.find("iou") != std::string::npos && shape.size() >= 2) {
          int numScores = (int)shape[1];
          for (int s = 0; s < numScores; ++s) {
            result.iouScores.push_back(data[s]);
          }
        }
      }

      auto end = std::chrono::high_resolution_clock::now();
      metrics->totalTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
      result.metrics = *metrics;

    } catch (const std::exception & e) {
      ERROR_MSG("SAM2 Decoder error: %s", e.what());
    }
    return result;
  }

  GenericModel::GenericModel(const std::string & modelPath, int inputSize) : DetectionModel(modelPath, inputSize) {}

  std::vector<Detection> GenericModel::parseOutput(float *outputData, const std::vector<int64_t> & outputShape,
                                                   float confThreshold, const cv::Size & originalSize) {
    // Generic models don't produce detections in the traditional sense
    // Return empty vector - use processFrameGeneric instead
    return {};
  }

  // ---- ASRModel (Parakeet TDT speech-to-text) ----
  //
  // Faithful C++ port of onnx-asr's NeMo Conformer TDT path (encoder + fused
  // decoder/joint, greedy TDT decode). Contract mirrors onnx-asr exactly:
  //   preproc  waveforms[1,N] f32, waveforms_lens[1] i64 -> features[1,128,T], features_lens[1] i64
  //   encoder  audio_signal[1,128,T], length[1] i64      -> outputs[1,D,T'], encoded_lengths[1] i64
  //   decoder  encoder_outputs[1,D,1], targets[1,1], target_length[1], input_states_1/2
  //            -> outputs[V+durations], output_states_1/2
  // Encoder 'outputs' is [1,D,T']; frame t is the column outputs[:, t] (onnx-asr transposes
  // to [T',D] then indexes t). TDT split: token=argmax(out[:V]); step=argmax(out[V:]).
  namespace {
    // NeMo v3: 10 ms window, subsampling 8 -> one encoder frame == 80 ms.
    const uint64_t ASR_FRAME_MS = 80;
    const int ASR_MAX_TOKENS_PER_STEP = 10; // onnx-asr default

    // U+2581 (SentencePiece word-boundary marker) as UTF-8.
    const char *SP_SPACE = "\xE2\x96\x81";

    // Read a float / float16 output tensor into out (always owned by the caller). FLOAT16 is
    // converted; any other element type is reinterpreted as float32 (best-effort — the ASR
    // boundary tensors are only ever fp32 or fp16). Copies rather than aliasing ORT memory so
    // the same code path serves fp32 and fp16 boundaries.
    bool tensorFloat(OrtValue *v, std::vector<float> &out, std::vector<int64_t> &shape) {
      const OrtApi *api = ORTHelpers::api();
      OrtTensorTypeAndShapeInfo *info = nullptr;
      if (api->GetTensorTypeAndShape(v, &info) != nullptr) { return false; }
      shape = ORTHelpers::getTensorShape(info);
      ONNXTensorElementDataType et = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
      if (OrtStatus *st = api->GetTensorElementType(info, &et)) { api->ReleaseStatus(st); }
      api->ReleaseTensorTypeAndShapeInfo(info);
      void *raw = nullptr;
      if (api->GetTensorMutableData(v, &raw) != nullptr) { return false; }
      // Concrete output tensors have real (non-negative) dims; treat any <=0 dim as an
      // empty tensor (0 elements) rather than clamping to 1, which would read past a
      // zero-length buffer (e.g. an empty encoder output for near-silent audio).
      size_t n = 1;
      for (int64_t d : shape) { n *= (d > 0 ? (size_t)d : 0); }
      out.assign(n, 0.0f);
      if (n == 0) { return true; }
      if (et == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16) {
        const uint16_t *h = (const uint16_t *)raw;
        for (size_t i = 0; i < n; ++i) { out[i] = halfToFloat(h[i]); }
      } else {
        std::memcpy(out.data(), raw, n * sizeof(float));  // FLOAT (or best-effort)
      }
      return true;
    }

    // First element of an int tensor, tolerating INT32 or INT64 element type.
    int64_t tensorFirstInt(OrtValue *v, int64_t fallback) {
      const OrtApi *api = ORTHelpers::api();
      OrtTensorTypeAndShapeInfo *info = nullptr;
      if (api->GetTensorTypeAndShape(v, &info) != nullptr) { return fallback; }
      ONNXTensorElementDataType et = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
      if (OrtStatus *st = api->GetTensorElementType(info, &et)) { api->ReleaseStatus(st); }
      api->ReleaseTensorTypeAndShapeInfo(info);
      void *raw = nullptr;
      if (api->GetTensorMutableData(v, &raw) != nullptr) { return fallback; }
      if (et == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32) { return (int64_t)((const int32_t *)raw)[0]; }
      return ((const int64_t *)raw)[0];
    }

    bool nameHas(const std::string &n, const char *needle) {
      std::string a = n, b = needle;
      for (auto &c : a) { c = (char)std::tolower((unsigned char)c); }
      return a.find(b) != std::string::npos;
    }

    int argMax(const float *p, size_t n) {
      int best = 0; float bv = p[0];
      for (size_t i = 1; i < n; ++i) { if (p[i] > bv) { bv = p[i]; best = (int)i; } }
      return best;
    }

    // Element count of a shape, treating dynamic/negative dims as 1.
    size_t shapeCount(const std::vector<int64_t> &shape) {
      size_t c = 1;
      for (int64_t d : shape) { c *= (d > 0 ? (size_t)d : 1); }
      return c;
    }
  } // namespace

  ASRModel::ASRModel() {}
  ASRModel::~ASRModel() {}

  bool ASRModel::loadVocab(const std::string & vocabPath) {
    std::ifstream f(vocabPath.c_str());
    if (!f.is_open()) { ERROR_MSG("ASRModel: cannot open vocab %s", vocabPath.c_str()); return false; }
    // Lines are "<token> <id>"; the SentencePiece marker U+2581 maps to a space. The
    // largest id + 1 is the vocab size (includes the trailing "<blk>" blank token).
    std::vector<std::pair<int, std::string>> entries;
    int maxId = -1;
    std::string line;
    while (std::getline(f, line)) {
      if (!line.empty() && line.back() == '\r') { line.pop_back(); }
      if (line.empty()) { continue; }
      std::string::size_type sp = line.find(' ');
      if (sp == std::string::npos) { continue; }
      std::string tok = line.substr(0, sp);
      int id = atoi(line.substr(sp + 1).c_str());
      if (tok == "<blk>") { blankId_ = id; }
      // Replace every U+2581 with a plain space.
      std::string piece;
      for (std::string::size_type i = 0; i < tok.size();) {
        if (tok.compare(i, 3, SP_SPACE) == 0) { piece += ' '; i += 3; }
        else { piece += tok[i]; i += 1; }
      }
      entries.push_back(std::make_pair(id, piece));
      if (id > maxId) { maxId = id; }
    }
    if (maxId < 0) { ERROR_MSG("ASRModel: empty vocab %s", vocabPath.c_str()); return false; }
    vocab_.assign(maxId + 1, std::string());
    for (const auto & e : entries) { vocab_[e.first] = e.second; }
    vocabSize_ = (int)vocab_.size();
    if (blankId_ < 0) { blankId_ = vocabSize_ - 1; } // export appends <blk> last
    return true;
  }

  std::string ASRModel::detokenize(const std::vector<int> & tokens) const {
    std::string out;
    for (int id : tokens) {
      if (id >= 0 && id < (int)vocab_.size()) { out += vocab_[id]; }
    }
    // Pieces already carry leading spaces from U+2581; drop a single leading space.
    if (!out.empty() && out[0] == ' ') { out.erase(0, 1); }
    return out;
  }

  bool ASRModel::bindPorts(std::string & err) {
    // Validate the exact port roles transcribe() relies on (matched by the same name
    // heuristics), so an unexpected model fails at load with a clear message instead of
    // silently mis-binding during the decode loop.

    // Each singular role must resolve to EXACTLY ONE port (a non-"len" waveform/features/
    // audio_signal/encoded, etc.) — more than one would make the substring sweep in
    // transcribe() ambiguous, so reject rather than guess.

    // preproc: one waveform input (non-"len") -> one features output (non-"len").
    int wave = 0, feat = 0;
    for (const TensorSpec & s : preproc_.inputs()) { if (!nameHas(s.name, "len")) { wave++; } }
    for (const TensorSpec & s : preproc_.outputs()) { if (!nameHas(s.name, "len")) { feat++; } }
    if (wave != 1 || feat != 1) { err = "preprocessor must have exactly one waveform input and features output"; return false; }

    // encoder: one audio_signal input (non-"len") -> one encoded frames output (non-"len").
    int sig = 0, encOut = 0;
    for (const TensorSpec & s : encoder_.inputs()) { if (!nameHas(s.name, "len")) { sig++; } }
    for (const TensorSpec & s : encoder_.outputs()) { if (!nameHas(s.name, "len")) { encOut++; } }
    if (sig != 1 || encOut != 1) { err = "encoder must have exactly one audio_signal input and one outputs tensor"; return false; }

    // decoder_joint: encoder-step + targets + target_length inputs, a matched set of LSTM
    // state in/out ports, and exactly one non-state (logits) output.
    int encStep = 0, tgt = 0, tgtLen = 0, stateIns = 0, stateOuts = 0, logitsOut = 0;
    for (const TensorSpec & s : decoderJoint_.inputs()) {
      if (nameHas(s.name, "state")) { stateIns++; }
      else if (nameHas(s.name, "encoder")) { encStep++; }
      else if (nameHas(s.name, "target") && nameHas(s.name, "len")) { tgtLen++; }
      else if (nameHas(s.name, "target")) { tgt++; }
    }
    for (const TensorSpec & s : decoderJoint_.outputs()) {
      if (nameHas(s.name, "state")) { stateOuts++; }
      else if (nameHas(s.name, "len")) { /* auxiliary length (e.g. prednet_lengths) — ignored */ }
      else { logitsOut++; }
    }
    if (encStep != 1) { err = "decoder must have exactly one encoder_outputs input"; return false; }
    if (tgt != 1) { err = "decoder must have exactly one targets input"; return false; }
    if (tgtLen != 1) { err = "decoder must have exactly one target_length input"; return false; }
    if (stateIns == 0 || stateIns != stateOuts) { err = "decoder state ports mismatch"; return false; }
    if (logitsOut != 1) { err = "decoder must have exactly one non-state (logits) output"; return false; }

    // Guard every boundary port's element type against what the reader/writer actually
    // handle. Data ports must be float32 or float16 (tensorFloat converts half->float on
    // read, createRealTensor float->half on write — so int8/fp32/fp16 variants all bind;
    // INT8 variants quantise internally via QDQ and still expose float32 boundaries).
    // Length/target ports must be int32 or int64 (fed via tensorFirstInt / createInt*Tensor).
    // A port whose declared dtype doesn't match its role fails load cleanly instead of
    // being misread (e.g. an int8-typed data tensor, or a float "target" fed as int).
    auto dtypesOk = [](const SessionRunner & r, const char * which, std::string & e) -> bool {
      auto check = [&](const TensorSpec & s, const char * io) -> bool {
        bool integerRole = nameHas(s.name, "len") || nameHas(s.name, "target");
        bool okType = integerRole
          ? (s.dtype == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32 || s.dtype == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64)
          : (s.dtype == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT || s.dtype == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16);
        if (!okType) {
          e = std::string(which) + " " + io + " '" + s.name + "' has an element type incompatible with its "
            + (integerRole ? "length/target role (needs int32/int64)" : "data role (needs float32/float16)");
          return false;
        }
        return true;
      };
      for (const TensorSpec & s : r.inputs()) { if (!check(s, "input")) { return false; } }
      for (const TensorSpec & s : r.outputs()) { if (!check(s, "output")) { return false; } }
      return true;
    };
    if (!dtypesOk(preproc_, "preprocessor", err)) { return false; }
    if (!dtypesOk(encoder_, "encoder", err)) { return false; }
    if (!dtypesOk(decoderJoint_, "decoder", err)) { return false; }
    return true;
  }

  bool ASRModel::initialize(const ModelBundle & bundle, int numThreads, const std::string & requestedEP, bool lowLatency) {
    std::string err;
    if (!preproc_.load(bundle.preproc, numThreads, requestedEP, err, lowLatency)) {
      ERROR_MSG("ASRModel: failed to load preprocessor %s: %s", bundle.preproc.c_str(), err.c_str());
      return false;
    }
    if (!encoder_.load(bundle.encoder, numThreads, requestedEP, err, lowLatency)) {
      ERROR_MSG("ASRModel: failed to load encoder %s: %s", bundle.encoder.c_str(), err.c_str());
      return false;
    }
    if (!decoderJoint_.load(bundle.decoderJoint, numThreads, requestedEP, err, lowLatency)) {
      ERROR_MSG("ASRModel: failed to load decoder_joint %s: %s", bundle.decoderJoint.c_str(), err.c_str());
      return false;
    }
    if (!loadVocab(bundle.vocab)) { return false; }
    if (!bindPorts(err)) { ERROR_MSG("ASRModel: %s", err.c_str()); return false; }
    ready_ = true;
    INFO_MSG("ASRModel ready (vocab=%d, blank=%d, EP=%s)", vocabSize_, blankId_, encoder_.activeEP().c_str());
    // Warm up the graph (memory allocation, cuDNN/conv autotuning) with a short silent
    // buffer so the first real chunk doesn't absorb that one-time cost. Silence yields empty
    // text but should still run cleanly (ok=true); a failure here signals a broken runtime
    // (bad EP binding, shape mismatch) so warn loudly — but don't fail init, since a healthy
    // model legitimately returns no tokens for silence.
    {
      std::vector<float> silence((size_t)(sampleRate_ / 2), 0.0f); // 0.5 s of silence
      TranscriptResult warm = transcribe(silence.data(), silence.size(), 0);
      if (!warm.ok) { WARN_MSG("ASRModel warmup inference failed — check EP binding / model I/O"); }
    }
    return true;
  }

  TranscriptResult ASRModel::transcribe(const float *samples, size_t count, uint64_t baseMs) {
    TranscriptResult result;
    if (!ready_ || !samples || count == 0) { return result; }

    // 1) Preprocess: waveforms[1,N] (+ lens) -> features[1,128,T] (+ lens).
    std::vector<float> wave(samples, samples + count);
    int64_t nSamples = (int64_t)count;
    std::vector<float> feat;             // owns features across the encoder run
    std::vector<int64_t> featShape;
    int64_t featLen = 0;
    {
      std::vector<OrtValue *> ins, outs;
      ORTHelpers::OrtOutputsGuard ig(ins), og(outs);
      std::string e;
      int32_t nSamples32 = (int32_t)nSamples;
      for (const TensorSpec & s : preproc_.inputs()) {
        OrtValue *t = nullptr;
        if (nameHas(s.name, "len")) {
          t = (s.dtype == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32)
                ? preproc_.createInt32Tensor(&nSamples32, 1, {1}, e)
                : preproc_.createInt64Tensor(&nSamples, 1, {1}, e);
        } else {
          t = preproc_.createRealTensor(wave.data(), wave.size(), {1, nSamples}, s.dtype, e);
        }
        if (!t) { ERROR_MSG("ASRModel preproc input %s: %s", s.name.c_str(), e.c_str()); return result; }
        ins.push_back(t);
      }
      if (!preproc_.run(ins, outs, e)) { ERROR_MSG("ASRModel preproc run: %s", e.c_str()); return result; }
      for (size_t i = 0; i < preproc_.outputs().size() && i < outs.size(); ++i) {
        const std::string & nm = preproc_.outputs()[i].name;
        if (nameHas(nm, "len")) { featLen = tensorFirstInt(outs[i], 0); continue; }
        std::vector<int64_t> sh;
        if (tensorFloat(outs[i], feat, sh)) { featShape = sh; }
      }
    }
    if (feat.empty() || featShape.size() != 3) { ERROR_MSG("ASRModel: bad features"); return result; }
    if (featLen <= 0) { featLen = featShape[2]; }

    // 2) Encode: audio_signal[1,128,T] (+ length) -> outputs[1,D,T'] (+ encoded_lengths).
    std::vector<float> enc;              // owns encoder outputs across the decode loop
    int64_t D = 0, Tp = 0, validT = 0;
    {
      std::vector<OrtValue *> ins, outs;
      ORTHelpers::OrtOutputsGuard ig(ins), og(outs);
      std::string e;
      int32_t featLen32 = (int32_t)featLen;
      for (const TensorSpec & s : encoder_.inputs()) {
        OrtValue *t = nullptr;
        if (nameHas(s.name, "len")) {
          t = (s.dtype == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32)
                ? encoder_.createInt32Tensor(&featLen32, 1, {1}, e)
                : encoder_.createInt64Tensor(&featLen, 1, {1}, e);
        } else {
          t = encoder_.createRealTensor(feat.data(), feat.size(), featShape, s.dtype, e);
        }
        if (!t) { ERROR_MSG("ASRModel encoder input %s: %s", s.name.c_str(), e.c_str()); return result; }
        ins.push_back(t);
      }
      if (!encoder_.run(ins, outs, e)) { ERROR_MSG("ASRModel encoder run: %s", e.c_str()); return result; }
      for (size_t i = 0; i < encoder_.outputs().size() && i < outs.size(); ++i) {
        const std::string & nm = encoder_.outputs()[i].name;
        if (nameHas(nm, "len")) { validT = tensorFirstInt(outs[i], 0); continue; }
        std::vector<int64_t> sh;
        if (tensorFloat(outs[i], enc, sh) && sh.size() == 3) { D = sh[1]; Tp = sh[2]; }
      }
    }
    if (enc.empty() || D <= 0 || Tp <= 0) { ERROR_MSG("ASRModel: bad encoder output"); return result; }
    if (validT <= 0 || validT > Tp) { validT = Tp; }

    // 3) Greedy TDT decode. Thread the decoder's LSTM state + previous token across
    //    encoder frames; advance time by the predicted duration.
    std::vector<TensorSpec> decIns = decoderJoint_.inputs();
    // Zero-initialised state buffers, one per decoder state input, shaped [L,1,H].
    std::vector<std::vector<float>> state;
    std::vector<std::vector<int64_t>> stateShape;
    std::vector<size_t> stateInIdx;
    for (size_t i = 0; i < decIns.size(); ++i) {
      if (!nameHas(decIns[i].name, "state")) { continue; }
      std::vector<int64_t> sh = decIns[i].shape;
      for (auto & d : sh) { if (d < 0) { d = 1; } } // batch (dim 1) or unknowns -> 1
      state.push_back(std::vector<float>(shapeCount(sh), 0.0f));
      stateShape.push_back(sh);
      stateInIdx.push_back(i);
    }

    std::vector<int> tokens;
    std::vector<int> timestamps;
    std::vector<float> tokConf;
    int64_t t = 0;
    int emitted = 0;
    std::vector<float> colVec((size_t)D);
    while (t < validT) {
      // Build decoder inputs in the model's declared order.
      int32_t prevTok = (int32_t)(tokens.empty() ? blankId_ : tokens.back());
      int64_t prevTok64 = prevTok;
      int32_t tgtLen = 1; int64_t tgtLen64 = 1;
      for (int64_t d = 0; d < D; ++d) { colVec[(size_t)d] = enc[(size_t)(d * Tp + t)]; }

      std::vector<OrtValue *> ins, outs;
      ORTHelpers::OrtOutputsGuard ig(ins), og(outs);
      std::string e;
      bool bad = false;
      size_t stateSlot = 0;
      for (size_t i = 0; i < decIns.size(); ++i) {
        const TensorSpec & s = decIns[i];
        OrtValue *tv = nullptr;
        if (nameHas(s.name, "state")) {
          tv = decoderJoint_.createRealTensor(state[stateSlot].data(), state[stateSlot].size(), stateShape[stateSlot], s.dtype, e);
          stateSlot++;
        } else if (nameHas(s.name, "encoder")) {
          tv = decoderJoint_.createRealTensor(colVec.data(), colVec.size(), {1, D, 1}, s.dtype, e);
        } else if (nameHas(s.name, "target") && nameHas(s.name, "len")) {
          tv = (s.dtype == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32)
                 ? decoderJoint_.createInt32Tensor(&tgtLen, 1, {1}, e)
                 : decoderJoint_.createInt64Tensor(&tgtLen64, 1, {1}, e);
        } else if (nameHas(s.name, "target")) {
          tv = (s.dtype == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32)
                 ? decoderJoint_.createInt32Tensor(&prevTok, 1, {1, 1}, e)
                 : decoderJoint_.createInt64Tensor(&prevTok64, 1, {1, 1}, e);
        } else {
          // Unknown extra input: feeding a placeholder is unsafe; bail out.
          ERROR_MSG("ASRModel: unexpected decoder input '%s'", s.name.c_str());
          bad = true; break;
        }
        if (!tv) { ERROR_MSG("ASRModel decoder input %s: %s", s.name.c_str(), e.c_str()); bad = true; break; }
        ins.push_back(tv);
      }
      if (bad) { break; }
      if (!decoderJoint_.run(ins, outs, e)) { ERROR_MSG("ASRModel decoder run: %s", e.c_str()); break; }

      // Read the logits output and the new state outputs. The decoder also emits an
      // auxiliary length tensor (e.g. prednet_lengths) which onnx-asr ignores — skip any
      // "len"-named output so logits binds to the real token/duration tensor ("outputs").
      std::vector<float> logitsVec; std::vector<int64_t> lsh;
      const float *logits = nullptr;
      std::vector<std::vector<float>> newState;
      for (size_t i = 0; i < decoderJoint_.outputs().size() && i < outs.size(); ++i) {
        const std::string & nm = decoderJoint_.outputs()[i].name;
        if (nameHas(nm, "state")) {
          std::vector<float> sv; std::vector<int64_t> ssh;
          if (tensorFloat(outs[i], sv, ssh)) { newState.push_back(std::move(sv)); }
        } else if (nameHas(nm, "len")) {
          // auxiliary length output — ignored
        } else {
          if (tensorFloat(outs[i], logitsVec, lsh)) { logits = logitsVec.data(); }
        }
      }
      if (!logits) { ERROR_MSG("ASRModel: decoder produced no logits"); break; }

      // TDT split: token over [0,V), duration over [V, end).
      size_t outLen = shapeCount(lsh);
      int V = vocabSize_ <= (int)outLen ? vocabSize_ : (int)outLen;
      int token = argMax(logits, (size_t)V);
      int step = 0;
      if ((size_t)V < outLen) { step = argMax(logits + V, outLen - (size_t)V); }

      if (token != blankId_) {
        if (newState.size() == state.size()) { state.swap(newState); } // commit state
        tokens.push_back(token);
        timestamps.push_back((int)t);
        // Confidence: softmax prob of the chosen token over the vocab logits.
        float mx = logits[0];
        for (int i = 1; i < V; ++i) { if (logits[i] > mx) { mx = logits[i]; } }
        float sum = 0.0f;
        for (int i = 0; i < V; ++i) { sum += std::exp(logits[i] - mx); }
        tokConf.push_back(sum > 0.0f ? std::exp(logits[token] - mx) / sum : 0.0f);
        emitted++;
      }

      if (step > 0) {
        t += step;
        emitted = 0;
      } else if (token == blankId_ || emitted >= ASR_MAX_TOKENS_PER_STEP) {
        t += 1;
        emitted = 0;
      }
    }

    // 4) Assemble result: full text plus word-level timed segments. loadVocab normalised
    //    the SentencePiece marker to a leading space, so a piece starting with a space marks
    //    a new word; merge the subword pieces between boundaries into a single segment
    //    (start = first subword, end = last subword, confidence = mean of subword probs).
    result.text = detokenize(tokens);
    auto tokStartMs = [&](size_t i) { return baseMs + (uint64_t)timestamps[i] * ASR_FRAME_MS; };
    auto tokEndMs = [&](size_t i) {
      uint64_t s = tokStartMs(i);
      uint64_t nextStart = (i + 1 < tokens.size()) ? tokStartMs(i + 1) : s + ASR_FRAME_MS;
      return nextStart > s ? nextStart : s + ASR_FRAME_MS;
    };
    TranscriptSegment cur;
    bool haveCur = false;
    float confSum = 0.0f; int confN = 0;
    auto flushWord = [&]() {
      if (!haveCur) { return; }
      cur.confidence = confN > 0 ? confSum / (float)confN : 0.0f;
      if (!cur.text.empty() && cur.text[0] == ' ') { cur.text.erase(0, 1); } // trim word-leading space
      if (!cur.text.empty()) { result.segments.push_back(cur); }
      haveCur = false; confSum = 0.0f; confN = 0;
    };
    for (size_t i = 0; i < tokens.size(); ++i) {
      std::string piece = (tokens[i] >= 0 && tokens[i] < (int)vocab_.size()) ? vocab_[tokens[i]] : std::string();
      if ((!piece.empty() && piece[0] == ' ') || !haveCur) { // word boundary (or first token)
        flushWord();
        cur = TranscriptSegment();
        cur.startMs = tokStartMs(i);
        haveCur = true;
      }
      cur.text += piece;
      cur.endMs = tokEndMs(i);
      if (i < tokConf.size()) { confSum += tokConf[i]; confN++; }
    }
    flushWord();
    result.ok = true;
    return result;
  }

  std::unique_ptr<ASRModel> ModelFactory::createTranscriptionModel(const ModelBundle & bundle, int numThreads,
                                                                   const std::string & executionProvider, bool lowLatency) {
    if (!bundle.ok) { ERROR_MSG("createTranscriptionModel: bundle not resolved"); return nullptr; }
    std::unique_ptr<ASRModel> model(new ASRModel());
    if (!model->initialize(bundle, numThreads, executionProvider, lowLatency)) { return nullptr; }
    return model;
  }

  // ModelFactory implementation
  AudioModel::AudioModel(const std::string & modelPath, ModelType task, const AudioModelConfig & cfg)
    : modelPath_(modelPath), task_(task), cfg_(cfg) {}

  bool AudioModel::initialize(int numThreads, const std::string & requestedEP, bool lowLatency,
                              std::string & err) {
    if (!runner_.load(modelPath_, numThreads, requestedEP, err, lowLatency)) { return false; }
    if (!bindPorts(err)) { return false; }
    ready_ = true;
    INFO_MSG("Audio model ready: %s (rate %d Hz, %s, %zu labels, EP %s)", modelPath_.c_str(),
             cfg_.sampleRate,
             cfg_.chunkSamples > 0 ? "fixed-chunk streaming" : "pause-windowed",
             cfg_.labels.size(), runner_.activeEP().c_str());
    return true;
  }

  bool AudioModel::bindPorts(std::string & err) {
    const std::vector<TensorSpec> & ins = runner_.inputs();
    const std::vector<TensorSpec> & outs = runner_.outputs();
    if (ins.empty() || outs.empty()) {
      err = "Audio model has no input or output ports";
      return false;
    }

    samplesIdx_ = SIZE_MAX;
    srIdx_ = SIZE_MAX;
    std::vector<std::string> stateOutputNames;
    for (size_t i = 0; i < ins.size(); ++i) {
      const TensorSpec & in = ins[i];
      bool isInt = (in.dtype == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64 ||
                    in.dtype == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32);
      if (isInt && in.shape.size() <= 1 &&
          (in.name.find("sr") != std::string::npos || in.name.find("rate") != std::string::npos)) {
        srIdx_ = i;
        srInt32_ = (in.dtype == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32);
        srRank1_ = (in.shape.size() == 1);
        continue;
      }
      if (in.dtype == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
        if (samplesIdx_ == SIZE_MAX) {
          // First float port is the waveform/features input
          samplesIdx_ = i;
          continue;
        }
        // Every further float input is recurrent state: bind it to the output port
        // whose name starts with the input's name (Silero: "state" -> "stateN").
        std::string outName;
        for (const TensorSpec & o : outs) {
          if (o.name.compare(0, in.name.size(), in.name) == 0 && o.name != in.name) { outName = o.name; break; }
          if (o.name == in.name) { outName = o.name; }
        }
        if (outName.empty()) {
          err = "No output port matches state input '" + in.name + "'";
          return false;
        }
        if (!runner_.bindStateLoop(outName, in.name, err)) { return false; }
        stateOutputNames.push_back(outName);
        continue;
      }
      err = "Unsupported audio input port '" + in.name + "' (dtype " + std::to_string((int)in.dtype) + ")";
      return false;
    }
    if (samplesIdx_ == SIZE_MAX) {
      err = "Audio model has no float waveform/features input";
      return false;
    }

    // Main output: first float output that is not a bound state output
    outIdx_ = SIZE_MAX;
    for (size_t i = 0; i < outs.size(); ++i) {
      if (outs[i].dtype != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) { continue; }
      bool isState = false;
      for (const std::string & sn : stateOutputNames) {
        if (outs[i].name == sn) { isState = true; break; }
      }
      if (!isState) { outIdx_ = i; break; }
    }
    if (outIdx_ == SIZE_MAX) {
      err = "Audio model has no non-state float output";
      return false;
    }
    return true;
  }

  AudioResult AudioModel::process(const float *samples, size_t count, uint64_t baseMs) {
    AudioResult res;
    res.startMs = baseMs;
    res.endMs = baseMs + (uint64_t)(1000.0 * count / (cfg_.sampleRate > 0 ? cfg_.sampleRate : 16000));
    if (!ready_ || !samples || !count) { return res; }

    auto start = std::chrono::high_resolution_clock::now();
    std::string err;

    // Waveform normalization (wav2vec2-style zero-mean/unit-variance per window)
    std::vector<float> normBuf;
    const float *feed = samples;
    if (cfg_.zeroMeanUnitVar) {
      normBuf.assign(samples, samples + count);
      double mean = 0.0;
      for (size_t i = 0; i < count; ++i) { mean += normBuf[i]; }
      mean /= count;
      double var = 0.0;
      for (size_t i = 0; i < count; ++i) {
        double d = normBuf[i] - mean;
        var += d * d;
      }
      float scale = 1.0f / (float)std::sqrt(var / count + 1e-7);
      for (size_t i = 0; i < count; ++i) { normBuf[i] = (float)((normBuf[i] - mean) * scale); }
      feed = normBuf.data();
    }

    // FBANK frontend: waveform -> log-mel features [1, frames, bins]
    std::vector<float> feats;
    std::vector<int64_t> inShape;
    size_t feedCount = count;
    if (cfg_.fbankBins > 0) {
      size_t frames = Utils::computeFbank(feed, count, cfg_.sampleRate, cfg_.fbankBins,
                                          cfg_.fbankHanning, feats);
      if (!frames) { return res; }
      if (cfg_.fixedFrames > 0 && frames != (size_t)cfg_.fixedFrames) {
        // Pad with zeros / truncate to the model's fixed frame count (AST: 1024).
        // Padding happens BEFORE normalization, matching the HF feature extractor.
        feats.resize((size_t)cfg_.fixedFrames * cfg_.fbankBins, 0.0f);
        frames = (size_t)cfg_.fixedFrames;
      }
      if (cfg_.cepstralMeanNorm) {
        // Subtract the per-utterance mean per mel bin (WeSpeaker convention)
        for (int b = 0; b < cfg_.fbankBins; ++b) {
          double m = 0.0;
          for (size_t f = 0; f < frames; ++f) { m += feats[f * cfg_.fbankBins + b]; }
          m /= frames;
          for (size_t f = 0; f < frames; ++f) { feats[f * cfg_.fbankBins + b] -= (float)m; }
        }
      }
      if (cfg_.featMean != 0.0f || cfg_.featStd != 1.0f) {
        const float inv = 1.0f / cfg_.featStd;
        for (size_t i = 0; i < feats.size(); ++i) { feats[i] = (feats[i] - cfg_.featMean) * inv; }
      }
      feed = feats.data();
      feedCount = feats.size();
      inShape.push_back(1);
      inShape.push_back((int64_t)frames);
      inShape.push_back(cfg_.fbankBins);
    } else {
      inShape.push_back(1);
      inShape.push_back((int64_t)count);
    }
    OrtValue *inTensor = runner_.createFloatTensor(feed, feedCount, inShape, err);
    if (!inTensor) {
      ERROR_MSG("Audio input tensor failed: %s", err.c_str());
      return res;
    }
    ORTHelpers::OrtValueGuard inGuard(inTensor);

    int64_t srVal64 = cfg_.sampleRate;
    int32_t srVal32 = cfg_.sampleRate;
    OrtValue *srTensor = nullptr;
    ORTHelpers::OrtValueGuard srGuard(srTensor);
    std::vector<OrtValue *> inputs(runner_.numInputs(), nullptr);
    inputs[samplesIdx_] = inTensor;
    if (srIdx_ != SIZE_MAX) {
      // Feed the sample rate in the port's own element type and rank (rank-0 scalar
      // or rank-1 [1] — both accepted at bind time).
      std::vector<int64_t> srShape;
      if (srRank1_) { srShape.push_back(1); }
      srTensor = srInt32_ ? runner_.createInt32Tensor(&srVal32, 1, srShape, err)
                          : runner_.createInt64Tensor(&srVal64, 1, srShape, err);
      if (!srTensor) {
        ERROR_MSG("Audio sr tensor failed: %s", err.c_str());
        return res;
      }
      inputs[srIdx_] = srTensor;
    }

    auto infStart = std::chrono::high_resolution_clock::now();
    std::vector<OrtValue *> outputs;
    ORTHelpers::OrtOutputsGuard outGuard(outputs);
    if (!runner_.run(inputs, outputs, err)) {
      ERROR_MSG("Audio inference failed: %s", err.c_str());
      return res;
    }
    auto infEnd = std::chrono::high_resolution_clock::now();
    res.metrics.preprocessTimeMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(infStart - start).count();
    res.metrics.inferenceTimeMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(infEnd - infStart).count();

    // Read the main output tensor
    const OrtApi *api = ORTHelpers::api();
    float *data = nullptr;
    OrtStatus *ds = api->GetTensorMutableData(outputs[outIdx_], (void **)&data);
    if (ds) { api->ReleaseStatus(ds); return res; }
    OrtTensorTypeAndShapeInfo *info = nullptr;
    OrtStatus *is = api->GetTensorTypeAndShape(outputs[outIdx_], &info);
    if (is) { api->ReleaseStatus(is); return res; }
    size_t n = 0;
    (void)api->GetTensorShapeElementCount(info, &n);
    api->ReleaseTensorTypeAndShapeInfo(info);
    if (!data || n == 0) { return res; }

    switch (task_) {
      case ModelType::AUDIO_VAD: {
        ClassScore cs;
        cs.class_id = 0;
        cs.class_name = "speech";
        cs.confidence = data[0];
        res.scores.push_back(cs);
        break;
      }
      case ModelType::AUDIO_EMBEDDING: {
        res.embedding.assign(data, data + n);
        float norm = 0.0f;
        for (size_t i = 0; i < n; ++i) { norm += res.embedding[i] * res.embedding[i]; }
        norm = std::sqrt(norm);
        if (norm > 1e-10f) {
          for (size_t i = 0; i < n; ++i) { res.embedding[i] /= norm; }
        }
        break;
      }
      case ModelType::AUDIO_CLASSIFICATION:
      case ModelType::AUDIO_TAGGING:
      default: {
        std::vector<float> conf(n);
        if (cfg_.multiLabel) {
          for (size_t i = 0; i < n; ++i) { conf[i] = 1.0f / (1.0f + std::exp(-data[i])); }
        } else {
          float maxLogit = data[0];
          for (size_t i = 1; i < n; ++i) {
            if (data[i] > maxLogit) { maxLogit = data[i]; }
          }
          float sumExp = 0.0f;
          for (size_t i = 0; i < n; ++i) {
            conf[i] = std::exp(data[i] - maxLogit);
            sumExp += conf[i];
          }
          for (size_t i = 0; i < n; ++i) { conf[i] /= sumExp; }
        }
        size_t k = cfg_.topK ? cfg_.topK : 1;
        if (k > n) { k = n; }
        std::vector<int> order(n);
        std::iota(order.begin(), order.end(), 0);
        std::partial_sort(order.begin(), order.begin() + k, order.end(),
                          [&conf](int a, int b) { return conf[a] > conf[b]; });
        for (size_t i = 0; i < k; ++i) {
          ClassScore cs;
          cs.class_id = order[i];
          cs.class_name = ((size_t)order[i] < cfg_.labels.size() && !cfg_.labels[order[i]].empty())
                            ? cfg_.labels[order[i]]
                            : "class_" + std::to_string(order[i]);
          cs.confidence = conf[order[i]];
          res.scores.push_back(cs);
        }
        break;
      }
    }

    auto end = std::chrono::high_resolution_clock::now();
    res.metrics.totalTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    res.ok = true;
    return res;
  }

  void EventSmoother::configure(float enterThresh, float exitThresh, double emaAlpha,
                                uint64_t minDurationMs) {
    enter_ = enterThresh;
    exit_ = exitThresh;
    alpha_ = (emaAlpha <= 0.0 || emaAlpha > 1.0) ? 1.0 : emaAlpha;
    minMs_ = minDurationMs;
    ema_ = 0.0f;
    primed_ = false;
    active_ = false;
    startedAt_ = 0;
  }

  EventSmoother::Event EventSmoother::update(float score, uint64_t timeMs) {
    if (!primed_) {
      ema_ = score;
      primed_ = true;
    } else {
      ema_ = (float)(alpha_ * score + (1.0 - alpha_) * ema_);
    }
    // Timeline regression (seek / loop restart): rebase the active phase so the
    // unsigned elapsed-time math below cannot wrap and defeat the min-duration debounce.
    if (active_ && timeMs < startedAt_) { startedAt_ = timeMs; }
    if (!active_ && ema_ >= enter_) {
      active_ = true;
      startedAt_ = timeMs;
      return STARTED;
    }
    if (active_ && ema_ <= exit_ && (timeMs - startedAt_) >= minMs_) {
      active_ = false;
      return ENDED;
    }
    return NONE;
  }

  std::unique_ptr<AudioModel> ModelFactory::createAudioModel(const std::string & modelPath, ModelType task,
                                                             int numThreads,
                                                             const std::string & executionProvider,
                                                             bool lowLatency) {
    AudioModelConfig cfg;
    Utils::SidecarConfig sidecar = Utils::loadModelSidecars(modelPath);
    if (!sidecar.labels.empty()) { cfg.labels = sidecar.labels; }
    if (sidecar.samplingRate > 0) { cfg.sampleRate = sidecar.samplingRate; }
    switch (task) {
      case ModelType::AUDIO_VAD:
        // Silero contract: 512-sample chunks at 16 kHz, 256 at 8 kHz
        cfg.chunkSamples = (cfg.sampleRate == 8000) ? 256 : 512;
        cfg.topK = 1;
        break;
      case ModelType::AUDIO_TAGGING:
        cfg.multiLabel = true;
        // Spectrogram tagger (AST): HF hanning-window fbank, fixed frame count, and
        // (x - mean) / (2 * std) normalization — the factor 2 is the HF AST convention.
        cfg.fbankBins = sidecar.numMelBins > 0 ? sidecar.numMelBins : 128;
        cfg.fbankHanning = true;
        cfg.fixedFrames = sidecar.maxFrames > 0 ? sidecar.maxFrames : 1024;
        if (sidecar.audioNormalize && sidecar.featStd > 0.0f) {
          cfg.featMean = sidecar.featMean;
          cfg.featStd = 2.0f * sidecar.featStd;
        }
        break;
      case ModelType::AUDIO_CLASSIFICATION:
        cfg.zeroMeanUnitVar = sidecar.audioNormalize;
        break;
      case ModelType::AUDIO_EMBEDDING:
        // Speaker embedders (WeSpeaker): kaldi povey-window fbank + per-utterance
        // cepstral mean normalization, no dataset mean/std.
        cfg.fbankBins = sidecar.numMelBins > 0 ? sidecar.numMelBins : 80;
        cfg.fbankHanning = false;
        cfg.cepstralMeanNorm = true;
        cfg.topK = 1;
        break;
      default: break;
    }

    std::unique_ptr<AudioModel> model(new AudioModel(modelPath, task, cfg));
    std::string err;
    if (!model->initialize(numThreads, executionProvider, lowLatency, err)) {
      ERROR_MSG("Failed to initialize audio model %s: %s", modelPath.c_str(), err.c_str());
      return nullptr;
    }
    return model;
  }

  std::unique_ptr<OCRModel> ModelFactory::createOCRModel(const OCRBundle & bundle, int numThreads,
                                                         const std::string & executionProvider, bool lowLatency) {
    std::unique_ptr<OCRModel> model(new OCRModel(bundle.det, bundle.rec, bundle.dict));
    // Force the type: the det model's [?,1,?,?] output would otherwise auto-detect as
    // depth/generic, and the OCR dispatch branch (getModelType() == OCR) never fires.
    model->setModelTypeOverride(ModelType::OCR);
    if (!executionProvider.empty()) { model->setExecutionProvider(executionProvider); }
    model->setLowLatency(lowLatency);
    std::string err;
    if (!model->initialize(numThreads)) {
      ERROR_MSG("Failed to initialize OCR detection model %s", bundle.det.c_str());
      return nullptr;
    }
    if (!model->initializeRec(numThreads, executionProvider, lowLatency, err)) {
      ERROR_MSG("Failed to initialize OCR recognition model %s: %s", bundle.rec.c_str(), err.c_str());
      return nullptr;
    }
    INFO_MSG("Created OCR model: det=%s rec=%s", bundle.det.c_str(), bundle.rec.c_str());
    return model;
  }

  std::unique_ptr<DetectionModel> ModelFactory::createModel(const std::string & modelPath, int inputSize, int numThreads,
                                                             ModelType typeOverride, const std::string & executionProvider,
                                                             bool lowLatency) {
    std::unique_ptr<DetectionModel> model;

    // Per-model sidecar assets (see Utils::loadModelSidecars). Loaded first because
    // inputSize <= 0 means "auto": prefer the model's native size from its
    // preprocessor sidecar, else the generic 640 default. Explicit sizes pass through
    // untouched — a dynamic-dim ViT/CLIP at a wrong explicit size loads but fails
    // inference (fixed position embeddings), so callers without better knowledge
    // should pass 0.
    Utils::SidecarConfig sidecar = Utils::loadModelSidecars(modelPath);
    if (inputSize <= 0) {
      inputSize = (sidecar.inputSize > 0) ? sidecar.inputSize : 640;
      if (sidecar.inputSize > 0) {
        INFO_MSG("Using model-native input_size %d from preprocessor sidecar", inputSize);
      }
    }

    // Use override if provided, otherwise probe the model once (via the neutral
    // runtime layer) to gate on input modality and classify the vision type.
    ModelType type;
    if (typeOverride != ModelType::UNKNOWN) {
      type = typeOverride;
    } else {
      SessionRunner probe;
      std::string err;
      if (!probe.load(modelPath, 1, "cpu", err)) {
        ERROR_MSG("Failed to load ONNX model for probing: %s (%s)", modelPath.c_str(), err.c_str());
        return nullptr;
      }
      // Modality gate: only vision models have an adapter today. Non-vision models
      // are reported (not force-fit into the vision pipeline, which would fail).
      ModelModality modality = classifyModality(probe.inputs());
      if (modality != ModelModality::VISION) {
        ERROR_MSG("Model %s has %s input modality, not vision; no adapter is available yet "
                  "(the vision pipeline requires a 4D NCHW image input).",
                  modelPath.c_str(), modalityName(modality));
        return nullptr;
      }
      int inputH = 0, inputW = 0;
      if (!probe.inputs().empty() && probe.inputs()[0].shape.size() == 4) {
        inputH = (int)probe.inputs()[0].shape[2];
        inputW = (int)probe.inputs()[0].shape[3];
      }
      std::vector<std::vector<int64_t>> outputShapes;
      std::vector<std::string> outputNames;
      for (const TensorSpec & o : probe.outputs()) {
        outputShapes.push_back(o.shape);
        outputNames.push_back(o.name);
      }
      type = classifyModelOutputs(outputShapes, outputNames, inputH, inputW);
    }

    switch (type) {
      case ModelType::YOLOV8_DETECTION:
      case ModelType::YOLO11_DETECTION: model.reset(new YOLOv8Model(modelPath, inputSize)); break;

      case ModelType::YOLOV8_POSE:
      case ModelType::YOLO11_POSE: model.reset(new YOLOv8PoseModel(modelPath, inputSize)); break;

      case ModelType::YOLOV8_SEGMENTATION:
      case ModelType::YOLO11_SEGMENTATION: model.reset(new YOLOv8SegmentationModel(modelPath, inputSize)); break;

      case ModelType::YOLOV8_CLASSIFICATION:
      case ModelType::YOLO11_CLASSIFICATION: model.reset(new YOLOv8ClassificationModel(modelPath, inputSize)); break;

      case ModelType::YOLOV8_OBB:
      case ModelType::YOLO11_OBB: model.reset(new YOLOv8OBBModel(modelPath, inputSize)); break;

      case ModelType::YOLO_NMS_DETECTION: model.reset(new YOLONMSModel(modelPath, inputSize)); break;
      case ModelType::YOLO_SPLIT_DETECTION: model.reset(new YOLOSplitModel(modelPath, inputSize)); break;

      case ModelType::RT_DETR_DETECTION: model.reset(new RTDETRModel(modelPath, inputSize)); break;

      case ModelType::DEPTH_ESTIMATION: model.reset(new DepthEstimationModel(modelPath, inputSize)); break;

      case ModelType::FACE_DETECTION_SCRFD: model.reset(new SCRFDModel(modelPath, inputSize)); break;

      case ModelType::FACE_RECOGNITION_ARCFACE: model.reset(new ArcFaceModel(modelPath, 112)); break;

      case ModelType::IMAGE_EMBEDDING: {
        EmbeddingModel *em = new EmbeddingModel(modelPath, inputSize);
        // Zero-shot tags: a text_embeddings.json next to the model turns this embedder
        // into a tagger (offline-computed CLIP text embeddings; see the Python script).
        size_t slash = modelPath.find_last_of("/\\");
        std::string mdir = (slash == std::string::npos) ? "" : modelPath.substr(0, slash + 1);
        em->loadMatchSet(mdir + "text_embeddings.json");
        model.reset(em);
        break;
      }

      case ModelType::FACE_ATTRIBUTE: {
        // [1,2] raw head = [age_years, gender_prob]; a RAW top-2 classifier surfaces
        // both values (class 0 = age, class 1 = gender), decoded by the caller.
        YOLOv8ClassificationModel *fa = new YOLOv8ClassificationModel(modelPath, inputSize);
        fa->setOutputMode(YOLOv8ClassificationModel::RAW);
        fa->setTopK(2);
        model.reset(fa);
        break;
      }

      case ModelType::POSE_RTMO: model.reset(new RTMOModel(modelPath, inputSize)); break;

      case ModelType::SAM2_ENCODER: model.reset(new SAM2EncoderModel(modelPath, 1024)); break;

      case ModelType::SAM2_DECODER: model.reset(new SAM2DecoderModel(modelPath, 256)); break;

      case ModelType::GENERIC_CLASSIFICATION:
      case ModelType::GENERIC_DETECTION:
      case ModelType::GENERIC_UNKNOWN:
      default: model.reset(new GenericModel(modelPath, inputSize)); break;
    }

    // Sidecar labels / preprocessing constants override the adapter's compiled-in
    // defaults; explicit user options still win because process_onnx applies them
    // after creation.
    if (!sidecar.labels.empty()) {
      model->setClassLabels(sidecar.labels);
      INFO_MSG("Model sidecar: %zu class labels", sidecar.labels.size());
    }
    if (sidecar.hasPreproc) {
      PreprocessConfig cfg = model->getPreprocessConfig();
      cfg.resizeMode = sidecar.preproc.resizeMode;
      cfg.normMode = sidecar.preproc.normMode;
      for (int c = 0; c < 3; ++c) {
        cfg.mean[c] = sidecar.preproc.mean[c];
        cfg.std[c] = sidecar.preproc.std[c];
      }
      model->setPreprocessConfig(cfg);
      model->setLetterbox(cfg.resizeMode == PreprocessConfig::LETTERBOX);
      INFO_MSG("Model sidecar: %s resize, %s normalization",
               cfg.resizeMode == PreprocessConfig::LETTERBOX
                 ? "letterbox"
                 : (cfg.resizeMode == PreprocessConfig::CENTER_CROP ? "center-crop" : "direct"),
               cfg.normMode == PreprocessConfig::IMAGENET
                 ? "mean/std"
                 : (cfg.normMode == PreprocessConfig::SCRFD_NORM ? "scrfd" : "scale01"));
    }

    if (typeOverride != ModelType::UNKNOWN) { model->setModelTypeOverride(typeOverride); }
    if (!executionProvider.empty()) { model->setExecutionProvider(executionProvider); }
    model->setLowLatency(lowLatency);
    if (model && model->initialize(numThreads)) {
      INFO_MSG("Created ONNX model: %s (type: %d)", modelPath.c_str(), static_cast<int>(type));
      return model;
    } else {
      ERROR_MSG("Failed to initialize ONNX model: %s", modelPath.c_str());
      return nullptr;
    }
  }

  ModelType ModelFactory::detectModelType(const std::string & modelPath) {
    // Inspect the model through the neutral runtime layer (handles load failure,
    // no duplicate manual ORT session juggling).
    SessionRunner probe;
    std::string err;
    if (!probe.load(modelPath, 1, "cpu", err)) {
      ERROR_MSG("Error analyzing model %s: %s", modelPath.c_str(), err.c_str());
      return ModelType::UNKNOWN;
    }

    const std::vector<TensorSpec> &ins = probe.inputs();
    int inputH = 0, inputW = 0;
    if (!ins.empty() && ins[0].shape.size() == 4) {
      inputH = (int)ins[0].shape[2];
      inputW = (int)ins[0].shape[3];
    }

    std::vector<std::vector<int64_t>> outputShapes;
    std::vector<std::string> outputNames;
    for (const TensorSpec &o : probe.outputs()) {
      outputShapes.push_back(o.shape);
      outputNames.push_back(o.name);
    }
    return classifyModelOutputs(outputShapes, outputNames, inputH, inputW);
  }

  ModelModality ModelFactory::detectModality(const std::string & modelPath) {
    SessionRunner probe;
    std::string err;
    if (!probe.load(modelPath, 1, "cpu", err)) {
      MEDIUM_MSG("Modality probe failed to load %s: %s", modelPath.c_str(), err.c_str());
      return ModelModality::UNKNOWN;
    }
    return classifyModality(probe.inputs());
  }

  ModelInfo ModelFactory::analyzeModel(const std::string & modelPath) {
    ModelInfo info;

    // Single inspection through the neutral runtime layer (one load, not two).
    SessionRunner probe;
    std::string err;
    if (!probe.load(modelPath, 1, "cpu", err)) {
      ERROR_MSG("Error analyzing model %s: %s", modelPath.c_str(), err.c_str());
      info.type = ModelType::UNKNOWN;
      info.name = "Unknown Model";
      return info;
    }

    for (const TensorSpec &in : probe.inputs()) {
      info.inputNames.push_back(in.name);
      info.inputShapes.push_back(in.shape);
    }
    for (const TensorSpec &o : probe.outputs()) {
      info.outputNames.push_back(o.name);
      info.outputShapes.push_back(o.shape);
    }

    int inputH = 0, inputW = 0;
    if (!info.inputShapes.empty() && info.inputShapes[0].size() == 4) {
      inputH = (int)info.inputShapes[0][2];
      inputW = (int)info.inputShapes[0][3];
    }
    info.type = classifyModelOutputs(info.outputShapes, info.outputNames, inputH, inputW);

    {
      // Set model-specific information based on type
      switch (info.type) {
        case ModelType::YOLOV8_DETECTION:
          info.name = "YOLOv8/YOLO11 Detection";
          info.numClasses = 80;
          break;
        case ModelType::YOLOV8_POSE:
          info.name = "YOLOv8/YOLO11 Pose";
          info.numClasses = 1;
          break;
        case ModelType::YOLOV8_SEGMENTATION:
          info.name = "YOLOv8/YOLO11 Segmentation";
          info.numClasses = 80;
          break;
        case ModelType::YOLOV8_CLASSIFICATION:
          info.name = "YOLOv8/YOLO11 Classification";
          if (!info.outputShapes.empty() && info.outputShapes[0].size() == 2) {
            info.numClasses = info.outputShapes[0][1];
          }
          break;
        case ModelType::YOLOV8_OBB:
          info.name = "YOLOv8/YOLO11 Oriented Bounding Boxes";
          // Determine number of classes based on collected output shape
          if (!info.outputShapes.empty() && info.outputShapes[0].size() == 3) {
            int64_t features = info.outputShapes[0][1];
            if (features == 20) {
              info.numClasses = 15; // Compact format: 15 classes + 5 OBB params
            } else {
              info.numClasses = 80; // Standard format: 80 classes + OBB params
            }
          } else {
            info.numClasses = 80; // Default fallback
          }
          break;
        case ModelType::YOLO_NMS_DETECTION:
          info.name = "YOLO NMS Detection";
          info.numClasses = 80;
          break;
        case ModelType::YOLO_SPLIT_DETECTION:
          info.name = "End-to-end Split-output Detection";
          info.numClasses = (!info.outputShapes.empty() && info.outputShapes[0].size() == 3 &&
                             info.outputShapes[0][2] != 4) ? (int)info.outputShapes[0][2] : 80;
          break;
        case ModelType::RT_DETR_DETECTION:
          info.name = "RT-DETR Detection";
          info.numClasses = 80; // COCO default
          if (!info.outputShapes.empty()) {
            for (size_t i = 0; i < info.outputNames.size(); ++i) {
              // scores tensor [1, maxDet, numClasses] carries the actual class count
              if (info.outputNames[i].find("score") != std::string::npos && info.outputShapes[i].size() == 3) {
                info.numClasses = (int)info.outputShapes[i][2];
              }
            }
          }
          break;
        case ModelType::DEPTH_ESTIMATION: info.name = "Depth Estimation"; break;
        case ModelType::FACE_DETECTION_SCRFD: info.name = "SCRFD Face Detection"; break;
        case ModelType::FACE_RECOGNITION_ARCFACE:
          info.name = "ArcFace Recognition";
          info.numClasses = 512;
          break;
        case ModelType::IMAGE_EMBEDDING: info.name = "Image Embedding"; break;
        case ModelType::OCR: info.name = "OCR"; break;
        case ModelType::FACE_ATTRIBUTE: info.name = "Face Attribute (age/gender)"; break;
        case ModelType::POSE_RTMO: info.name = "RTMO Pose"; break;
        case ModelType::SAM2_ENCODER: info.name = "SAM2 Encoder"; break;
        case ModelType::SAM2_DECODER: info.name = "SAM2 Decoder"; break;
        case ModelType::GENERIC_CLASSIFICATION:
          info.name = "Generic Classification";
          if (!info.outputShapes.empty() && info.outputShapes[0].size() == 2) {
            info.numClasses = info.outputShapes[0][1];
          }
          break;
        case ModelType::GENERIC_DETECTION: info.name = "Generic Detection"; break;
        default: info.name = "Unknown Model"; break;
      }
    }

    return info;
  }

  DetectionModel::TensorData DetectionModel::createInputTensor(const cv::Mat & processedFrame) {
    // Create input tensor shape and size (NCHW)
    std::vector<int64_t> inputShape = {1, 3, inputHeight_, inputWidth_};
    size_t inputTensorSize = 1 * 3 * inputHeight_ * inputWidth_;
    std::vector<float> inputTensorValues(inputTensorSize);

    // Convert BGR to RGB and normalize according to preprocessConfig_
    cv::Mat rgbFrame;
    try {
      cv::cvtColor(processedFrame, rgbFrame, cv::COLOR_BGR2RGB);
      if (rgbFrame.empty()) {
        ERROR_MSG("Failed to convert BGR to RGB - result is empty");
        throw std::runtime_error("BGR to RGB conversion failed");
      }

      rgbFrame.convertTo(rgbFrame, CV_32F);

      switch (preprocessConfig_.normMode) {
        case PreprocessConfig::IMAGENET:
          // pixel/255.0, then (pixel - mean) / std per channel
          rgbFrame /= 255.0f;
          {
            std::vector<cv::Mat> channels(3);
            cv::split(rgbFrame, channels);
            for (int c = 0; c < 3; ++c) {
              channels[c] = (channels[c] - preprocessConfig_.mean[c]) / preprocessConfig_.std[c];
            }
            cv::merge(channels, rgbFrame);
          }
          break;
        case PreprocessConfig::SCRFD_NORM:
          // (pixel - 127.5) / 128.0
          rgbFrame = (rgbFrame - 127.5f) / 128.0f;
          break;
        case PreprocessConfig::SCALE_01:
        default:
          rgbFrame /= 255.0f;
          break;
      }

      if (rgbFrame.empty()) {
        ERROR_MSG("Failed to normalize frame");
        throw std::runtime_error("Normalization failed");
      }
    } catch (const cv::Exception & e) {
      ERROR_MSG("OpenCV error during color conversion: %s", e.what());
      throw std::runtime_error("Color conversion failed");
    }

    if (rgbFrame.empty() || rgbFrame.type() != CV_32FC3) {
      ERROR_MSG("Failed to convert frame to float RGB - empty=%d, type=%d (expected %d)", rgbFrame.empty(), rgbFrame.type(), CV_32FC3);
      throw std::runtime_error("Invalid frame conversion");
    }

    // CHW layout via split + memcpy
    std::vector<cv::Mat> ch(3);
    cv::split(rgbFrame, ch);
    for (int c = 0; c < 3; ++c) {
      std::memcpy(inputTensorValues.data() + c * inputHeight_ * inputWidth_, ch[c].data, inputHeight_ * inputWidth_ * sizeof(float));
    }

    TensorData td;
    td.inputTensorValues = std::move(inputTensorValues);
    td.inputShape = inputShape;
    std::string err;
    OrtValue *val = runner_.createFloatTensor(td.inputTensorValues.data(), td.inputTensorValues.size(),
                                              td.inputShape, err);
    if (!val) {
      throw std::runtime_error("Failed to create input tensor: " + err);
    }
    td.inputTensor = val;
    return td;
  }

  // Explicit template instantiations for the types we use
  template cv::Mat
    Utils::drawDetectionsWithOptionalTracking<Detection>(const cv::Mat & image, const std::vector<Detection> & detections,
                                                         bool showTrackIds, bool showConfidence, bool withTracking);
  template cv::Mat
    Utils::drawDetectionsWithOptionalTracking<SegmentationDetection>(const cv::Mat & image,
                                                                     const std::vector<SegmentationDetection> & detections,
                                                                     bool showTrackIds, bool showConfidence, bool withTracking);

  template std::vector<uchar> Utils::encodeJPEG<Detection>(const cv::Mat & frame, const std::vector<Detection> & detections,
                                                           const ProcessingStats & stats, int quality, InferenceMetrics *metrics);
  template std::vector<uchar>
    Utils::encodeJPEG<SegmentationDetection>(const cv::Mat & frame, const std::vector<SegmentationDetection> & detections,
                                             const ProcessingStats & stats, int quality, InferenceMetrics *metrics);

  // ---- ModelRegistry implementation ----

  static const std::vector<ModelRegistryEntry> knownModels = {
    {"yolo11n",      "YOLO11 Nano - Detection (fastest)",          "yolo11n.onnx",      ModelType::YOLOV8_DETECTION,      640},
    {"yolo11s",      "YOLO11 Small - Detection",                   "yolo11s.onnx",      ModelType::YOLOV8_DETECTION,      640},
    {"yolo11m",      "YOLO11 Medium - Detection",                  "yolo11m.onnx",      ModelType::YOLOV8_DETECTION,      640},
    {"yolo11l",      "YOLO11 Large - Detection",                   "yolo11l.onnx",      ModelType::YOLOV8_DETECTION,      640},
    {"yolo11x",      "YOLO11 XLarge - Detection (most accurate)",  "yolo11x.onnx",      ModelType::YOLOV8_DETECTION,      640},
    {"yolo11n-seg",  "YOLO11 Nano - Segmentation",                 "yolo11n-seg.onnx",  ModelType::YOLOV8_SEGMENTATION,   640},
    {"yolo11s-seg",  "YOLO11 Small - Segmentation",                "yolo11s-seg.onnx",  ModelType::YOLOV8_SEGMENTATION,   640},
    {"yolo11m-seg",  "YOLO11 Medium - Segmentation",               "yolo11m-seg.onnx",  ModelType::YOLOV8_SEGMENTATION,   640},
    {"yolo11l-seg",  "YOLO11 Large - Segmentation",                "yolo11l-seg.onnx",  ModelType::YOLOV8_SEGMENTATION,   640},
    {"yolo11x-seg",  "YOLO11 XLarge - Segmentation",               "yolo11x-seg.onnx",  ModelType::YOLOV8_SEGMENTATION,   640},
    {"yolo11n-pose", "YOLO11 Nano - Pose Estimation",              "yolo11n-pose.onnx", ModelType::YOLOV8_POSE,           640},
    {"yolo11s-pose", "YOLO11 Small - Pose Estimation",             "yolo11s-pose.onnx", ModelType::YOLOV8_POSE,           640},
    {"yolo11m-pose", "YOLO11 Medium - Pose Estimation",            "yolo11m-pose.onnx", ModelType::YOLOV8_POSE,           640},
    {"yolo11l-pose", "YOLO11 Large - Pose Estimation",             "yolo11l-pose.onnx", ModelType::YOLOV8_POSE,           640},
    {"yolo11x-pose", "YOLO11 XLarge - Pose Estimation",            "yolo11x-pose.onnx", ModelType::YOLOV8_POSE,           640},
    {"yolo11n-cls",  "YOLO11 Nano - Classification",               "yolo11n-cls.onnx",  ModelType::YOLOV8_CLASSIFICATION, 224},
    {"yolo11s-cls",  "YOLO11 Small - Classification",              "yolo11s-cls.onnx",  ModelType::YOLOV8_CLASSIFICATION, 224},
    {"yolo11m-cls",  "YOLO11 Medium - Classification",             "yolo11m-cls.onnx",  ModelType::YOLOV8_CLASSIFICATION, 224},
    {"yolo11l-cls",  "YOLO11 Large - Classification",              "yolo11l-cls.onnx",  ModelType::YOLOV8_CLASSIFICATION, 224},
    {"yolo11x-cls",  "YOLO11 XLarge - Classification",             "yolo11x-cls.onnx",  ModelType::YOLOV8_CLASSIFICATION, 224},
    {"yolo11n-obb",  "YOLO11 Nano - Oriented Bounding Boxes",      "yolo11n-obb.onnx",  ModelType::YOLOV8_OBB,            640},
    {"yolo11s-obb",  "YOLO11 Small - Oriented Bounding Boxes",     "yolo11s-obb.onnx",  ModelType::YOLOV8_OBB,            640},
    {"yolo11m-obb",  "YOLO11 Medium - Oriented Bounding Boxes",    "yolo11m-obb.onnx",  ModelType::YOLOV8_OBB,            640},
    {"yolo11l-obb",  "YOLO11 Large - Oriented Bounding Boxes",     "yolo11l-obb.onnx",  ModelType::YOLOV8_OBB,            640},
    {"yolo11x-obb",  "YOLO11 XLarge - Oriented Bounding Boxes",    "yolo11x-obb.onnx",  ModelType::YOLOV8_OBB,            640},
    // YOLO26 Detection (NMS-free — auto-detect handles output format)
    {"yolo26n",      "YOLO26 Nano - Detection (fastest)",          "yolo26n.onnx",      ModelType::YOLO_SPLIT_DETECTION,  640},
    {"yolo26s",      "YOLO26 Small - Detection",                   "yolo26s.onnx",      ModelType::UNKNOWN,               640},
    {"yolo26m",      "YOLO26 Medium - Detection",                  "yolo26m.onnx",      ModelType::UNKNOWN,               640},
    {"yolo26l",      "YOLO26 Large - Detection",                   "yolo26l.onnx",      ModelType::UNKNOWN,               640},
    {"yolo26x",      "YOLO26 XLarge - Detection",                  "yolo26x.onnx",      ModelType::UNKNOWN,               640},
    // YOLO26 Segmentation
    {"yolo26n-seg",  "YOLO26 Nano - Segmentation",                 "yolo26n-seg.onnx",  ModelType::UNKNOWN,               640},
    {"yolo26s-seg",  "YOLO26 Small - Segmentation",                "yolo26s-seg.onnx",  ModelType::UNKNOWN,               640},
    {"yolo26m-seg",  "YOLO26 Medium - Segmentation",               "yolo26m-seg.onnx",  ModelType::UNKNOWN,               640},
    {"yolo26l-seg",  "YOLO26 Large - Segmentation",                "yolo26l-seg.onnx",  ModelType::UNKNOWN,               640},
    {"yolo26x-seg",  "YOLO26 XLarge - Segmentation",               "yolo26x-seg.onnx",  ModelType::UNKNOWN,               640},
    // YOLO26 Pose
    {"yolo26n-pose", "YOLO26 Nano - Pose Estimation",              "yolo26n-pose.onnx", ModelType::UNKNOWN,               640},
    {"yolo26s-pose", "YOLO26 Small - Pose Estimation",             "yolo26s-pose.onnx", ModelType::UNKNOWN,               640},
    {"yolo26m-pose", "YOLO26 Medium - Pose Estimation",            "yolo26m-pose.onnx", ModelType::UNKNOWN,               640},
    {"yolo26l-pose", "YOLO26 Large - Pose Estimation",             "yolo26l-pose.onnx", ModelType::UNKNOWN,               640},
    {"yolo26x-pose", "YOLO26 XLarge - Pose Estimation",            "yolo26x-pose.onnx", ModelType::UNKNOWN,               640},
    // YOLO26 Classification
    {"yolo26n-cls",  "YOLO26 Nano - Classification",               "yolo26n-cls.onnx",  ModelType::UNKNOWN,               224},
    {"yolo26s-cls",  "YOLO26 Small - Classification",              "yolo26s-cls.onnx",  ModelType::UNKNOWN,               224},
    {"yolo26m-cls",  "YOLO26 Medium - Classification",             "yolo26m-cls.onnx",  ModelType::UNKNOWN,               224},
    {"yolo26l-cls",  "YOLO26 Large - Classification",              "yolo26l-cls.onnx",  ModelType::UNKNOWN,               224},
    {"yolo26x-cls",  "YOLO26 XLarge - Classification",             "yolo26x-cls.onnx",  ModelType::UNKNOWN,               224},
    // YOLO26 OBB
    {"yolo26n-obb",  "YOLO26 Nano - Oriented Bounding Boxes",      "yolo26n-obb.onnx",  ModelType::UNKNOWN,               640},
    {"yolo26s-obb",  "YOLO26 Small - Oriented Bounding Boxes",     "yolo26s-obb.onnx",  ModelType::UNKNOWN,               640},
    {"yolo26m-obb",  "YOLO26 Medium - Oriented Bounding Boxes",    "yolo26m-obb.onnx",  ModelType::UNKNOWN,               640},
    {"yolo26l-obb",  "YOLO26 Large - Oriented Bounding Boxes",     "yolo26l-obb.onnx",  ModelType::UNKNOWN,               640},
    {"yolo26x-obb",  "YOLO26 XLarge - Oriented Bounding Boxes",    "yolo26x-obb.onnx",  ModelType::UNKNOWN,               640},
    // RT-DETR
    {"rtdetr-l",     "RT-DETR Large - NMS-free Detection",         "rtdetr-l.onnx",     ModelType::RT_DETR_DETECTION,     640},
    {"rtdetr-x",     "RT-DETR XLarge - NMS-free Detection",        "rtdetr-x.onnx",     ModelType::RT_DETR_DETECTION,     640},
    {"depth-anything-v2-small", "Depth Anything v2 Small",         "depth-anything-v2-small.onnx", ModelType::DEPTH_ESTIMATION, 518},
    {"depth-anything-v2-base",  "Depth Anything v2 Base",          "depth-anything-v2-base.onnx",  ModelType::DEPTH_ESTIMATION, 518},
    {"depth-anything-v2-large", "Depth Anything v2 Large",         "depth-anything-v2-large.onnx", ModelType::DEPTH_ESTIMATION, 518},
    {"scrfd-10g",    "SCRFD 10G - Face Detection",                 "scrfd-10g.onnx",    ModelType::FACE_DETECTION_SCRFD,  640},
    {"arcface-w600k-r50", "ArcFace w600k-r50 - Face Recognition",  "arcface-w600k-r50.onnx", ModelType::FACE_RECOGNITION_ARCFACE, 112},
    {"rtmo-s",       "RTMO Small - Multi-person Pose",             "rtmo-s.onnx",       ModelType::POSE_RTMO,             640},
    {"rtmo-l",       "RTMO Large - Multi-person Pose",             "rtmo-l.onnx",       ModelType::POSE_RTMO,             640},
    // Content moderation. Each lives in its own subdirectory with sidecar assets
    // (config.json / preprocessor_config.json / labels.txt) that prepare_models.sh
    // downloads alongside the model; labels and normalization come from those files
    // (see Utils::loadModelSidecars), not from code.
    {"nsfw-vit",     "NSFW image classification (ViT)",            "nsfw_vit/model.onnx",     ModelType::YOLOV8_CLASSIFICATION, 224},
    {"violence-vit", "Violence image classification (ViT)",        "violence_vit/model.onnx", ModelType::YOLOV8_CLASSIFICATION, 224},
    {"nudenet-320n", "NudeNet nudity detection 320 (fast)",        "nudenet/320n.onnx",       ModelType::YOLOV8_DETECTION,      320},
    {"nudenet-640m", "NudeNet nudity detection 640 (accurate)",    "nudenet/640m.onnx",       ModelType::YOLOV8_DETECTION,      640},
    // Image embedding (CLIP vision tower; 512-d embeddings on the meta track).
    // Registry-typed: [1,D] embedding heads can't be shape-auto-detected.
    {"clip-vitb32-vision", "CLIP ViT-B/32 - Image Embedding",      "clip_vitb32/model.onnx",  ModelType::IMAGE_EMBEDDING,       224},
    // Face age/gender attribute head ([1,2] = [age_years, gender_prob]). Registry-typed;
    // best used as a secondary model on SCRFD face crops (secondary_model=age-gender).
    {"age-gender",  "Face Age + Gender (ViT, on face crops)",      "age_gender/model.onnx",   ModelType::FACE_ATTRIBUTE,       224},
    // Single-file audio models (AudioModel adapter — NOT multi-file ASR bundles).
    // modality AUDIO + non-empty filename = single file; type picks the task.
    {"silero-vad",       "Silero VAD v5 - Voice Activity Detection", "silero_vad/model.onnx",       ModelType::AUDIO_VAD,            0, ModelModality::AUDIO},
    {"wav2vec2-emotion", "Speech Emotion Recognition (wav2vec2)",    "wav2vec2_emotion/model.onnx", ModelType::AUDIO_CLASSIFICATION, 0, ModelModality::AUDIO},
    {"ast-audioset",     "Audio Event Tagging (AST, 527 AudioSet classes)", "ast_audioset/model.onnx", ModelType::AUDIO_TAGGING,     0, ModelModality::AUDIO},
    {"wespeaker-resnet34", "Speaker Embedding (WeSpeaker ResNet34)",  "wespeaker_resnet34/model.onnx", ModelType::AUDIO_EMBEDDING,   0, ModelModality::AUDIO},
    // OCR (VISION bundle: detection + recognition + charset under subdir). Positional
    // fields after modality: subdir, then the 5 ASR slots (unused → nullptr), then the
    // OCR roles detFile/recFile/charsetFile.
    {"ppocr-v5-en", "PP-OCRv5 - Text Detection + Recognition (English)", "", ModelType::OCR, 0,
     ModelModality::VISION, "ppocr_v5_en", nullptr, nullptr, nullptr, nullptr, nullptr,
     "det.onnx", "rec.onnx", "dict.txt"},
    // Parakeet TDT 0.6B v3 — multilingual speech-to-text (AUDIO bundle: mel preproc +
    // Conformer encoder + TDT decoder/joint + vocab). Provision via
    // scripts/ONNX/prepare_models.sh <id>. Vision fields (type/inputSize) unused.
    // Positional fields: id,label,filename,type,inputSize,modality,subdir,encoderFile,
    // encoderDataFile,decoderFile,preprocFile,vocabFile. Only fp32 has an external-data sidecar.
    {"parakeet-tdt-0.6b-int8", "Parakeet TDT 0.6B v3 - Transcription (INT8, fastest)", "", ModelType::UNKNOWN, 0,
     ModelModality::AUDIO, "parakeet-tdt-0.6b-int8", "encoder-model.int8.onnx", nullptr, "decoder_joint-model.int8.onnx", "nemo128.onnx", "vocab.txt"},
    {"parakeet-tdt-0.6b-fp16", "Parakeet TDT 0.6B v3 - Transcription (FP16, balanced)", "", ModelType::UNKNOWN, 0,
     ModelModality::AUDIO, "parakeet-tdt-0.6b-fp16", "encoder-model.fp16.onnx", nullptr, "decoder_joint-model.fp16.onnx", "nemo128.onnx", "vocab.txt"},
    {"parakeet-tdt-0.6b-fp32", "Parakeet TDT 0.6B v3 - Transcription (FP32, most accurate)", "", ModelType::UNKNOWN, 0,
     ModelModality::AUDIO, "parakeet-tdt-0.6b-fp32", "encoder-model.onnx", "encoder-model.onnx.data", "decoder_joint-model.onnx", "nemo128.onnx", "vocab.txt"},
  };

  const std::vector<ModelRegistryEntry> & ModelRegistry::getAvailableModels() {
    return knownModels;
  }

  std::string ModelRegistry::getModelDir() {
    // A persistent per-user cache by default. MIST_MODEL_DIR remains the deployment
    // override for containers/shared caches; temporary storage is only a last resort.
    const char *envDir = getenv("MIST_MODEL_DIR");
    if (envDir && envDir[0]) {
      std::string d = envDir;
      if (d.back() != '/') { d += '/'; }
      return d;
    }
    const char *xdgDir = getenv("XDG_CACHE_HOME");
    if (xdgDir && xdgDir[0]) { return std::string(xdgDir) + "/mistserver/onnx/"; }
#ifdef _WIN32
    const char *localDir = getenv("LOCALAPPDATA");
    if (localDir && localDir[0]) { return std::string(localDir) + "/MistServer/onnx/"; }
#else
    const char *homeDir = getenv("HOME");
    if (homeDir && homeDir[0]) { return std::string(homeDir) + "/.cache/mistserver/onnx/"; }
#endif
    return Util::getTmpFolder() + "models/";
  }

  std::string ModelRegistry::getScriptDir() {
    // Read-only location of the provisioning scripts. Probe for prepare_models.sh so we
    // return a dir that actually contains it.
    auto hasScript = [](const std::string & dir) {
      return access((dir + "prepare_models.sh").c_str(), R_OK) == 0;
    };
    const char *envDir = getenv("MIST_ONNX_SCRIPTS");
    if (envDir && envDir[0]) {
      std::string d = envDir;
      if (d.back() != '/') { d += '/'; }
      if (hasScript(d)) { return d; }
    }
    std::string binDir = Util::getMyPath();
    if (!binDir.empty()) {
      if (hasScript(binDir)) { return binDir; }                    // installed: beside binary
      std::string dev = binDir + "../scripts/ONNX/";               // dev: binary in build/
      if (hasScript(dev)) { return dev; }
      dev = binDir + "scripts/ONNX/";                              // dev: binary in repo root
      if (hasScript(dev)) { return dev; }
    }
    return "";
  }

  std::string ModelRegistry::resolveModelPath(const std::string & modelIdOrPath) {
    if (modelIdOrPath.find('/') != std::string::npos || modelIdOrPath.find('\\') != std::string::npos) {
      return modelIdOrPath;
    }
    std::string dir = getModelDir();
    for (const auto & entry : knownModels) {
      if (modelIdOrPath == entry.id) {
        if (!entry.filename || !entry.filename[0]) {
          WARN_MSG("Model '%s' is a multi-file bundle; use resolveModelSet() (ASR) or "
                   "resolveOCRSet() (OCR). Provision it with: scripts/ONNX/prepare_models.sh %s",
                   entry.id, entry.id);
          return "";
        }
        std::string path = dir + entry.filename;
        if (access(path.c_str(), R_OK) == 0) { return path; }
        WARN_MSG("Model '%s' not found at %s. Run: scripts/ONNX/prepare_models.sh %s",
                 entry.id, path.c_str(), entry.id);
        return "";
      }
    }
    std::string cachePath = dir + modelIdOrPath + ".onnx";
    if (access(cachePath.c_str(), R_OK) == 0) { return cachePath; }
    return modelIdOrPath;
  }

  const ModelRegistryEntry * ModelRegistry::findModel(const std::string & id) {
    for (const auto & entry : knownModels) {
      if (id == entry.id) { return &entry; }
    }
    return nullptr;
  }

  namespace {
    // Resolve one registry file role to an on-disk path under `base`. Returns false
    // (with a WARN) if a required file is absent from the registry entry or disk; a
    // null/empty registry field for an optional role is skipped (leaves *out empty).
    bool resolveBundleFile(const char *file, const std::string & base, const std::string & modelId,
                           bool required, std::string *out) {
      if (!file || !file[0]) {
        if (required) {
          WARN_MSG("Bundle '%s' is missing a required file entry in the registry", modelId.c_str());
          return false;
        }
        return true;
      }
      std::string path = base + file;
      if (access(path.c_str(), R_OK) != 0) {
        WARN_MSG("Bundle file for '%s' not found at %s. Run: scripts/ONNX/prepare_models.sh %s",
                 modelId.c_str(), path.c_str(), modelId.c_str());
        return false;
      }
      *out = path;
      return true;
    }
  }

  ModelBundle ModelRegistry::resolveModelSet(const std::string & modelId) {
    ModelBundle bundle;
    const ModelRegistryEntry *entry = findModel(modelId);
    if (!entry || entry->modality != ModelModality::AUDIO || (entry->filename && entry->filename[0])) {
      WARN_MSG("Model '%s' is not a known audio bundle", modelId.c_str());
      return bundle;
    }
    std::string base = getModelDir();
    if (entry->subdir && entry->subdir[0]) { base += std::string(entry->subdir) + "/"; }

    // encoder/decoder/preproc/vocab are all required for an ASR bundle.
    if (!resolveBundleFile(entry->encoderFile, base, modelId, true, &bundle.encoder) ||
        !resolveBundleFile(entry->decoderFile, base, modelId, true, &bundle.decoderJoint) ||
        !resolveBundleFile(entry->preprocFile, base, modelId, true, &bundle.preproc) ||
        !resolveBundleFile(entry->vocabFile, base, modelId, true, &bundle.vocab)) {
      return bundle;
    }
    // ORT external-data sidecar (fp32): the encoder .onnx references it by filename, so it
    // must sit next to the encoder or the load fails deep inside ORT. Validate it here so a
    // half-downloaded bundle doesn't falsely pass provisioning.
    if (entry->encoderDataFile && entry->encoderDataFile[0]) {
      std::string dataPath = base + entry->encoderDataFile;
      if (access(dataPath.c_str(), R_OK) != 0) {
        WARN_MSG("External-data file for '%s' not found at %s (encoder weights). Re-run: "
                 "scripts/ONNX/prepare_models.sh %s", modelId.c_str(), dataPath.c_str(), modelId.c_str());
        return bundle;
      }
    }
    bundle.ok = true;
    return bundle;
  }

  OCRBundle ModelRegistry::resolveOCRSet(const std::string & modelId) {
    OCRBundle bundle;
    const ModelRegistryEntry *entry = findModel(modelId);
    if (!entry || entry->type != ModelType::OCR) {
      WARN_MSG("Model '%s' is not a known OCR bundle", modelId.c_str());
      return bundle;
    }
    std::string base = getModelDir();
    if (entry->subdir && entry->subdir[0]) { base += std::string(entry->subdir) + "/"; }
    // OCR file roles map onto the registry's generic bundle fields: det=detFile,
    // rec=recFile, dict=charsetFile (see the registry entry).
    if (!resolveBundleFile(entry->detFile, base, modelId, true, &bundle.det) ||
        !resolveBundleFile(entry->recFile, base, modelId, true, &bundle.rec) ||
        !resolveBundleFile(entry->charsetFile, base, modelId, true, &bundle.dict)) {
      return bundle;
    }
    bundle.ok = true;
    return bundle;
  }

  bool ModelRegistry::isKnownModelId(const std::string & id) {
    for (const auto & entry : knownModels) {
      if (id == entry.id) { return true; }
    }
    return false;
  }

  bool ModelRegistry::provision(const std::string & id, std::string & hint) {
    std::string scriptDir = getScriptDir();
    if (scriptDir.empty()) {
      hint = "provisioning script prepare_models.sh not found; install the MistServer ONNX "
             "scripts or set MIST_ONNX_SCRIPTS to their directory";
      return false;
    }
    std::string modelDir = getModelDir();
    // Run the bundled script via `/bin/sh -c` with stderr merged into stdout, so the
    // captured hint includes curl/wget/Python failure messages (which land on stderr;
    // getOutputOf captures stdout only). Single-quote each argument to keep it safe.
    auto shq = [](const std::string & s) {
      std::string out = "'";
      for (char c : s) { if (c == '\'') { out += "'\\''"; } else { out += c; } }
      out += "'";
      return out;
    };
    std::string cmd = shq(scriptDir + "prepare_models.sh") + " --dir " + shq(modelDir) + " " + shq(id) + " 2>&1";
    std::deque<std::string> args;
    args.push_back("/bin/sh");
    args.push_back("-c");
    args.push_back(cmd);
    INFO_MSG("Provisioning model '%s' via %sprepare_models.sh (dir: %s)...", id.c_str(),
             scriptDir.c_str(), modelDir.c_str());
    // Large cap for multi-GB downloads; getOutputOf returns as soon as the child exits.
    hint = Util::Procs::getOutputOf(args, 3600000);
    // Success signal is what actually matters: the model now resolves on disk. Each
    // model shape has its own resolver — an OCR bundle, an ASR bundle, or a single file.
    const ModelRegistryEntry *e = findModel(id);
    bool ok;
    if (e && e->type == ModelType::OCR) {
      ok = resolveOCRSet(id).ok;
    } else if (e && e->modality == ModelModality::AUDIO && (!e->filename || !e->filename[0])) {
      ok = resolveModelSet(id).ok;
    } else {
      ok = !resolveModelPath(id).empty();
    }
    if (ok) {
      INFO_MSG("Provisioned model '%s'", id.c_str());
    } else {
      WARN_MSG("Provisioning '%s' did not produce the expected files", id.c_str());
    }
    return ok;
  }

} // namespace ONNX
