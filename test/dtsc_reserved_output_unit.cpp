// A processing buffer reserves a track index for every output it knows a process will produce;
// the producer claims the reservation by output key. Reservations are invisible to readers until
// claimed, and a released one is never claimed.
#include <mist/defines.h>
#include <mist/dtsc.h>

#include <iostream>
#include <string>

namespace {
  int failures = 0;

  void expect(bool ok, const std::string & what) {
    if (!ok) {
      std::cerr << "FAIL: " << what << std::endl;
      ++failures;
    }
  }

  DTSC::TrackMetadata rendition(const std::string & name) {
    DTSC::TrackMetadata trk;
    trk.type = "video";
    trk.codec = "H264";
    trk.width = 320;
    trk.height = 180;
    trk.output = name;
    return trk;
  }
} // namespace

int main() {
  const std::string identity = DTSC::processIdentity("{\"process\":\"Livepeer\",\"source\":\"vod+a\"}");
  DTSC::Meta meta;
  meta.reInit("", true);

  // The original tracks are registered first; the buffer then reserves the process outputs.
  DTSC::outputKeyScope.clear();
  DTSC::TrackMetadata source = rendition("");
  source.width = 640;
  source.height = 360;
  const size_t original = meta.addOrResumeTrack(source);
  const size_t p1 = meta.reserveOutputTrack(DTSC::outputKey(identity, "p1"));
  const size_t p2 = meta.reserveOutputTrack(DTSC::outputKey(identity, "p2"));
  const size_t p3 = meta.reserveOutputTrack(DTSC::outputKey(identity, "p3"));
  expect(p1 != INVALID_TRACK_ID && p1 > original && p2 > p1 && p3 > p2, "reservations take the next track indexes");
  expect(meta.reserveOutputTrack(DTSC::outputKey(identity, "p1")) == p1, "reserving an output twice keeps one reservation");
  expect(meta.isReservedTrack(p1) && !meta.isReservedTrack(original), "only reservations are reserved records");
  expect(meta.getValidTracks().size() == 1 && meta.getValidTracks().count(original), "reservations are invisible to readers");
  expect(meta.getReservedTracks().size() == 3, "every reservation is listed");

  // A track registered later (another input, an unkeyed process) gets an index after them.
  meta.breakClaim(original);
  const size_t later = meta.addTrack();
  expect(later > p3, "tracks created after a reservation do not take its index");

  // The producer claims its reservations by key, in any order.
  DTSC::outputKeyScope = identity;
  expect(meta.addOrResumeTrack(rendition("p2")) == p2, "a producer claims the reservation of its output");
  expect(!meta.isReservedTrack(p2) && meta.getValidTracks().count(p2), "a claimed reservation becomes a track");
  expect(meta.getOutputKey(p2) == DTSC::outputKey(identity, "p2") && meta.getWidth(p2) == 320,
         "a claimed reservation carries its key and the producer's description");
  expect(meta.addOrResumeDelayedTrack(rendition("p1")) == p1, "the delayed path claims the reservation too");
  expect(!meta.getValidTracks().count(p1), "a delayed claim becomes visible only once validated");
  meta.validateTrack(p1);
  expect(meta.getValidTracks().count(p1), "a validated delayed claim is a track");

  // A released reservation (its producer was retired) is never claimed.
  meta.releaseReservedTrack(p3);
  expect(!meta.isReservedTrack(p3) && meta.getReservedTracks().empty(), "a released reservation is gone");
  const size_t p3Track = meta.addOrResumeTrack(rendition("p3"));
  expect(p3Track != p3 && p3Track > later, "an output whose reservation was released gets a new track");

  // A restarted producer continues the track it claimed, not a new reservation.
  meta.breakClaim(p2);
  expect(meta.reserveOutputTrack(DTSC::outputKey(identity, "p2")) == p2, "reserving an output that has a track returns it");
  expect(meta.addOrResumeTrack(rendition("p2")) == p2, "a restarted producer continues its claimed reservation");

  if (failures) { return 1; }
  std::cout << "process outputs claim their reserved tracks by key" << std::endl;
  return 0;
}
