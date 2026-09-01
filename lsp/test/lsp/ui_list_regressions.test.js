import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { ROOT } from './helpers/module_loader.js';

test('expanded sidebar sublists preserve the navigation grid', () => {
  const css = fs.readFileSync(path.join(ROOT, 'css/core.css'), 'utf8');
  const activeRule = css.match(/nav \.hiddenmenu \.button\.active\s*\{([^}]*)\}/);
  const expandedRule = css.match(/nav \.hiddenmenu:has\(\.button\.active\) \.button\s*\{([^}]*)\}/);

  assert.ok(activeRule, 'expected active hidden-menu item rule');
  assert.ok(expandedRule, 'expected expanded hidden-menu item rule');
  assert.match(activeRule[1], /display:\s*grid\s*;/);
  assert.match(expandedRule[1], /display:\s*grid\s*;/);
});

test('streams table header does not shadow the translation renderer', () => {
  const source = fs.readFileSync(path.join(ROOT, 'modules/streams/streams.js'), 'utf8');

  assert.doesNotMatch(source, /const\s+tr\s*=\s*el\s*\(/);
  assert.match(source, /const\s+headerRow\s*=\s*el\s*\(["']tr["']/);
});
