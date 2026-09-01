import test from 'node:test';
import assert from 'node:assert/strict';
import { loadModule, createStorage } from './helpers/module_loader.js';

test('matchLocale prefers exact locales, then language families, then English', () => {
  const ctx = loadModule('modules/core/i18n.js');
  const available = { en: 'English', 'de-DE': 'Deutsch', 'es-ES': 'Español' };

  assert.equal(ctx.matchLocale(['de-DE'], available), 'de-DE');
  assert.equal(ctx.matchLocale(['de-AT'], available), 'de-DE');
  assert.equal(ctx.matchLocale(['es-MX'], available), 'es-ES');
  assert.equal(ctx.matchLocale(['fr-FR'], available), 'en');
});

test('initI18n loads the stored catalog and translates singulars and plurals', async () => {
  const requested = [];
  const localStorage = createStorage({ 'mist-language': 'de-DE' });
  const ctx = loadModule('modules/core/i18n.js', {
    localStorage,
    fetch(path) {
      requested.push(path);
      if (path === '/translations/index.json') {
        return Promise.resolve({ ok: true, json: async () => ({ 'de-DE': 'Deutsch' }) });
      }
      return Promise.resolve({
        ok: true,
        json: async () => ({ Save: 'Speichern', '%s item': ['%s Element', '%s Elemente'] })
      });
    }
  });

  assert.equal(await ctx.initI18n(), 'de-DE');
  assert.deepEqual(requested, ['/translations/index.json', '/translations/de-DE.json']);
  assert.equal(ctx.getLocale(), 'de-DE');
  assert.equal(ctx.tr('Save'), 'Speichern');
  assert.equal(ctx.tr('Unknown source string'), 'Unknown source string');
  assert.equal(ctx.trn('%s item', '%s items', 1), '1 Element');
  assert.equal(ctx.trn('%s item', '%s items', 2), '2 Elemente');
  assert.equal(ctx.document.documentElement.getAttribute('lang'), 'de-DE');
});

test('initI18n falls back to English when a selected catalog cannot load', async () => {
  const ctx = loadModule('modules/core/i18n.js', {
    navigator: { languages: ['de-AT'], language: 'de-AT' },
    fetch(path) {
      if (path === '/translations/index.json') {
        return Promise.resolve({ ok: true, json: async () => ({ 'de-DE': 'Deutsch' }) });
      }
      return Promise.resolve({ ok: false, status: 404 });
    }
  });

  assert.equal(await ctx.initI18n(), 'en');
  assert.equal(ctx.tr('Save'), 'Save');
});

test('setLanguage stores a supported locale and reloads the current URL', async () => {
  let reloads = 0;
  const localStorage = createStorage();
  const window = { location: { reload() { reloads++; } } };
  const ctx = loadModule('modules/core/i18n.js', {
    localStorage,
    window,
    fetch(path) {
      if (path === '/translations/index.json') {
        return Promise.resolve({ ok: true, json: async () => ({ 'de-DE': 'Deutsch' }) });
      }
      return Promise.resolve({ ok: true, json: async () => ({}) });
    }
  });
  await ctx.initI18n();

  ctx.setLanguage('de-DE');
  assert.equal(localStorage.getItem('mist-language'), 'de-DE');
  assert.equal(reloads, 1);
});
