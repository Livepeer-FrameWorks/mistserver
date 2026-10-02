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

  // Live carries no configured deadline: every segment's budget is its
  // duration plus one second, sent as an integer deadlineMs.
  JSON::Value live;
  live["target_profiles"] = JSON::fromString("[{\"name\":\"360p\"}]");
  assert(Mist::livepeerSegmentDeadlineMs(live, 2000) == 3000);
  assert(Mist::livepeerSegmentDeadlineMs(live, 4170) == 5170);
  configuration = Mist::buildLivepeerTranscodeConfiguration(live, Mist::livepeerSegmentDeadlineMs(live, 2000));
  assert(configuration["deadlineMs"].isInt());
  assert(configuration["deadlineMs"].asInt() == 3000);
  live["deadline_ms"] = -5;
  assert(Mist::livepeerSegmentDeadlineMs(live, 2000) == 3000);
  // A configured deadline (VOD) wins over the segment duration and is capped
  // at the gateway's maximum.
  assert(Mist::livepeerSegmentDeadlineMs(options, 2000) == 45000);
  options["deadline_ms"] = 7200000;
  assert(Mist::livepeerSegmentDeadlineMs(options, 2000) == 3600000);
  assert(Mist::livepeerSegmentDeadlineMs(live, 7200000) == 3600000);

  assert(Mist::livepeerFatalUploadStatus(401));
  assert(Mist::livepeerFatalUploadStatus(403));
  // 503 means the gateway had no result within the budget; it is retried, not fatal.
  assert(!Mist::livepeerFatalUploadStatus(503));
  assert(!Mist::livepeerFatalUploadStatus(422));
  assert(!Mist::livepeerFatalUploadStatus(500));
  assert(!Mist::livepeerSegmentBudgetSpent(2999, 3000));
  assert(Mist::livepeerSegmentBudgetSpent(3000, 3000));
  assert(Mist::livepeerSegmentBudgetSpent(3600, 3000));

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

  // The socket outlasts the gateway's budget by one second, rounded up to
  // whole seconds; one attempt per request.
  assert(Mist::livepeerSocketTimeoutSeconds(3000) == 4);
  assert(Mist::livepeerSocketTimeoutSeconds(5170) == 7);
  assert(Mist::livepeerSocketTimeoutSeconds(30000) == 31);
  assert(Mist::LIVEPEER_DOWNLOADER_RETRY_COUNT == 1);

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
