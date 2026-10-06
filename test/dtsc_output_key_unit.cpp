// Process outputs are registered and resumed by their output key (configured process identity
// plus output name), never by a match on codec, init or size alone.
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

  std::string fromHex(const std::string & hex) {
    std::string out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) { out.push_back((char)std::stoi(hex.substr(i, 2), 0, 16)); }
    return out;
  }

  // 854x480 H264 avcC, as a transcoder produces it for one profile.
  const std::string avcc =
    fromHex("0164001effe1001a6764001eacd940d83de6f0110000030001000003001e0f162d9601000468ef8fcbfdf8f800");
  // Another sequence parameter set, as an encoder configured differently produces it.
  const std::string otherAvcc =
    fromHex("0164001effe1001a6764001eacd940a02ff97011000003000100000300320f162d9601000468ef8fcbfdf8f800");

  DTSC::TrackMetadata rendition(const std::string & name, const std::string & init = avcc) {
    DTSC::TrackMetadata trk;
    trk.type = "video";
    trk.codec = "H264";
    trk.init = init;
    trk.output = name;
    return trk;
  }

  DTSC::TrackMetadata opus(const std::string & output) {
    DTSC::TrackMetadata trk;
    trk.type = "audio";
    trk.codec = "opus";
    trk.init = fromHex("4f707573486561640102380180bb0000000000");
    trk.output = output;
    return trk;
  }

  void releaseAll(DTSC::Meta & meta) {
    for (const size_t idx : meta.getValidTracks()) {
      if (meta.isClaimed(idx)) { meta.breakClaim(idx); }
    }
  }
} // namespace

int main() {
  const std::string configA = "{\"process\":\"Livepeer\",\"source\":\"live+a\",\"target_profiles\":[]}";
  const std::string configB = "{\"process\":\"AV\",\"codec\":\"opus\",\"source\":\"live+a\"}";
  const std::string idA = DTSC::processIdentity(configA);
  const std::string idB = DTSC::processIdentity(configB);
  expect(idA.size() == 16 && idA == DTSC::processIdentity(configA), "a process identity is a stable 16 hex digit hash");
  expect(idA != idB, "two configurations have different identities");
  const std::string session1 =
    "{\"process\":\"Livepeer\",\"source\":\"live+a\",\"target_profiles\":[],\"job_token\":\"v1.one\","
    "\"hardcoded_broadcasters\":\"[{\\\"address\\\":\\\"https://gw1\\\"}]\","
    "\"frameworks_gateway_cluster_ids\":[\"cell-1\"]}";
  const std::string session2 =
    "{\"process\":\"Livepeer\",\"source\":\"live+a\",\"target_profiles\":[],\"job_token\":\"v1.two\","
    "\"hardcoded_broadcasters\":\"[{\\\"address\\\":\\\"https://gw2\\\"}]\","
    "\"frameworks_gateway_cluster_ids\":[\"cell-2\"]}";
  expect(DTSC::processIdentity(session1) == DTSC::processIdentity(session2),
         "a new session's job token and gateways keep the process identity");
  expect(DTSC::processIdentity(session1) !=
           DTSC::processIdentity("{\"process\":\"Livepeer\",\"source\":\"live+a\",\"target_profiles\":[{"
                                 "\"name\":\"720p\"}],\"job_token\":\"v1.one\"}"),
         "changed renditions change the process identity");
  expect(DTSC::outputKey(idA, "720p") == idA + "/720p", "an output key is identity/output");
  expect(DTSC::outputKey(idA, "") == "" && DTSC::outputKey("", "720p") == "", "a key needs both parts");
  const std::string longName(150, 'x');
  const std::string longKey = DTSC::outputKey(idA, longName);
  expect(longKey.size() < 128 && longKey == DTSC::outputKey(idA, longName), "a long output name is replaced by a stable hash");
  expect(DTSC::outputKeyIdentity(idA + "/720p") == idA && DTSC::outputKeyIdentity("") == "", "the identity part of a key");

  DTSC::Meta meta;
  meta.reInit("", true);

  // An original opus track, published by an ingest: it has no key.
  DTSC::outputKeyScope.clear();
  DTSC::TrackMetadata original = opus("");
  const size_t originalOpus = meta.addOrResumeTrack(original);
  expect(meta.getOutputKey(originalOpus).empty(), "an ingest track carries no output key");
  meta.breakClaim(originalOpus);

  // Livepeer registers two profiles whose renditions have identical init and size.
  DTSC::outputKeyScope = idA;
  const size_t p1 = meta.addOrResumeTrack(rendition("p1"));
  const size_t p2 = meta.addOrResumeTrack(rendition("p2"));
  expect(p1 != p2, "two outputs get two tracks");
  expect(meta.getOutputKey(p1) == idA + "/p1" && meta.getOutputKey(p2) == idA + "/p2", "each track stores its output key");

  // A restart (another orchestrator, same encoder output) registers them in the other order.
  releaseAll(meta);
  expect(meta.addOrResumeTrack(rendition("p2")) == p2, "a restarted producer resumes p2 by key, not the first look-alike");
  expect(meta.addOrResumeTrack(rendition("p1")) == p1, "a restarted producer resumes p1 by key");
  expect(meta.getValidTracks().size() == 3, "resuming creates no tracks");

  // The delayed path (MPEG-TS renditions) resumes by key the same way.
  releaseAll(meta);
  expect(meta.addOrResumeDelayedTrack(rendition("p2")) == p2, "a delayed registration resumes by key");
  expect(meta.addOrResumeDelayedTrack(rendition("p1")) == p1, "a delayed registration resumes its own key");

  // A restart whose rendition changed (another resolution) replaces its track: a new track gets
  // the key, the old one keeps its data, unclaimed, for the buffer to retire.
  releaseAll(meta);
  const size_t replaced = meta.addOrResumeTrack(rendition("p1", otherAvcc));
  expect(replaced != p1 && replaced != p2, "a changed rendition gets a replacement track");
  expect(meta.getOutputKey(replaced) == idA + "/p1", "the replacement carries the output key");
  expect(meta.findOutputKeyTrack(idA + "/p1") == replaced, "the key now finds the replacement");
  expect(!meta.isClaimed(p1) && meta.getInit(p1) == avcc, "the replaced track is left unchanged and unclaimed");

  // An AV opus encoder never resumes the original opus track, however alike they are.
  DTSC::outputKeyScope = idB;
  const size_t encoded = meta.addOrResumeTrack(opus("audio"));
  expect(encoded != originalOpus, "a keyed producer never takes an ingest track");
  // An AV audio encoder registers before its first packet, without init, and resumes anyway.
  meta.breakClaim(encoded);
  DTSC::TrackMetadata noInitYet = opus("audio");
  noInitYet.init.clear();
  expect(meta.addOrResumeTrack(noInitYet) == encoded, "an output registered without its init yet resumes its track");

  // A replacement configuration (another identity) never takes the replaced producer's outputs.
  releaseAll(meta);
  DTSC::outputKeyScope = idB;
  expect(meta.addOrResumeTrack(rendition("p2")) != p2, "another process identity gets its own track");
  // An ingest reconnecting never takes a process output.
  releaseAll(meta);
  DTSC::outputKeyScope.clear();
  DTSC::TrackMetadata ingest = rendition("");
  expect(meta.addOrResumeTrack(ingest) != p2, "an ingest never resumes a process output");

  if (failures) { return 1; }
  std::cout << "process outputs resume by output key only" << std::endl;
  return 0;
}
