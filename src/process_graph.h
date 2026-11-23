#pragma once

#include <mist/defines.h>
#include <mist/dtsc.h>
#include <mist/http_parser.h>
#include <mist/json.h>
#include <mist/stream.h>

#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

/// The processing graph of a stream: which configured processes run and which tracks they add,
/// decided before any of them runs. Every process declares the selection it reads its input with
/// and the tracks it adds for its configuration (src/process/process_outputs.h). Starting from the
/// tracks the stream has, a process runs when its own track selection selects a track, either an
/// existing one or one a running process declares; its declared outputs then join the candidate
/// tracks. Repeating this until nothing changes resolves chains (a transcode feeding an inference
/// process) without either of them running. Recorders select from the same candidates with their
/// own selection, so they know which outputs to wait for before writing a header.
namespace Mist {
  /// The tracks a process selects with its declared selection (see process_outputs.h), as it
  /// would at runtime: through the processing validity mask.
  inline std::set<size_t> declaredSelectionMatches(const DTSC::Meta & C, const JSON::Value & select) {
    std::map<std::string, std::string> params;
    if (select["target"].isObject()) {
      jsonForEachConst (select["target"], it) { params[it.key()] = it->asString(); }
    }
    JSON::Value capa;
    if (select.isMember("codecs")) { capa["codecs"] = select["codecs"]; }
    const uint8_t oldMask = DTSC::trackValidMask;
    DTSC::trackValidMask = TRACK_VALID_INT_PROCESS;
    std::set<size_t> selected = Util::wouldSelect(C, params, capa);
    DTSC::trackValidMask = oldMask;
    return selected;
  }

  /// The tracks of M a process reads: what its declared selection selects, or, without a
  /// declaration, one track of each type its track_select names (as MistProcAV narrows its
  /// selection) or what its source_track selects. A process without any selection reads every
  /// track. The size inhibitors of a process and its outputs judge only these tracks.
  inline std::set<size_t> processSourceTracks(const DTSC::Meta & M, const JSON::Value & proc, const JSON::Value & declaration) {
    if (declaration.isObject()) {
      if (declaration["select"].isNull()) { return std::set<size_t>(); }
      return declaredSelectionMatches(M, declaration["select"]);
    }
    if (proc["track_select"].isString()) {
      std::map<std::string, std::string> params;
      HTTP::parseVars(proc["track_select"].asStringRef(), params);
      for (const char *type : {"audio", "video", "subtitle", "meta"}) {
        std::map<std::string, std::string>::iterator param = params.find(type);
        if (param == params.end() || param->second.empty() || param->second == "none" || param->second == "-1") {
          continue;
        }
        param->second += ",|first";
      }
      return Util::wouldSelect(M, params);
    }
    if (proc["source_track"].isString()) {
      return Util::findTracks(M, JSON::Value(), "", proc["source_track"].asStringRef());
    }
    return M.getValidTracks(true);
  }

  /// Why one rendition (a process, or one declared output of it) is not produced: an original
  /// track satisfies its track_retain (Util::retainedTrack, against its target bitrate), or its
  /// track_inhibit selects one of the original tracks the process reads (sources). Empty when it
  /// is produced.
  inline std::string renditionSkipReason(const DTSC::Meta & M, const JSON::Value & rendition, uint64_t bitrate,
                                         const std::set<size_t> & sources) {
    if (rendition["track_retain"].isString() && rendition["track_retain"].asStringRef().size()) {
      const size_t retained = Util::retainedTrack(M, rendition["track_retain"].asStringRef(), bitrate);
      if (retained != INVALID_TRACK_ID) {
        return "track_retain '" + rendition["track_retain"].asString() + "' satisfied by track " + std::to_string(retained);
      }
    }
    if (rendition.isMember("track_inhibit") && Util::inhibitorMatchesSource(M, rendition["track_inhibit"].asStringRef(), sources)) {
      return "track_inhibit '" + rendition["track_inhibit"].asString() + "' matches source tracks";
    }
    return "";
  }

  /// Whether a process, or a rendition it declares, carries a track_retain.
  inline bool carriesTrackRetain(const JSON::Value & proc, const JSON::Value & declaration) {
    if (proc["track_retain"].isString() && proc["track_retain"].asStringRef().size()) { return true; }
    if (!declaration["outputs"].isArray()) { return false; }
    jsonForEachConst (declaration["outputs"], it) {
      if ((*it)["track_retain"].isString() && (*it)["track_retain"].asStringRef().size()) { return true; }
    }
    return false;
  }

  /// Why the stream's tags or original tracks keep a configured process (or one declared output,
  /// for its track_inhibit) from running: a tags_inhibit tag set on the stream, an original track
  /// that satisfies its track_retain, or a track_inhibit that selects an original (ingest) track it
  /// reads (see processSourceTracks; declaration is its --describe-outputs answer, null when it
  /// gave none). Outputs of processes never inhibit. While an original video track does not report
  /// its geometry and bitrate yet, a process carrying a track_retain waits, unless waitForRetention
  /// is false (the source ended, so nothing more will be known). Empty when nothing inhibits it.
  inline std::string processInhibitReason(const JSON::Value & proc, const DTSC::Meta & M, const std::set<std::string> & streamTags,
                                          const JSON::Value & declaration = JSON::Value(), bool waitForRetention = true) {
    if (proc.isMember("tags_inhibit")) {
      auto matchesTag = [&streamTags](const JSON::Value & tag) {
        if (!tag.isString()) { return false; }
        const std::string & name = tag.asStringRef();
        return streamTags.count(name.size() && name[0] == '#' ? name.substr(1) : name) != 0;
      };
      const JSON::Value & inhibit = proc["tags_inhibit"];
      if (inhibit.isString() && matchesTag(inhibit)) { return "inhibited by stream tag '" + inhibit.asString() + "'"; }
      if (inhibit.isArray()) {
        jsonForEachConst (inhibit, it) {
          if (matchesTag(*it)) { return "inhibited by stream tag '" + it->asString() + "'"; }
        }
      }
    }
    size_t pending = INVALID_TRACK_ID;
    if (waitForRetention && carriesTrackRetain(proc, declaration) && Util::retentionPending(M, &pending)) {
      return "track_retain waits for the geometry and bitrate of original track " + std::to_string(pending);
    }
    const std::set<size_t> sources = processSourceTracks(M, proc, declaration);
    if (proc.isMember("track_retain") || proc.isMember("track_inhibit")) {
      const std::string reason = renditionSkipReason(M, proc, proc["bitrate"].asInt(), sources);
      if (reason.size()) { return reason; }
    }
    // A process whose every declared rendition is retained or inhibited has nothing to produce.
    if (!declaration["outputs"].isArray() || !declaration["outputs"].size()) { return ""; }
    std::string firstReason;
    jsonForEachConst (declaration["outputs"], it) {
      if (!it->isMember("track_retain") && !it->isMember("track_inhibit")) { return ""; }
      const std::string reason = renditionSkipReason(M, *it, (*it)["track_retain_bitrate"].asInt(), sources);
      if (reason.empty()) { return ""; }
      if (firstReason.empty()) { firstReason = (*it)["output"].asString() + ": " + reason; }
    }
    return "every declared rendition is retained or inhibited (" + firstReason + ")";
  }

  /// The description of an existing track that track selection looks at, plus its validity mask,
  /// output key, whether it is an original (ingest) track and whether it carries data.
  inline JSON::Value describeExistingTrack(const DTSC::Meta & M, size_t idx, bool hasData) {
    JSON::Value desc;
    desc["type"] = M.getType(idx);
    desc["codec"] = M.getCodec(idx);
    desc["lang"] = M.getLang(idx);
    desc["width"] = M.getWidth(idx);
    desc["height"] = M.getHeight(idx);
    desc["rotation"] = M.getRotation(idx);
    desc["mirror"] = M.getMirror(idx);
    desc["rate"] = M.getRate(idx);
    desc["channels"] = M.getChannels(idx);
    desc["size"] = M.getSize(idx);
    desc["bps"] = M.getBps(idx);
    desc["fpks"] = M.getFpks(idx);
    desc["mask"] = M.trackValid(idx);
    desc["key"] = M.getOutputKey(idx);
    desc["original"] = M.getSourceTrack(idx) == INVALID_TRACK_ID;
    desc["data"] = hasData;
    return desc;
  }

  /// Adds a described track to a memory-backed meta, holding one packet when it carries data, so
  /// track selection treats it like the track it describes.
  inline size_t addCandidateTrack(DTSC::Meta & C, const JSON::Value & desc) {
    const size_t idx = C.addTrack(2, 2, 2, 2);
    if (idx == INVALID_TRACK_ID) { return idx; }
    C.setID(idx, idx + 1);
    C.setType(idx, desc["type"].asString());
    C.setCodec(idx, desc["codec"].asString());
    C.setLang(idx, desc["lang"].asString());
    C.setWidth(idx, desc["width"].asInt());
    C.setHeight(idx, desc["height"].asInt());
    C.setRotation(idx, desc["rotation"].asInt());
    C.setMirror(idx, desc["mirror"].asBool());
    C.setRate(idx, desc["rate"].asInt());
    C.setChannels(idx, desc["channels"].asInt());
    C.setSize(idx, desc["size"].asInt());
    C.setFpks(idx, desc["fpks"].asInt());
    if (!desc["original"].asBool()) { C.setSourceTrack(idx, idx); }
    if (desc["key"].asString().size()) { C.setOutputKey(idx, desc["key"].asString()); }
    C.validateTrack(idx, desc["mask"].asInt());
    if (desc["data"].asBool()) { C.update(0, 0, idx, 1, 0, true, 1); }
    C.setBps(idx, desc["bps"].asInt());
    // Tracks claimed by this process would pass every validity mask.
    C.breakClaim(idx);
    return idx;
  }

  /// Whether a process without a declaration selects a track: by its configured source_track and
  /// track_select, as the supervisor decided before processes declared their selection.
  inline bool undeclaredSelectionMatches(const DTSC::Meta & C, const JSON::Value & proc) {
    if (proc.isMember("source_track") && Util::findTracks(C, JSON::Value(), "", proc["source_track"].asStringRef()).empty()) {
      return false;
    }
    if (proc.isMember("track_select") && Util::wouldSelect(C, proc["track_select"].asStringRef()).empty()) {
      return false;
    }
    return true;
  }

  /// One configured process as the graph sees it.
  struct ProcessGraphNode {
      std::string config; ///< the configuration the buffer keys and starts it with
      JSON::Value proc; ///< that configuration, parsed
      JSON::Value declaration; ///< its --describe-outputs answer; null when it gave none
      std::string inhibited; ///< processInhibitReason for it, empty when not inhibited
      bool producesHere = true; ///< its outputs go to this stream (its sink is this stream)
      bool producing = true; ///< it will (re)produce its outputs: not retired
  };

  struct ProcessGraph {
      std::set<std::string> runs; ///< configs that run
      std::map<std::string, std::string> skipReasons; ///< why each other config does not run
      /// The outputs the running, producing processes add to this stream: one description per
      /// declared output (see describeExistingTrack) with its output key and the config producing it.
      JSON::Value outputs;
  };

  /// The declared outputs of a node that go to this stream and are not dropped by their own
  /// track_retain or track_inhibit (renditionSkipReason), keyed by output key.
  inline std::map<std::string, JSON::Value> declaredNodeOutputs(const ProcessGraphNode & node, const DTSC::Meta & M) {
    std::map<std::string, JSON::Value> outputs;
    if (!node.producesHere || !node.declaration["outputs"].isArray()) { return outputs; }
    const std::string identity = DTSC::processIdentity(node.config);
    std::set<size_t> sources;
    bool sourcesKnown = false;
    jsonForEachConst (node.declaration["outputs"], it) {
      const std::string key = DTSC::outputKey(identity, (*it)["output"].asString());
      if (key.empty()) { continue; }
      if (it->isMember("track_retain") || it->isMember("track_inhibit")) {
        if (!sourcesKnown) {
          sources = processSourceTracks(M, node.proc, node.declaration);
          sourcesKnown = true;
        }
        if (renditionSkipReason(M, *it, (*it)["track_retain_bitrate"].asInt(), sources).size()) { continue; }
      }
      JSON::Value desc = *it;
      desc.removeMember("track_inhibit");
      desc.removeMember("track_retain");
      desc.removeMember("track_retain_bitrate");
      desc["key"] = key;
      desc["original"] = false;
      desc["data"] = true;
      desc["producer"] = node.config;
      outputs[key] = desc;
    }
    return outputs;
  }

  /// Builds the candidate tracks the processes select from: the tracks of M (holding data when
  /// they do, or when a running process will keep producing them) plus the declared outputs of the
  /// running processes that do not exist yet. Returns, for every candidate that is a process
  /// output, the identity of the process that produces it.
  inline std::map<size_t, std::string> buildCandidates(DTSC::Meta & C, const DTSC::Meta & M,
                                                       const std::map<std::string, std::map<std::string, JSON::Value>> & declared,
                                                       const std::set<std::string> & runs) {
    std::map<size_t, std::string> producers;
    std::map<std::string, const JSON::Value *> predicted;
    for (const std::string & config : runs) {
      std::map<std::string, std::map<std::string, JSON::Value>>::const_iterator outputs = declared.find(config);
      if (outputs == declared.end()) { continue; }
      for (const auto & output : outputs->second) { predicted[output.first] = &output.second; }
    }
    const uint8_t oldMask = DTSC::trackValidMask;
    DTSC::trackValidMask = TRACK_VALID_ALL;
    const std::set<size_t> withData = M.getValidTracks(true);
    const std::set<size_t> existing = M.getValidTracks();
    DTSC::trackValidMask = oldMask;
    // Room for every candidate up front: this runs on every buffer tick and recording check.
    C.reInitMemory(existing.size() + predicted.size());
    std::set<std::string> existingKeys;
    for (const size_t idx : existing) {
      const std::string key = M.getOutputKey(idx);
      const size_t candidate =
        addCandidateTrack(C, describeExistingTrack(M, idx, withData.count(idx) || (key.size() && predicted.count(key))));
      if (key.empty()) { continue; }
      existingKeys.insert(key);
      producers[candidate] = DTSC::outputKeyIdentity(key);
    }
    for (const auto & output : predicted) {
      if (existingKeys.count(output.first)) { continue; }
      producers[addCandidateTrack(C, *output.second)] = DTSC::outputKeyIdentity(output.first);
    }
    return producers;
  }

  /// Whether a process selects a track among the candidates, leaving out its own outputs.
  inline bool nodeSelects(DTSC::Meta & C, const std::map<size_t, std::string> & producers, const ProcessGraphNode & node) {
    if (node.declaration.isObject() && node.declaration["select"].isNull()) { return true; }
    const std::string identity = DTSC::processIdentity(node.config);
    std::map<size_t, uint8_t> own;
    for (const auto & producer : producers) {
      if (producer.second != identity) { continue; }
      own[producer.first] = C.trackValid(producer.first);
      C.validateTrack(producer.first, 0);
    }
    const bool selects = node.declaration.isObject() ? !declaredSelectionMatches(C, node.declaration["select"]).empty()
                                                     : undeclaredSelectionMatches(C, node.proc);
    for (const auto & mask : own) { C.validateTrack(mask.first, mask.second); }
    return selects;
  }

  /// Resolves which processes run and which outputs they add, before any of them runs (see the
  /// comment at the top of this file). A process never selects its own outputs, and the number of
  /// rounds is bounded by the number of processes, since every round that changes anything adds a
  /// process.
  inline ProcessGraph buildProcessGraph(const DTSC::Meta & M, const std::vector<ProcessGraphNode> & nodes) {
    ProcessGraph graph;
    graph.outputs.append(JSON::Value());
    graph.outputs.shrink(0);
    std::map<std::string, std::map<std::string, JSON::Value>> declared;
    for (const ProcessGraphNode & node : nodes) { declared[node.config] = declaredNodeOutputs(node, M); }
    DTSC::Meta C;
    for (size_t round = 0; round <= nodes.size(); ++round) {
      const std::map<size_t, std::string> producers = buildCandidates(C, M, declared, graph.runs);
      std::set<std::string> joined;
      for (const ProcessGraphNode & node : nodes) {
        if (graph.runs.count(node.config) || node.inhibited.size()) { continue; }
        if (nodeSelects(C, producers, node)) { joined.insert(node.config); }
      }
      if (joined.empty()) { break; }
      graph.runs.insert(joined.begin(), joined.end());
    }
    for (const ProcessGraphNode & node : nodes) {
      if (graph.runs.count(node.config)) {
        if (!node.producing) { continue; }
        for (const auto & output : declared[node.config]) { graph.outputs.append(output.second); }
        continue;
      }
      if (node.inhibited.size()) {
        graph.skipReasons[node.config] = node.inhibited;
      } else {
        graph.skipReasons[node.config] = "its track selection matches no source track or declared process output";
      }
    }
    return graph;
  }

  /// Whether a process selects a track that exists with data now, without counting outputs of
  /// processes that have not produced them yet.
  inline bool nodeSelectsExistingTracks(const DTSC::Meta & M, const ProcessGraphNode & node) {
    DTSC::Meta C;
    const std::map<size_t, std::string> producers =
      buildCandidates(C, M, std::map<std::string, std::map<std::string, JSON::Value>>(), std::set<std::string>());
    return nodeSelects(C, producers, node);
  }

  /// The page a processing stream's buffer publishes its graph on (SHM_STREAM_PGRAPH): a sequence
  /// number that is odd while the buffer writes, the length of the JSON that follows, and
  /// {"resolved": bool, "outputs": [...]} with the expected outputs of ProcessGraph::outputs.
  /// The graph is resolved once the stream has its source tracks.
  static const size_t PROCESS_GRAPH_HEADER_LEN = 8;

  inline bool writeProcessGraphPage(IPC::sharedPage & page, const std::string & json) {
    if (!page.mapped || page.len < PROCESS_GRAPH_HEADER_LEN + json.size()) { return false; }
    uint32_t *seq = (uint32_t *)page.mapped;
    uint32_t next = __atomic_load_n(seq, __ATOMIC_RELAXED) | 1;
    __atomic_store_n(seq, next, __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    const uint32_t len = json.size();
    memcpy(page.mapped + 4, &len, sizeof(len));
    memcpy(page.mapped + PROCESS_GRAPH_HEADER_LEN, json.data(), json.size());
    __atomic_store_n(seq, next + 1, __ATOMIC_RELEASE);
    return true;
  }

  inline bool readProcessGraphPage(const IPC::sharedPage & page, JSON::Value & graph) {
    if (!page.mapped || page.len < PROCESS_GRAPH_HEADER_LEN) { return false; }
    const uint32_t *seq = (const uint32_t *)page.mapped;
    for (size_t attempt = 0; attempt < 3; ++attempt) {
      const uint32_t before = __atomic_load_n(seq, __ATOMIC_ACQUIRE);
      if (before & 1) { continue; }
      uint32_t len = 0;
      memcpy(&len, page.mapped + 4, sizeof(len));
      if (len > page.len - PROCESS_GRAPH_HEADER_LEN) { return false; }
      const std::string json(page.mapped + PROCESS_GRAPH_HEADER_LEN, len);
      __atomic_thread_fence(__ATOMIC_ACQUIRE);
      if (__atomic_load_n(seq, __ATOMIC_ACQUIRE) != before) { continue; }
      graph = JSON::fromString(json);
      return graph.isObject();
    }
    return false;
  }

  /// The tracks of M that carry data, whatever validity mask hides them from this process.
  inline std::set<size_t> tracksWithDataAnyMask(const DTSC::Meta & M) {
    const uint8_t mask = DTSC::trackValidMask;
    DTSC::trackValidMask = TRACK_VALID_ALL;
    const std::set<size_t> withData = M.getValidTracks(true);
    DTSC::trackValidMask = mask;
    return withData;
  }

  /// The original continuous-video tracks a ladder replaces (mask_replaced_originals), with the
  /// validity mask each gets. Once a running, producing process that sets mask_replaced_originals
  /// has one of its renditions here carrying data, every original continuous-video track that
  /// retains no rendition of any configured process (process or declared-output track_retain),
  /// the transcode source included, is hidden from viewers and pushes and stays readable by
  /// processes: the process's source_mask, TRACK_VALID_INT_PROCESS without one. A source that
  /// retains no rendition while none is produced (it is below every rung) keeps playing. Empty
  /// while no such process produces.
  inline std::map<size_t, uint8_t>
    replacedOriginals(const DTSC::Meta & M, const std::vector<ProcessGraphNode> & nodes, const ProcessGraph & graph) {
    std::map<size_t, uint8_t> replaced;
    const std::set<size_t> withData = tracksWithDataAnyMask(M);
    uint8_t mask = TRACK_VALID_ALL;
    bool producing = false;
    for (const ProcessGraphNode & node : nodes) {
      if (!node.proc["mask_replaced_originals"].asBool() || !node.producing || !graph.runs.count(node.config)) {
        continue;
      }
      for (const auto & output : declaredNodeOutputs(node, M)) {
        if (output.second["type"].asStringRef() != "video") { continue; }
        const size_t track = M.findOutputKeyTrack(output.first);
        if (track == INVALID_TRACK_ID || !withData.count(track)) { continue; }
        producing = true;
        mask &= node.proc.isMember("source_mask") ? node.proc["source_mask"].asInt() : TRACK_VALID_INT_PROCESS;
        break;
      }
    }
    if (!producing) { return replaced; }
    std::set<size_t> retaining;
    for (const ProcessGraphNode & node : nodes) {
      if (node.proc["track_retain"].isString()) {
        retaining.insert(Util::retainedTrack(M, node.proc["track_retain"].asStringRef(), node.proc["bitrate"].asInt()));
      }
      if (!node.declaration["outputs"].isArray()) { continue; }
      jsonForEachConst (node.declaration["outputs"], it) {
        if (!(*it)["track_retain"].isString()) { continue; }
        retaining.insert(Util::retainedTrack(M, (*it)["track_retain"].asStringRef(), (*it)["track_retain_bitrate"].asInt()));
      }
    }
    for (const size_t track : withData) {
      if (M.getType(track) != "video" || Util::isImageCodec(M.getCodec(track)) || M.getSourceTrack(track) != INVALID_TRACK_ID) {
        continue;
      }
      if (!retaining.count(track)) { replaced[track] = mask; }
    }
    return replaced;
  }

  /// Of the outputs a processing graph expects (ProcessGraph::outputs), those a recording selects
  /// from the stream as it will be once they all exist: the selection runs over the tracks of M
  /// plus every expected output, with the recording's own target parameters, capabilities and
  /// validity mask. Counts how many of them it selects, and how many of those carry data in
  /// `withData` (tracksWithDataAnyMask). A recording passes the set it took before its last track
  /// selection: an output whose first packet landed after that is not selected yet, so it must
  /// not count as ready either, or the header would be written without it.
  inline void recordingExpectedOutputs(const DTSC::Meta & M, const std::set<size_t> & withData, const JSON::Value & expected,
                                       const std::map<std::string, std::string> & targetParams, const JSON::Value & capa,
                                       const std::string & UA, size_t & selected, size_t & ready) {
    selected = 0;
    ready = 0;
    std::map<std::string, const JSON::Value *> outputs;
    jsonForEachConst (expected, it) { outputs[(*it)["key"].asString()] = &*it; }
    std::map<size_t, bool> expectedCandidates; // candidate -> carries data in withData
    const uint8_t mask = DTSC::trackValidMask;
    DTSC::trackValidMask = TRACK_VALID_ALL;
    const std::set<size_t> existing = M.getValidTracks();
    DTSC::trackValidMask = mask;
    // Room for every candidate up front: a recording runs this on every track check.
    DTSC::Meta C;
    C.reInitMemory(existing.size() + outputs.size());
    std::set<std::string> existingKeys;
    for (const size_t idx : existing) {
      const std::string key = M.getOutputKey(idx);
      const bool isExpected = key.size() && outputs.count(key);
      const size_t candidate = addCandidateTrack(C, describeExistingTrack(M, idx, withData.count(idx) || isExpected));
      if (!isExpected) { continue; }
      existingKeys.insert(key);
      expectedCandidates[candidate] = withData.count(idx);
    }
    for (const auto & output : outputs) {
      if (existingKeys.count(output.first)) { continue; }
      expectedCandidates[addCandidateTrack(C, *output.second)] = false;
    }
    const std::set<size_t> selection = Util::wouldSelect(C, targetParams, capa, UA);
    for (const size_t idx : selection) {
      std::map<size_t, bool>::const_iterator candidate = expectedCandidates.find(idx);
      if (candidate == expectedCandidates.end()) { continue; }
      ++selected;
      if (candidate->second) { ++ready; }
    }
  }
} // namespace Mist
