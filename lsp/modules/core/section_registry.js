import { apiClient } from './api_client.js';
import { el } from './dom_helpers.js';

const _sections = {};
const _dynamicProviders = [];
let _indexBuilt = false;
let _index = [];

export function defineSection(descriptor) {
  if (!descriptor.id || !descriptor.title) return;
  _sections[descriptor.id] = descriptor;
  _indexBuilt = false;
}

export function getSection(id) {
  return _sections[id] || null;
}

export function getAllSections() {
  const result = [];
  for (const id in _sections) {
    result.push(_sections[id]);
  }
  for (let i = 0; i < _dynamicProviders.length; i++) {
    const dynamic = _dynamicProviders[i]();
    for (let j = 0; j < dynamic.length; j++) {
      result.push(dynamic[j]);
    }
  }
  return result;
}

export function registerDynamicProvider(fn) {
  _dynamicProviders.push(fn);
}

export function extractKeywords() {
  const words = [];
  for (let a = 0; a < arguments.length; a++) {
    var arg = arguments[a];
    if (!arg) continue;
    if (Array.isArray(arg)) {
      for (let i = 0; i < arg.length; i++) {
        var e = arg[i];
        if (e && e.label) words.push(e.label);
        if (e && e.help) words.push(stripHTML(e.help));
      }
    } else if (typeof arg === 'object') {
      for (var key in arg) {
        var field = arg[key];
        if (field && field.name) words.push(field.name);
        if (field && field.help) words.push(stripHTML(field.help));
      }
    }
  }
  return words;
}

function stripHTML(str) {
  if (typeof str !== 'string') return '';
  return str.replace(/<[^>]*>/g, ' ').replace(/&[a-z]+;/g, ' ').replace(/\s+/g, ' ').trim();
}

function buildHaystack(entry) {
  var parts = [entry.title || ''];
  if (entry.subtitle) parts.push(entry.subtitle);
  if (entry.page) parts.push(entry.page);
  if (entry.keywords) {
    for (var i = 0; i < entry.keywords.length; i++) {
      parts.push(entry.keywords[i]);
    }
  }
  return parts.join(' ').toLowerCase();
}

export function searchSections(query, opts) {
  if (!query || !query.trim()) return [];
  var terms = query.toLowerCase().trim().split(/\s+/);
  var entries = getAllSections();
  var mode = opts && opts.mode;
  var results = [];

  for (var i = 0; i < entries.length; i++) {
    var entry = entries[i];

    if (mode && entry.mode && entry.mode !== mode) continue;

    var haystack = buildHaystack(entry);
    var titleLower = (entry.title || '').toLowerCase();
    var subtitleLower = (entry.subtitle || '').toLowerCase();
    var pageLower = (entry.page || '').toLowerCase();
    var score = 0;
    var allMatch = true;
    var matchedKeywords = [];

    for (var t = 0; t < terms.length; t++) {
      var term = terms[t];
      if (haystack.indexOf(term) < 0) {
        allMatch = false;
        break;
      }
      if (titleLower.indexOf(term) >= 0) {
        score += 10;
      } else if (subtitleLower.indexOf(term) >= 0) {
        score += 5;
      } else if (pageLower.indexOf(term) >= 0) {
        score += 3;
      } else {
        score += 2;
      }

      if (entry.keywords) {
        for (var k = 0; k < entry.keywords.length; k++) {
          if (entry.keywords[k].toLowerCase().indexOf(term) >= 0) {
            if (matchedKeywords.indexOf(entry.keywords[k]) < 0) {
              matchedKeywords.push(entry.keywords[k]);
            }
          }
        }
      }
    }

    if (allMatch) {
      results.push({ section: entry, score: score, matchedKeywords: matchedKeywords });
    }
  }

  results.sort(function(a, b) { return b.score - a.score; });
  return results;
}

export function renderSection(id, $target) {
  var all = getAllSections();
  var section = null;
  for (var i = 0; i < all.length; i++) {
    if (all[i].id === id) { section = all[i]; break; }
  }
  if (!section) return;
  if (!section.render) return;

  var requires = section.requires || [];
  var missing = [];
  for (var r = 0; r < requires.length; r++) {
    if (!(requires[r] in mist.data)) {
      missing.push(requires[r]);
    }
  }

  if (missing.length) {
    $target.textContent = 'Loading..';
    var request = {};
    for (var m = 0; m < missing.length; m++) {
      request[missing[m]] = true;
    }
    apiClient.send(function() {
      $target.textContent = '';
      section.render($target);
    }, request);
  } else {
    section.render($target);
  }
}
