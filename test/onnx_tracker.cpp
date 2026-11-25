#include <mist/onnx.h>

#include <cmath>
#include <cstdio>

#define CHECK(test)                                                \
  do {                                                             \
    if (!(test)) {                                                 \
      std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #test); \
      return 1;                                                    \
    }                                                              \
  } while (0)

int main() {
  ONNX::TemporalTracker tracker(0.3f, 0, 2000);
  ONNX::Detection detection{};
  detection.x = 0.2f;
  detection.y = 0.3f;
  detection.w = 0.1f;
  detection.h = 0.2f;
  detection.confidence = 0.9f;
  auto tracks = tracker.updateTracks({detection}, 1000);
  CHECK(tracks.size() == 1);
  CHECK(tracks[0].kalmanInitialized && tracks[0].kalmanFilter);
  const auto & filter = *tracks[0].kalmanFilter;
  for (int row = 0; row < 6; ++row) {
    for (int column = 0; column < 6; ++column) {
      const float expected = (row == column || (row == 0 && column == 4) || (row == 1 && column == 5)) ? 1 : 0;
      CHECK(filter.transitionMatrix.at<float>(row, column) == expected);
      if (row < 4) { CHECK(filter.measurementMatrix.at<float>(row, column) == (row == column ? 1 : 0)); }
    }
  }
  const uint64_t id = tracks[0].track_id;
  detection.x += 0.01f;
  detection.y += 0.01f;
  tracks = tracker.updateTracks({detection}, 1200);
  CHECK(tracks.size() == 1 && tracks[0].track_id == id);
  CHECK(std::isfinite(tracks[0].x) && tracks[0].x > 0.2f && tracks[0].x <= detection.x);
  CHECK(std::isfinite(tracks[0].y) && tracks[0].y > 0.3f && tracks[0].y <= detection.y);
  CHECK(tracks[0].w > 0 && tracks[0].h > 0);
  tracks = tracker.updateTracks({}, 1300);
  CHECK(tracks.size() == 1 && tracks[0].track_id == id);
  CHECK(std::isfinite(tracks[0].x) && std::isfinite(tracks[0].y));
  return 0;
}
