#pragma once

#include <mist/config.h>
#include <mist/dtsc.h>
#include <mist/http_parser.h>
#include <mist/json.h>

#include <iostream>
#include <map>
#include <string>

/// A process declares, for its configuration, the selection it reads its input with and the tracks
/// it adds. The buffer of a processing stream asks for it with --describe-outputs before starting
/// the process, and builds its processing graph from the declarations. Every process builds its
/// declaration with the same functions it describes its source selection and output tracks with
/// at runtime, and warns when it creates an output track it did not declare.
///
/// Declaration JSON:
///   select:  the target parameters and codec capabilities the process selects its input tracks
///            with ({"target": {...}, "codecs": [...]}), or null for a process that does not read
///            tracks of the stream it runs for.
///   outputs: one entry per track it adds: output name, type, codec, validity mask and, where the
///            configuration fixes them, lang, width, height and a track_inhibit that drops the
///            output when it selects an original track of the stream.
///   dynamic: true when the process adds tracks its configuration does not determine; such a
///            process declares no outputs and is never warned about the tracks it adds.
namespace Mist {
  inline void addDescribeOutputsOption(Util::Config & config) {
    config.addOption("describe",
                     JSON::fromString(R"({"long":"describe-outputs","short":"O","value":[0],)"
                                      R"("help":"Print the input selection and output tracks of the )"
                                      R"(configuration as JSON, then exit."})"));
  }

  inline JSON::Value declaredOutput(const DTSC::TrackMetadata & trk, uint8_t mask) {
    JSON::Value out;
    out["output"] = trk.output;
    out["type"] = trk.type;
    out["codec"] = trk.codec;
    out["mask"] = mask;
    if (trk.lang.size()) { out["lang"] = trk.lang; }
    if (trk.width) { out["width"] = trk.width; }
    if (trk.height) { out["height"] = trk.height; }
    return out;
  }

  inline JSON::Value declaredSelection(const std::map<std::string, std::string> & targetParams, const JSON::Value & capa) {
    JSON::Value select;
    select["target"] = JSON::Value();
    for (const auto & param : targetParams) { select["target"][param.first] = param.second; }
    if (capa.isMember("codecs")) { select["codecs"] = capa["codecs"]; }
    return select;
  }

  /// The target parameters a source connecting with target "-?" + query starts out with.
  inline std::map<std::string, std::string> selectionQuery(const std::string & query) {
    std::map<std::string, std::string> params;
    HTTP::parseVars(query, params);
    return params;
  }

  /// Prints the declaration when the process was asked to describe itself (returns true; the
  /// process then exits), or records its outputs for the undeclared-output check (returns false).
  inline bool describeOrDeclare(Util::Config & config, const JSON::Value & declaration) {
    if (config.getBool("describe")) {
      std::cout << declaration.toString() << std::endl;
      return true;
    }
    if (declaration["dynamic"].asBool()) { return false; }
    DTSC::declaredOutputs = declaration["outputs"];
    if (!DTSC::declaredOutputs.isArray()) {
      DTSC::declaredOutputs.append(JSON::Value());
      DTSC::declaredOutputs.shrink(0);
    }
    return false;
  }
} // namespace Mist
