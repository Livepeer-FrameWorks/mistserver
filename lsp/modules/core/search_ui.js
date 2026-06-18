import { registerTab } from './tab_registry.js';
import { searchSections, renderSection } from './section_registry.js';
import { navto } from './navigation.js';
import { AppShell } from './appshell.js';
import { apiClient } from './api_client.js';
import { el } from './dom_helpers.js';

var $searchInput = null;
var lastTab = '';
var lastResultState = 'idle'; // 'idle' | 'has-results' | 'no-results'

function setSearchState(state) {
  if (!$searchInput) return;
  lastResultState = state;
  $searchInput.setAttribute('data-search-state', state);
}

export function initSearchUI() {
  var toolbar = document.getElementById('header-toolbar');
  if (!toolbar) return;

  var control = el('div', {class: 'header-control header-control--search'});
  $searchInput = el('input', {
    type: 'search',
    class: 'global-search-input',
    placeholder: 'Search settings, protocols...',
    'aria-label': 'Search'
  });
  setSearchState('idle');

  var debounce = null;
  $searchInput.addEventListener('input', function() {
    clearTimeout(debounce);
    var val = this.value;
    debounce = setTimeout(function() {
      if (val.trim()) {
        navto('Search', encodeURIComponent(val));
      } else {
        setSearchState('idle');
      }
    }, 150);
  });

  $searchInput.addEventListener('keydown', function(e) {
    if (e.key === 'Escape') {
      e.preventDefault();
      $searchInput.value = '';
      $searchInput.blur();
      setSearchState('idle');
      if (lastTab && lastTab !== 'Search') {
        navto(lastTab);
      }
    }
  });

  document.addEventListener('keydown', function(e) {
    if ((e.ctrlKey || e.metaKey) && e.key === 'k') {
      e.preventDefault();
      $searchInput.focus();
      $searchInput.select();
      return;
    }

    if (e.ctrlKey || e.metaKey || e.altKey) return;
    if (document.activeElement === $searchInput) return;
    var tag = document.activeElement && document.activeElement.tagName;
    if (tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT') return;
    if (document.activeElement && document.activeElement.isContentEditable) return;

    if (e.key.length === 1 && /[a-zA-Z0-9]/.test(e.key)) {
      $searchInput.focus();
    }
  });

  control.appendChild($searchInput);
  toolbar.insertBefore(control, toolbar.firstChild);
}

registerTab('Search', function(tab, other, prev, $main, $pageHeader) {
  if (prev && prev[0] && prev[0] !== 'Search') {
    lastTab = prev[0];
  }

  var query = other ? decodeURIComponent(other) : '';
  if ($searchInput && $searchInput.value !== query) {
    $searchInput.value = query;
  }

  $main.classList.add('search-page-body');

  if (!mist.data.capabilities) {
    $main.textContent = 'Loading..';
    apiClient.send(function() {
      navto('Search', other);
    }, {capabilities: true});
    return;
  }

  if (!query.trim()) {
    setSearchState('idle');
    $main.appendChild(el('div', {class: 'search-empty-state'}, [
      el('span', {class: 'search-empty-icon', 'data-icon': 'search'}),
      el('h3', null, 'Search MistServer settings'),
      el('p', null, 'Press Ctrl+K or start typing to find settings, protocols, and more.')
    ]));
    return;
  }

  var mode = AppShell.getMode();
  var results = searchSections(query, { mode: mode });

  if (!results.length) {
    setSearchState('no-results');
    $main.appendChild(el('div', {class: 'search-empty-state'}, [
      el('span', {class: 'search-empty-icon', 'data-icon': 'search'}),
      el('h3', null, 'No results for \u201c' + query + '\u201d'),
      el('p', null, 'Try different keywords or check the spelling.')
    ]));
    return;
  }

  setSearchState('has-results');

  var list = el('div', {class: 'search-results'});
  for (var i = 0; i < results.length; i++) {
    (function(result, idx) {
      var section = result.section;
      var card = el('div', {class: 'search-result-card'});

      var header = el('div', {class: 'search-result-header'});
      header.appendChild(el('span', {class: 'search-result-badge'}, section.page));
      header.appendChild(el('span', {class: 'search-result-title'}, section.title));
      if (section.subtitle) {
        header.appendChild(el('span', {class: 'search-result-subtitle'}, section.subtitle));
      }
      card.appendChild(header);

      if (result.matchedKeywords && result.matchedKeywords.length) {
        var pills = el('div', {class: 'search-result-keywords'});
        for (var k = 0; k < result.matchedKeywords.length && k < 5; k++) {
          pills.appendChild(el('span', {class: 'search-keyword-pill'}, result.matchedKeywords[k]));
        }
        card.appendChild(pills);
      }

      var contentWrap = el('div', {class: 'search-result-content'});
      var expanded = (idx === 0);

      if (section.render) {
        if (expanded) {
          contentWrap.classList.add('expanded');
          renderSection(section.id, contentWrap);
        }

        header.style.cursor = 'pointer';
        header.addEventListener('click', function(e) {
          if (contentWrap.classList.contains('expanded')) {
            contentWrap.classList.remove('expanded');
            contentWrap.innerHTML = '';
          } else {
            contentWrap.classList.add('expanded');
            renderSection(section.id, contentWrap);
          }
        });
      } else {
        header.style.cursor = 'pointer';
        header.addEventListener('click', function() {
          if (section.navTo) {
            navto(section.navTo.tab, section.navTo.other || '');
          } else {
            navto(section.page);
          }
        });
      }

      card.appendChild(contentWrap);

      var footer = el('div', {class: 'search-result-footer'});
      var openLink = el('a', {
        class: 'search-result-open',
        href: '#',
        onclick: function(e) {
          e.preventDefault();
          if (section.navTo) {
            navto(section.navTo.tab, section.navTo.other || '');
          } else {
            navto(section.page);
          }
        }
      }, 'Open in ' + section.page + ' \u2192');
      footer.appendChild(openLink);
      card.appendChild(footer);

      list.appendChild(card);
    })(results[i], i);
  }
  $main.appendChild(list);
});

// Clear search when navigating away from Search tab
window.addEventListener('hashchange', function() {
  var loc = decodeURIComponent(location.hash).substring(1).split('@');
  var tab = (loc[1] || '').split('&')[0] || 'Overview';
  if (tab !== 'Search') {
    if ($searchInput && $searchInput.value) {
      $searchInput.value = '';
    }
    setSearchState('idle');
  }
});
