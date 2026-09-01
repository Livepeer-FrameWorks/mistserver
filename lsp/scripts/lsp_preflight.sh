#!/bin/bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

echo "[preflight] css-lint"
if grep -q 'main\[data-tab=' main.css; then
  echo "FAIL: main[data-tab=] selectors found in main.css"
  grep -n 'main\[data-tab=' main.css | head -5
  exit 1
fi

echo "[preflight] smoke"
node scripts/lsp_smoke_check.js

echo "[preflight] unit"
node --test test/lsp/*.test.js

echo "[preflight] done"
