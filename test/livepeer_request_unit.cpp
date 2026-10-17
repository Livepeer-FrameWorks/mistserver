#include "../src/process/livepeer_request.h"

#include <cassert>
#include <map>
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
  assert(Mist::livepeerSegmentDeadlineMs(live, 2000, false) == 3000);
  assert(Mist::livepeerSegmentDeadlineMs(live, 4170, false) == 5170);
  configuration = Mist::buildLivepeerTranscodeConfiguration(live, Mist::livepeerSegmentDeadlineMs(live, 2000, false));
  assert(configuration["deadlineMs"].isInt());
  assert(configuration["deadlineMs"].asInt() == 3000);
  // The segment that starts the session on a gateway also pays for the session setup there,
  // and its socket outlasts that budget too.
  assert(Mist::LIVEPEER_SESSION_SETUP_ALLOWANCE_MS == 2000);
  assert(Mist::livepeerSegmentDeadlineMs(live, 2000, true) == 5000);
  assert(Mist::livepeerSocketTimeoutSeconds(Mist::livepeerSegmentDeadlineMs(live, 2000, true)) == 6);
  configuration = Mist::buildLivepeerTranscodeConfiguration(live, Mist::livepeerSegmentDeadlineMs(live, 2000, true));
  assert(configuration["deadlineMs"].asInt() == 5000);
  live["deadline_ms"] = -5;
  assert(Mist::livepeerSegmentDeadlineMs(live, 2000, false) == 3000);
  // A configured deadline (VOD) wins over the segment duration and is capped
  // at the gateway's maximum.
  assert(Mist::livepeerSegmentDeadlineMs(options, 2000, false) == 45000);
  assert(Mist::livepeerSegmentDeadlineMs(options, 2000, true) == 47000);
  options["deadline_ms"] = 7200000;
  assert(Mist::livepeerSegmentDeadlineMs(options, 2000, false) == 3600000);
  assert(Mist::livepeerSegmentDeadlineMs(options, 2000, true) == 3600000);
  assert(Mist::livepeerSegmentDeadlineMs(live, 7200000, false) == 3600000);

  {
    // Segment 0 starts the session on the first broadcaster, including its re-sends there;
    // the concurrent segment 1 and every later one do not. After a switch, the first segment
    // sent to the new broadcaster starts the session there; a re-send of segment 0 to the
    // first broadcaster still does.
    std::map<std::string, uint64_t> starts;
    assert(Mist::livepeerSegmentStartsSession(starts, "http://a", 0));
    assert(!Mist::livepeerSegmentStartsSession(starts, "http://a", 1));
    assert(Mist::livepeerSegmentStartsSession(starts, "http://a", 0));
    assert(!Mist::livepeerSegmentStartsSession(starts, "http://a", 2));
    assert(Mist::livepeerSegmentStartsSession(starts, "http://b", 7));
    assert(Mist::livepeerSegmentStartsSession(starts, "http://b", 7));
    assert(!Mist::livepeerSegmentStartsSession(starts, "http://b", 8));
    assert(!Mist::livepeerSegmentStartsSession(starts, "http://a", 9));
  }

  assert(Mist::livepeerFatalUploadStatus(401));
  assert(Mist::livepeerFatalUploadStatus(403));
  // 503 means the gateway had no result within the budget; it is retried, not fatal.
  assert(!Mist::livepeerFatalUploadStatus(503));
  assert(!Mist::livepeerFatalUploadStatus(422));
  assert(!Mist::livepeerFatalUploadStatus(500));
  assert(!Mist::livepeerSegmentBudgetSpent(2999, 3000));
  assert(Mist::livepeerSegmentBudgetSpent(3000, 3000));
  assert(Mist::livepeerSegmentBudgetSpent(3600, 3000));
  // Until as many consecutive budgets were missed as segments may be
  // rejected, a live segment that missed its budget is skipped and a VOD or
  // processing segment is sent again; then every job falls back.
  assert(Mist::livepeerBudgetMissStep(live, "live+stream", 1) == Mist::LivepeerBudgetMissStep::SkipSegment);
  assert(Mist::livepeerBudgetMissStep(live, "live+stream", 4) == Mist::LivepeerBudgetMissStep::SkipSegment);
  assert(Mist::livepeerBudgetMissStep(live, "live+stream", 5) == Mist::LivepeerBudgetMissStep::Fallback);
  assert(Mist::livepeerBudgetMissStep(live, "processing+abc", 1) == Mist::LivepeerBudgetMissStep::ResendSegment);
  assert(Mist::livepeerBudgetMissStep(live, "processing+abc", 4) == Mist::LivepeerBudgetMissStep::ResendSegment);
  assert(Mist::livepeerBudgetMissStep(live, "processing+abc", 5) == Mist::LivepeerBudgetMissStep::Fallback);
  JSON::Value vod;
  vod["workload"] = "vod";
  assert(Mist::livepeerBudgetMissStep(vod, "vod+abc", 1) == Mist::LivepeerBudgetMissStep::ResendSegment);
  assert(Mist::livepeerBudgetMissStep(vod, "vod+abc", 5) == Mist::LivepeerBudgetMissStep::Fallback);

  assert(!Mist::livepeerShouldFallback(4));
  assert(Mist::livepeerShouldFallback(5));
  assert(Mist::livepeerShouldFallback(6));

  // A first rejection moves the segment to another broadcaster; a second gives it up.
  assert(Mist::livepeerRejectionStep(false) == Mist::LivepeerRejectionStep::SwitchBroadcaster);
  assert(Mist::livepeerRejectionStep(true) == Mist::LivepeerRejectionStep::RejectSegment);

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

  // The session warm-up is sent for live streams through a configured gateway only.
  {
    JSON::Value liveJob;
    liveJob["hardcoded_broadcasters"] = "http://127.0.0.1:8935";
    assert(Mist::livepeerSessionWarmupWanted(liveJob, "live+abc→live+abc"));
    assert(!Mist::livepeerSessionWarmupWanted(liveJob, "processing+abc→processing+abc"));
    JSON::Value vodJob = liveJob;
    vodJob["workload"] = "vod";
    assert(!Mist::livepeerSessionWarmupWanted(vodJob, "live+abc→live+abc"));
    JSON::Value hosted = liveJob;
    hosted["access_token"] = "secret";
    assert(!Mist::livepeerSessionWarmupWanted(hosted, "live+abc→live+abc"));

    // It never carries a duration-derived deadline, which a gateway without an auth webhook would
    // take as the whole stream's deadline; a configured one it carries like every segment.
    assert(Mist::livepeerSessionWarmupDeadlineMs(liveJob) == 0);
    liveJob["deadline_ms"] = 12000;
    assert(Mist::livepeerSessionWarmupDeadlineMs(liveJob) == 12000);
    liveJob["deadline_ms"] = -1;
    assert(Mist::livepeerSessionWarmupDeadlineMs(liveJob) == 0);
    liveJob["deadline_ms"] = 999999999;
    assert(Mist::livepeerSessionWarmupDeadlineMs(liveJob) == Mist::LIVEPEER_MAX_DEADLINE_MS);

    assert(Mist::livepeerSessionWarmupFrameMs(25000) == 40);
    assert(Mist::livepeerSessionWarmupFrameMs(30000) == 33);
    assert(Mist::livepeerSessionWarmupFrameMs(0) == 40);
    assert(Mist::livepeerSessionWarmupFrameMs(5000000) == 1);

    // The gateway cannot probe a frame rate from the one keyframe; the warm-up states it.
    assert(Mist::livepeerSessionWarmupFrameRate(15000) == "15.000");
    assert(Mist::livepeerSessionWarmupFrameRate(29970) == "29.970");
    assert(Mist::livepeerSessionWarmupFrameRate(0) == "");
  }
  return 0;
}
