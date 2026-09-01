#!/bin/sh
set -eu

tmp=$(mktemp -d "${TMPDIR:-/tmp}/mist-onnx-cache-test.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
cp "$1" "$tmp/prepare_models.sh"
printf 'verified model\n' > "$tmp/source.onnx"

if command -v sha256sum >/dev/null 2>&1; then
  digest=$(sha256sum "$tmp/source.onnx" | awk '{print $1}')
elif command -v shasum >/dev/null 2>&1; then
  digest=$(shasum -a 256 "$tmp/source.onnx" | awk '{print $1}')
else
  digest=$(openssl dgst -sha256 "$tmp/source.onnx" | awk '{print $NF}')
fi
bytes=$(wc -c < "$tmp/source.onnx" | tr -d ' ')
printf 'cache-test\tpack/model.onnx\tfile://%s\t%s\t%s\tMIT\thttps://example.invalid/source\n' \
  "$tmp/source.onnx" "$digest" "$bytes" > "$tmp/models.manifest.tsv"

sh "$tmp/prepare_models.sh" --dir "$tmp/cache" cache-test >/dev/null
cmp "$tmp/source.onnx" "$tmp/cache/pack/model.onnx"

printf 'corrupt\n' > "$tmp/cache/pack/model.onnx"
sh "$tmp/prepare_models.sh" --dir "$tmp/cache" cache-test >/dev/null
cmp "$tmp/source.onnx" "$tmp/cache/pack/model.onnx"
find "$tmp/cache/pack" -name 'model.onnx.corrupt.*' | grep . >/dev/null

rm "$tmp/cache/pack/model.onnx"
sh "$tmp/prepare_models.sh" --dir "$tmp/cache" cache-test >/dev/null &
first=$!
sh "$tmp/prepare_models.sh" --dir "$tmp/cache" cache-test >/dev/null &
second=$!
wait "$first"
wait "$second"
cmp "$tmp/source.onnx" "$tmp/cache/pack/model.onnx"
[ ! -d "$tmp/cache/pack/model.onnx.lock" ]
