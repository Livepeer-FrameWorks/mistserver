const STORAGE_KEY = 'mist-language';
const DEFAULT_LOCALE = 'en';

const state = {
  locale: DEFAULT_LOCALE,
  catalog: {},
  available: {en: 'English'},
  initialized: false
};

// Extraction-only marker for strings translated later at a rendering boundary.
export function msg(message) {
  return message;
}

function isEnglish(locale) {
  return !locale || locale.toLowerCase() === 'en' || locale.toLowerCase().startsWith('en-');
}

function interpolate(message, args) {
  let next = 0;
  return message.replace(/%(?:(\d+)\$)?([%s])/g, function(match, position, kind) {
    if (kind === '%') return '%';
    const value = position ? args[Number(position) - 1] : args[next++];
    return value === undefined || value === null ? '' : String(value);
  });
}

export function tr(message) {
  if (typeof message !== 'string') return message;
  let translated = state.catalog[message];
  if (Array.isArray(translated)) translated = translated[0];
  if (!translated) translated = message;
  if (arguments.length > 1) {
    translated = interpolate(translated, Array.prototype.slice.call(arguments, 1));
  }
  return translated;
}

export function trn(singular, plural, count) {
  const entry = state.catalog[singular];
  let translated;
  if (Array.isArray(entry)) translated = entry[count === 1 ? 0 : 1];
  else if (count === 1 && typeof entry === 'string') translated = entry;
  if (!translated) translated = count === 1 ? singular : plural;
  const args = Array.prototype.slice.call(arguments, 3);
  if (!args.length) args.push(count);
  return interpolate(translated, args);
}

export function getLocale() {
  return state.locale;
}

export function getAvailableLocales() {
  return Object.assign({}, state.available);
}

export function matchLocale(preferences, available) {
  const codes = Object.keys(available || state.available);
  for (const preference of preferences || []) {
    const wanted = String(preference || '').toLowerCase();
    if (!wanted) continue;
    const exact = codes.find(function(code) { return code.toLowerCase() === wanted; });
    if (exact) return exact;
    const base = wanted.split('-')[0];
    const baseMatch = codes.find(function(code) { return code.toLowerCase().split('-')[0] === base; });
    if (baseMatch) return baseMatch;
  }
  return DEFAULT_LOCALE;
}

async function fetchJson(path) {
  const response = await fetch(path, {cache: 'no-cache'});
  if (!response.ok) throw new Error('HTTP ' + response.status);
  return response.json();
}

async function loadCatalog(locale) {
  if (isEnglish(locale)) return {};
  const paths = ['/translations/' + encodeURIComponent(locale) + '.json'];
  if (location.protocol === 'file:') paths.push('lang/' + encodeURIComponent(locale) + '.json');
  for (const path of paths) {
    try {
      const catalog = await fetchJson(path);
      if (catalog && typeof catalog === 'object') return catalog;
    } catch (error) {}
  }
  return null;
}

export async function initI18n() {
  if (state.initialized) return state.locale;
  try {
    const index = await fetchJson('/translations/index.json');
    if (index && typeof index === 'object') state.available = Object.assign({en: 'English'}, index);
  } catch (error) {}

  let stored = null;
  try { stored = localStorage.getItem(STORAGE_KEY); } catch (error) {}
  const preferences = stored ? [stored] : Array.from(navigator.languages || [navigator.language || DEFAULT_LOCALE]);
  const wanted = matchLocale(preferences, state.available);
  const catalog = await loadCatalog(wanted);
  state.locale = catalog === null ? DEFAULT_LOCALE : wanted;
  state.catalog = catalog || {};
  state.initialized = true;
  document.documentElement.setAttribute('lang', state.locale);
  return state.locale;
}

export function setLanguage(locale) {
  const selected = Object.prototype.hasOwnProperty.call(state.available, locale) ? locale : DEFAULT_LOCALE;
  try { localStorage.setItem(STORAGE_KEY, selected); } catch (error) {}
  window.location.reload();
}

export function translateDocument(root) {
  const scope = root || document;
  scope.querySelectorAll('[data-i18n]').forEach(function(element) {
    element.textContent = tr(element.getAttribute('data-i18n'));
  });
  scope.querySelectorAll('[data-i18n-title]').forEach(function(element) {
    element.setAttribute('title', tr(element.getAttribute('data-i18n-title')));
  });
  scope.querySelectorAll('[data-i18n-aria-label]').forEach(function(element) {
    element.setAttribute('aria-label', tr(element.getAttribute('data-i18n-aria-label')));
  });
}

export const shellMessages = [
  msg('Mode'), msg('Theme'), msg('Language'), msg('Guided mode'), msg('Dark theme'),
  msg('Switch to advanced mode'), msg('Switch to light theme'), msg('Disconnect'),
  msg('Connection status details'), msg('Disconnected'), msg('Loading..'),
  msg('Please enable JavaScript.')
];
