#!/bin/bash
# Refresh gettext templates and merge them into all existing language files.
# The POTs intentionally retain manually registered strings because several
# shared UI renderers receive their English msgids through descriptor objects.
set -euo pipefail

cd "$(dirname "$0")/.."

command -v xgettext >/dev/null || { echo "xgettext is required" >&2; exit 1; }
command -v msgmerge >/dev/null || { echo "msgmerge is required" >&2; exit 1; }

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

python3 scripts/lsp_literal_tools.py --extract >"$tmpdir/lsp-markers.js"

xgettext --force-po --language=JavaScript --from-code=UTF-8 \
  --keyword=msg --keyword=tr --keyword=trn:1,2 \
  --package-name=mistserver-lsp --copyright-holder=DDVTech \
  --output="$tmpdir/lsp.pot" \
  $(find lsp/modules -name '*.js' -type f | sort) "$tmpdir/lsp-markers.js"
msgcat --use-first --force-po --output=lsp/lang/mistserver-lsp.pot \
  lsp/lang/mistserver-lsp.pot "$tmpdir/lsp.pot"

python3 scripts/po_catalog.py extract-backend \
  src/controller/controller_capabilities.cpp \
  src/input/*.cpp src/input/*.h src/output/*.cpp src/output/*.h \
  src/process/*.cpp src/process/*.h src/process/*.hpp \
  >"$tmpdir/backend-markers.cpp"

xgettext --force-po --language=C++ --from-code=UTF-8 \
  --keyword=tr --package-name=mistserver-backend --copyright-holder=DDVTech \
  --output="$tmpdir/backend.pot" \
  "$tmpdir/backend-markers.cpp"
msgcat --use-first --force-po --output=src/lang/mistserver-backend.pot \
  src/lang/mistserver-backend.pot "$tmpdir/backend.pot"

for po in lsp/lang/*.po; do
  msgmerge --quiet --update --backup=off "$po" lsp/lang/mistserver-lsp.pot
done
for po in src/lang/*.po; do
  msgmerge --quiet --update --backup=off "$po" src/lang/mistserver-backend.pot
done

# Runtime catalogs are committed build inputs. Keeping compilation here makes
# Python/gettext development dependencies rather than product build dependencies.
for po in lsp/lang/*.po src/lang/*.po; do
  python3 scripts/po_catalog.py compile "$po" "${po%.po}.json"
done

scripts/check-translations.sh
