#!/bin/sh
# Prints the path of a clang-format 21 binary, the major version the CI lint job installs
# (.github/workflows/lint.yml); other versions format some constructs differently. Uses
# clang-format-21 or a clang-format 21 on PATH, else the clang-format==21.1.8 wheel through pipx.
# Fails with a hint when none is available.
set -eu

for candidate in clang-format-21 clang-format; do
  path=$(command -v "$candidate" 2>/dev/null || true)
  if [ -n "$path" ] && "$path" --version 2>/dev/null | grep -q 'version 21\.'; then
    printf '%s\n' "$path"
    exit 0
  fi
done

if command -v pipx >/dev/null 2>&1; then
  path=$(pipx run --spec clang-format==21.1.8 python -c \
    'import clang_format, os; print(os.path.join(os.path.dirname(clang_format.__file__), "data", "bin", "clang-format"))' \
    2>/dev/null || true)
  if [ -n "$path" ] && [ -x "$path" ]; then
    printf '%s\n' "$path"
    exit 0
  fi
fi

echo "clang-format 21 is required (CI uses clang-format-21): install clang-format-21, or pipx so" >&2
echo "clang-format==21.1.8 can be fetched" >&2
exit 1
