#!/bin/sh
set -eu

manifest="$1"
dependencies="${2:-}"

awk -F '\t' '
  BEGIN { ok = 1 }
  /^#/ || NF == 0 { next }
  {
    if (NF != 7) { print "manifest line " NR ": expected 7 fields" > "/dev/stderr"; ok = 0; next }
    if ($1 !~ /^[a-z0-9][a-z0-9._-]*$/) { print "manifest line " NR ": invalid id" > "/dev/stderr"; ok = 0 }
    if ($2 ~ /^\// || $2 ~ /(^|\/)\.\.($|\/)/) { print "manifest line " NR ": unsafe path" > "/dev/stderr"; ok = 0 }
    if ($3 !~ /^https:\/\// || $3 !~ /\/resolve\/[0-9a-f]{40}\//) { print "manifest line " NR ": URL is not commit-pinned" > "/dev/stderr"; ok = 0 }
    if ($4 !~ /^[0-9a-f]{64}$/) { print "manifest line " NR ": invalid SHA-256" > "/dev/stderr"; ok = 0 }
    if ($5 !~ /^[1-9][0-9]*$/) { print "manifest line " NR ": invalid byte size" > "/dev/stderr"; ok = 0 }
    if ($6 == "" || $7 !~ /^https:\/\//) { print "manifest line " NR ": missing license/provenance" > "/dev/stderr"; ok = 0 }
    key = $1 SUBSEP $2
    if (seen[key]++) { print "manifest line " NR ": duplicate id/path" > "/dev/stderr"; ok = 0 }
    models[$1]++
  }
  END {
    required["yolo26n"] = 1
    required["parakeet-tdt-0.6b-int8"] = 4
    required["parakeet-tdt-0.6b-fp16"] = 4
    required["parakeet-tdt-0.6b-fp32"] = 5
    for (id in required) {
      if (models[id] != required[id]) {
        print "manifest: " id " has " models[id] " files, expected " required[id] > "/dev/stderr"
        ok = 0
      }
    }
    exit !ok
  }
' "$manifest"

if [ -n "$dependencies" ]; then
  awk -F '\t' '
    BEGIN { ok = 1 }
    /^#/ || NF == 0 { next }
    {
      rows++
      if (NF != 5) { print "dependency lock line " NR ": expected 5 fields" > "/dev/stderr"; ok = 0; next }
      if ($1 !~ /^[a-z0-9][a-z0-9._-]*$/ || seen[$1]++) { print "dependency lock line " NR ": invalid/duplicate name" > "/dev/stderr"; ok = 0 }
      if ($2 !~ /^[0-9]+\.[0-9]+\.[0-9]+$/) { print "dependency lock line " NR ": invalid version" > "/dev/stderr"; ok = 0 }
      if ($1 == "onnxruntime" || $1 == "opencv") {
        if ($3 !~ /^[0-9a-f]{40}$/) { print "dependency lock line " NR ": invalid source commit" > "/dev/stderr"; ok = 0 }
      } else {
        if ($3 !~ /^[0-9a-f]{64}$/) { print "dependency lock line " NR ": invalid archive SHA-256" > "/dev/stderr"; ok = 0 }
        if ($5 !~ /^https:\/\/github\.com\/microsoft\/onnxruntime\/releases\/download\/v[0-9]+\.[0-9]+\.[0-9]+\//) {
          print "dependency lock line " NR ": runtime archive is not an official versioned release" > "/dev/stderr"
          ok = 0
        }
      }
      if ($4 == "" || $5 !~ /^https:\/\/github\.com\//) { print "dependency lock line " NR ": missing license/source" > "/dev/stderr"; ok = 0 }
    }
    END {
      required["onnxruntime"] = 1
      required["opencv"] = 1
      required["onnxruntime-linux-x64-gpu-cuda12"] = 1
      required["onnxruntime-linux-x64-gpu-cuda13"] = 1
      required["onnxruntime-linux-x64"] = 1
      required["onnxruntime-linux-aarch64"] = 1
      required["onnxruntime-osx-arm64"] = 1
      for (dependency in required) {
        if (!seen[dependency]) {
          print "dependency lock missing required entry: " dependency > "/dev/stderr"
          ok = 0
        }
      }
      if (rows != 7) {
        print "dependency lock contains unexpected entries" > "/dev/stderr"
        ok = 0
      }
      exit !ok
    }
  ' "$dependencies"
fi
