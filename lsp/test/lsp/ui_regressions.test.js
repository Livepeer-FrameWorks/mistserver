import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { ROOT, loadModule } from './helpers/module_loader.js';

test('hints footer imports its translation renderer', () => {
  const source = fs.readFileSync(path.join(ROOT, 'modules/components/hints_footer.js'), 'utf8');
  assert.match(source, /import\s+\{\s*tr\s*\}\s+from\s+['"]\.\.\/core\/i18n\.js['"]/);
});

test('hidden conditional field variants stop influencing the active process form', () => {
  const ctx = loadModule('modules/core/form_engine.js', {
    format: { capital(value) { return value; } }
  });
  const build = ctx.formEngine.convertBuildOptions({
    required: {
      codec: {
        type: 'select',
        select: ['H264', 'JPEG'],
        influences: ['quality']
      }
    }
  }, {});
  const codec = build.find(function(item) {
    return item && item.pointer && item.pointer.index === 'codec';
  });
  const style = {
    _content: '[data-dependent-codec] { display: none; }',
    innerHTML: '[data-dependent-codec] { display: none; }'
  };
  const owner = { querySelectorAll() { return [style]; } };

  codec.observe.call(owner, false);
  assert.equal(style.innerHTML, '');

  codec.observe.call(owner, true);
  assert.equal(style.innerHTML, style._content);
});
