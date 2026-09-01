#!/bin/bash
set -euo pipefail

cd "$(dirname "$0")/.."

command -v msgfmt >/dev/null || { echo "msgfmt is required" >&2; exit 1; }

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

for po in lsp/lang/*.po src/lang/*.po; do
  msgfmt --check --check-format --output-file=/dev/null "$po"
  if msgattrib --untranslated --no-obsolete "$po" | grep -q '^msgid '; then
    echo "$po contains untranslated messages" >&2
    exit 1
  fi
  if msgattrib --only-fuzzy --no-obsolete "$po" | grep -q '^msgid '; then
    echo "$po contains fuzzy messages" >&2
    exit 1
  fi
done

python3 scripts/po_catalog.py check lsp/lang/*.po src/lang/*.po
python3 scripts/lsp_literal_tools.py

# Every literal passed directly to msg(), tr(), or trn() must exist in the MI
# catalog. Descriptor-driven strings stay registered in the persistent POT.
xgettext --force-po --language=JavaScript --from-code=UTF-8 \
  --keyword=msg --keyword=tr --keyword=trn:1,2 \
  --output="$tmpdir/lsp-explicit.pot" \
  $(find lsp/modules -name '*.js' -type f | sort)
msgcmp --use-fuzzy lsp/lang/de-DE.po "$tmpdir/lsp-explicit.pot" 2>/dev/null

python3 scripts/lsp_literal_tools.py --extract >"$tmpdir/lsp-markers.js"
xgettext --force-po --language=JavaScript --from-code=UTF-8 \
  --keyword=msg --output="$tmpdir/lsp-rendered.pot" "$tmpdir/lsp-markers.js"
msgcmp --use-fuzzy lsp/lang/de-DE.po "$tmpdir/lsp-rendered.pot" 2>/dev/null

python3 -m json.tool lsp/lang/index.json >/dev/null
echo "Translation catalogs are valid."
