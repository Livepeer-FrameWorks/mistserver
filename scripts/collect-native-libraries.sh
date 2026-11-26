#!/bin/sh
# Collect the ELF dependency closure for a relocatable native bundle.
set -eu
if [ "$#" -ne 1 ] || [ ! -d "$1/bin" ]; then
  echo "usage: $0 INSTALL_PREFIX" >&2
  exit 2
fi
prefix=$1
mkdir -p "$prefix/lib"
paths=$(mktemp)
trap 'rm -f "$paths"' EXIT HUP INT TERM
for file in "$prefix"/bin/Mist* "$prefix"/lib/*.so* "$prefix"/lib/*/*.so*; do
  [ -f "$file" ] || continue
  ldd "$file" 2>/dev/null || true
done > "$paths"
if grep -q 'not found' "$paths"; then
  cat "$paths" >&2
  echo "native bundle has unresolved linked libraries" >&2
  exit 1
fi
# NSS loads these crypto providers at runtime; they do not appear in ldd's closure.
# Their integrity files describe the exact provider bytes, so copy both from the
# same installation as the linked NSS library and keep the providers unmodified.
nss=$(awk '$1 == "libnss3.so" && $2 == "=>" && $3 ~ /^\// {print $3; exit}' "$paths")
if [ -n "$nss" ]; then
  nss_dir=${nss%/*}
  for module in libsoftokn3 libfreeblpriv3 libfreebl3; do
    for suffix in so chk; do
      provider="$nss_dir/$module.$suffix"
      if [ ! -f "$provider" ]; then
        echo "NSS runtime provider is unavailable: $provider" >&2
        exit 1
      fi
      [ "$provider" = "$prefix/lib/$module.$suffix" ] || cp -L "$provider" "$prefix/lib/$module.$suffix"
    done
    ldd "$prefix/lib/$module.so" >> "$paths" 2>/dev/null
  done
  if grep -q 'not found' "$paths"; then
    cat "$paths" >&2
    echo "native bundle has unresolved NSS provider libraries" >&2
    exit 1
  fi
fi
awk '$2 == "=>" && $3 ~ /^\// {print $3}' "$paths" | sort -u | while IFS= read -r library; do
  name=${library##*/}
  case "$name" in
    libc.so.*|libm.so.*|libdl.so.*|libpthread.so.*|librt.so.*|libresolv.so.*|libutil.so.*|ld-linux*) continue ;;
  esac
  [ -e "$prefix/lib/$name" ] || cp -L "$library" "$prefix/lib/$name"
done
