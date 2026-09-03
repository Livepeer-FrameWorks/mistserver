#include "../src/process/livepeer_request.h"

#include <cassert>

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

  assert(Mist::livepeerShouldRetryCurrentBroadcaster(false, true));
  assert(!Mist::livepeerShouldRetryCurrentBroadcaster(false, false));
  assert(!Mist::livepeerShouldRetryCurrentBroadcaster(true, true));

  assert(Mist::livepeerSocketTimeoutSeconds(3900, 0) == 5);
  assert(Mist::livepeerSocketTimeoutSeconds(3900, 45000) == 50);
  assert(Mist::livepeerDownloaderRetryCount(0) == 2);
  assert(Mist::livepeerDownloaderRetryCount(45000) == 1);
  return 0;
}
