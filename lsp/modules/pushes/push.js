import { APP_NAME } from '@brand';
import { registerTab } from '../core/tab_registry.js';
import { register } from '../core/mode_dispatch.js';
import { pushScenarios } from './push_scenarios.js';
import { pushRenderers } from './push_renderers.js';
import { uiHelpers } from '../core/ui_helpers.js';
import { variables } from '../pages/variables.js';
import * as format from '../core/formatters.js';
import { apiClient } from '../core/api_client.js';
import { formEngine } from '../core/form_engine.js';
import { navto } from '../core/navigation.js';
import { tabView } from '../core/tab_view.js';
import { dynamicUI } from '../core/dynamic.js';
import { uiCore } from '../core/ui_core.js';
import { copy } from '../core/clipboard.js';
import { mistHelpers } from '../core/mist_helpers.js';
import { sockets } from '../core/sockets.js';
import { el, getval } from '../core/dom_helpers.js';
import { defineSection } from '../core/section_registry.js';

defineSection({
  id: 'push/active',
  page: 'Push',
  title: 'Active Pushes',
  subtitle: 'Running push outputs and their statistics',
  keywords: ['push list', 'active push', 'recording', 'RTMP push', 'SRT push', 'restream', 'statistics', 'latency', 'bytes'],
  requires: ['push_list'],
  navTo: { tab: 'Push', other: '' }
});

defineSection({
  id: 'push/automatic',
  page: 'Push',
  title: 'Automatic Pushes',
  subtitle: 'Scheduled and conditional push rules',
  keywords: ['automatic push', 'auto push', 'schedule', 'recording', 'conditional', 'retry', 'deactivated', 'enabled', 'disabled'],
  requires: ['auto_push'],
  navTo: { tab: 'Push', other: '' }
});

defineSection({
  id: 'push/config',
  page: 'Push',
  title: 'Push Configuration',
  subtitle: 'Create or edit push targets',
  keywords: ['push config', 'push target', 'DVR', 'recording path', 'platform', 'Twitch', 'YouTube', 'Facebook', 'stream key', 'schedule', 'start rule', 'end rule'],
  requires: ['capabilities'],
  navTo: { tab: 'Push Config', other: '' }
});

const ps = pushScenarios;
const pr = pushRenderers;
const friendlyTarget = ps.friendlyTarget;
const scenarioIcon = ps.getIcon;
const scenarioLabel = ps.getLabel;
const setPageTitle = uiHelpers.setPageTitle;
const appendPageActions = uiHelpers.appendPageActions;
const pageIntro = uiHelpers.pageIntro;
const pageToolbar = uiHelpers.pageToolbar;
const pageActionRow = uiHelpers.pageActionRow;

function pushGuided(tab, other, prev, $main, $pageHeader) {
  $pageHeader.classList.add('page-header--dense');
  $main.classList.add('page-body--dense', 'page-body--flex-col', 'push-page-body', 'push-guided-content', 'push-page-shell', 'slab-shell');
  setPageTitle($pageHeader, 'Push & Record');
  const newPushBtn = el('button', {class: 'save', 'data-icon': 'plus'}, 'New Push');
  newPushBtn.addEventListener('click', function() {
    navto('Push Config');
  });
  appendPageActions($pageHeader, newPushBtn);

  apiClient.send(function(d) {
    mist.data.push_list = d.push_list;
    const context_menu = new uiCore.context_menu();
    $main.appendChild(context_menu.ele);

    const hasAuto = d.auto_push && Object.keys(d.auto_push).length > 0;
    const hasActive = d.push_list && d.push_list.length > 0;

    if (!hasAuto && !hasActive) {
      const emptyState = el('div', {class: 'push-empty-state'});
      emptyState.appendChild(el('div', {'data-icon': 'send', class: 'push-empty-icon'}));
      emptyState.appendChild(el('h3', null, 'No pushes configured'));
      emptyState.appendChild(el('p', null, 'Push a stream to another server, record to a file, or stream to platforms like YouTube and Twitch.'));
      const createBtn = el('button', {class: 'save', 'data-icon': 'plus'}, 'Create your first push');
      createBtn.addEventListener('click', function() {
        navto('Push Config');
      });
      emptyState.appendChild(createBtn);
      $main.appendChild(emptyState);
      return;
    }

    const $guidedContent = $main;
    const guidedIntro = pageIntro('Push streams to other servers, record to files, or stream to platforms like YouTube and Twitch.');
    guidedIntro.classList.add('push-shell-intro');
    $guidedContent.appendChild(guidedIntro);

    const $activeSection = el('section', {class: 'push-section'});
    const $autoSection = el('section', {class: 'push-section'});

    let filterText = '';
    let filterType = 'all';

    const $filterInput = formEngine.buildUI([{
      label: 'Filter pushes',
      classes: ['filter'],
      help: 'Filter by stream name, target, or notes.',
      'function': function() {
        filterText = (getval(this) || '').toLowerCase();
        applyFilter();
      }
    }]);

    const $typeFilter = el('span', {class: 'push-type-filter'});
    ['All', 'Active', 'Automatic'].forEach(function(label) {
      const val = label.toLowerCase();
      const btn = el('button', null, label);
      btn.addEventListener('click', function() {
        filterType = val;
        Array.from($typeFilter.querySelectorAll('button')).forEach(function(b) { b.classList.remove('active'); });
        this.classList.add('active');
        applyFilter();
      });
      if (val === 'all') btn.classList.add('active');
      $typeFilter.appendChild(btn);
    });
    $guidedContent.appendChild(pageToolbar([$filterInput, $typeFilter], 'push-toolbar'));

    function applyFilter() {
      if (!$activeSection) return;
      Array.from($activeSection.querySelectorAll('.push-card')).forEach(function(c) {
        const text = ((c._pushStream || '') + ' ' + (c._pushTarget || '')).toLowerCase();
        const matchText = !filterText || text.indexOf(filterText) >= 0;
        c.classList.toggle('hidden', !matchText);
      });

      Array.from($autoSection.querySelectorAll('.push-card')).forEach(function(c) {
        const text = ((c._pushStream || '') + ' ' + (c._pushTarget || '') + ' ' + (c._pushNotes || '')).toLowerCase();
        const matchText = !filterText || text.indexOf(filterText) >= 0;
        c.classList.toggle('hidden', !matchText);
      });

      $activeSection.classList.toggle('hidden', filterType === 'automatic');
      $autoSection.classList.toggle('hidden', filterType === 'active');

      updateSectionCounts();
    }

    function updateSectionCounts() {
      const activeVisible = $activeSection.querySelectorAll('.push-card:not(.hidden)').length;
      const autoVisible = $autoSection.querySelectorAll('.push-card:not(.hidden)').length;
      let activeCount = $activeSection.querySelector('.push-section-count');
      let autoCount = $autoSection.querySelector('.push-section-count');
      if (activeCount) activeCount.textContent = '(' + activeVisible + ')';
      if (autoCount) autoCount.textContent = '(' + autoVisible + ')';
    }

    const $activeHeader = el('h3', {class: 'push-section-header push-section-header--collapsible', 'data-icon': 'activity'}, 'Active Pushes');
    $activeHeader.appendChild(el('span', {class: 'push-section-count'}));
    let $activeCards = el('div', {class: 'push-card-grid'});
    $activeHeader.addEventListener('click', function() {
      $activeSection.classList.toggle('collapsed');
    });

    $activeSection.appendChild($activeHeader);
    $activeSection.appendChild($activeCards);

    if (hasActive) {
      for (let i = 0; i < d.push_list.length; i++) {
        const push = mistHelpers.convertPushArr2Obj(d.push_list[i]);
        $activeCards.appendChild(buildActivePushCard(push, context_menu));
      }
    }

    $guidedContent.appendChild($activeSection);
    if (!hasActive) $activeSection.classList.add('hidden');

    sockets.http.api.subscribe(function(sd) {
      mist.data.push_list = sd.push_list;
      $activeCards.innerHTML = '';
      if (!sd.push_list || !sd.push_list.length) {
        $activeSection.classList.add('hidden');
      } else {
        $activeSection.classList.remove('hidden');
        for (let j = 0; j < sd.push_list.length; j++) {
          const p = mistHelpers.convertPushArr2Obj(sd.push_list[j]);
          $activeCards.appendChild(buildActivePushCard(p, context_menu));
        }
      }
      applyFilter();
    }, { push_list: 1 });

    const $autoHeader = el('h3', {class: 'push-section-header push-section-header--collapsible', 'data-icon': 'repeat'}, 'Automatic Pushes');
    $autoHeader.appendChild(el('span', {class: 'push-section-count'}));
    const $autoCards = el('div', {class: 'push-card-grid'});
    $autoHeader.addEventListener('click', function() {
      $autoSection.classList.toggle('collapsed');
    });

    $autoSection.appendChild($autoHeader);
    $autoSection.appendChild($autoCards);

    if (hasAuto) {
      for (const id in d.auto_push) {
        let autoPush = Object.assign({}, d.auto_push[id]);
        autoPush.id = id;
        $autoCards.appendChild(buildAutoPushCard(autoPush, context_menu));
      }
    }

    $guidedContent.appendChild($autoSection);
    if (!hasAuto) $autoSection.classList.add('hidden');

    const $varSection = el('section', {class: 'push-section push-variables-section'});
    const $varHeader = el('h3', {class: 'push-section-header', 'data-icon': 'variable'}, 'Variables');
    const $varIntro = el('p', {class: 'wizard-intro'}, 'Variables are substituted in push targets, recording paths, and stream sources. Use them for reusable paths or conditional push scheduling.');
    const newVarBtn = el('button', {'data-icon': 'plus'}, 'New Variable');
    newVarBtn.addEventListener('click', function() {
      variables.editPopup('', {
        onSave: function() { refreshVarTable(); }
      });
    });
    const $varActions = pageActionRow(newVarBtn);
    const $varTable = el('div', {class: 'push-vars-table-shell'});
    $varSection.appendChild($varHeader);
    $varSection.appendChild($varIntro);
    $varSection.appendChild($varActions);
    $varSection.appendChild($varTable);
    $guidedContent.appendChild($varSection);

    function refreshVarTable() {
      variables.renderTable($varTable, {
        onMutate: refreshVarTable
      });
    }
    refreshVarTable();

    applyFilter();

  }, { push_auto_list: 1, push_list: 1, push_settings: 1 });
}

function buildPushCardBase(target, streamName, dotClass, statusText) {
  const icon = scenarioIcon(target);
  const $status = el('div', {class: 'push-card-status'});
  $status.appendChild(el('span', {class: 'push-card-dot ' + dotClass}));
  $status.appendChild(el('span', {class: 'push-card-status-text'}, statusText));
  const $kind = el('div', {class: 'push-card-kind'});
  if (icon) {
    $kind.appendChild(el('span', {class: 'push-card-type-icon', 'data-icon': icon}));
  }
  const label = scenarioLabel(target);
  if (label) {
    $kind.appendChild(el('span', {class: 'push-card-type-label'}, label));
  }
  const $stream = el('div', {class: 'push-card-stream'}, streamName);
  const $target = el('div', {class: 'push-card-target', title: target}, friendlyTarget(target));
  const $identity = el('div', {class: 'push-card-identity'});
  $identity.appendChild($stream);
  $identity.appendChild($target);
  return { $status: $status, $kind: $kind, $stream: $stream, $target: $target, $identity: $identity };
}

function buildCardMetaRow(label, value, icon) {
  const $item = el('div', {class: 'push-card-meta-row'});
  const $key = el('span', {class: 'push-card-meta-key'});
  if (icon) {
    $key.appendChild(el('span', {class: 'push-card-meta-key-icon', 'data-icon': icon}));
  }
  $key.appendChild(document.createTextNode(label || ''));
  $item.appendChild($key);
  $item.appendChild(el('span', {class: 'push-card-meta-value'}, value));
  return $item;
}

function renderPushTableEmpty($container, text) {
  $container.innerHTML = '';
  $container.appendChild(el('div', {class: 'push-table-empty'}, text));
}

function buildActivePushCard(push, context_menu) {
  let $card = el('div', {class: 'push-card push-card--active'});
  let target = push.resolved_target || push.target;
  if (push.stats && push.stats.current_target) target = push.stats.current_target;

  $card._pushStream = push.stream || '';
  $card._pushTarget = target || '';

  const base = buildPushCardBase(target, push.stream, 'active', 'Active');

  const $header = el('div', {class: 'push-card-header'});
  $header.appendChild(base.$status);

  const $body = el('div', {class: 'push-card-body'});
  $body.appendChild(base.$identity);
  if (base.$kind.children.length) {
    $body.appendChild(base.$kind);
  }

  const $stats = el('div', {class: 'push-card-meta'});
  if (push.stats) {
    if (push.stats.active_ms) {
      $stats.appendChild(buildCardMetaRow('Uptime', format.duration(push.stats.active_ms * 1e-3), 'clock'));
    }
    if (push.stats.bytes) {
      $stats.appendChild(buildCardMetaRow('Sent', format.bytes(push.stats.bytes)));
    }
  }
  if (!$stats.children.length) {
    $stats.appendChild(buildCardMetaRow('Stats', 'Collecting'));
  }
  $body.appendChild($stats);

  const $footer = el('div', {class: 'push-card-footer'});
  const $actions = el('div', {class: 'push-card-actions'});
  const stopBtn = el('button', {class: 'push-card-stop', 'data-icon': 'square'}, 'Stop');
  stopBtn.addEventListener('click', function(e) {
    e.stopPropagation();
    if (confirm('Stop this push?\n' + push.stream + ' \u2192 ' + push.target)) {
      apiClient.send(function() {}, { push_stop: [push.id] });
    }
  });
  $actions.appendChild(stopBtn);
  $footer.appendChild($actions);

  $card.appendChild($header);
  $card.appendChild($body);
  $card.appendChild($footer);

  const openContextMenu = function(pos) {
    const targetCells = formatTargetCells(target);
    const $menuHeader = pr.buildContextMenuHeader(push, targetCells);
    const actions = [
      ['Stop', function() {
        if (confirm('Stop this push?\n' + push.stream + ' to ' + push.target)) {
          apiClient.send(function() {
            context_menu.hide();
          }, { push_stop: [push.id] });
        }
      }, 'stop', 'Stop this push.'],
      ['Copy target', function() {
        const me = this;
        copy(target).then(function() {
          me._setText('Copied!');
          setTimeout(function() { context_menu.hide(); }, 300);
        }).catch(function() {
          me._setText('Failed');
        });
        return false;
      }, 'copy', 'Copy the current push target URL to clipboard.']
    ];
    context_menu.show([[$menuHeader], actions], pos);
  };

  $card.addEventListener('contextmenu', function(e) {
    e.preventDefault();
    e.stopPropagation();
    openContextMenu(e);
  });

  $card.addEventListener('click', function() {
    navto('Preview', push.stream);
  });

  return $card;
}

function buildAutoPushCard(push, context_menu) {
  const parsed = pr.parseDeactivation(push);
  const deactivated = parsed.deactivated;
  const streamName = parsed.stream;
  const rawTarget = push.target || '';
  const target = rawTarget.split('?')[0];

  let $card = el('div', {class: 'push-card push-card--auto'});
  if (deactivated) $card.classList.add('push-card--disabled');

  $card._pushStream = streamName;
  $card._pushTarget = target;
  $card._pushNotes = push['x-LSP-notes'] || '';

  const base = buildPushCardBase(target, streamName, deactivated ? 'disabled' : 'auto', deactivated ? 'Disabled' : 'Automatic');

  let scheduleIcon = 'repeat';
  let scheduleText = 'Always active';
  if (push.start_rule) {
    scheduleIcon = 'variable';
    scheduleText = 'Conditional';
  } else if (push.scheduletime) {
    scheduleIcon = 'calendar';
    const startDate = new Date(push.scheduletime * 1e3).toLocaleDateString();
    scheduleText = 'Scheduled: ' + startDate;
    if (push.completetime) {
      scheduleText += ' \u2013 ' + new Date(push.completetime * 1e3).toLocaleDateString();
    }
  }
  const $header = el('div', {class: 'push-card-header'});
  $header.appendChild(base.$status);

  const $body = el('div', {class: 'push-card-body'});
  $body.appendChild(base.$identity);
  if (base.$kind.children.length) {
    $body.appendChild(base.$kind);
  }

  const $schedule = el('div', {class: 'push-card-meta'});
  $schedule.appendChild(buildCardMetaRow('Schedule', scheduleText, scheduleIcon));
  $body.appendChild($schedule);

  if (push['x-LSP-notes']) {
    const $notes = el('div', {class: 'push-card-meta'});
    $notes.appendChild(buildCardMetaRow('Notes', push['x-LSP-notes']));
    $body.appendChild($notes);
  }

  const $toggle = el('label', {class: 'toggle-switch push-card-toggle'});
  let toggleInput = el('input', {type: 'checkbox'});
  toggleInput.checked = !deactivated;
  toggleInput.addEventListener('change', function() {
    let newPush = Object.assign({}, push);
    if (deactivated) {
      newPush.stream = streamName;
    } else {
      newPush.stream = pr.DEACTIVATION_MARKER + streamName;
    }
    let o = {};
    o[push.id] = newPush;
    apiClient.send(function() {
      tabView.showTab('Push');
    }, { push_auto_add: o });
  });
  $toggle.appendChild(toggleInput);
  $toggle.appendChild(el('div', {class: 'slider'}));
  $toggle.addEventListener('click', function(e) { e.stopPropagation(); });

  $header.appendChild($toggle);

  $card.appendChild($header);
  $card.appendChild($body);

  const openContextMenu = function(pos) {
    const targetCells = formatTargetCells(push.target || '');
    const pushForHeader = Object.assign({}, push, {stream: streamName});
    const $menuHeader = pr.buildContextMenuHeader(pushForHeader, targetCells);
    const actions = [
      ['Edit', function() {
        navto('Push Config', 'auto_' + push.id);
      }, 'Edit', 'Edit this automatic push.'],
      ['Copy target', function() {
        const me = this;
        copy(push.target || '').then(function() {
          me._setText('Copied!');
          setTimeout(function() { context_menu.hide(); }, 300);
        }).catch(function() {
          me._setText('Failed');
        });
        return false;
      }, 'copy', 'Copy the full target URL to clipboard.']
    ];

    if (deactivated) {
      actions.push(['Enable', function() {
        const o = {};
        o[push.id] = Object.assign({}, push, {stream: streamName});
        apiClient.send(function() {
          tabView.showTab('Push');
          context_menu.hide();
        }, { push_auto_add: o });
        return false;
      }, 'wake', 'Enable this automatic push.']);
    } else {
      actions.push(['Disable', function() {
        const o = {};
        o[push.id] = Object.assign({}, push, {stream: pr.DEACTIVATION_MARKER + streamName});
        apiClient.send(function() {
          tabView.showTab('Push');
          context_menu.hide();
        }, { push_auto_add: o });
        return false;
      }, 'sleep', 'Disable this automatic push.']);
    }

    actions.push(['Remove', function() {
      if (confirm('Remove this automatic push?\n' + streamName + ' to ' + push.target)) {
        apiClient.send(function() {
          tabView.showTab('Push');
          context_menu.hide();
        }, { push_auto_remove: push.id });
      }
      return false;
    }, 'trash', 'Remove this automatic push.']);

    context_menu.show([[$menuHeader], actions], pos);
  };

  $card.addEventListener('contextmenu', function(e) {
    e.preventDefault();
    e.stopPropagation();
    openContextMenu(e);
  });

  $card.addEventListener('click', function() {
    navto('Push Config', 'auto_' + push.id);
  });

  return $card;
}

const formatTargetCells = pr.formatTargetCells;
const printCondition = pr.formatCondition;

function buildActiveTable($container, d, menuHost) {
  const context_menu = new uiCore.context_menu();
  (menuHost || $container).appendChild(context_menu.ele);

  const statLabels = pr.STAT_LABELS;
  const statFormatters = pr.STAT_FORMATTERS;

  const $table = dynamicUI.dynamic({
    create: function() {
      const $t = el('table', {'data-pushtype': 'active'});
      const $header = el('tr');
      const thead = el('thead');
      thead.appendChild($header);
      $t.appendChild(thead);
      $t.appendChild(el('tbody'));

      $header.appendChild(el('th', {class: 'header', 'data-index': 'Stream'}, 'Stream'));
      $header.appendChild(el('th', {class: 'header', 'data-index': 'Target'}, 'Target'));
      $header.appendChild(el('th', {class: 'header', 'data-index': 'Statistics'}, 'Statistics'));
      $header.appendChild(el('th', {class: 'header'}));

      let stored = mistHelpers.stored.get();
      if (!('sort_pushes' in stored)) stored.sort_pushes = {};
      let $whichStat = el('select');
      [['pid','Pid'],['active_seconds','Time active'],['bytes','Transferred data'],['mediatime','Transferred media time']].forEach(function(o) {
        let opt = el('option', null, o[1]);
        opt.value = o[0];
        $whichStat.appendChild(opt);
      });
      $whichStat.value = stored.sort_pushes_statistics_type || 'pid';
      $whichStat.addEventListener('change', function() {
        mistHelpers.stored.set('sort_pushes_statistics_type', this.value);
        $t.sort('Statistics');
      });
      const statsHeader = $header.querySelector('[data-index="Statistics"]');
      if (statsHeader) statsHeader.appendChild($whichStat);

      uiCore.sortableItems($t, function(sortby) {
        if (sortby === 'Statistics') {
          let stats = $t._children[this.getAttribute('data-pushid')].values;
          if (stats) stats = stats.stats;
          const which = $whichStat.value;
          if (stats && (which in stats)) return stats[which];
          return null;
        }
        return $t._children[this.getAttribute('data-pushid')]._children[sortby].raw;
      }, {
        controls: $header,
        sortby: 'Statistics',
        sortsave: 'sort_pushes',
        container: $t.children[1]
      });

      return $t;
    },
    add: {
      create: function(id) {
        let $tr = el('tr', {'data-pushid': id});
        $tr._children = {};

        const $tdStream = el('td', {'data-index': 'Stream'});
        const $tdTarget = el('td', {'data-index': 'Target'});
        let $tdStats = el('td', {'data-index': 'Statistics'});
        const $tdActions = el('td');

        $tdStats.dynamic = dynamicUI.dynamic({
          create: function() { return el('div', {class: 'statistics'}); },
          add: {
            create: function(sid) {
              if (sid in statLabels) {
                const d = el('div');
                d.setAttribute('beforeifnotempty', statLabels[sid]);
                return d;
              }
            },
            update: function(val, allValues) {
              if (this._id in statFormatters) {
                var result = statFormatters[this._id](val, allValues);
                if (result instanceof HTMLElement) {
                  this.innerHTML = '';
                  this.appendChild(result);
                } else {
                  this.innerHTML = result;
                }
              }
            }
          }
        });
        $tdStats.innerHTML = '';
        $tdStats.appendChild($tdStats.dynamic);

        $tr._children.Stream = $tdStream;
        $tr._children.Target = $tdTarget;
        $tr._children.Statistics = $tdStats;
        $tr.appendChild($tdStream);
        $tr.appendChild($tdTarget);
        $tr.appendChild($tdStats);
        $tr.appendChild($tdActions);

        const openContextMenu = function(pos) {
          const push = $table._children[id].values;
          const targetChildren = Array.from($tr._children.Target.children);
          const $header = pr.buildContextMenuHeader(push, targetChildren);
          context_menu.show([[$header], [
            ['Stop', function() {
              if (confirm('Stop this push?\n' + push.stream + ' to ' + push.target)) {
                apiClient.send(function() {}, { push_stop: [push.id] });
              }
            }, 'trash', 'Stop this push.'],
            ['Copy target', function() {
              const me = this;
              copy(push.resolved_target || push.target).then(function() {
                me._setText('Copied!');
                setTimeout(function() { context_menu.hide(); }, 300);
              }).catch(function() {
                me._setText('Failed');
              });
                return false;
            }, 'copy', 'Copy target URL to clipboard.']
          ]], pos);
        };
        $tr._openContextMenu = openContextMenu;
        $tr.addEventListener('contextmenu', function(e) {
          e.preventDefault();
          openContextMenu(e);
        });
        return $tr;
      },
      update: function(push) {
        let target = push.resolved_target || push.target;
        if (push.stats && push.stats.current_target) target = push.stats.current_target;

        const streamLink = el('a', {class: 'clickable'}, push.stream);
        streamLink.addEventListener('click', function(e) {
          e.stopPropagation();
          navto('Preview', push.stream);
        });
        this._children.Stream.innerHTML = '';
        this._children.Stream.appendChild(streamLink);
        this._children.Stream.raw = push.stream;

        const targetCells = formatTargetCells(target);
        this._children.Target.innerHTML = '';
        targetCells.forEach(function(c) { this._children.Target.appendChild(c); }.bind(this));
        this._children.Target.raw = target;

        let v = { pid: push.id };
        if (push.stats) v = Object.assign(v, push.stats);
        this._children.Statistics.dynamic.update(v);

        let lastTd = this.querySelector('td:last-child');
        if (lastTd) {
          lastTd.setAttribute('data-index', 'Actions');
          const btnsDiv = el('div', {class: 'buttons'});
          const actionsBtn = el('button', {class: 'push-row-actions', type: 'button', title: 'Actions'}, 'Actions');
          actionsBtn.addEventListener('click', function(e) {
            e.stopPropagation();
            if (typeof this.closest('tr')._openContextMenu === 'function') {
              const rect = this.getBoundingClientRect();
              this.closest('tr')._openContextMenu({
                pageX: rect.left + window.scrollX + rect.width / 2,
                pageY: rect.bottom + window.scrollY + 4
              });
            }
          });
          btnsDiv.appendChild(actionsBtn);
          lastTd.innerHTML = '';
          lastTd.appendChild(btnsDiv);
        }
        const t = this.closest('table'); if (t && t.sort) t.sort();
      }
    },
    update: function(values) {
      if (Object.keys(values).length) {
        if (!this.parentNode) $container.appendChild(this);
      } else if (this.parentNode) {
        this.parentNode.removeChild(this);
        renderPushTableEmpty($container, 'No active pushes.');
      }
      this.sort();
    },
    values: d.push_list,
    getEntries: function(data) {
      let out = {};
      for (const i in data) {
        let values = mistHelpers.convertPushArr2Obj(data[i]);
        out[values.id] = values;
      }
      return out;
    }
  });

  if (!d.push_list || !d.push_list.length) {
    renderPushTableEmpty($container, 'No active pushes.');
  }

  sockets.http.api.subscribe(function(sd) {
    mist.data.push_list = sd.push_list;
    if (!sd.push_list || !sd.push_list.length) {
      if ($table.parentNode) $table.parentNode.removeChild($table);
      renderPushTableEmpty($container, 'No active pushes.');
    } else {
      Array.from($container.children).forEach(function(child) {
        if (!child.classList.contains('context_menu') && child !== $table) child.remove();
      });
      $table.update(sd.push_list);
    }
  }, { push_list: 1 });
}

function buildAutoTable($container, d, menuHost) {
  const context_menu = new uiCore.context_menu();
  (menuHost || $container).appendChild(context_menu.ele);

  const $table = dynamicUI.dynamic({
    create: function() {
      const $t = el('table', {'data-pushtype': 'auto'});
      const $header = el('tr');
      const thead = el('thead');
      thead.appendChild($header);
      $t.appendChild(thead);
      $t.appendChild(el('tbody'));

      $header.appendChild(el('th', {class: 'header', 'data-index': 'Stream'}, 'Stream'));
      $header.appendChild(el('th', {class: 'header', 'data-index': 'Target'}, 'Target'));
      $header.appendChild(el('th', {class: 'header', 'data-index': 'Conditions'}, 'Conditions'));
      $header.appendChild(el('th', {class: 'header', 'data-index': 'Notes'}, 'Notes'));

      uiCore.sortableItems($t, function(sortby) {
        const row = $t._children[this.getAttribute('data-pushid')];
        const cell = row && row._children ? row._children[sortby] : null;
        return cell ? cell.raw : null;
      }, {
        controls: $header,
        sortby: 'Stream',
        sortsave: 'sort_autopushes',
        container: $t.children[1]
      });

      return $t;
    },
    add: {
      create: function(id) {
        let $tr = el('tr', {'data-pushid': id});
        $tr._children = {};

        let cols = ['Stream', 'Target', 'Conditions', 'Notes'];
        for (let c = 0; c < cols.length; c++) {
          const $td = el('td', {'data-index': cols[c]});
          $tr._children[cols[c]] = $td;
          $tr.appendChild($td);
        }

        $tr.addEventListener('click', function(e) {
          if (window.getSelection().toString().length) return;
          const push = $table._children[id].values;
          navto('Push Config', 'auto_' + push.id);
        });

        const openContextMenu = function(pos) {
          let push = $table._children[id].values;
          const targetChildren = Array.from($tr._children.Target.children);
          const $header = pr.buildContextMenuHeader(push, targetChildren);

          const actions = [];
          actions.push(['Edit', function() {
            navto('Push Config', 'auto_' + push.id);
          }, 'Edit', 'Edit this automatic push']);
          actions.push(['Copy target', function() {
            const me = this;
            copy(push.target).then(function() {
              me._setText('Copied!');
              setTimeout(function() { context_menu.hide(); }, 300);
            }).catch(function() { me._setText('Failed'); });
            return false;
          }, 'copy', 'Copy the full target URL to clipboard.']);

          if (push.deactivated) {
            actions.push(['Enable', function() {
              let o = {};
              o[push.id] = push;
              apiClient.send(function(rd) {
                $table.update(rd.auto_push);
                context_menu.hide();
              }, { push_auto_add: o });
              return false;
            }, 'wake', 'Enable this automatic push']);
          } else {
            actions.push(['Disable', function() {
              push.stream = pr.DEACTIVATION_MARKER + push.stream;
              let o = {};
              o[push.id] = push;
              apiClient.send(function(rd) {
                $table.update(rd.auto_push);
                context_menu.hide();
              }, { push_auto_add: o });
              return false;
            }, 'sleep', 'Disable this automatic push']);
          }
          actions.push(['Remove', function() {
            if (confirm('Remove this automatic push?\n' + push.stream + ' to ' + push.target)) {
              apiClient.send(function(rd) {
                $table.update(rd.auto_push);
                context_menu.hide();
              }, { push_auto_remove: push.id });
            }
            return false;
          }, 'trash', 'Remove this automatic push.']);

          context_menu.show([[$header], actions], pos);
        };
        $tr._openContextMenu = openContextMenu;

        $tr.addEventListener('contextmenu', function(e) {
          e.preventDefault();
          openContextMenu(e);
        });

        return $tr;
      },
      update: function(push) {
        const streamRow = el('div', {class: 'push-row-title'});

        // Stream
        let streamLabel;
        if (push.stream[0] === '#') {
          streamLabel = el('span', {class: 'push-row-stream-label'}, push.stream);
        } else {
          const streamLink = el('a', {class: 'clickable'}, push.stream);
          streamLink.addEventListener('click', function(e) {
            e.stopPropagation();
            navto('Preview', push.stream);
          });
          streamLabel = streamLink;
        }
        streamRow.appendChild(streamLabel);
        this._children.Stream.raw = push.stream;

        // Target
        const targetCells = formatTargetCells(push.target);
        this._children.Target.innerHTML = '';
        targetCells.forEach(function(c) { this._children.Target.appendChild(c); }.bind(this));
        this._children.Target.raw = push.target;

        // Conditions
        const $cond = el('div');
        if (push.scheduletime) $cond.appendChild(el('span', null, 'schedule on ' + new Date(push.scheduletime * 1e3).toLocaleString()));
        if (push.completetime) $cond.appendChild(el('span', null, 'complete on ' + new Date(push.completetime * 1e3).toLocaleString()));
        if (push.start_rule) $cond.appendChild(el('span', null, 'starts if ' + printCondition.apply(null, push.start_rule)));
        if (push.end_rule) $cond.appendChild(el('span', null, 'stops if ' + printCondition.apply(null, push.end_rule)));
        if ($cond.children.length) {
          this._children.Conditions.innerHTML = '';
          this._children.Conditions.appendChild($cond);
        } else {
          this._children.Conditions.innerHTML = '';
        }
        this._children.Conditions.raw = $cond.textContent;

        // Notes
        const notes = push['x-LSP-notes'] || '';
        this._children.Notes.textContent = notes;
        this._children.Notes.setAttribute('title', notes);
        this._children.Notes.raw = notes;

        // Row controls
        const btnsDiv = el('div', {class: 'buttons push-row-controls'});
        const toggleLabel = el('label', {class: 'toggle-switch push-row-toggle'});
        let toggleInput = el('input', {type: 'checkbox'});
        toggleInput.checked = !push.deactivated;
        toggleInput.setAttribute('aria-label', toggleInput.checked ? 'Disable automatic push' : 'Enable automatic push');
        toggleLabel.setAttribute('title', toggleInput.checked ? 'Enabled' : 'Disabled');
        toggleInput.addEventListener('change', function() {
          this.setAttribute('aria-label', this.checked ? 'Disable automatic push' : 'Enable automatic push');
          toggleLabel.setAttribute('title', this.checked ? 'Enabled' : 'Disabled');
          if (push.deactivated) {
            let o = {};
            o[push.id] = Object.assign({}, push, {stream: push.stream});
            apiClient.send(function(rd) { $table.update(rd.auto_push); }, { push_auto_add: o });
          } else {
            let o = {};
            o[push.id] = Object.assign({}, push, {stream: pr.DEACTIVATION_MARKER + push.stream});
            apiClient.send(function(rd) { $table.update(rd.auto_push); }, { push_auto_add: o });
          }
        });
        toggleLabel.appendChild(toggleInput);
        toggleLabel.appendChild(el('div', {class: 'slider'}));
        toggleLabel.addEventListener('click', function(e) { e.stopPropagation(); });
        btnsDiv.appendChild(toggleLabel);

        const actionsBtn = el('button', {class: 'push-row-actions', type: 'button', title: 'Actions'}, 'Actions');
        actionsBtn.addEventListener('click', function(e) {
          e.stopPropagation();
          if (typeof this.closest('tr')._openContextMenu === 'function') {
            const rect = this.getBoundingClientRect();
            this.closest('tr')._openContextMenu({
              pageX: rect.left + window.scrollX + rect.width / 2,
              pageY: rect.bottom + window.scrollY + 4
            });
          }
        });
        btnsDiv.appendChild(actionsBtn);
        streamRow.appendChild(btnsDiv);
        this._children.Stream.innerHTML = '';
        this._children.Stream.appendChild(streamRow);

        const t = this.closest('table'); if (t && t.sort) t.sort();
      },
      getEntries: function(values) {
        return pr.parseDeactivation(values);
      }
    },
    update: function(values) {
      if (Object.keys(values).length) {
        if (!this.parentNode) $container.appendChild(this);
      } else if (this.parentNode) {
        this.parentNode.removeChild(this);
        renderPushTableEmpty($container, 'No automatic pushes configured.');
      }
      this.sort();
    },
    values: d.auto_push,
    getEntries: function(data) {
      let out = {};
      for (const i in data) {
        let values = data[i];
        values.id = i;
        out[values.id] = values;
      }
      return out;
    }
  });

  if (!d.auto_push || !Object.keys(d.auto_push).length) {
    renderPushTableEmpty($container, 'No automatic pushes configured.');
  }
}

function pushAdvanced(tab, other, prev, $main, $pageHeader) {
  $pageHeader.classList.add('page-header--dense');
  $main.classList.add('page-body--dense', 'page-body--flex-col', 'push-page-body', 'push-advanced-content', 'push-page-shell', 'push-advanced-shell', 'slab-shell');
  setPageTitle($pageHeader, 'Push & Record');
  const newPushBtn = el('button', {class: 'save', 'data-icon': 'plus'}, 'New Push');
  newPushBtn.addEventListener('click', function() {
    navto('Push Config');
  });
  appendPageActions($pageHeader, newPushBtn);

  apiClient.send(function(d) {
    mist.data.push_list = d.push_list;

    const $activeCont = el('div', {class: 'push-table-shell push-table-shell--active'});
    const $autoCont = el('div', {class: 'push-table-shell push-table-shell--auto'});
    const $varTable = el('div', {class: 'push-vars-table-shell'});
    const $advancedContent = $main;
    const advancedIntro = pageIntro('Push streams to other servers, record to files, or stream to platforms like YouTube and Twitch.');
    advancedIntro.classList.add('push-shell-intro');
    $advancedContent.appendChild(advancedIntro);

    buildActiveTable($activeCont, d, $advancedContent);
    buildAutoTable($autoCont, d, $advancedContent);
    variables.renderTable($varTable, {
      onMutate: function() { tabView.showTab('Push'); }
    });

    const $settingsSection = el('section', {class: 'push-section'});
    const settingsHeader = el('h3', {class: 'push-section-header', 'data-icon': 'settings'}, 'Automatic Push Settings');
    $settingsSection.appendChild(settingsHeader);
    $settingsSection.appendChild(formEngine.buildUI([
      pageIntro('These settings control retry behavior for automatic pushes. The delay sets how long to wait before retrying a failed push. Maximum retries limits overall retry throughput.'),
      {
        label: 'Delay before retry',
        unit: 's',
        type: 'int',
        min: 0,
        help: 'How long the delay should be before '+APP_NAME+' retries an automatic push.<br>If set to 0, it does not retry.',
        'default': 3,
        pointer: { main: d.push_settings, index: 'wait' }
      },
      {
        label: 'Maximum retries',
        unit: '/s',
        type: 'int',
        min: 0,
        help: 'The maximum amount of retries per second (for all automatic pushes).<br>If set to 0, there is no limit.',
        'default': 0,
        pointer: { main: d.push_settings, index: 'maxspeed' }
      },
      {
        type: 'buttons',
        buttons: [{
          type: 'save',
          label: 'Save',
          'function': function() {
            apiClient.send(function() {
              navto('Push');
            }, { push_settings: d.push_settings });
          }
        }]
      }
    ]));

    const $activeSection = el('section', {class: 'push-section'});
    const $activeHeader = el('h3', {class: 'push-section-header push-section-header--collapsible', 'data-icon': 'activity'}, 'Active Pushes');
    $activeHeader.addEventListener('click', function() { $activeSection.classList.toggle('collapsed'); });
    $activeSection.appendChild($activeHeader);
    $activeSection.appendChild(pageIntro('Live view of all running pushes with real-time statistics. Right-click a row for actions.'));
    $activeSection.appendChild($activeCont);

    const $autoSection = el('section', {class: 'push-section'});
    const $autoHeader = el('h3', {class: 'push-section-header push-section-header--collapsible', 'data-icon': 'repeat'}, 'Automatic Pushes');
    $autoHeader.addEventListener('click', function() { $autoSection.classList.toggle('collapsed'); });
    $autoSection.appendChild($autoHeader);
    $autoSection.appendChild(pageIntro('Automatic pushes start whenever their matched stream becomes active and retry on failure. Click a row to edit, right-click for more actions.'));
    $autoSection.appendChild($autoCont);

    const settingsHeaderEl = $settingsSection.querySelector('.push-section-header');
    if (settingsHeaderEl) {
      settingsHeaderEl.classList.add('push-section-header--collapsible');
      settingsHeaderEl.addEventListener('click', function() { $settingsSection.classList.toggle('collapsed'); });
    }

    const $varSection = el('section', {class: 'push-section push-variables-section'});
    const $varSectionHeader = el('h3', {class: 'push-section-header push-section-header--collapsible', 'data-icon': 'variable'}, 'Variables');
    $varSectionHeader.addEventListener('click', function() { $varSection.classList.toggle('collapsed'); });
    $varSection.appendChild($varSectionHeader);
    $varSection.appendChild(pageIntro('Variables are substituted in push targets, recording paths, and stream sources. Use them for reusable paths or conditional push scheduling.'));
    const newVarBtn = el('button', {'data-icon': 'plus'}, 'New Variable');
    newVarBtn.addEventListener('click', function() {
      variables.editPopup('', {
        onSave: function() { tabView.showTab('Push'); }
      });
    });
    $varSection.appendChild(pageActionRow(newVarBtn));
    $varSection.appendChild($varTable);

    $advancedContent.appendChild($activeSection);
    $advancedContent.appendChild($autoSection);
    $advancedContent.appendChild($settingsSection);
    $advancedContent.appendChild($varSection);
  }, { push_auto_list: 1, push_list: 1, push_settings: 1 });
}

register('Push', {
  guided: pushGuided,
  advanced: pushAdvanced
});

export function startPush(other) {
  navto('Push Config', other || '');
}

registerTab('Start Push', function(tab, other) {
  navto('Push Config', other || '');
});
