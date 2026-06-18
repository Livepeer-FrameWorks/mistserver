#!/bin/bash

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT_DIR" || exit 1

if [ "${LSP_SKIP_PREFLIGHT:-0}" != "1" ]; then
  echo "Running LSP preflight.."
  ./scripts/lsp_preflight.sh || exit 1
fi

# --- MI variant ---
echo "Assembling MI CSS.."
mkdir -p dist/mi
cat css/core.css css/auth.css css/overview.css css/streams.css \
    css/push.css css/cameras.css css/admin.css css/search.css css/connections.css > dist/mi/main.css

echo "Bundling/minifying MI modules.."
npx esbuild modules/main.js --bundle --format=esm --minify \
  --alias:@brand=./modules/brands/mi.js \
  --alias:@player=../embed/src/index.js \
  --outfile=dist/mi/main.js
if [ $? -ne 0 ]; then
  echo "esbuild (MI) failed!"
  exit 1
fi

echo "Assembling MI HTML.."
cp templates/mi.html dist/mi/index.html
echo "Done (MI)."

# --- EFG variant ---
if [ -f "modules/main_efg.js" ]; then
  echo "Assembling EFG CSS.."
  mkdir -p dist/efg
  cp css/efg.css dist/efg/main.css

  echo "Bundling/minifying EFG modules.."
  npx esbuild modules/main_efg.js --bundle --format=esm --minify \
    --alias:@brand=./modules/brands/efg.js \
    --outfile=dist/efg/main.js
  if [ $? -ne 0 ]; then
    echo "esbuild (EFG) failed!"
    exit 1
  fi

  echo "Assembling EFG HTML.."
  cp templates/efg.html dist/efg/index.html
  echo "Done (EFG)."
fi

# --- Playground variant ---
if [ -f "modules/main_playground.js" ]; then
  echo "Assembling Playground CSS.."
  mkdir -p dist/playground
  cp css/playground.css dist/playground/main.css

  echo "Bundling/minifying Playground modules.."
  npx esbuild modules/main_playground.js --bundle --format=esm --minify \
    --alias:@brand=./modules/brands/playground.js \
    --alias:@player=../embed/src/index.js \
    --outfile=dist/playground/main.js
  if [ $? -ne 0 ]; then
    echo "esbuild (Playground) failed!"
    exit 1
  fi

  echo "Assembling Playground HTML.."
  cp templates/playground.html dist/playground/index.html
  echo "Done (Playground)."
fi

echo "Build complete. Output in dist/."
exit 0
