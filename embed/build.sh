#!/bin/bash

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT_DIR" || exit 1

ERRORS=0

echo "Building @optimist-video/mistplayer-core.."

mkdir -p dist/skins

# 1. ESM (unminified — for npm consumers, bundler tree-shaking)
echo "  Bundling ESM.."
npx esbuild src/index.js --bundle --format=esm --outfile=dist/index.js
ERRORS=$((ERRORS + $?))

# 2. ESM minified (for MistServer VFS /player.mjs, browser <script type="module">)
echo "  Bundling ESM (minified).."
npx esbuild src/index.js --bundle --format=esm --minify --outfile=dist/player.min.mjs
ERRORS=$((ERRORS + $?))

# 3. IIFE (minified — for MistServer binary embedding, CDN)
echo "  Bundling IIFE.."
npx esbuild src/iife.js --bundle --format=iife --minify --outfile=dist/player.iife.js
ERRORS=$((ERRORS + $?))

# 3. CSS
echo "  Minifying CSS.."
npx esbuild skins/general.css skins/default.css --bundle --loader=css --minify --outfile=dist/skins/default.css 2>/dev/null
if [ $? -ne 0 ]; then
  cat skins/general.css skins/default.css | npx esbuild --loader=css --minify > dist/skins/default.css
fi
ERRORS=$((ERRORS + $?))

if [ -f skins/dev.css ]; then
  npx esbuild skins/general.css skins/default.css skins/dev.css --bundle --loader=css --minify --outfile=dist/skins/dev.css 2>/dev/null
  if [ $? -ne 0 ]; then
    cat skins/general.css skins/default.css skins/dev.css | npx esbuild --loader=css --minify > dist/skins/dev.css
  fi
  ERRORS=$((ERRORS + $?))
fi

if [ -f skins/light.css ]; then
  npx esbuild skins/general.css skins/light.css --bundle --loader=css --minify --outfile=dist/skins/light.css 2>/dev/null
  if [ $? -ne 0 ]; then
    cat skins/general.css skins/light.css | npx esbuild --loader=css --minify > dist/skins/light.css
  fi
  ERRORS=$((ERRORS + $?))
fi

if [ $ERRORS -eq 0 ]; then
  echo "Build complete. Output in dist/."
else
  echo "Build failed with $ERRORS error(s)."
  exit $ERRORS
fi
