/**
 * Enhanced accordion tree with nesting, actions, summaries, and lazy loading.
 * @param {Object} options
 * @param {Array} options.sections     Array of section descriptors
 * @param {boolean=} options.allowMultiple  Allow multiple sections open (default false)
 * @param {string=} options.groupId    Session storage key for remembering expanded section
 * @return {Object} Controller with $el, expand(id), collapse(id), refresh(id)
 *
 * Section descriptor: {
 *   id, title, subtitle, icon,
 *   actions: [{ icon, label, onClick, confirm }],
 *   content: DOM Element | function($inner),
 *   children: [section descriptors],
 *   expanded: boolean
 * }
 */
import { el } from '../core/dom_helpers.js';
import { defineSection } from '../core/section_registry.js';

export function accordionTree(options) {
  const sections = options.sections || [];
  const allowMultiple = options.allowMultiple || false;
  const groupId = options.groupId || null;
  const sectionMap = {};

  let savedExpandIndex = -1;
  if (groupId) {
    try {
      const saved = sessionStorage.getItem('accordion-' + groupId);
      if (saved !== null) savedExpandIndex = parseInt(saved, 10);
    } catch(e) {}
  }

  const $group = el('div', {class: 'accordion-group'});

  function buildSection(sectionOpts, isNested) {
    const $chevron = el('span', {class: 'accordion-chevron'});
    const $titleEl = el('span', {class: 'accordion-title'}, sectionOpts.title || '');
    const $header = el('div', {class: 'accordion-header'});
    $header.appendChild($chevron);

    if (sectionOpts.icon) {
      const $icon = el('span', {class: 'accordion-icon', 'data-icon': sectionOpts.icon});
      $header.appendChild($icon);
    }

    $header.appendChild($titleEl);

    if (sectionOpts.subtitle) {
      $header.appendChild(
        el('span', {class: 'accordion-subtitle'}, sectionOpts.subtitle)
      );
    }

    if (sectionOpts.summary) {
      $header.appendChild(
        el('span', {class: 'accordion-summary'}, sectionOpts.summary)
      );
    }

    if (sectionOpts.actions && sectionOpts.actions.length) {
      const $actions = el('div', {class: 'accordion-actions'});
      for (let a = 0; a < sectionOpts.actions.length; a++) {
        (function(action) {
          const $btn = el('button', {
            onclick: function(e) {
              e.stopPropagation();
              if (action.confirm) {
                if (!window.confirm(action.confirm)) return;
              }
              if (action.onClick) action.onClick(e);
            }
          });
          if (action.icon) $btn.setAttribute('data-icon', action.icon);
          if (action.label) $btn.textContent = action.label;
          if (action.title) $btn.setAttribute('title', action.title);
          $actions.appendChild($btn);
        })(sectionOpts.actions[a]);
      }
      $header.appendChild($actions);
    }

    const $inner = el('div', {class: 'accordion-inner'});
    const $body = el('div', {class: 'accordion-body'});
    $body.appendChild($inner);
    const $section = el('div', {class: 'accordion-section'});
    $section.appendChild($header);
    $section.appendChild($body);

    if (isNested) $section.classList.add('accordion-section--nested');

    let contentBuilt = false;
    function ensureContent() {
      if (contentBuilt) return;
      contentBuilt = true;

      if (typeof sectionOpts.content === 'function') {
        sectionOpts.content($inner);
      } else if (sectionOpts.content) {
        $inner.appendChild(sectionOpts.content);
      }

      if (sectionOpts.children && sectionOpts.children.length) {
        const $childGroup = el('div', {class: 'accordion-group'});
        for (let c = 0; c < sectionOpts.children.length; c++) {
          $childGroup.appendChild(buildSection(sectionOpts.children[c], true));
        }
        $inner.appendChild($childGroup);
      }
    }

    if (sectionOpts.expanded) {
      ensureContent();
      $section.classList.add('expanded');
    }

    function toggle() {
      if (!$section.classList.contains('expanded')) {
        if (!allowMultiple) {
          const $parentGroup = $section.closest('.accordion-group');
          if ($parentGroup) {
            const expandedSections = $parentGroup.querySelectorAll('.accordion-section.expanded');
            for (let i = 0; i < expandedSections.length; i++) {
              expandedSections[i].classList.remove('expanded');
            }
          }
        }
        ensureContent();
        $section.classList.add('expanded');
      } else {
        $section.classList.remove('expanded');
      }
      if (groupId && !isNested) {
        try {
          const idx = $section.classList.contains('expanded')
            ? Array.prototype.indexOf.call($group.children, $section)
            : -1;
          sessionStorage.setItem('accordion-' + groupId, idx);
        } catch(e) {}
      }
    }

    $header.setAttribute('tabindex', '0');
    $header.addEventListener('click', function(e) {
      if (e.target.closest('.accordion-actions')) return;
      toggle();
    });
    $header.addEventListener('keydown', function(e) {
      if (e.which === 13 || e.which === 32) {
        e.preventDefault();
        toggle();
      }
    });

    if (sectionOpts.id) {
      sectionMap[sectionOpts.id] = {
        $section: $section,
        $inner: $inner,
        $title: $titleEl,
        opts: sectionOpts,
        ensureContent: ensureContent,
        toggle: toggle
      };
    }

    return $section;
  }

  for (let i = 0; i < sections.length; i++) {
    const opts = sections[i];

    if (opts.render && !opts.content) {
      opts.content = opts.render;
    }

    if (opts.expanded === undefined && !allowMultiple) {
      if (savedExpandIndex >= 0) {
        opts.expanded = (i === savedExpandIndex);
      } else if (i === 0) {
        opts.expanded = true;
      }
    }
    $group.appendChild(buildSection(opts, false));
  }

  if (options.page) {
    for (let i = 0; i < sections.length; i++) {
      const s = sections[i];
      if (!s.title) continue;
      defineSection({
        id: (options.page + '/' + (s.id || s.title)).toLowerCase().replace(/[^a-z0-9]+/g, '-'),
        page: options.page,
        title: s.title,
        subtitle: s.subtitle || null,
        keywords: s.keywords || [],
        mode: options.mode || null,
        requires: s.requires || [],
        render: s.render || null,
        navTo: { tab: options.page, other: '' }
      });
    }
  }

  return {
    $el: $group,
    expand: function(id) {
      const entry = sectionMap[id];
      if (!entry) return;
      entry.ensureContent();
      if (!entry.$section.classList.contains('expanded')) entry.toggle();
    },
    collapse: function(id) {
      const entry = sectionMap[id];
      if (!entry) return;
      if (entry.$section.classList.contains('expanded')) entry.toggle();
    },
    refresh: function(id) {
      const entry = sectionMap[id];
      if (!entry) return;
      entry.$inner.innerHTML = '';
      if (typeof entry.opts.content === 'function') {
        entry.opts.content(entry.$inner);
      } else if (entry.opts.content) {
        entry.$inner.appendChild(entry.opts.content);
      }
    },
    updateTitle: function(id, title) {
      const entry = sectionMap[id];
      if (entry) entry.$title.textContent = title;
    },
    updateSubtitle: function(id, subtitle) {
      const entry = sectionMap[id];
      if (!entry) return;
      const subtitleEl = entry.$section.querySelector('.accordion-subtitle');
      if (subtitleEl) subtitleEl.textContent = subtitle;
    }
  };
}
