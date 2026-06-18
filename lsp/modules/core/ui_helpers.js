import { formEngine } from './form_engine.js';
import { uiCore } from './ui_core.js';
import { el } from './dom_helpers.js';

function appendContent(target, content) {
  if (content === null || typeof content === 'undefined' || content === false) return;
  if (Array.isArray(content)) {
    for (let i = 0; i < content.length; i++) {
      appendContent(target, content[i]);
    }
    return;
  }
  if (content instanceof Node) {
    target.appendChild(content);
  } else if (typeof content === 'string' || typeof content === 'number') {
    target.appendChild(document.createTextNode(String(content)));
  }
}

function setContent(target, content) {
  target.innerHTML = '';
  appendContent(target, content);
}

function ensureArray(input) {
  if (Array.isArray(input)) return input;
  if (typeof input === 'undefined' || input === null || input === false) return [];
  return [input];
}

function hasContent(target) {
  if (target.children.length > 0) return true;
  return target.textContent.trim().length > 0;
}

function addClasses(target, classes) {
  if (!classes) return;
  if (Array.isArray(classes)) {
    for (let i = 0; i < classes.length; i++) {
      addClasses(target, classes[i]);
    }
    return;
  }
  if (typeof classes === 'string') {
    const tokens = classes.trim().split(/\s+/);
    for (let i = 0; i < tokens.length; i++) {
      if (tokens[i]) {
        target.classList.add(tokens[i]);
      }
    }
    return;
  }
  const token = String(classes).trim();
  if (token) {
    target.classList.add(token);
  }
}

function mergeClasses(a, b) {
  const out = [];
  function push(input) {
    if (!input) return;
    if (Array.isArray(input)) {
      for (let i = 0; i < input.length; i++) {
        push(input[i]);
      }
      return;
    }
    out.push(input);
  }
  push(a);
  push(b);
  return out;
}

function toggleVisible(elem, show) {
  elem.hidden = !show;
}

export const uiHelpers = {
  statCard: function(icon, value, label) {
    return el('div', {class: 'overview-stat-card'}, [
      el('span', {'data-icon': icon}),
      el('div', {class: 'stat-content'}, [
        value,
        el('div', {class: 'stat-label'}, label)
      ])
    ]);
  },

  infoRow: function(label, value) {
    const row = el('div', {class: 'overview-info-row'}, [
      el('span', {class: 'info-label'}, label),
      el('span', {class: 'info-value'})
    ]);
    appendContent(row.querySelector('.info-value'), value);
    return row;
  },

  unavailableText: function(reason) {
    return el('span', {class: 'text-muted'}, reason || 'Not available');
  },

  randomKey: function(length) {
    const chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789';
    let str = '';
    while (str.length < length) {
      str += chars[Math.floor(Math.random() * chars.length)];
    }
    return str;
  },

  reviewSection: function(title, stepId, callbacks) {
    return el('div', {class: 'review-section-header'}, [
      el('h4', {class: 'review-section-title'}, title),
      el('button', {class: 'review-section-edit', onclick: function() {
        callbacks.goTo(stepId);
      }}, 'Edit')
    ]);
  },

  setPageTitle: function(pageHeader, title, subtitle) {
    if (!pageHeader) return;

    let titleGroup = pageHeader.querySelector(':scope > .page-title-group');
    if (!titleGroup) {
      const existingH2 = pageHeader.querySelector(':scope > h2');
      if (existingH2) {
        titleGroup = el('div', {class: 'page-title-group'});
        existingH2.parentElement.replaceChild(titleGroup, existingH2);
        titleGroup.appendChild(existingH2);
      } else {
        titleGroup = el('div', {class: 'page-title-group'});
        pageHeader.insertBefore(titleGroup, pageHeader.firstChild);
      }
    }

    let titleEl = titleGroup.querySelector(':scope > h2');
    if (!titleEl) {
      titleEl = el('h2');
      titleGroup.insertBefore(titleEl, titleGroup.firstChild);
    }
    if (typeof title !== 'undefined') {
      titleEl.textContent = title || '';
    }

    let subtitleEl = titleGroup.querySelector(':scope > .page-subtitle');
    if (subtitle) {
      if (!subtitleEl) {
        subtitleEl = el('p', {class: 'page-subtitle'});
        titleGroup.appendChild(subtitleEl);
      }
      subtitleEl.textContent = subtitle;
    } else if (subtitleEl) {
      subtitleEl.parentElement.removeChild(subtitleEl);
    }
  },

  appendPageActions: function(pageHeader, actions, classes) {
    if (!pageHeader) return null;

    let actionsEl = pageHeader.querySelector(':scope > .page-header-actions');
    if (!actionsEl) {
      actionsEl = el('div', {class: 'page-header-actions'});
      pageHeader.appendChild(actionsEl);
    }
    addClasses(actionsEl, classes);
    appendContent(actionsEl, ensureArray(actions));
    return actionsEl;
  },

  pageIntro: function(help, classes) {
    return formEngine.buildUI([{
      type: 'help',
      classes: mergeClasses('page-intro', classes),
      help: help
    }]);
  },

  pageToolbar: function(content, classes) {
    const toolbar = el('div', {class: 'page-toolbar'});
    addClasses(toolbar, classes);
    appendContent(toolbar, ensureArray(content));
    return toolbar;
  },

  createSubtabHeader: function(options) {
    options = options || {};
    const root = el('div', {class: 'subtab-header'});
    addClasses(root, options.classes);

    const top = el('div', {class: 'subtab-header-top'});
    const titleGroup = el('div', {class: 'subtab-title-group'});
    const titleTag = options.titleTag || 'h2';
    const titleEl = el(titleTag, null, options.title || '');
    titleGroup.appendChild(titleEl);
    if (options.subtitle) {
      titleGroup.appendChild(el('p', {class: 'subtab-subtitle'}, options.subtitle));
    }
    top.appendChild(titleGroup);

    if (options.status) {
      const statusWrap = el('div', {class: 'subtab-header-status'});
      appendContent(statusWrap, options.status);
      top.appendChild(statusWrap);
    }
    root.appendChild(top);

    const tabs = options.tabs;
    let nav = null;
    if (Array.isArray(tabs) && tabs.length) {
      nav = el('ul', {class: 'tabnav subtab-nav'});
      if (options.currentIcon) {
        nav.appendChild(el('span', {class: 'curtab-icon', 'data-icon': options.currentIcon}));
      }
      for (let i = 0; i < tabs.length; i++) {
        const tabInfo = tabs[i];
        if (!tabInfo) {
          nav.appendChild(el('span', {class: 'spacer'}));
          continue;
        }
        const attrs = {
          class: 'tab' + (tabInfo.active ? ' active' : '') + (tabInfo.disabled ? ' disabled' : '')
        };
        if (tabInfo.icon || tabInfo.tab) {
          attrs['data-icon'] = tabInfo.icon || tabInfo.tab;
        }
        if (tabInfo.disabled) {
          attrs['aria-disabled'] = 'true';
          if (tabInfo.disabledTitle) {
            attrs.title = tabInfo.disabledTitle;
          }
        } else {
          attrs.tabindex = '0';
        }
        const tabEl = el('li', attrs, tabInfo.label || tabInfo.tab || '');
        if (!tabInfo.disabled && (typeof tabInfo.onClick === 'function')) {
          tabEl.addEventListener('click', tabInfo.onClick);
        }
        nav.appendChild(tabEl);
      }
      root.appendChild(nav);
    }

    root.subtabTop = top;
    root.subtabTitleGroup = titleGroup;
    root.subtabTitle = titleEl;
    root.subtabNav = nav;
    return root;
  },

  createSmartSelect: function(options) {
    options = options || {};
    const allowEmpty = ('allowEmpty' in options) ? !!options.allowEmpty : true;
    const maxQuickPicks = Number(options.maxQuickPicks || 5);
    const state = {
      options: [],
      quickPicks: [],
      value: ''
    };

    const root = el('div', {class: 'smart-select'});
    const labelText = options.label || 'Select';
    const label = el('label', {class: 'smart-select-label'}, labelText + ':');
    const controls = el('div', {class: 'smart-select-controls'});
    const search = el('input', {
      class: 'smart-select-search',
      type: 'search',
      placeholder: options.searchPlaceholder || 'Type to filter...'
    });
    const select = el('select', {class: 'smart-select-dropdown'});
    const quickLabel = el('div', {class: 'smart-select-quick-label'}, options.quickLabel || 'Top picks');
    const quickPicks = el('div', {class: 'smart-select-quick-picks'});

    controls.appendChild(search);
    controls.appendChild(select);
    root.appendChild(label);
    root.appendChild(controls);
    root.appendChild(quickLabel);
    root.appendChild(quickPicks);

    function normalizeOption(input) {
      if (input && typeof input === 'object' && !Array.isArray(input)) {
        const value = ('value' in input) ? String(input.value) : String(input.label || '');
        return {
          value: value,
          label: ('label' in input) ? String(input.label) : value,
          search: ('search' in input) ? String(input.search).toLowerCase() : ''
        };
      }
      const out = String(input);
      return {value: out, label: out, search: ''};
    }

    function getMatchText(opt) {
      return (opt.search || (opt.label + ' ' + opt.value)).toLowerCase();
    }

    function refreshQuickPickState() {
      const buttons = quickPicks.querySelectorAll('button[data-value]');
      for (let i = 0; i < buttons.length; i++) {
        buttons[i].setAttribute('data-active', buttons[i].getAttribute('data-value') === state.value ? 'true' : 'false');
      }
    }

    function renderSelect() {
      const filter = search.value.trim().toLowerCase();
      const selectedValue = state.value;
      select.innerHTML = '';

      if (allowEmpty) {
        const defaultOpt = el('option', null, options.emptyLabel || 'Select...');
        defaultOpt.value = '';
        select.appendChild(defaultOpt);
      }

      let matchCount = 0;
      for (let i = 0; i < state.options.length; i++) {
        const opt = state.options[i];
        const keep = !filter || (getMatchText(opt).indexOf(filter) >= 0) || (opt.value === selectedValue);
        if (!keep) { continue; }

        const item = el('option', null, opt.label);
        item.value = opt.value;
        select.appendChild(item);
        matchCount++;
      }

      if (!matchCount && allowEmpty) {
        const noMatch = el('option', null, options.noMatchLabel || 'No matches');
        noMatch.value = '';
        noMatch.disabled = true;
        select.appendChild(noMatch);
      }

      select.value = selectedValue;
      if (select.value !== selectedValue) {
        if (allowEmpty) {
          state.value = '';
          select.value = '';
        } else if (select.options.length) {
          state.value = select.options[0].value;
          select.value = state.value;
        }
      }
      refreshQuickPickState();
    }

    function renderQuickPicks() {
      quickPicks.innerHTML = '';
      const entries = state.quickPicks.slice(0, maxQuickPicks);
      for (let i = 0; i < entries.length; i++) {
        const pick = entries[i];
        const btn = el('button', {
          type: 'button',
          class: 'cam-pill smart-select-pill',
          title: pick.title || pick.label,
          'data-value': pick.value
        }, pick.label);
        btn.addEventListener('click', function() {
          api.setValue(this.getAttribute('data-value'));
        });
        quickPicks.appendChild(btn);
      }
      quickLabel.hidden = entries.length === 0;
      quickPicks.hidden = entries.length === 0;
      refreshQuickPickState();
    }

    function emitChange() {
      if (options.onChange) {
        options.onChange(state.value, api.getOption(state.value), api);
      }
    }

    search.addEventListener('input', function() {
      renderSelect();
    });

    select.addEventListener('change', function() {
      state.value = this.value;
      refreshQuickPickState();
      emitChange();
    });

    const api = {
      element: root,
      label: label,
      search: search,
      select: select,
      setOptions: function(list) {
        state.options = [];
        const seen = {};
        const inList = Array.isArray(list) ? list : [];
        for (let i = 0; i < inList.length; i++) {
          const normalized = normalizeOption(inList[i]);
          if (seen[normalized.value]) { continue; }
          seen[normalized.value] = true;
          state.options.push(normalized);
        }
        renderSelect();
        return api;
      },
      getOption: function(value) {
        const v = (value === null || typeof value === 'undefined') ? '' : String(value);
        for (let i = 0; i < state.options.length; i++) {
          if (state.options[i].value === v) { return state.options[i]; }
        }
        return null;
      },
      setQuickPicks: function(list) {
        state.quickPicks = [];
        const inList = Array.isArray(list) ? list : [];
        for (let i = 0; i < inList.length; i++) {
          const item = inList[i];
          if (item && typeof item === 'object' && !Array.isArray(item)) {
            state.quickPicks.push({
              value: String(item.value),
              label: ('label' in item) ? String(item.label) : String(item.value),
              title: ('title' in item) ? String(item.title) : ''
            });
          } else {
            const v = String(item);
            state.quickPicks.push({value: v, label: v, title: ''});
          }
        }
        renderQuickPicks();
        return api;
      },
      setValue: function(value, opts) {
        opts = opts || {};
        state.value = (value === null || typeof value === 'undefined') ? '' : String(value);
        renderSelect();
        if (!opts.silent) { emitChange(); }
        return api;
      },
      getValue: function() {
        return state.value;
      },
      setSearchValue: function(value) {
        search.value = (value === null || typeof value === 'undefined') ? '' : String(value);
        renderSelect();
        return api;
      }
    };

    quickLabel.hidden = true;
    quickPicks.hidden = true;
    renderSelect();
    return api;
  },

  pageActionRow: function(content, classes) {
    const row = el('div', {class: 'page-action-row button_container'});
    addClasses(row, classes);
    appendContent(row, ensureArray(content));
    return row;
  },

  openCopyFallback: function(options) {
    options = options || {};
    const text = options.text || '';
    const title = options.title || 'Copy to clipboard';
    const label = options.label || 'Text';
    const rows = options.rows || Math.ceil(text.length / 50 + 2);
    const err = (typeof options.error === 'undefined' || options.error === null) ? '' : String(options.error);
    const help = options.help || (
      'Automatic copying failed' + (err ? ' (' + err + ')' : '') +
      '. Instead you can manually copy from the field below.'
    );

    const modal = uiHelpers.openModal({
      size: options.size || 'md',
      title: title
    });

    modal.setBody(formEngine.buildUI([{
      type: 'help',
      help: help
    }, {
      type: 'textarea',
      label: label,
      value: text,
      rows: rows
    }]));

    const field = modal.element.querySelector('textarea');
    if (field) {
      field.select();
    }

    return modal;
  },

  openConfirm: function(options) {
    options = options || {};
    let body = ('body' in options) ? options.body : null;
    if (body === null && ('message' in options)) {
      body = el('p', null, options.message);
    }

    const modal = uiHelpers.openModal({
      size: options.size || 'sm',
      title: options.title,
      subtitle: options.subtitle,
      body: body,
      headerActions: options.headerActions,
      popupClasses: options.popupClasses,
      shellClasses: options.shellClasses,
      bodyClasses: options.bodyClasses,
      footerClasses: options.footerClasses
    });

    let actions = options.actions || [];
    if (!actions.length) {
      actions = [{
        type: 'cancel',
        label: options.cancelLabel || 'Cancel',
        close: true
      }, {
        type: 'save',
        label: options.confirmLabel || 'Confirm',
        close: true,
        onClick: options.onConfirm
      }];
    }

    const buttons = [];
    for (let i = 0; i < actions.length; i++) {
      const action = actions[i];
      if (!action) continue;

      const button = {};
      for (const key in action) {
        if (key === 'onClick' || key === 'function' || key === 'close') continue;
        button[key] = action[key];
      }

      const handler = action.onClick || action['function'];
      const shouldClose = ('close' in action) ? !!action.close : true;
      button['function'] = (function(fn, closeAfter, source) {
        return function() {
          if (fn) {
            fn.call(this, this, modal, source);
          }
          if (closeAfter) {
            modal.close();
          }
        };
      })(handler, shouldClose, action);

      buttons.push(button);
    }

    if (buttons.length) {
      modal.setFooter(formEngine.buildUI([{
        type: 'buttons',
        css: options.buttonsCss || { 'text-align': 'center' },
        buttons: buttons
      }]));
    }

    return modal;
  },

  openFormModal: function(options) {
    options = options || {};
    const buttonPlacement = options.buttonPlacement || 'body';
    const content = ('form' in options) ? options.form : options.body;
    const buttons = options.buttons || null;

    const modal = uiHelpers.openModal({
      size: options.size,
      title: options.title,
      subtitle: options.subtitle,
      headerActions: options.headerActions,
      popupClasses: options.popupClasses,
      shellClasses: options.shellClasses,
      bodyClasses: options.bodyClasses,
      footerClasses: options.footerClasses,
      loadingText: options.loadingText
    });

    function wrapButtons(defs) {
      const out = [];
      for (let i = 0; i < defs.length; i++) {
        const def = defs[i];
        if (!def) continue;

        const wrapped = {};
        for (const key in def) {
          if (key === 'onClick' || key === 'function' || key === 'close') continue;
          wrapped[key] = def[key];
        }

        const handler = def.onClick || def['function'];
        const shouldClose = ('close' in def) ? !!def.close : (def.type === 'cancel');
        wrapped['function'] = (function(fn, closeAfter, source) {
          return function() {
            if (fn) {
              fn.call(this, this, modal, source);
            }
            if (closeAfter) {
              modal.close();
            }
          };
        })(handler, shouldClose, def);

        out.push(wrapped);
      }
      return out;
    }

    if (buttons && buttons.length) {
      const wrappedButtons = wrapButtons(buttons);
      if ((buttonPlacement === 'body') && Array.isArray(content)) {
        modal.setBody(formEngine.buildUI(content.concat([{
          type: 'buttons',
          buttons: wrappedButtons
        }])));
      } else {
        if (Array.isArray(content)) {
          modal.setBody(formEngine.buildUI(content));
        } else if (typeof content !== 'undefined' && content !== null) {
          modal.setBody(content);
        }
        modal.setFooter(formEngine.buildUI([{
          type: 'buttons',
          css: options.buttonsCss,
          buttons: wrappedButtons
        }]));
      }
    } else if (Array.isArray(content)) {
      modal.setBody(formEngine.buildUI(content));
    } else if (typeof content !== 'undefined' && content !== null) {
      modal.setBody(content);
    }

    return modal;
  },

  createShell: function(options) {
    options = options || {};

    const includeHeaderActions = (options.includeHeaderActions !== false);
    const addDefaultSlotClasses = (options.addDefaultSlotClasses !== false);
    const titleTag = options.titleTag || 'h3';
    const subtitleTag = options.subtitleTag || 'p';

    const root = el(options.rootTag || 'div', {class: 'ui-shell'});
    addClasses(root, options.classes);

    const header = el(options.headerTag || 'div');
    if (addDefaultSlotClasses) header.classList.add('ui-shell-header');
    addClasses(header, options.headerClasses);

    const headerMain = el(options.headerMainTag || 'div');
    if (addDefaultSlotClasses) headerMain.classList.add('ui-shell-header-main');
    addClasses(headerMain, options.headerMainClasses);

    const titleEl = el(titleTag);
    if (addDefaultSlotClasses) titleEl.classList.add('ui-shell-title');
    addClasses(titleEl, options.titleClasses);

    const subtitleEl = el(subtitleTag);
    if (addDefaultSlotClasses) subtitleEl.classList.add('ui-shell-subtitle');
    addClasses(subtitleEl, options.subtitleClasses);

    const headerActions = includeHeaderActions ? el('div') : null;
    if (headerActions && addDefaultSlotClasses) headerActions.classList.add('ui-shell-header-actions');
    if (headerActions) addClasses(headerActions, options.headerActionsClasses);
    headerMain.appendChild(titleEl);
    headerMain.appendChild(subtitleEl);
    header.appendChild(headerMain);
    if (headerActions) header.appendChild(headerActions);

    const bodyEl = el(options.bodyTag || 'div');
    const footerEl = el(options.footerTag || 'div');
    if (addDefaultSlotClasses) {
      bodyEl.classList.add('ui-shell-body');
      footerEl.classList.add('ui-shell-footer');
    }
    addClasses(bodyEl, options.bodyClasses);
    addClasses(footerEl, options.footerClasses);

    root.appendChild(header);
    root.appendChild(bodyEl);
    root.appendChild(footerEl);

    function refreshVisibility() {
      toggleVisible(titleEl, hasContent(titleEl));
      toggleVisible(subtitleEl, hasContent(subtitleEl));
      const hasActions = headerActions ? hasContent(headerActions) : false;
      if (headerActions) toggleVisible(headerActions, hasActions);
      toggleVisible(header, hasContent(titleEl) || hasContent(subtitleEl) || hasActions);
      toggleVisible(footerEl, hasContent(footerEl));
    }

    const api = {
      element: root,
      header: header,
      headerMain: headerMain,
      headerActions: headerActions,
      body: bodyEl,
      footer: footerEl,
      setTitle: function(content) {
        setContent(titleEl, content);
        refreshVisibility();
        return api;
      },
      setSubtitle: function(content) {
        setContent(subtitleEl, content);
        refreshVisibility();
        return api;
      },
      setHeaderActions: function(content) {
        if (!headerActions) return api;
        setContent(headerActions, content);
        refreshVisibility();
        return api;
      },
      setBody: function(content) {
        setContent(bodyEl, content);
        refreshVisibility();
        return api;
      },
      appendBody: function(content) {
        appendContent(bodyEl, content);
        refreshVisibility();
        return api;
      },
      setFooter: function(content) {
        setContent(footerEl, content);
        refreshVisibility();
        return api;
      },
      appendFooter: function(content) {
        appendContent(footerEl, content);
        refreshVisibility();
        return api;
      }
    };

    if ('title' in options) api.setTitle(options.title);
    if ('subtitle' in options) api.setSubtitle(options.subtitle);
    if ('headerActions' in options) api.setHeaderActions(options.headerActions);
    if ('body' in options) api.setBody(options.body);
    if ('footer' in options) api.setFooter(options.footer);
    refreshVisibility();

    return api;
  },

  createPageShell: function(options) {
    options = options || {};
    return uiHelpers.createShell({
      classes: mergeClasses('ui-shell--page', options.shellClasses),
      addDefaultSlotClasses: false,
      includeHeaderActions: false,
      headerClasses: mergeClasses('page-header', options.headerClasses),
      headerMainClasses: mergeClasses('page-title-group', options.headerMainClasses),
      bodyClasses: mergeClasses('page-body', options.bodyClasses),
      footerClasses: mergeClasses('page-footer', options.footerClasses),
      titleTag: 'h2',
      subtitleClasses: mergeClasses('page-subtitle', options.subtitleClasses),
      title: options.title,
      subtitle: options.subtitle,
      body: options.body,
      footer: options.footer
    });
  },

  openModal: function(options) {
    options = options || {};

    const shell = uiHelpers.createShell({
      classes: mergeClasses('ui-shell--modal', options.shellClasses),
      bodyClasses: options.bodyClasses,
      footerClasses: options.footerClasses,
      title: options.title,
      subtitle: options.subtitle,
      body: ('body' in options) ? options.body : el('div', null, options.loadingText || 'Loading...'),
      footer: options.footer,
      headerActions: options.headerActions
    });

    const popup = uiCore.popup(shell.element);
    if (options.size) popup.element.classList.add('popup--' + options.size);
    addClasses(popup.element, options.popupClasses);

    return {
      popup: popup,
      element: popup.element,
      shell: shell,
      close: function() { popup.close(); },
      setTitle: shell.setTitle,
      setSubtitle: shell.setSubtitle,
      setHeaderActions: shell.setHeaderActions,
      setBody: shell.setBody,
      appendBody: shell.appendBody,
      setFooter: shell.setFooter,
      appendFooter: shell.appendFooter
    };
  },

  dashboardSectionHeader: function(icon, title) {
    return el('h3', {class: 'dashboard-section-header', 'data-icon': icon}, title);
  }
};
