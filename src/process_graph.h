#pragma once

#include <mist/defines.h>
#include <mist/dtsc.h>
#include <mist/json.h>
#include <mist/stream.h>

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
  /// Why the stream's tags or original tracks keep a configured process (or one declared output,
  /// for its track_inhibit) from running: a tags_inhibit tag set on the stream, or a track_inhibit
  /// that selects an original (ingest) track. Outputs of processes never inhibit. Empty when
  /// nothing inhibits it.
  inline std::string processInhibitReason(const JSON::Value & proc, const DTSC::Meta & M, const std::set<std::string> & streamTags) {
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
    if (proc.isMember("track_inhibit") && Util::inhibitorMatchesSource(M, proc["track_inhibit"].asStringRef())) {
      return "track_inhibit '" + proc["track_inhibit"].asString() + "' matches source tracks";
    }
    return "";
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
  /// track_inhibit, keyed by output key.
  inline std::map<std::string, JSON::Value> declaredNodeOutputs(const ProcessGraphNode & node, const DTSC::Meta & M) {
    std::map<std::string, JSON::Value> outputs;
    if (!node.producesHere || !node.declaration["outputs"].isArray()) { return outputs; }
    const std::string identity = DTSC::processIdentity(node.config);
    jsonForEachConst (node.declaration["outputs"], it) {
      const std::string key = DTSC::outputKey(identity, (*it)["output"].asString());
      if (key.empty()) { continue; }
      if (it->isMember("track_inhibit") && Util::inhibitorMatchesSource(M, (*it)["track_inhibit"].asStringRef())) {
        continue;
      }
      JSON::Value desc = *it;
      desc.removeMember("track_inhibit");
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
    C.reInit("", true);
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
} // namespace Mist
