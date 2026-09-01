#pragma once

#include "json.h"
#include "onnxruntime_c_api.h"
#include "util.h"

#include <memory>
#include <mutex>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/video/tracking.hpp>
#if __has_include(<opencv2/geometry.hpp>)
#include <opencv2/geometry.hpp>
#endif
#include <cstdint>
#include <string>
#include <vector>

namespace ONNX {

  // Minimal helpers for the C API to ease migration
  namespace ORTHelpers {
    // Accessor for the Ort API table (cached per-process)
    const OrtApi *api();

    // The single process-wide OrtEnv, created once and intentionally leaked (never
    // released). ONNX Runtime expects one OrtEnv per process, and releasing it during
    // C++ static/atexit teardown makes ORT's LoggingManager destructor terminate — so all
    // SessionRunners share this and none owns/releases an env. Returns nullptr on failure.
    OrtEnv *sharedEnv();

    // Extract tensor shape into a vector
    std::vector<int64_t> getTensorShape(const OrtTensorTypeAndShapeInfo *info);

    // Utility to convert a shape vector to a human-readable string
    std::string shapeToString(const std::vector<int64_t> & dims);

    // RAII guard for a single OrtValue* — releases on scope exit
    struct OrtValueGuard {
        OrtValue *&val;
        OrtValueGuard(OrtValue *&v) : val(v) {}
        ~OrtValueGuard() { if (val) { api()->ReleaseValue(val); val = nullptr; } }
        OrtValueGuard(const OrtValueGuard &) = delete;
        OrtValueGuard &operator=(const OrtValueGuard &) = delete;
    };

    // RAII guard for a vector of OrtValue* — releases all on scope exit
    struct OrtOutputsGuard {
        std::vector<OrtValue *> &vals;
        OrtOutputsGuard(std::vector<OrtValue *> &v) : vals(v) {}
        ~OrtOutputsGuard() { const OrtApi *a = api(); for (auto *&v : vals) { if (v) { a->ReleaseValue(v); v = nullptr; } } }
        OrtOutputsGuard(const OrtOutputsGuard &) = delete;
        OrtOutputsGuard &operator=(const OrtOutputsGuard &) = delete;
    };
  } // namespace ORTHelpers

  // One model input or output port, as reported by the loaded ONNX model.
  struct TensorSpec {
      std::string name;
      std::vector<int64_t> shape;                                     // -1 marks dynamic dims
      ONNXTensorElementDataType dtype = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
  };

  // Owned tensor used by the modality-neutral API and the ONNXTENSOR track codec.
  // `bytes` is a contiguous, little-endian representation of the tensor elements.
  struct TensorData {
      std::string name;
      std::vector<int64_t> shape;
      ONNXTensorElementDataType dtype = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
      std::vector<uint8_t> bytes;
  };

  namespace TensorWire {
    static const uint8_t VERSION = 1;
    static const size_t DEFAULT_MAX_PACKET_BYTES = 64 * 1024 * 1024;
    size_t elementSize(ONNXTensorElementDataType dtype);
    std::string dtypeName(ONNXTensorElementDataType dtype);
    ONNXTensorElementDataType parseDtype(const std::string &name);
    bool encode(const std::vector<TensorData> &tensors, std::vector<uint8_t> &packet,
                std::string &err, size_t maxPacketBytes = DEFAULT_MAX_PACKET_BYTES);
    bool decode(const void *packet, size_t packetBytes, std::vector<TensorData> &tensors,
                std::string &err, size_t maxPacketBytes = DEFAULT_MAX_PACKET_BYTES);
  }

  // Modality-agnostic wrapper around an ONNX Runtime session. Owns the ORT env,
  // session options, session and default allocator; enumerates the model's I/O
  // ports; and runs N-input / N-output inference. It makes NO assumptions about
  // image data, tensor rank, channel count, or dtype — that is the concern of the
  // adapter layered on top (e.g. DetectionModel for vision, ASRModel for audio). This is
  // the seam both adapters plug into without touching each other's code.
  class SessionRunner {
    public:
      SessionRunner();
      ~SessionRunner();
      SessionRunner(const SessionRunner &) = delete;
      SessionRunner &operator=(const SessionRunner &) = delete;

      // Configure session options, register execution providers, create the session
      // and enumerate I/O ports. requestedEP: "" = auto-detect best available,
      // "cpu" = force CPU only, or a named EP ("cuda"/"tensorrt"/"coreml"/"openvino").
      // numThreads is the intra-op thread count (clamped to >=1). lowLatency enables intra-op
      // hot-spinning (off by default — it burns a core per session, bad for many processes).
      // Returns false and fills err on failure.
      bool load(const std::string &modelPath, int numThreads,
                const std::string &requestedEP, std::string &err, bool lowLatency = false);

      bool loaded() const { return session_ != nullptr; }
      const std::vector<TensorSpec> &inputs() const { return inputSpecs_; }
      const std::vector<TensorSpec> &outputs() const { return outputSpecs_; }
      size_t numInputs() const { return inputSpecs_.size(); }
      size_t numOutputs() const { return outputSpecs_.size(); }
      const std::string &activeEP() const { return activeEP_; }
      OrtSession *session() const { return session_; }
      OrtMemoryInfo *memInfo() const { return memInfo_; }

      // Run inference. inputs are borrowed (caller retains ownership and releases
      // them). outputs is resized to numOutputs() and filled with owned OrtValue*
      // that the caller must release (e.g. via ORTHelpers::OrtOutputsGuard).
      // Returns false and fills err on failure.
      // With state loops bound (bindStateLoop), pass nullptr at each bound input's
      // position — the runner substitutes its owned state tensor there.
      bool run(const std::vector<OrtValue *> &inputs, std::vector<OrtValue *> &outputs,
               std::string &err);

      // Fully generic owned-tensor interface. Inputs are matched to model ports by name
      // (or by position when name is empty); all tensor outputs are copied with their
      // original names, shapes and element types. No media assumptions or truncation.
      bool runTensors(const std::vector<TensorData> &inputs,
                      std::vector<TensorData> &outputs, std::string &err);

      // Stateful streaming models: bind an output port to an input port by name
      // (e.g. Silero VAD's "stateN" -> "state"). The runner then owns that state:
      // pass nullptr at the bound input's position in run() and the previous run's
      // bound output (zeros before the first run) is fed in automatically; after each
      // successful run() the bound output is copied back into the owned buffer.
      // Float32 ports only. Dynamic dims in the input port's shape seed as 1; if the
      // model emits a differently-sized state, the buffer adopts the emitted shape.
      bool bindStateLoop(const std::string &outputName, const std::string &inputName,
                         std::string &err);
      // Zero all bound state buffers (stream restart / discontinuity).
      void resetState();
      bool hasStateLoops() const { return !stateLoops_.empty(); }

      // Wrap a caller-owned float buffer as an OrtValue tensor WITHOUT copying; the
      // buffer must outlive the returned tensor. Returns nullptr and fills err on
      // failure. Caller owns the returned tensor.
      OrtValue *createFloatTensor(const float *data, size_t count,
                                  const std::vector<int64_t> &shape, std::string &err);

      // Integer counterparts of createFloatTensor, for models with integer input ports
      // (e.g. an RNN-T/TDT decoder's token/length tensors). Pick the one matching the
      // port's TensorSpec::dtype. Same borrow-no-copy contract as createFloatTensor.
      OrtValue *createInt32Tensor(const int32_t *data, size_t count,
                                  const std::vector<int64_t> &shape, std::string &err);
      OrtValue *createInt64Tensor(const int64_t *data, size_t count,
                                  const std::vector<int64_t> &shape, std::string &err);

      // Create a float-valued input tensor matching a port's element type. FLOAT is wrapped
      // zero-copy (buffer must outlive the tensor); FLOAT16 is allocated and converted, so
      // the source buffer need not outlive it. Use this instead of createFloatTensor when a
      // port may be FP16 (e.g. an fp16 model variant).
      OrtValue *createRealTensor(const float *data, size_t count,
                                 const std::vector<int64_t> &shape,
                                 ONNXTensorElementDataType dtype, std::string &err);

    private:
      OrtEnv *env_ = nullptr;
      OrtSessionOptions *opts_ = nullptr;
      OrtSession *session_ = nullptr;
      OrtMemoryInfo *memInfo_ = nullptr;
      OrtAllocator *allocator_ = nullptr;                 // default allocator (not owned)
      std::vector<char *> inputNameStrings_;              // owned, freed via allocator_
      std::vector<char *> outputNameStrings_;
      std::vector<const char *> inputNames_;              // views into *NameStrings_
      std::vector<const char *> outputNames_;
      std::vector<TensorSpec> inputSpecs_;
      std::vector<TensorSpec> outputSpecs_;
      std::string activeEP_ = "CPU";

      // Output->input state loops for stateful streaming models (see bindStateLoop)
      struct StateLoop {
          size_t outIdx;
          size_t inIdx;
          std::vector<int64_t> shape; // concrete shape (dynamic dims seeded as 1)
          std::vector<float> buffer;  // owned state, zeroed by resetState()
      };
      std::vector<StateLoop> stateLoops_;
  };

  // Detection result structure
  struct Detection {
      float x, y, w, h; // Normalized coordinates (0-1)
      float confidence;
      int class_id;
      std::string class_name;
      uint64_t track_id = 0; // For temporal tracking
      uint64_t first_seen_time = 0; // Timestamp when first detected (milliseconds)
      uint64_t last_seen_time = 0; // Timestamp when last detected (milliseconds)

      // Enhanced tracking features
      float track_confidence = 0.0f; // Confidence in track stability

      // Simple trail system - fixed buffer of position history
      std::vector<cv::Point2f> trail; // Position history for drawing trails
      static const size_t MAX_TRAIL_LENGTH = 30; // Maximum trail points (1 sec at 30 fps)

      // Kalman filter for robust state estimation (x, y, w, h, vx, vy)
      std::shared_ptr<cv::KalmanFilter> kalmanFilter;
      bool kalmanInitialized = false;

      // Helper methods for time-based tracking
      uint64_t getTrackDurationMs() const {
        return last_seen_time > first_seen_time ? last_seen_time - first_seen_time : 0;
      }
      uint64_t getTimeSinceLastSeenMs(uint64_t currentTime) const {
        return currentTime > last_seen_time ? currentTime - last_seen_time : 0;
      }

      // Trail management - simple position history
      cv::Point2f getCenter() const { return cv::Point2f(x + w / 2, y + h / 2); }

      void addTrailPoint() {
        // Add center if moved enough since last point
        cv::Point2f currentPos = getCenter();
        const float minStep = 0.01f; // 1% of normalized diagonal
        if (!trail.empty()) {
          cv::Point2f lastPos = trail.back();
          if (cv::norm(currentPos - lastPos) < minStep) { return; }
        }
        trail.push_back(currentPos);
        if (trail.size() > MAX_TRAIL_LENGTH) { trail.erase(trail.begin()); }
      }

      void clearTrail() { trail.clear(); }
  };

  // Pose keypoint structure
  struct Keypoint {
      float x, y; // Normalized coordinates (0-1)
      float confidence;
      bool visible;
  };

  // Pose detection result (extends Detection)
  struct PoseDetection : public Detection {
      std::vector<Keypoint> keypoints; // 17 COCO keypoints
      float pose_confidence; // Overall pose confidence
  };

  // Segmentation result (extends Detection)
  struct SegmentationDetection : public Detection {
      cv::Mat mask; // Segmentation mask for this detection
      std::vector<cv::Point> contour; // Contour points
      float mask_confidence; // Mask quality score
  };

  // Oriented bounding box result (extends Detection)
  struct OBBDetection : public Detection {
      float angle; // Rotation angle in radians
      cv::Point2f center; // Center point
      cv::Size2f size; // Width and height
      std::vector<cv::Point2f> corners; // 4 corner points
  };

  // One class score (used for classification top-K lists)
  struct ClassScore {
      int class_id;
      std::string class_name;
      float confidence;
  };

  // Classification result
  struct ClassificationResult {
      int class_id;
      std::string class_name;
      float confidence;
      uint64_t timestamp;
      std::vector<ClassScore> top; // top-K classes, best first; filled only when topK > 1
  };

  // Performance metrics for a single inference
  struct InferenceMetrics {
      int64_t preprocessTimeMs = 0;
      int64_t inferenceTimeMs = 0;
      int64_t postprocessTimeMs = 0;
      int64_t jpegEncodeTimeMs = 0;
      int64_t totalTimeMs = 0;

      // Enhanced bottleneck detection metrics
      int64_t videoDecodeTimeMs = 0; // Video decoding time
      int64_t temporalTrackingTimeMs = 0; // Temporal tracking time
      int64_t sceneChangeTimeMs = 0; // Scene change detection time
      int64_t tensorCreationTimeMs = 0; // ONNX tensor creation time
      int64_t tensorCopyTimeMs = 0; // Input/output tensor copy time
      int64_t nmsTimeMs = 0; // NMS processing time
      int64_t kalmanFilterTimeMs = 0; // Kalman filter update time

      size_t inputWidth = 0;
      size_t inputHeight = 0;
      size_t detectionCount = 0;
      size_t trackedObjectCount = 0; // Number of tracked objects
      size_t newTrackCount = 0; // Number of new tracks created
      size_t lostTrackCount = 0; // Number of tracks lost

      // Memory usage metrics
      size_t peakMemoryUsageMB = 0; // Peak memory during processing
      size_t tensorMemoryMB = 0; // Memory used by tensors
  };

  // One recognized text line (OCR)
  struct OCRLine {
      std::string text;
      float confidence; // mean per-character probability of the kept CTC symbols
      float x, y, w, h; // axis-aligned bounding box, normalized [0,1] in the source frame
  };

  // OCR result for one frame
  struct OCRResult {
      std::vector<OCRLine> lines; // reading order (top-to-bottom, left-to-right)
      std::string text;           // all line texts joined with newlines
      InferenceMetrics metrics;
      bool ok = false;
  };

  // Comprehensive processing statistics (matches original process_onnx.cpp)
  struct ProcessingStats {
      uint64_t totalFrames = 0;
      uint64_t totalDetections = 0;
      uint64_t totalInferenceTimeMs = 0;
      uint64_t totalPreprocessTimeMs = 0;
      uint64_t totalPostprocessTimeMs = 0;
      uint64_t totalJpegEncodeTimeMs = 0;

      // Enhanced bottleneck tracking
      uint64_t totalVideoDecodeTimeMs = 0;
      uint64_t totalTemporalTrackingTimeMs = 0;
      uint64_t totalSceneChangeTimeMs = 0;
      uint64_t totalTensorCreationTimeMs = 0;
      uint64_t totalTensorCopyTimeMs = 0;
      uint64_t totalNmsTimeMs = 0;
      uint64_t totalKalmanFilterTimeMs = 0;

      uint64_t framesWithDetections = 0;
      uint64_t sceneChangesDetected = 0;
      uint64_t totalTracksCreated = 0;
      uint64_t totalTracksLost = 0;
      uint64_t lastStatsTime = 0;

      // Averages
      double avgInferenceMs = 0.0;
      double avgPreprocessMs = 0.0;
      double avgPostprocessMs = 0.0;
      double avgJpegEncodeMs = 0.0;
      double avgVideoDecodeMs = 0.0;
      double avgTemporalTrackingMs = 0.0;
      double avgSceneChangeMs = 0.0;
      double avgTensorCreationMs = 0.0;
      double avgTensorCopyMs = 0.0;
      double avgNmsMs = 0.0;
      double avgKalmanFilterMs = 0.0;
      double avgDetectionsPerFrame = 0.0;
      double avgTrackedObjectsPerFrame = 0.0;
      double fps = 0.0;

      // ONNX inference timing analysis
      int64_t maxInferenceTimeMs = 0; // Maximum inference time seen
      int64_t minInferenceTimeMs = INT64_MAX; // Minimum inference time seen
      uint64_t inferenceTimesCount = 0; // Count for rolling average
      double rollingAvgInferenceMs = 0.0; // Rolling average for recent frames

      std::string lastCodec;
      uint64_t lastWidth = 0;
      uint64_t lastHeight = 0;

      // Thread safety
      mutable std::mutex statsMutex;

      void updateStats(const InferenceMetrics & metrics, int detectionCount, const std::string & codec, uint64_t width, uint64_t height);
      void calculateAverages();
      void logStats() const;
  };

  // Temporal tracking for detection stability
  class TemporalTracker {
    public:
      TemporalTracker(float iouThreshold = 0.3f, int minConsecutiveMs = 1000, int maxMissingMs = 2000);

      // Update tracks with new detections (now takes timestamp)
      std::vector<Detection> updateTracks(const std::vector<Detection> & newDetections, uint64_t timestamp);

      // Clear all tracks
      void clearTracks() { tracks_.clear(); }

      // Soft reset for scene changes - preserve tracks for objects still present
      void softReset(const std::vector<Detection> & currentDetections, uint64_t timestamp);

      // Get current track count
      size_t getTrackCount() const { return tracks_.size(); }

      // Set tracking parameters (now in milliseconds)
      void setParameters(float iouThreshold, int minConsecutiveMs, int maxMissingMs) {
        iouThreshold_ = iouThreshold;
        minConsecutiveMs_ = minConsecutiveMs;
        maxMissingMs_ = maxMissingMs;
      }

      // Kalman filter configuration
      void enableKalmanFilter(bool enable) { useKalmanFilter_ = enable; }
      void setKalmanProcessNoise(float processNoise) { kalmanProcessNoise_ = processNoise; }
      void setKalmanMeasurementNoise(float measurementNoise) { kalmanMeasurementNoise_ = measurementNoise; }

    private:
      std::vector<Detection> tracks_;
      uint64_t nextTrackId_;
      float iouThreshold_;
      int minConsecutiveMs_; // Minimum time in milliseconds before detection appears
      int maxMissingMs_; // Maximum time in milliseconds before track is removed

      // Kalman filter configuration
      bool useKalmanFilter_ = true;
      float kalmanProcessNoise_ = 0.01f;
      float kalmanMeasurementNoise_ = 0.1f;

      float calculateIoU(const Detection & a, const Detection & b);
      float calculateIoUWithPrediction(Detection & track, const Detection & detection, uint64_t timestamp);

      // Kalman filter helper methods
      void initializeKalmanFilter(Detection & detection);
      void updateKalmanFilter(Detection & track, const Detection & measurement, uint64_t timestamp);
      Detection predictKalmanState(Detection & track, uint64_t timestamp);

      // Prediction validation to prevent "flying boxes"
      bool isValidPrediction(const Detection & predicted, const Detection & original);
  };

  // Video packet structure (matches original process_onnx.cpp)
  struct VideoPacket {
      Util::ResizeablePointer packetData; // Use ResizeablePointer for safe data copying
      uint64_t timestamp;
      size_t trackIdx;
      std::string codec;
      uint64_t width;
      uint64_t height;
  };

  // Processed video frame structure (matches original process_onnx.cpp)
  struct ProcessedVideoFrame {
      Util::ResizeablePointer jpegData; // JPEG encoded frame with bounding boxes
      uint64_t timestamp;
      uint64_t width;
      uint64_t height;
      int detectionCount;
  };

  // Model type enumeration for auto-detection
  enum class ModelType {
    UNKNOWN,
    // YOLO family (standard raw output)
    YOLOV8_DETECTION,
    YOLOV8_POSE,
    YOLOV8_SEGMENTATION,
    YOLOV8_CLASSIFICATION,
    YOLOV8_OBB,
    YOLO11_DETECTION,
    YOLO11_POSE,
    YOLO11_SEGMENTATION,
    YOLO11_CLASSIFICATION,
    YOLO11_OBB,
    // YOLO with baked-in NMS (output [1, 300, 6])
    YOLO_NMS_DETECTION,
  // End-to-end, NMS-free detection export with separate class logits [1,N,C] and
  // normalized cxcywh boxes [1,N,4] (the verified YOLO26 Hugging Face ONNX layout).
    YOLO_SPLIT_DETECTION,
    // RT-DETR (NMS-free transformer detection)
    RT_DETR_DETECTION,
    // Depth estimation
    DEPTH_ESTIMATION,
    // Face pipeline
    FACE_DETECTION_SCRFD,
    FACE_RECOGNITION_ARCFACE,
    // Pose (non-YOLO)
    POSE_RTMO,
    // SAM2 (promptable segmentation)
    SAM2_ENCODER,
    SAM2_DECODER,
    // Generic image embedding (CLIP vision tower etc). Never shape-auto-detected: a
    // [1,D] embedding head is indistinguishable from a [1,N] classifier head, so this
    // type is always set by a registry entry or explicit model_type override.
    IMAGE_EMBEDDING,
    // Two-stage text recognition (PP-OCR det + rec + charset). Registry-routed vision
    // bundle: three files, so it never goes through single-file auto-detection.
    OCR,
    // Face attribute head: [1,2] raw output = [age_years, gender_prob]. Registry-routed
    // (a [1,2] head is shape-ambiguous); meant as a secondary model on face crops.
    FACE_ATTRIBUTE,
    // Single-file audio models (modality AUDIO, handled by AudioModel — not the
    // DetectionModel hierarchy). Registry-typed; the type picks the task semantics:
    AUDIO_VAD,            // streaming voice activity detection (Silero: fixed chunks + state)
    AUDIO_CLASSIFICATION, // windowed single-label classification (speech emotion etc)
    AUDIO_TAGGING,        // windowed multi-label tagging, sigmoid scores (AudioSet/AST)
    AUDIO_EMBEDDING,      // windowed embedding (speaker id / WeSpeaker)
    // Generic fallbacks
    GENERIC_CLASSIFICATION,
    GENERIC_DETECTION,
    GENERIC_UNKNOWN
  };

  // Coarse input modality, decided from the model's input port(s) before the
  // vision-specific output classifier runs. VISION has the DetectionModel family; AUDIO
  // has ASRModel (transcription). The gate keeps each modality's models out of the other's
  // pipeline; UNKNOWN is recognised and reported rather than force-fit.
  enum class ModelModality { VISION, AUDIO, TENSOR, UNKNOWN };

  // Model information structure
  struct ModelInfo {
      ModelType type = ModelType::UNKNOWN;
      std::string name;
      std::string version;
      std::vector<std::vector<int64_t>> inputShapes;
      std::vector<std::vector<int64_t>> outputShapes;
      std::vector<std::string> inputNames;
      std::vector<std::string> outputNames;
      int numClasses = 0;
  };

  // Preprocessing configuration for different model families
  struct PreprocessConfig {
      // LETTERBOX: scale to fit + pad (YOLO). DIRECT_RESIZE: stretch to WxH.
      // CENTER_CROP: scale the short edge to the target, then crop the center (the HF
      // "shortest_edge" + do_center_crop convention, e.g. CLIP). CENTER_CROP is for
      // whole-frame models (classification/embedding) — detection coordinate remapping
      // does not account for the crop.
      enum ResizeMode { LETTERBOX, DIRECT_RESIZE, CENTER_CROP };
      enum NormMode { SCALE_01, IMAGENET, SCRFD_NORM };

      ResizeMode resizeMode = LETTERBOX;
      NormMode normMode = SCALE_01;
      float letterboxPadColor[3] = {114.0f, 114.0f, 114.0f};
      // ImageNet mean/std (used when normMode == IMAGENET)
      float mean[3] = {0.485f, 0.456f, 0.406f};
      float std[3] = {0.229f, 0.224f, 0.225f};
  };

  // Depth estimation result
  struct DepthResult {
      cv::Mat depthMap;
      InferenceMetrics metrics;
  };

  // Face detection with landmarks (extends Detection)
  struct FaceDetection : public Detection {
      float landmarks[10]; // 5 facial landmarks (x,y pairs), normalized [0,1]
  };

  // Face embedding result
  struct FaceEmbedding {
      std::vector<float> embedding; // 512-d normalized vector
      float confidence;
      InferenceMetrics metrics;
  };

  // SAM2 segmentation result
  struct SAM2Result {
      std::vector<cv::Mat> masks; // Candidate binary masks
      std::vector<float> iouScores; // Quality score per mask
      InferenceMetrics metrics;
  };

  // Generic ONNX result for unknown models
  struct GenericResult {
      uint64_t timestamp;
      std::string modelName;
      std::string modelType;
      JSON::Value rawOutput; // Raw tensor data as JSON
      InferenceMetrics metrics;
  };

  // Vision adapter over SessionRunner.
  //
  // DetectionModel is the base of the VISION model family: it owns a SessionRunner
  // (the neutral runtime) and adds image-specific concerns — letterbox/resize,
  // BGR->RGB + normalization (PreprocessConfig), a {1,3,H,W} float input tensor
  // (preprocessImage + createInputTensor), and detection/pose/segmentation/etc.
  // output parsing via the parseOutput() override in each subclass. The 4D-NCHW,
  // 3-channel input assumption lives HERE, not in SessionRunner.
  //
  // Transcription seam: the audio model (ASRModel, Parakeet TDT) is a SIBLING adapter
  // over SessionRunner — NOT a DetectionModel subclass. It owns an audio->feature
  // preprocessor (mel) that produces the model's input tensor(s) via
  // SessionRunner::createFloatTensor, drives N-input inference through SessionRunner::run,
  // and decodes the output tensors into timed text segments. The modality gate
  // (classifyModality / ModelFactory::detectModality) keeps each modality out of the
  // other's path.
  class DetectionModel {
    public:
      DetectionModel(const std::string & modelPath, int inputSize = 640);
      virtual ~DetectionModel();

      // Initialize the model and auto-detect type
      bool initialize(int numThreads = 1);

      // Auto-detect model type from metadata
      ModelType detectModelType();
      ModelInfo getModelInfo() const { return modelInfo_; }

      // Process a frame and return detections (for known models)
      virtual std::vector<Detection> processFrame(const cv::Mat & frame, float confThreshold = 0.5f,
                                                  float nmsThreshold = 0.4f,
                                                  InferenceMetrics *metrics = nullptr);

      // Process frame with generic fallback (for unknown models)
      GenericResult processFrameGeneric(const cv::Mat & frame, InferenceMetrics *metrics = nullptr);

      // Get model info
      const std::string & getModelPath() const { return modelPath_; }
      int getInputWidth() const { return inputWidth_; }
      int getInputHeight() const { return inputHeight_; }
      bool isInitialized() const { return initialized_; }
      ModelType getModelType() const { return modelInfo_.type; }

      // Override auto-detected model type (set before initialize(), or call after to force)
      void setModelTypeOverride(ModelType type) { modelTypeOverride_ = type; }

      // Configuration
      void setImageEnhancement(bool enable) { enhanceImage_ = enable; }
      bool getImageEnhancement() const { return enhanceImage_; }
      void setSoftNmsSigma(float sigma) { softNmsSigma_ = sigma; }
      float getSoftNmsSigma() const { return softNmsSigma_; }
      void setLetterbox(bool enable) { useLetterbox_ = enable; }
      bool getLetterbox() const { return useLetterbox_; }
      void setPreprocessConfig(const PreprocessConfig & cfg) { preprocessConfig_ = cfg; }
      const PreprocessConfig & getPreprocessConfig() const { return preprocessConfig_; }
      // Custom class labels (from a model sidecar / registry asset, index = class id).
      // When set, they take precedence over the built-in COCO/ImageNet/DOTA tables in
      // every parser; ids outside the list resolve to "class_<id>" (no COCO bleed-through).
      void setClassLabels(const std::vector<std::string> & labels) { classLabels_ = labels; }
      const std::vector<std::string> & getClassLabels() const { return classLabels_; }

      // Execution provider preference (set before initialize())
      // Empty string = auto-detect best available; "cpu" = force CPU only
      void setExecutionProvider(const std::string & ep) { requestedEP_ = ep; }
      const std::string & getExecutionProvider() const { return activeEP_; }

      // Enable intra-op hot-spinning (lower latency, one busy core per session). Off by
      // default; set before initialize(). See SessionRunner::load.
      void setLowLatency(bool enable) { lowLatency_ = enable; }

    protected:
      // Virtual methods for different model types
      virtual std::vector<Detection> parseOutput(float *outputData, const std::vector<int64_t> & outputShape,
                                                 float confThreshold, const cv::Size & originalSize) = 0;

      // Undo letterbox transform on normalized [0,1] coordinates
      void remapLetterboxCoords(Detection & det);

      // Helper methods
      cv::Mat preprocessImage(const cv::Mat & image);
      cv::Mat enhanceImage(const cv::Mat & image);
      std::vector<Detection> applyNMS(const std::vector<Detection> & detections, float nmsThreshold);
      std::vector<Detection> applySoftNMS(const std::vector<Detection> & detections, float nmsThreshold,
                                          float confThreshold, float sigma = 0.5f);
      float calculateIoU(const Detection & a, const Detection & b);
      float calculateDIoU(const Detection & a, const Detection & b);

      // Generic processing for unknown models
      JSON::Value tensorToJSON(float *data, const std::vector<int64_t> & shape, const std::string & name);

      // Unified preprocessing and tensor creation
      struct TensorData {
          std::vector<float> inputTensorValues;
          std::vector<int64_t> inputShape;
          OrtValue *inputTensor = nullptr;

          TensorData() = default;
      };

      TensorData createInputTensor(const cv::Mat & processedFrame);

      // Run inference through runner_ with the given input tensors, throwing
      // std::runtime_error (tagged with ctx) on failure. outputs is resized and
      // filled by the runner; the caller owns it (guard with OrtOutputsGuard).
      void runSession(const std::vector<OrtValue *> & inputs, std::vector<OrtValue *> & outputs,
                      const char *ctx);

      // Resolve a class id to a name: custom labels first, then the given built-in
      // fallback table (only when no custom labels are set), then "class_<id>".
      std::string className(int classId, const std::vector<std::string> & fallback) const;

      std::string modelPath_;
      int inputWidth_;
      int inputHeight_;
      bool initialized_;
      bool enhanceImage_;
      float softNmsSigma_;
      ModelInfo modelInfo_;
      ModelType modelTypeOverride_ = ModelType::UNKNOWN;
      std::vector<std::string> classLabels_; // custom labels; empty = use built-in tables

      // Preprocessing configuration
      PreprocessConfig preprocessConfig_;

      // Execution provider state
      std::string requestedEP_; // User-requested EP (empty = auto)
      std::string activeEP_ = "CPU"; // Which EP was actually activated (mirrors runner_)
      bool lowLatency_ = false; // Intra-op hot-spinning (see setLowLatency)

      // Letterbox transform state (set by preprocessImage, used by coordinate remapping)
      bool useLetterbox_ = true;
      float letterboxScale_ = 1.0f; // Scale factor applied during letterbox
      int letterboxPadX_ = 0; // Padding added on left (half of total horizontal padding)
      int letterboxPadY_ = 0; // Padding added on top (half of total vertical padding)

      // Neutral ONNX runtime layer: owns the session and all ORT I/O. The vision
      // adapter (this class and its subclasses) builds image tensors and calls
      // runner_.run(); the runner itself is modality-agnostic.
      SessionRunner runner_;
  };

  // Generic ONNX Model (fallback for unknown models)
  class GenericModel : public DetectionModel {
    public:
      GenericModel(const std::string & modelPath, int inputSize = 640);

    protected:
      std::vector<Detection> parseOutput(float *outputData, const std::vector<int64_t> & outputShape,
                                         float confThreshold, const cv::Size & originalSize) override;
  };

  // YOLOv8/YOLO11 Detection Model
  class YOLOv8Model : public DetectionModel {
    public:
      YOLOv8Model(const std::string & modelPath, int inputSize = 640);

    protected:
      std::vector<Detection> parseOutput(float *outputData, const std::vector<int64_t> & outputShape,
                                         float confThreshold, const cv::Size & originalSize) override;
  };

  // YOLOv8/YOLO11 Pose Model
  class YOLOv8PoseModel : public DetectionModel {
    public:
      YOLOv8PoseModel(const std::string & modelPath, int inputSize = 640);

      // Pose-specific processing
      std::vector<PoseDetection> processPoseFrame(const cv::Mat & frame, float confThreshold = 0.5f,
                                                  float nmsThreshold = 0.4f, InferenceMetrics *metrics = nullptr);

    protected:
      std::vector<Detection> parseOutput(float *outputData, const std::vector<int64_t> & outputShape,
                                         float confThreshold, const cv::Size & originalSize) override;

      // Pose-specific parsing
      std::vector<PoseDetection> parsePoseOutput(float *outputData, const std::vector<int64_t> & outputShape,
                                                 float confThreshold, const cv::Size & originalSize);
  };

  // YOLOv8/YOLO11 Segmentation Model
  class YOLOv8SegmentationModel : public DetectionModel {
    public:
      YOLOv8SegmentationModel(const std::string & modelPath, int inputSize = 640);

      // Segmentation-specific processing
      std::vector<SegmentationDetection> processSegmentationFrame(const cv::Mat & frame, float confThreshold = 0.5f,
                                                                  float nmsThreshold = 0.4f, InferenceMetrics *metrics = nullptr);

    protected:
      std::vector<Detection> parseOutput(float *outputData, const std::vector<int64_t> & outputShape,
                                         float confThreshold, const cv::Size & originalSize) override;

      // Segmentation-specific parsing
      std::vector<SegmentationDetection> parseSegmentationOutput(float *outputData, const std::vector<int64_t> & outputShape,
                                                                 float confThreshold, const cv::Size & originalSize);

      // New methods for proper mask generation
      std::vector<SegmentationDetection>
        parseSegmentationOutputWithMasks(float *detectionData, const std::vector<int64_t> & detectionShape,
                                         float *prototypeData, const std::vector<int64_t> & prototypeShape,
                                         float confThreshold, const cv::Size & originalSize);

      std::vector<SegmentationDetection> applySegmentationNMS(const std::vector<SegmentationDetection> & detections, float nmsThreshold);

    private:
      // Helper methods for mask generation
      cv::Mat generateMask(const std::vector<float> & maskCoeffs, float *prototypeData,
                           const std::vector<int64_t> & prototypeShape, const cv::Rect & bbox, const cv::Size & originalSize);

      std::vector<cv::Point> extractContour(const cv::Mat & mask);
  };

  // YOLOv8/YOLO11 Classification Model
  class YOLOv8ClassificationModel : public DetectionModel {
    public:
      YOLOv8ClassificationModel(const std::string & modelPath, int inputSize = 224);

      // How raw output values become confidences: SOFTMAX for mutually-exclusive
      // classes (default; YOLO-cls, ViT classifiers), SIGMOID for independent
      // multi-label heads (e.g. AudioSet taggers), RAW for heads whose values are not
      // probabilities (e.g. packed regression+logit outputs) — RAW reports values
      // unchanged, ranked by signed value (descending), not by magnitude.
      enum OutputMode { SOFTMAX, SIGMOID, RAW };
      void setOutputMode(OutputMode mode) { outputMode_ = mode; }
      OutputMode getOutputMode() const { return outputMode_; }
      // Number of ranked classes reported (ClassificationResult::top). 1 = best only.
      void setTopK(unsigned k) { topK_ = k ? k : 1; }
      unsigned getTopK() const { return topK_; }

      // Classification-specific processing
      ClassificationResult processClassificationFrame(const cv::Mat & frame, InferenceMetrics *metrics = nullptr);

    protected:
      std::vector<Detection> parseOutput(float *outputData, const std::vector<int64_t> & outputShape,
                                         float confThreshold, const cv::Size & originalSize) override;

      // Classification-specific parsing
      ClassificationResult parseClassificationOutput(float *outputData, const std::vector<int64_t> & outputShape);

      OutputMode outputMode_ = SOFTMAX;
      unsigned topK_ = 1;
  };

  // YOLOv8/YOLO11 OBB (Oriented Bounding Box) Model
  class YOLOv8OBBModel : public DetectionModel {
    public:
      YOLOv8OBBModel(const std::string & modelPath, int inputSize = 640);

      // OBB-specific processing
      std::vector<OBBDetection> processOBBFrame(const cv::Mat & frame, float confThreshold = 0.5f,
                                                float nmsThreshold = 0.4f, InferenceMetrics *metrics = nullptr);

    protected:
      std::vector<Detection> parseOutput(float *outputData, const std::vector<int64_t> & outputShape,
                                         float confThreshold, const cv::Size & originalSize) override;

      // OBB-specific parsing
      std::vector<OBBDetection> parseOBBOutput(float *outputData, const std::vector<int64_t> & outputShape,
                                               float confThreshold, const cv::Size & originalSize);
  };

  // YOLO NMS-embedded Model (output [1, max_det, 6]: x1,y1,x2,y2,conf,class_id)
  class YOLONMSModel : public DetectionModel {
    public:
      YOLONMSModel(const std::string & modelPath, int inputSize = 640);

    protected:
      std::vector<Detection> parseOutput(float *outputData, const std::vector<int64_t> & outputShape,
                                         float confThreshold, const cv::Size & originalSize) override;
  };

  // Split-output detection model: class logits [1,N,C] + normalized cxcywh boxes
  // [1,N,4]. Used by the release-default YOLO26n ONNX pack.
  class YOLOSplitModel : public DetectionModel {
    public:
      YOLOSplitModel(const std::string & modelPath, int inputSize = 640);
      std::vector<Detection> processFrame(const cv::Mat & frame, float confThreshold = 0.5f,
                                          float nmsThreshold = 0.4f,
                                          InferenceMetrics *metrics = nullptr) override;

    protected:
      std::vector<Detection> parseOutput(float *outputData, const std::vector<int64_t> & outputShape,
                                         float confThreshold, const cv::Size & originalSize) override;
  };

  // RT-DETR NMS-free transformer detection (outputs: labels, boxes, scores)
  class RTDETRModel : public DetectionModel {
    public:
      RTDETRModel(const std::string & modelPath, int inputSize = 640);

      std::vector<Detection> processRTDETRFrame(const cv::Mat & frame, float confThreshold = 0.5f,
                                                 InferenceMetrics *metrics = nullptr);

    protected:
      std::vector<Detection> parseOutput(float *outputData, const std::vector<int64_t> & outputShape,
                                         float confThreshold, const cv::Size & originalSize) override;
  };

  // Depth estimation model (single spatial output)
  class DepthEstimationModel : public DetectionModel {
    public:
      DepthEstimationModel(const std::string & modelPath, int inputSize = 518);

      DepthResult processDepthFrame(const cv::Mat & frame, InferenceMetrics *metrics = nullptr);

    protected:
      std::vector<Detection> parseOutput(float *outputData, const std::vector<int64_t> & outputShape,
                                         float confThreshold, const cv::Size & originalSize) override;
  };

  // SCRFD multi-scale face detection with 5 landmarks
  class SCRFDModel : public DetectionModel {
    public:
      SCRFDModel(const std::string & modelPath, int inputSize = 640);

      std::vector<FaceDetection> processFaceFrame(const cv::Mat & frame, float confThreshold = 0.5f,
                                                   float nmsThreshold = 0.4f, InferenceMetrics *metrics = nullptr);

    protected:
      std::vector<Detection> parseOutput(float *outputData, const std::vector<int64_t> & outputShape,
                                         float confThreshold, const cv::Size & originalSize) override;
  };

  // ArcFace face recognition embedding model
  // Generic image-embedding adapter: a single [1,D] float output reported as an
  // L2-normalized embedding of any dimension (CLIP vision tower, face/speaker
  // embedders, ...). Input size and normalization come from the registry entry and/or
  // model sidecar. Defaults: direct resize, 0-1 scaling.
  class EmbeddingModel : public DetectionModel {
    public:
      EmbeddingModel(const std::string & modelPath, int inputSize = 224);

      FaceEmbedding processEmbeddingFrame(const cv::Mat & frame, InferenceMetrics *metrics = nullptr);

      // Zero-shot matching: load a set of L2-normalized label embeddings (produced
      // offline from CLIP's text tower — see scripts/ONNX/clip_text_embeddings.py).
      // Once loaded, matchEmbedding() ranks an image embedding against them by cosine
      // similarity, so the same [1,D] vision model tags frames with any label set.
      bool loadMatchSet(const std::string & path);
      bool hasMatchSet() const { return !matchLabels_.empty(); }
      // Cosine-rank an embedding against the loaded label set; top-K best first.
      ClassificationResult matchEmbedding(const std::vector<float> & embedding, unsigned topK) const;
      // Ranked tags reported per frame (matches the classifier `top_k` option).
      void setMatchTopK(unsigned k) { matchTopK_ = k ? k : 1; }
      unsigned getMatchTopK() const { return matchTopK_; }

    protected:
      std::vector<Detection> parseOutput(float *outputData, const std::vector<int64_t> & outputShape,
                                         float confThreshold, const cv::Size & originalSize) override;

      std::vector<std::string> matchLabels_;         // zero-shot label names
      std::vector<std::vector<float>> matchEmbeds_;   // L2-normalized, one row per label
      unsigned matchTopK_ = 5;                        // ranked tags per frame
  };

  // ArcFace face recognition: EmbeddingModel specialized for aligned 112x112 face
  // crops with SCRFD normalization ((x-127.5)/128).
  class ArcFaceModel : public EmbeddingModel {
    public:
      ArcFaceModel(const std::string & modelPath, int inputSize = 112);
  };

  // Two-stage OCR: DB-style text detection (the base DetectionModel session, run at
  // per-frame dynamic /32 input dims) followed by per-line CTC recognition (a second
  // SessionRunner + charset). Text lines come back as boxes with the recognized string.
  class OCRModel : public DetectionModel {
    public:
      OCRModel(const std::string & detPath, const std::string & recPath, const std::string & dictPath);

      // Load the recognition session + charset. The detection session loads via the
      // base initialize(); the factory calls both.
      bool initializeRec(int numThreads, const std::string & requestedEP, bool lowLatency,
                         std::string & err);
      bool recReady() const { return recReady_; }

      // Run det -> box extraction -> per-line rec -> CTC decode on one frame.
      // confThreshold filters lines by mean character probability.
      OCRResult processOCRFrame(const cv::Mat & frame, float confThreshold,
                                InferenceMetrics *metrics = nullptr);

    protected:
      std::vector<Detection> parseOutput(float *outputData, const std::vector<int64_t> & outputShape,
                                         float confThreshold, const cv::Size & originalSize) override;

    private:
      // Threshold + contour the detection probability map into text-line boxes
      // (DB unclip approximation: expand each box by area*ratio/perimeter).
      std::vector<cv::RotatedRect> extractBoxes(const cv::Mat & probMap, float binThresh);
      // Recognize one upright line crop (height-normalized); returns the decoded text
      // and fills conf with the mean kept-character probability.
      std::string recognizeLine(const cv::Mat & lineImg, float & conf);

      SessionRunner recRunner_;
      std::string recPath_;
      std::string dictPath_;
      std::vector<std::string> charset_; // [0] = CTC blank; dict entries; last = space
      bool recReady_ = false;
  };

  // RTMO one-stage multi-person pose (SimCC output)
  class RTMOModel : public DetectionModel {
    public:
      RTMOModel(const std::string & modelPath, int inputSize = 640);

      std::vector<PoseDetection> processRTMOFrame(const cv::Mat & frame, float confThreshold = 0.3f,
                                                   InferenceMetrics *metrics = nullptr);

    protected:
      std::vector<Detection> parseOutput(float *outputData, const std::vector<int64_t> & outputShape,
                                         float confThreshold, const cv::Size & originalSize) override;
  };

  // SAM2 image encoder (produces image embeddings)
  class SAM2EncoderModel : public DetectionModel {
    public:
      SAM2EncoderModel(const std::string & modelPath, int inputSize = 1024);

      // Returns raw embeddings as multi-channel Mat for decoder input
      std::vector<cv::Mat> encodeImage(const cv::Mat & frame, InferenceMetrics *metrics = nullptr);

    protected:
      std::vector<Detection> parseOutput(float *outputData, const std::vector<int64_t> & outputShape,
                                         float confThreshold, const cv::Size & originalSize) override;
  };

  // SAM2 mask decoder (takes embeddings + prompts, returns masks)
  class SAM2DecoderModel : public DetectionModel {
    public:
      SAM2DecoderModel(const std::string & modelPath, int inputSize = 256);

      SAM2Result decodeMasks(const std::vector<cv::Mat> & imageEmbeddings,
                             const std::vector<cv::Point2f> & pointPrompts,
                             const std::vector<int> & pointLabels,
                             const cv::Size & originalSize,
                             InferenceMetrics *metrics = nullptr);

    protected:
      std::vector<Detection> parseOutput(float *outputData, const std::vector<int64_t> & outputShape,
                                         float confThreshold, const cv::Size & originalSize) override;
  };

  struct ModelBundle; // resolved multi-file audio bundle (defined below)
  struct OCRBundle;   // resolved OCR file set (defined below)

  // One transcribed span of speech, timestamped against the source stream.
  struct TranscriptSegment {
      uint64_t startMs = 0;
      uint64_t endMs = 0;
      std::string text;
      float confidence = 0.0f;
  };

  // Result of transcribing one audio window.
  struct TranscriptResult {
      std::string text;                        // concatenated text for the window
      std::vector<TranscriptSegment> segments; // per-token/word timed spans
      bool ok = false;
  };

  // Parakeet TDT speech-to-text — the AUDIO sibling of DetectionModel. It deliberately
  // does NOT derive from DetectionModel (no image/tensor-rank/box assumptions). It holds
  // three SessionRunners — mel preprocessor -> Conformer encoder -> TDT decoder+joint —
  // and turns 16 kHz mono f32 PCM into timed text via a stateful greedy TDT loop that
  // threads the decoder's LSTM state and previous token across encoder frames. This is
  // the transcription seam described on DetectionModel.
  class ASRModel {
    public:
      ASRModel();
      ~ASRModel();
      ASRModel(const ASRModel &) = delete;
      ASRModel &operator=(const ASRModel &) = delete;

      // Load the three ONNX files + vocab from a resolved bundle. requestedEP as in
      // SessionRunner::load ("" = auto, "cpu", "cuda", ...). Returns false on any failure.
      bool initialize(const ModelBundle & bundle, int numThreads = 1,
                      const std::string & requestedEP = "", bool lowLatency = false);
      bool ready() const { return ready_; }

      // Required input sample rate (Hz). Callers must feed mono PCM at this rate.
      int sampleRate() const { return sampleRate_; }

      // Execution provider the encoder session actually bound to ("CPU", "CUDA", ...).
      const std::string & executionProvider() const { return encoder_.activeEP(); }

      // Transcribe one contiguous window of mono f32 PCM in [-1,1]. baseMs is the stream
      // timestamp of sample 0, used to stamp the returned segments.
      TranscriptResult transcribe(const float *samples, size_t count, uint64_t baseMs);

    private:
      bool loadVocab(const std::string & vocabPath);
      // Map each runner's I/O ports to roles (by name/shape/dtype) after load.
      bool bindPorts(std::string & err);
      std::string detokenize(const std::vector<int> & tokens) const;

      SessionRunner preproc_;      // nemo128: waveform -> log-mel features
      SessionRunner encoder_;      // Conformer: features -> encoded frames
      SessionRunner decoderJoint_; // fused RNN-T/TDT decoder + joint (stateful)

      std::vector<std::string> vocab_;   // token id -> piece (U+2581 already -> space)
      int blankId_ = -1;
      int vocabSize_ = 0;                // vocab length (includes the <blk> blank token)
      int sampleRate_ = 16000;
      bool ready_ = false;
  };

  // AudioModel is the generic AUDIO sibling of DetectionModel for SINGLE-file audio
  // models over one SessionRunner (ASRModel stays the bespoke multi-session pipeline).
  // The config captures each model's contract: sample rate, fixed-chunk streaming vs
  // pause-windowed feed, waveform normalization, and output semantics. Labels and
  // sample rate come from model sidecars where available (HF preprocessor_config.json /
  // config.json id2label — same files the vision path reads).

  struct AudioModelConfig {
      int sampleRate = 16000;
      // >0: fixed-chunk streaming — feed exactly this many samples per process() call
      // (Silero VAD: 512 @ 16 kHz). 0: pause-windowed — feed AudioWindower windows.
      int chunkSamples = 0;
      // wav2vec2-style per-window zero-mean/unit-variance waveform normalization
      bool zeroMeanUnitVar = false;
      // Multi-label heads score with sigmoid (AUDIO_TAGGING); default softmax
      bool multiLabel = false;
      unsigned topK = 5;               // ranked classes reported per window
      std::vector<std::string> labels; // class labels, index = class id
      // FBANK frontend (kaldi-style log-mel, 25 ms / 10 ms): 0 = raw waveform input.
      int fbankBins = 0;               // mel bins (AST: 128, WeSpeaker: 80)
      bool fbankHanning = false;       // hanning window (HF/AST convention) vs kaldi povey
      int fixedFrames = 0;             // pad/truncate to this many frames (AST: 1024); 0 = variable
      float featMean = 0.0f;           // post-fbank normalization: (x - mean) / std ...
      float featStd = 1.0f;            // ... (AST uses std = 2x the dataset std, per HF convention)
      bool cepstralMeanNorm = false;   // subtract the per-utterance mean per bin (WeSpeaker)
  };

  // One result per processed window/chunk.
  struct AudioResult {
      uint64_t startMs = 0, endMs = 0; // stream time span of the processed samples
      std::vector<ClassScore> scores;  // CLASSIFY/TAGGING: ranked top-K; VAD: [0] = speech prob
      std::vector<float> embedding;    // AUDIO_EMBEDDING: L2-normalized
      InferenceMetrics metrics;
      bool ok = false;
  };

  class AudioModel {
    public:
      AudioModel(const std::string & modelPath, ModelType task, const AudioModelConfig & cfg);
      AudioModel(const AudioModel &) = delete;
      AudioModel &operator=(const AudioModel &) = delete;

      // Load the session and map ports: first float input = samples; an int64 scalar
      // input whose name contains "sr" is fed the sample rate; every other input port
      // is treated as recurrent state and bound to the output port whose name starts
      // with the input's name (Silero: "state" -> "stateN") via bindStateLoop.
      bool initialize(int numThreads, const std::string & requestedEP, bool lowLatency,
                      std::string & err);
      bool ready() const { return ready_; }
      ModelType task() const { return task_; }
      const AudioModelConfig & config() const { return cfg_; }
      int sampleRate() const { return cfg_.sampleRate; }
      int chunkSamples() const { return cfg_.chunkSamples; }
      const std::string & executionProvider() const { return runner_.activeEP(); }

      // Process one window (chunkSamples == 0) or exactly one chunk (chunkSamples > 0)
      // of mono f32 PCM in [-1,1] at sampleRate(). baseMs = stream time of sample 0.
      AudioResult process(const float *samples, size_t count, uint64_t baseMs);

      // Reset recurrent state (stream restart / discontinuity).
      void reset() { runner_.resetState(); }

    private:
      bool bindPorts(std::string & err);

      SessionRunner runner_;
      std::string modelPath_;
      ModelType task_;
      AudioModelConfig cfg_;
      size_t samplesIdx_ = 0;   // main waveform input port
      size_t srIdx_ = SIZE_MAX; // optional sample-rate scalar port (int64 or int32)
      bool srInt32_ = false;    // sr port element type is int32 (fed accordingly)
      bool srRank1_ = false;    // sr port is rank-1 [1] instead of a rank-0 scalar
      size_t outIdx_ = 0;       // main output port (first float output not bound as state)
      bool ready_ = false;
  };

  // Turns a per-frame/per-window score stream into enter/exit events with EMA
  // smoothing, hysteresis and a minimum active duration — so moderation/VAD emit
  // "started"/"ended" state changes instead of per-frame score noise.
  class EventSmoother {
    public:
      enum Event { NONE, STARTED, ENDED };

      // enterThresh > exitThresh gives hysteresis. emaAlpha in (0,1]: weight of the
      // NEW score (1 = no smoothing). minDurationMs: an active phase younger than
      // this cannot end (debounce).
      void configure(float enterThresh, float exitThresh, double emaAlpha, uint64_t minDurationMs);

      // Feed the next score; returns the state change this score caused, if any.
      Event update(float score, uint64_t timeMs);

      float value() const { return ema_; }   // current smoothed score
      bool active() const { return active_; }
      uint64_t activeSinceMs() const { return startedAt_; }

    private:
      float enter_ = 0.7f, exit_ = 0.4f;
      double alpha_ = 0.3;
      uint64_t minMs_ = 0;
      float ema_ = 0.0f;
      bool primed_ = false;
      bool active_ = false;
      uint64_t startedAt_ = 0;
  };

  // ONNX Model Factory
  class ModelFactory {
    public:
      // Create appropriate model based on auto-detection (or override type)
      // executionProvider: "" = auto, "cpu" = CPU only, "cuda", "tensorrt", "coreml", "openvino"
      static std::unique_ptr<DetectionModel> createModel(const std::string & modelPath, int inputSize = 640,
                                                          int numThreads = 1, ModelType typeOverride = ModelType::UNKNOWN,
                                                          const std::string & executionProvider = "", bool lowLatency = false);

      // Create a transcription (audio) model from a resolved multi-file bundle. Returns
      // null on failure. This is the AUDIO counterpart to createModel; the caller selects
      // which to use from the registry entry's modality (no shape-sniffing / force-fit).
      static std::unique_ptr<ASRModel> createTranscriptionModel(const ModelBundle & bundle, int numThreads = 1,
                                                                const std::string & executionProvider = "", bool lowLatency = false);

      // Create a generic single-file audio model (AUDIO_VAD / AUDIO_CLASSIFICATION /
      // AUDIO_TAGGING / AUDIO_EMBEDDING). Builds the AudioModelConfig from the task
      // type + model sidecars (labels from config.json/labels.txt, sample rate from
      // preprocessor_config.json). Returns null on failure.
      static std::unique_ptr<AudioModel> createAudioModel(const std::string & modelPath, ModelType task,
                                                          int numThreads = 1,
                                                          const std::string & executionProvider = "",
                                                          bool lowLatency = false);

      // Create a two-stage OCR model from a resolved OCRBundle (det/rec/dict paths).
      // Returns null on failure.
      static std::unique_ptr<OCRModel> createOCRModel(const OCRBundle & bundle, int numThreads = 1,
                                                      const std::string & executionProvider = "",
                                                      bool lowLatency = false);

      // Auto-detect model type without full initialization
      static ModelType detectModelType(const std::string & modelPath);

      // Decide the coarse input modality from the model's input port(s), without a
      // full DetectionModel initialization. Returns UNKNOWN if the model can't load.
      static ModelModality detectModality(const std::string & modelPath);

      // Get model information
      static ModelInfo analyzeModel(const std::string & modelPath);
  };

  // Known model entry for the built-in registry.
  //
  // A single-file model sets `filename` (vision, or single-file audio). A multi-file
  // model leaves `filename` empty and fills the role fields under `subdir`: an ASR
  // bundle uses encoder/decoder/preproc/vocab (modality AUDIO); an OCR bundle uses
  // det/rec/charset (type OCR). Unused fields stay null. Trailing members have defaults
  // so the existing positional vision entries keep compiling unchanged.
  struct ModelRegistryEntry {
      const char *id;
      const char *label;
      const char *filename;
      ModelType type;
      int defaultInputSize;
      ModelModality modality = ModelModality::VISION;
      const char *subdir = nullptr;       // per-variant subfolder for a multi-file bundle
      // ASR bundle roles (modality AUDIO):
      const char *encoderFile = nullptr;
      const char *encoderDataFile = nullptr; // ORT external-data sidecar for the encoder (fp32), if any
      const char *decoderFile = nullptr;  // fused decoder+joint
      const char *preprocFile = nullptr;  // mel/feature preprocessor
      const char *vocabFile = nullptr;
      // OCR bundle roles (type OCR): detection + recognition + charset under `subdir`.
      const char *detFile = nullptr;
      const char *recFile = nullptr;
      const char *charsetFile = nullptr;
  };

  // Resolved absolute paths for a multi-file (audio) model bundle.
  struct ModelBundle {
      std::string encoder;
      std::string decoderJoint;
      std::string preproc;
      std::string vocab;
      bool ok = false;                    // true only if every required file resolved
  };

  // Resolved file set for a two-stage OCR model (its own roles, not ASR's).
  struct OCRBundle {
      std::string det;    // text-detection .onnx
      std::string rec;    // text-recognition .onnx
      std::string dict;   // recognition charset (one token per line)
      bool ok = false;
  };

  // Model provisioning registry — maps model IDs to files on disk.
  namespace ModelRegistry {
      const std::vector<ModelRegistryEntry> & getAvailableModels();
      // Writable persistent cache: MIST_MODEL_DIR, else XDG/LOCALAPPDATA/~/.cache,
      // with Util::getTmpFolder()+"models/" only as a final fallback.
      std::string getModelDir();
      // Read-only directory holding the provisioning scripts (prepare_models.sh): env
      // MIST_ONNX_SCRIPTS, else next to the binary (installed), else the dev scripts/ONNX/.
      // Returns "" if prepare_models.sh can't be located.
      std::string getScriptDir();
      std::string resolveModelPath(const std::string & modelIdOrPath);
      // Resolve every file of a multi-file (audio) bundle by model id. ok=false if the
      // id is unknown, not an audio bundle, or any file is missing on disk.
      ModelBundle resolveModelSet(const std::string & modelId);
      // Resolve an OCR bundle (det/rec/dict) by model id. ok=false if the id is unknown,
      // not an OCR entry, or any file is missing on disk.
      OCRBundle resolveOCRSet(const std::string & modelId);
      // Look up an entry by id (nullptr if unknown), so callers can read its modality.
      const ModelRegistryEntry * findModel(const std::string & id);
      bool isKnownModelId(const std::string & id);
      // Provision (download/export) a model by running the bundled prepare_models.sh into
      // getModelDir(). Blocks until the script exits; streams its output to the log. On
      // failure, fills `hint` with the script's tail (e.g. the needs-Python message).
      // Returns true only if the model's files actually resolve on disk afterwards
      // (the script exiting 0 isn't enough — a half-download must count as failure).
      bool provision(const std::string & id, std::string & hint);
  }

  // Utility functions
  namespace Utils {
    // Scene change detection (standalone, not part of TemporalTracker)
    struct SceneChangeDetector {
        std::vector<Detection> previousDetections;
        uint64_t lastTimestamp = 0;
        float threshold = 0.85f;
        bool enabled = true;

        // Rate limiting to prevent too frequent scene changes
        uint64_t lastSceneChangeTime = 0;
        uint64_t minMsBetweenChanges = 1000; // Minimum 1 second between scene changes
    };

    // COCO class names
    extern const std::vector<std::string> COCO_CLASSES;

    // ImageNet class names (1000 classes for classification models)
    extern const std::vector<std::string> IMAGENET_CLASSES;

    // COCO pose keypoint names
    extern const std::vector<std::string> COCO_KEYPOINTS;

    // DOTA dataset class names (15 classes for OBB models)
    extern const std::vector<std::string> DOTA_CLASSES;

    // Model sidecar assets: files living next to a .onnx carrying per-model data, so
    // labels/normalization come from data instead of hardcoded C++ tables. Lookup order
    // (first hit wins) for a model at /path/to/<stem>.onnx:
    //   labels:  <stem>.labels.txt → labels.txt → config.json ("id2label", HF layout)
    //   preproc: <stem>.preprocessor.json → preprocessor_config.json (HF layout)
    // The generic names (labels.txt / config.json / preprocessor_config.json) are meant
    // for models living in their own subdirectory (the HF download layout that
    // prepare_models.sh creates); use the <stem>-prefixed names for models sharing a
    // flat directory.
    struct SidecarConfig {
        std::vector<std::string> labels; // class labels, index = class id; empty = none found
        bool hasPreproc = false;         // true if `preproc`/`inputSize` below are meaningful
        PreprocessConfig preproc;        // resize mode + normalization from the preprocessor config
        int inputSize = 0;               // model-native input size from the config, 0 = unspecified
        // Audio preprocessor fields (HF audio feature extractors):
        int samplingRate = 0;            // "sampling_rate", 0 = unspecified
        bool audioNormalize = false;     // "do_normalize" on an audio preprocessor (zero-mean/unit-var)
        int numMelBins = 0;              // "num_mel_bins" (spectrogram models), 0 = raw waveform
        int maxFrames = 0;               // "max_length" (fixed spectrogram frame count), 0 = variable
        float featMean = 0.0f;           // "mean" (post-fbank normalization constant)
        float featStd = 0.0f;            // "std" (0 = no post-fbank normalization)
    };
    // Greedy CTC decode over a [timesteps, numClasses] probability matrix: argmax per
    // timestep, collapse repeats, drop blanks (class 0). charset[0] is the blank and
    // must be present; out-of-range classes are skipped. Returns the number of kept
    // characters; confidence = mean probability of the kept symbols.
    size_t ctcGreedyDecode(const float *probs, size_t timesteps, size_t numClasses,
                           const std::vector<std::string> & charset, std::string & text,
                           float & confidence);

    // Kaldi-compatible log-mel filterbank features (torchaudio.compliance.kaldi.fbank
    // with dither=0): 25 ms frames / 10 ms shift, snip_edges, per-frame DC-offset
    // removal, preemphasis 0.97, povey (kaldi default) or hanning (HF/AST) window,
    // power spectrum via cv::dft, mel banks from 20 Hz to Nyquist, natural log with an
    // epsilon floor. Appends frames*numBins values to `out` (row-major [frame][bin])
    // and returns the number of frames (0 if input is shorter than one frame).
    size_t computeFbank(const float *samples, size_t count, int sampleRate, int numBins,
                        bool hanningWindow, std::vector<float> &out);

    // Load labels from a plain text file, one label per line (index = class id).
    std::vector<std::string> loadLabelsFile(const std::string & path);
    // Load labels from a HuggingFace config.json ("id2label" object).
    std::vector<std::string> loadLabelsFromHFConfig(const std::string & path);
    // Discover and parse all sidecar files for a model path (see lookup order above).
    SidecarConfig loadModelSidecars(const std::string & modelPath);

    // Convert video codec data to OpenCV Mat
    cv::Mat decodeVideoFrame(const char *data, size_t dataLen, const std::string & codec, uint64_t width, uint64_t height);

    // Template function to draw detections with optional tracking features
    template<typename DetectionType>
    cv::Mat drawDetectionsWithOptionalTracking(const cv::Mat & image, const std::vector<DetectionType> & detections,
                                               bool showTrackIds, bool showConfidence, bool withTracking);

    // Draw pose detections on image
    cv::Mat drawPoseDetections(const cv::Mat & image, const std::vector<PoseDetection> & detections,
                               bool showTrackIds = true, bool showConfidence = true);

    // Draw pose detections with tracking features (trails, etc.)
    cv::Mat drawPoseDetectionsWithTracking(const cv::Mat & image, const std::vector<PoseDetection> & detections,
                                           bool showTrackIds = true, bool showConfidence = true);

    // Draw segmentation detections on image
    cv::Mat drawSegmentationDetections(const cv::Mat & image, const std::vector<SegmentationDetection> & detections,
                                       bool showTrackIds = true, bool showConfidence = true);

    // Draw segmentation detections with tracking features (trails, etc.)
    cv::Mat drawSegmentationDetectionsWithTracking(const cv::Mat & image, const std::vector<SegmentationDetection> & detections,
                                                   bool showTrackIds = true, bool showConfidence = true);

    // Draw OBB detections on image
    cv::Mat drawOBBDetections(const cv::Mat & image, const std::vector<OBBDetection> & detections,
                              bool showTrackIds = true, bool showConfidence = true);

    // Draw OBB detections with tracking features (trails, etc.)
    cv::Mat drawOBBDetectionsWithTracking(const cv::Mat & image, const std::vector<OBBDetection> & detections,
                                          bool showTrackIds = true, bool showConfidence = true);

    // Draw classification result on image
    cv::Mat drawClassificationResult(const cv::Mat & image, const ClassificationResult & result);

    // Template function to encode JPEG with detections and overlay
    template<typename DetectionType>
    std::vector<uchar> encodeJPEG(const cv::Mat & frame, const std::vector<DetectionType> & detections,
                                  const ProcessingStats & stats, int quality = 90, InferenceMetrics *metrics = nullptr);

    // Create JSON Value from detections (using MistServer JSON library)
    JSON::Value detectionsToJSON(const std::vector<Detection> & detections, uint64_t timestamp,
                                 const InferenceMetrics & metrics, const std::string & modelName = "yolov8");

    // Create JSON from pose detections
    JSON::Value poseDetectionsToJSON(const std::vector<PoseDetection> & detections, uint64_t timestamp,
                                     const InferenceMetrics & metrics, const std::string & modelName = "yolov8-pose");

    // Create JSON from segmentation detections
    JSON::Value segmentationDetectionsToJSON(const std::vector<SegmentationDetection> & detections, uint64_t timestamp,
                                             const InferenceMetrics & metrics, const std::string & modelName = "yolov8-seg");

    // Create JSON from OBB detections
    JSON::Value obbDetectionsToJSON(const std::vector<OBBDetection> & detections, uint64_t timestamp,
                                    const InferenceMetrics & metrics, const std::string & modelName = "yolov8-obb");

    // Create JSON from classification result
    JSON::Value classificationToJSON(const ClassificationResult & result, const InferenceMetrics & metrics,
                                     const std::string & modelName = "yolov8-cls");

    // Create JSON from generic result (for unknown models)
    JSON::Value genericResultToJSON(const GenericResult & result);

    // Create JSON from depth estimation result
    JSON::Value depthResultToJSON(const DepthResult & result, uint64_t timestamp, const std::string & modelName = "depth");

    // Create JSON from face detections (with landmarks)
    JSON::Value faceDetectionsToJSON(const std::vector<FaceDetection> & detections, uint64_t timestamp,
                                     const InferenceMetrics & metrics, const std::string & modelName = "scrfd");

    // Create JSON from face embedding result
    JSON::Value faceEmbeddingToJSON(const FaceEmbedding & embedding, uint64_t timestamp,
                                    const std::string & modelName = "arcface");

    // Create JSON from an OCR result (text lines with boxes + the joined text)
    JSON::Value ocrResultToJSON(const OCRResult & result, uint64_t timestamp,
                                const std::string & modelName = "ocr");

    // Create JSON from a generic audio model result (classification/tagging/embedding
    // window, or a periodic VAD score). kind: "vad" / "audio_classification" /
    // "audio_tagging" / "audio_embedding".
    JSON::Value audioResultToJSON(const AudioResult & result, const std::string & modelName,
                                  const std::string & kind);

    // Create JSON for a state-change event from an EventSmoother (e.g. speech/nsfw
    // started/ended). score is the smoothed value at the transition.
    JSON::Value eventToJSON(const std::string & label, bool started, float score,
                            uint64_t timestamp, const std::string & modelName);

    // Create JSON from SAM2 result
    JSON::Value sam2ResultToJSON(const SAM2Result & result, uint64_t timestamp,
                                 const std::string & modelName = "sam2");

    // Draw depth visualization (colormap overlay)
    cv::Mat drawDepthVisualization(const cv::Mat & frame, const DepthResult & result);

    // Draw face detections with landmarks
    cv::Mat drawFaceDetections(const cv::Mat & image, const std::vector<FaceDetection> & detections,
                               bool showConfidence = true);
    // Process VideoPacket with auto-detected model
    std::pair<JSON::Value, ProcessedVideoFrame>
      processVideoPacketAuto(const VideoPacket & packet, DetectionModel & model, TemporalTracker & tracker,
                             ProcessingStats & stats, SceneChangeDetector & sceneDetector, float confThreshold = 0.5f,
                             float nmsThreshold = 0.4f, bool enhanceImage = false, int jpegQuality = 80,
                             bool annotatedVideo = false, bool trackingEnabled = false,
                             bool sceneChangeEnabled = false);

    // Process VideoPacket with generic fallback
    std::pair<JSON::Value, ProcessedVideoFrame> processVideoPacketGeneric(const VideoPacket & packet, DetectionModel & model,
                                                                          ProcessingStats & stats, bool enhanceImage = false,
                                                                          bool annotatedVideo = false, int jpegQuality = 80);

    // Configuration validation functions
    bool validateModelPath(const std::string & modelPath);
    bool validateThreshold(float threshold, const std::string & name);
    bool validateInputSize(int inputSize);
    bool validateThreadCount(int threads);

    bool detectSceneChange(SceneChangeDetector & detector, const std::vector<Detection> & newDetections, uint64_t timestamp);
    float calculateDetectionSimilarity(const std::vector<Detection> & dets1, const std::vector<Detection> & dets2);

    void drawFilledRectAlpha(cv::Mat &image, const cv::Rect &rect,
                             const cv::Scalar &color, double alpha);
  } // namespace Utils

} // namespace ONNX
