#!/usr/bin/env node

import fs from 'node:fs';
import path from 'node:path';

const root = path.resolve(import.meta.dirname, '..');

function read(relPath) {
  return fs.readFileSync(path.join(root, relPath), 'utf8');
}

function fail(msg) {
  console.error('[smoke] ' + msg);
  process.exitCode = 1;
}

function extractMainModules(text) {
  const out = [];
  const seen = {};
  const re = /import\s+['"]\.\/([^'"]+)['"]/g;
  let m;
  while ((m = re.exec(text))) {
    const p = 'modules/' + m[1];
    if (!seen[p]) { seen[p] = true; out.push(p); }
  }
  return out;
}

function checkDirectPopupUsage() {
  const files = extractMainModules(read('modules/main.js'));
  for (const rel of files) {
    if (rel === 'modules/core/ui_helpers.js') continue;
    if (read(rel).indexOf('UI.popup(') >= 0) {
      fail('direct UI.popup usage found outside ui_helpers: ' + rel);
    }
  }
}

checkDirectPopupUsage();

if (process.exitCode) process.exit(process.exitCode);
console.log('[smoke] OK');
