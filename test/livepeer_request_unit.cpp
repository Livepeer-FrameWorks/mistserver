#include "../src/process/livepeer_request.h"

#include <cassert>
#include <set>
#include <string>

int main() {
  JSON::Value options;
  options["target_profiles"] = JSON::fromString("[{\"name\":\"720p\"}]");
  options["workload"] = "vod";
  options["deadline_ms"] = 45000;
  options["min_speed"] = 1.25;

  JSON::Value configuration = Mist::buildLivepeerTranscodeConfiguration(options, 45000);
  assert(configuration["profiles"][0u]["name"].asString() == "720p");
  assert(configuration["workload"].asString() == "vod");
  assert(configuration["deadlineMs"].asInt() == 45000);
  assert(configuration["minSpeed"].asDouble() == 1.25);
  assert(!configuration.isMember("jobToken"));

  options["job_token"] = "opaque-job-token";
  configuration = Mist::buildLivepeerTranscodeConfiguration(options, 45000);
  assert(configuration["jobToken"].asString() == "opaque-job-token");

  options["job_token"] = 12345;
  configuration = Mist::buildLivepeerTranscodeConfiguration(options, 45000);
  assert(!configuration.isMember("jobToken"));

  options["workload"] = 7;
  configuration = Mist::buildLivepeerTranscodeConfiguration(options, 0);
  assert(!configuration.isMember("workload"));
  assert(!configuration.isMember("deadlineMs"));

  assert(Mist::livepeerFatalUploadStatus(401));
  assert(Mist::livepeerFatalUploadStatus(403));
  assert(Mist::livepeerFatalUploadStatus(503));
  assert(!Mist::livepeerFatalUploadStatus(422));
  assert(!Mist::livepeerFatalUploadStatus(500));

  assert(!Mist::livepeerShouldFallback(4));
  assert(Mist::livepeerShouldFallback(5));
  assert(Mist::livepeerShouldFallback(6));

  assert(Mist::livepeerRejectionStep(1, false) == Mist::LivepeerRejectionStep::RetrySame);
  assert(Mist::livepeerRejectionStep(2, false) == Mist::LivepeerRejectionStep::RetrySame);
  assert(Mist::livepeerRejectionStep(2, true) == Mist::LivepeerRejectionStep::RetrySame);
  assert(Mist::livepeerRejectionStep(3, false) == Mist::LivepeerRejectionStep::SwitchBroadcaster);
  assert(Mist::livepeerRejectionStep(3, true) == Mist::LivepeerRejectionStep::RejectSegment);
  assert(Mist::livepeerRejectionBackoffMs(1) == 250);
  assert(Mist::livepeerRejectionBackoffMs(2) == 500);
  assert(Mist::livepeerRejectionBackoffMs(3) == 1000);
  assert(Mist::livepeerRejectionBackoffMs(9) == 2000);

  JSON::Value vodOptions;
  vodOptions["workload"] = "vod";
  JSON::Value liveOptions;
  assert(Mist::livepeerRejectedSegmentStopsJob(vodOptions, "live+abc"));
  assert(Mist::livepeerRejectedSegmentStopsJob(liveOptions, "processing+abc"));
  assert(!Mist::livepeerRejectedSegmentStopsJob(liveOptions, "live+abc"));
  liveOptions["workload"] = "live";
  assert(!Mist::livepeerRejectedSegmentStopsJob(liveOptions, "live+abc"));

  assert(Mist::livepeerShouldRetryCurrentBroadcaster(false, true));
  assert(!Mist::livepeerShouldRetryCurrentBroadcaster(false, false));
  assert(!Mist::livepeerShouldRetryCurrentBroadcaster(true, true));

  assert(Mist::livepeerSocketTimeoutSeconds(3900, 0) == 5);
  assert(Mist::livepeerSocketTimeoutSeconds(3900, 45000) == 50);
  assert(Mist::livepeerDownloaderRetryCount(0) == 2);
  assert(Mist::livepeerDownloaderRetryCount(45000) == 1);

  std::set<std::string> two;
  two.insert("http://a");
  two.insert("http://b");
  std::set<std::string> three = two;
  three.insert("http://c");
  for (size_t pick = 0; pick < 6; ++pick) {
    // Two upload threads fail on the same broadcaster at once. The first one
    // switches; the second must keep that choice instead of switching back.
    std::string current = "http://a";
    std::set<std::string> failedFirst;
    failedFirst.insert("http://a");
    assert(Mist::livepeerSwitchBroadcaster(current, "http://a", failedFirst, two, pick) == Mist::LivepeerSwitchOutcome::Switched);
    assert(current == "http://b");
    std::set<std::string> failedSecond;
    failedSecond.insert("http://a");
    assert(Mist::livepeerSwitchBroadcaster(current, "http://a", failedSecond, two, pick) == Mist::LivepeerSwitchOutcome::AlreadySwitched);
    assert(current == "http://b");

    // A segment that failed on a and then b moves on to c, never back to a.
    current = "http://b";
    std::set<std::string> failedAB;
    failedAB.insert("http://a");
    failedAB.insert("http://b");
    assert(Mist::livepeerSwitchBroadcaster(current, "http://b", failedAB, three, pick) == Mist::LivepeerSwitchOutcome::Switched);
    assert(current == "http://c");

    // Another thread moved to a broadcaster this segment already failed on.
    current = "http://a";
    assert(Mist::livepeerSwitchBroadcaster(current, "http://b", failedAB, three, pick) == Mist::LivepeerSwitchOutcome::Switched);
    assert(current == "http://c");

    // Every broadcaster failed this segment: nothing changes.
    current = "http://b";
    assert(Mist::livepeerSwitchBroadcaster(current, "http://b", failedAB, two, pick) == Mist::LivepeerSwitchOutcome::NoAlternative);
    assert(current == "http://b");

    std::set<std::string> one;
    one.insert("http://a");
    current = "http://a";
    assert(Mist::livepeerSwitchBroadcaster(current, "http://a", failedFirst, one, pick) == Mist::LivepeerSwitchOutcome::NoAlternative);
    assert(current == "http://a");

    // Initial selection.
    current.clear();
    assert(Mist::livepeerSwitchBroadcaster(current, "", std::set<std::string>(), two, pick) == Mist::LivepeerSwitchOutcome::Switched);
    assert(two.count(current));
  }
  return 0;
}
