import { registerTab } from '../core/tab_registry.js';
import { cardPicker } from '../components/card_picker.js';
import { classifyFields, disclosureForm } from '../components/disclosure_form.js';
import { mountWizard } from '../components/wizard.js';
import { pushScenarios } from './push_scenarios.js';
import { uiHelpers } from '../core/ui_helpers.js';
import { variables } from '../pages/variables.js';
import { apiClient } from '../core/api_client.js';
import { formEngine } from '../core/form_engine.js';
import { navto } from '../core/navigation.js';
import { el, getval, setval, validateFeedback } from '../core/dom_helpers.js';

function buildAssemblyPreview(callbacks, ctx) {
  const completed = callbacks.getCompletedSummaries();
  if (!completed.length) return null;

  const ps = pushScenarios;
  const bar = el('div', {class: 'assembly-preview'});

  for (let i = 0; i < completed.length; i++) {
    const step = completed[i];
    const chip = el('div', {class: 'assembly-chip completed'});
    if (step.icon) chip.setAttribute('data-icon', step.icon);

    const chipLabel = el('span', {class: 'assembly-chip-label'}, step.label);
    let chipValue = el('span', {class: 'assembly-chip-value'});

    if (step.id === 'scenario') {
      const scenario = ps.SCENARIOS[ctx._scenario];
      chipValue.textContent = scenario ? scenario.title : '';
    } else if (step.id === 'target') {
      let display = ctx.stream || '';
      if (ctx.target) display += (display ? ' \u2192 ' : '') + truncate(ctx.target, 30);
      chipValue.textContent = display;
    } else if (step.id === 'params') {
      let count = 0;
      if (ctx.params) {
        for (const k in ctx.params) {
          if (ctx.params[k] !== null && ctx.params[k] !== '' && ctx.params[k] !== undefined && ctx.params[k] !== false) count++;
        }
      }
      chipValue.textContent = count ? count + ' param' + (count !== 1 ? 's' : '') : 'defaults';
    } else {
      chipValue.textContent = step.summary || '';
    }

    chip.appendChild(chipLabel);
    chip.appendChild(chipValue);
    bar.appendChild(chip);
  }

  return bar;
}

function parseDvrTarget(target) {
  const parts = (target || '').split('?');
  let result = {
    segmentPath: parts[0] || '/recordings/$basename/$yday/$hour/$minute_$segmentCounter.ts',
    m3u8: '../../$basename.m3u8',
    split: 24,
    maxEntries: 3600,
    targetAge: null,
    append: true,
    noendlist: true
  };
  if (parts.length > 1) {
    const params = parts[1].split('&');
    for (let i = 0; i < params.length; i++) {
      const kv = params[i].split('=');
      const key = kv[0];
      const val = kv.slice(1).join('=');
      if (key === 'm3u8') result.m3u8 = val;
      else if (key === 'split') result.split = parseInt(val, 10) || 24;
      else if (key === 'maxEntries') result.maxEntries = parseInt(val, 10) || 0;
      else if (key === 'targetAge') result.targetAge = parseInt(val, 10) || null;
      else if (key === 'append') result.append = val === '1';
      else if (key === 'noendlist') result.noendlist = val === '1';
    }
  }
  return result;
}

function assembleDvrTarget(ctx) {
  if (!ctx._dvr) return;
  const d = ctx._dvr;
  let target = d.segmentPath || '';
  const params = [];
  if (d.m3u8) params.push('m3u8=' + d.m3u8);
  if (d.split) params.push('split=' + d.split);
  if (d.maxEntries) params.push('maxEntries=' + d.maxEntries);
  if (d.targetAge) params.push('targetAge=' + d.targetAge);
  if (d.append) params.push('append=1');
  if (d.noendlist) params.push('noendlist=1');
  if (params.length) target += '?' + params.join('&');
  ctx.target = target;
}

function buildVarReference(vars, opts) {
  opts = opts || {};
  const row = el('div', {class: 'var-reference'});
  const label = el('span', {class: 'var-reference-label'}, opts.label || 'Variables');
  let lastFocusedInput = null;
  const inputSelector = opts.inputSelector || 'input[type="text"], textarea';
  const fallbackSelector = opts.fallbackSelector || inputSelector;
  const renderName = (typeof opts.renderName === 'function')
    ? opts.renderName
    : function(v) { return v.name; };
  const insertToken = (typeof opts.insertToken === 'function')
    ? opts.insertToken
    : function(v) { return v.name; };

  if (opts.classes && opts.classes.length) {
    for (let c = 0; c < opts.classes.length; c++) {
      if (opts.classes[c]) row.classList.add(opts.classes[c]);
    }
  }

  setTimeout(function() {
    const parent = row.parentElement;
    if (!parent) return;
    parent.addEventListener('focusin', function(e) {
      if (e.target.matches(inputSelector)) {
        lastFocusedInput = e.target;
      }
    });
  }, 0);

  row.appendChild(label);
  for (let i = 0; i < vars.length; i++) {
    (function(v) {
      const item = el('button', {type: 'button', class: 'var-reference-item'});
      item.appendChild(el('code', null, renderName(v)));

      item.addEventListener('mouseenter', function(e) {
        const tipContent = el('div');
        tipContent.appendChild(el('b', null, v.name));
        tipContent.appendChild(el('br'));
        tipContent.appendChild(document.createTextNode(v.desc));
        const tipLine = el('div', {class: 'var-reference-tip'}, 'Click to insert');
        tipContent.appendChild(tipLine);
        UI.tooltip.show(e, tipContent);
      });
      item.addEventListener('mouseleave', function() {
        UI.tooltip.hide();
      });

      item.addEventListener('click', function(e) {
        e.preventDefault();
        let input = lastFocusedInput;
        if (input && !input.matches(inputSelector)) {
          input = null;
        }
        if (!input) {
          const inputs = row.parentElement.querySelectorAll(fallbackSelector);
          if (inputs.length) input = inputs[0];
        }
        if (!input) return;

        const token = insertToken(v, input);
        if ((token === null) || (typeof token === 'undefined')) return;
        const strToken = String(token);
        const start = input.selectionStart || 0;
        const end = input.selectionEnd || 0;
        const val = input.value || '';
        input.value = val.slice(0, start) + strToken + val.slice(end);
        const newPos = start + strToken.length;
        input.setSelectionRange(newPos, newPos);
        input.focus();
        input.dispatchEvent(new Event('change', {bubbles: true}));
        input.dispatchEvent(new Event('input', {bubbles: true}));
        UI.tooltip.hide();
      });

      row.appendChild(item);
    })(vars[i]);
  }
  return row;
}

function buildStreamReference(suggestions, inputEl) {
  let map = {};
  const base = [];
  const wildcard = [];
  const tags = [];
  const maxVisible = 12;

  function add(value) {
    if (!value || map[value]) return;
    map[value] = true;
    if (value.charAt(0) === '#') {
      tags.push(value);
    } else if (value.indexOf('+') >= 0) {
      wildcard.push(value);
    } else {
      base.push(value);
    }
  }

  const configuredStreams = Object.keys(mist.data.streams || {}).sort();
  for (let i = 0; i < configuredStreams.length; i++) add(configuredStreams[i]);
  for (let j = 0; j < suggestions.length; j++) add(suggestions[j]);
  if (!inputEl) return null;
  const curVal = getval(inputEl);
  if (curVal) add(curVal);

  const all = base.concat(wildcard).concat(tags);
  if (!all.length) return null;

  const row = el('div', {class: 'var-reference stream-reference'});
  const labelEl = el('span', {class: 'var-reference-label'}, 'Stream hints');
  let emptyEl = el('span', {class: 'stream-reference-empty'}, 'No matching streams');
  const items = [];

  function setInputValue(value) {
    setval(inputEl, value, ['stream_hint']);
    if (inputEl.setSelectionRange) {
      const pos = value.length;
      inputEl.setSelectionRange(pos, pos);
    }
    inputEl.focus();
    inputEl.dispatchEvent(new Event('input', {bubbles: true}));
  }

  function updateMatches() {
    const query = ((inputEl.value || '') + '').toLowerCase().trim();
    let shown = 0;
    for (let k = 0; k < items.length; k++) {
      let entry = items[k];
      const value = entry.value.toLowerCase();
      const match = !query || (value.indexOf(query) === 0) || (value.indexOf(query) >= 0);
      if (match && shown < maxVisible) {
        entry.el.hidden = false;
        shown++;
      } else {
        entry.el.hidden = true;
      }
    }
    emptyEl.hidden = shown !== 0;
    row.classList.toggle('empty', shown === 0);
  }

  row.appendChild(labelEl);
  for (let n = 0; n < all.length; n++) {
    (function(value) {
      const item = el('button', {type: 'button', class: 'var-reference-item stream-reference-item'});
      item.appendChild(el('code', null, value));
      item.addEventListener('click', function(e) {
        e.preventDefault();
        setInputValue(value);
        updateMatches();
      });
      items.push({ value: value, el: item });
      row.appendChild(item);
    })(all[n]);
  }
  row.appendChild(emptyEl);

  inputEl.addEventListener('input', updateMatches);
  inputEl.addEventListener('change', updateMatches);
  updateMatches();
  return row;
}

function buildTimeReference(inputEl) {
  if (!inputEl) return null;
  const row = el('div', {class: 'var-reference time-reference'});
  row.appendChild(el('span', {class: 'var-reference-label'}, 'Quick time'));
  const actions = [
    { label: 'Now', offset: 0 },
    { label: '+15m', offset: 15 * 60 },
    { label: '+1h', offset: 60 * 60 },
    { label: '+1d', offset: 24 * 60 * 60 }
  ];

  function applyOffset(offset) {
    const base = Math.floor((new Date()).getTime() / 1e3);
    setval(inputEl, base + offset, ['time_hint']);
    inputEl.dispatchEvent(new Event('input', {bubbles: true}));
    inputEl.focus();
  }

  for (let i = 0; i < actions.length; i++) {
    (function(action) {
      const btn = el('button', {type: 'button', class: 'var-reference-item time-reference-item'}, action.label);
      btn.addEventListener('click', function(e) {
        e.preventDefault();
        applyOffset(action.offset);
      });
      row.appendChild(btn);
    })(actions[i]);
  }
  const clearBtn = el('button', {type: 'button', class: 'var-reference-item time-reference-item clear'}, 'Clear');
  clearBtn.addEventListener('click', function(e) {
    e.preventDefault();
    inputEl.value = '';
    inputEl.dispatchEvent(new Event('change', {bubbles: true}));
    inputEl.dispatchEvent(new Event('input', {bubbles: true}));
    inputEl.focus();
  });
  row.appendChild(clearBtn);

  return row;
}

function listUnique(items) {
  const out = [];
  const seen = new Set();
  for (let i = 0; i < items.length; i++) {
    const val = items[i];
    if ((val === null) || (typeof val === 'undefined') || (val === '')) continue;
    const key = String(val);
    if (seen.has(key)) continue;
    seen.add(key);
    out.push(key);
  }
  return out;
}

function extractVariableCurrentValue(raw) {
  if (raw && (typeof raw === 'object') && !Array.isArray(raw)) {
    if (Object.prototype.hasOwnProperty.call(raw, 'value')) {
      return raw.value;
    }
    return '';
  }
  if (Array.isArray(raw)) {
    return raw.length ? raw[0] : '';
  }
  return raw;
}

function renderHintValue(value) {
  if ((value === null) || (typeof value === 'undefined')) return '';
  if ((typeof value === 'string') || (typeof value === 'number') || (typeof value === 'boolean')) {
    return String(value);
  }
  try {
    return JSON.stringify(value);
  } catch (err) {
    return String(value);
  }
}

function buildConditionalVariableSuggestionSet(variableList) {
  const customVars = variableList || {};
  const customNames = Object.keys(customVars).sort();
  const commonHints = pushScenarios.TEMPLATE_VARS.map(function(v) {
    return {
      name: v.name,
      desc: v.desc || 'Common variable'
    };
  });
  const customHints = customNames.map(function(name) {
    const cur = renderHintValue(extractVariableCurrentValue(customVars[name]));
    const desc = cur
      ? 'Custom variable. Current value: ' + truncate(cur, 80)
      : 'Custom variable.';
    return { name: '$' + name, desc: desc };
  });
  const allHints = commonHints.concat(customHints);
  const commonNames = commonHints.map(function(v) { return v.name.replace(/^\$/, ''); });
  const varNames = listUnique(customNames.concat(commonNames));
  const valueHints = listUnique(
    Object.values(customVars).map(function(raw) {
      return renderHintValue(extractVariableCurrentValue(raw));
    }).concat(
      allHints.map(function(v) { return v.name; })
    )
  );

  return {
    customCount: customNames.length,
    varNames: varNames,
    valueHints: valueHints,
    allHints: allHints
  };
}

function buildScenarioStep($container, ctx, callbacks) {
  const ps = pushScenarios;

  if (ctx._scenario) {
    const cur = ps.SCENARIOS[ctx._scenario];
    if (cur) callbacks.setStepSummary(cur.title);
    callbacks.markValid(true);
  } else {
    callbacks.markValid(false);
  }

  const cards = [];
  for (const id in ps.SCENARIOS) {
    const s = ps.SCENARIOS[id];
    cards.push({
      id: id,
      group: s.group,
      title: s.title,
      desc: s.desc,
      icon: s.icon,
      available: s.available(),
      selected: id === ctx._scenario,
      onClick: function(cardId) {
        const oldScenario = ctx._scenario ? ps.SCENARIOS[ctx._scenario] : null;
        ctx._scenario = cardId;
        const sc = ps.SCENARIOS[cardId];
        callbacks.setStepSummary(sc.title);
        if (oldScenario && ctx.target) {
          const oldDefault = (oldScenario.defaultTarget || '').split('?')[0];
          const oldPrefix = oldScenario.targetPrefix || '';
          const curBase = (ctx.target || '').split('?')[0];
          if ((oldDefault && curBase === oldDefault) || (oldPrefix && ctx.target.indexOf(oldPrefix) === 0)) {
            ctx.target = '';
            ctx._platformKey = null;
            ctx._connectorMatch = null;
          }
        }
        ctx._lockedPrefix = null;
        ctx._lockedSuffix = null;
        ctx._dvr = null;
        if (sc.defaultTarget && !ctx.target) {
          ctx.target = sc.defaultTarget;
        }
        if (sc.isPlatform && sc.targetPrefix && !ctx._platformKey) {
          ctx._platformKey = '';
        }
        callbacks.goTo('target');
      }
    });
  }

  $container.appendChild(cardPicker({
    intro: 'What would you like to do with this push?',
    groups: ps.GROUPS,
    cards: cards,
    searchable: true
  }));
  wrapStepContent($container, 'push-step-shell--scenario');
}

function buildTargetStep($container, ctx, callbacks, allthestreams) {
  const ps = pushScenarios;
  const scenario = ps.SCENARIOS[ctx._scenario];

  if (!scenario) {
    $container.appendChild(el('p', null, 'Please select a scenario first.'));
    callbacks.markValid(false);
    wrapStepContent($container, 'push-step-shell--target');
    return;
  }

  callbacks.setStepSummary('');

  const preview = buildAssemblyPreview(callbacks, ctx);
  if (preview) $container.appendChild(preview);

  const introText = 'Configure the stream source and ' + (scenario.isPlatform ? scenario.title + ' destination.' : 'push target.');
  $container.appendChild(el('p', {class: 'wizard-intro'}, introText));

  const elements = [];

  if (scenario.hint) {
    let hintEl = el('div', {class: 'push-platform-hint'});
    hintEl.innerHTML = scenario.hint;
    elements.push(hintEl);
  }

  var _uriHighlight = null;

  function updateTargetPreview() {
    if (ctx._syncUriFooter) ctx._syncUriFooter();
    if (!_uriHighlight) return;
    const t = ctx.target || '';
    _uriHighlight.innerHTML = '';
    if (!t) return;

    const parts = t.split('?');
    const basePath = parts[0];
    const queryStr = parts.length > 1 ? parts.slice(1).join('?') : '';

    appendHighlighted(_uriHighlight, basePath);

    if (queryStr) {
      const params = queryStr.split('&');
      for (let i = 0; i < params.length; i++) {
        const sep = el('span', {class: 'target-preview-sep'}, i === 0 ? '?' : '&');
        _uriHighlight.appendChild(sep);
        const kv = params[i].split('=');
        const keySpan = el('span', {class: 'target-preview-key'}, kv[0]);
        _uriHighlight.appendChild(keySpan);
        if (kv.length > 1) {
          _uriHighlight.appendChild(document.createTextNode('='));
          appendHighlighted(_uriHighlight, kv.slice(1).join('='));
        }
      }
    }
  }

  function appendHighlighted(parent, text) {
    const re = /(\$[a-zA-Z_]\w*)/g;
    let last = 0;
    let match;
    while ((match = re.exec(text)) !== null) {
      if (match.index > last) {
        parent.appendChild(document.createTextNode(text.slice(last, match.index)));
      }
      const varName = match[1];
      let varInfo = null;
      for (let i = 0; i < ps.TEMPLATE_VARS.length; i++) {
        if (ps.TEMPLATE_VARS[i].name === varName) {
          varInfo = ps.TEMPLATE_VARS[i];
          break;
        }
      }
      const desc = varInfo ? varInfo.desc : '';
      const span = el('span', {class: 'target-preview-var'}, varName);
      if (desc) {
        span.addEventListener('mouseenter', (function(d) {
          return function(e) { UI.tooltip.show(e, d); };
        })(desc));
        span.addEventListener('mouseleave', function() { UI.tooltip.hide(); });
      }
      parent.appendChild(span);
      last = re.lastIndex;
    }
    if (last < text.length) {
      parent.appendChild(document.createTextNode(text.slice(last)));
    }
  }

  elements.push({
    label: 'Stream',
    type: 'str',
    help: 'The stream to push.<br><b>Matching patterns:</b> <code>live</code> - exact match only. <code>live+</code> - all wildcard children. <code>live+alice</code> - one specific child. <code>#tag</code> - any stream with this tag.',
    pointer: { main: ctx, index: 'stream' },
    validate: ['required', function(val) {
      if (val[0] === '#') {
        if (val.indexOf(' ') >= 0) {
          return { msg: 'Spaces are not allowed in tag names.', classes: ['red'] };
        }
        return false;
      }
      const base = val.split('+')[0];
      if (base in mist.data.streams) return false;
      return { msg: "'" + base + "' is not a configured stream name.", classes: ['orange'], 'break': false };
    }]
  });

  const lockedPrefixes = scenario.lockedPrefixes || (scenario.getLockedPrefixes ? scenario.getLockedPrefixes() : null);

  if (ctx._scenario === 'record_dvr') {
    if (!ctx._dvr) {
      ctx._dvr = parseDvrTarget(ctx.target || ps.SCENARIOS.record_dvr.defaultTarget);
    }

    function dvrChanged() {
      assembleDvrTarget(ctx);
      updateSummary();
    }

    elements.push({
      label: 'Segment file path',
      type: 'str',
      help: 'Where each .ts segment is written. Use <b>template variables</b> (shown below) for dynamic naming.',
      placeholder: '/recordings/$basename/$yday/$hour/$minute_$segmentCounter.ts',
      pointer: { main: ctx._dvr, index: 'segmentPath' },
      validate: ['required'],
      'function': dvrChanged
    });
    elements.push({
      label: 'Playlist path (m3u8)',
      type: 'str',
      help: 'Path for the .m3u8 playlist file. Can be relative (to the segment path) or absolute.',
      placeholder: '../../$basename.m3u8',
      pointer: { main: ctx._dvr, index: 'm3u8' },
      validate: ['required'],
      'function': dvrChanged
    });

    elements.push(buildVarReference(ps.TEMPLATE_VARS));
    elements.push({
      label: 'Segment duration',
      type: 'int',
      unit: 's',
      min: 1,
      help: 'Duration of each segment in seconds (split at keyframe boundaries).',
      pointer: { main: ctx._dvr, index: 'split' },
      'function': dvrChanged
    });
    elements.push({
      label: 'Max playlist entries',
      type: 'int',
      min: 0,
      help: 'Maximum number of segments kept in the playlist. 0 or empty for unlimited.',
      pointer: { main: ctx._dvr, index: 'maxEntries' },
      'function': dvrChanged
    });
    elements.push({
      label: 'Max segment age',
      type: 'int',
      unit: 's',
      min: 0,
      help: 'Remove segments older than this many seconds. Leave empty to rely only on max entries.',
      pointer: { main: ctx._dvr, index: 'targetAge' },
      'function': dvrChanged
    });
    elements.push({
      label: 'Append to existing playlist',
      type: 'checkbox',
      help: 'Reuse the playlist file if it already exists when the push restarts.',
      pointer: { main: ctx._dvr, index: 'append' },
      'function': dvrChanged
    });
    elements.push({
      label: 'Keep playlist open (no EXT-X-ENDLIST)',
      type: 'checkbox',
      help: 'Do not write the end tag. Use for continuous streams so players treat it as live.',
      pointer: { main: ctx._dvr, index: 'noendlist' },
      'function': dvrChanged
    });

    assembleDvrTarget(ctx);
  } else if (scenario.isPlatform && scenario.targetPrefix) {
    const prefixDiv = el('div', {class: 'push-platform-prefix'});
    prefixDiv.appendChild(el('span', {class: 'push-platform-label'}, scenario.title));
    prefixDiv.appendChild(el('code', null, scenario.targetPrefix));
    elements.push(prefixDiv);

    elements.push({
      label: scenario.targetLabel || 'Stream key',
      type: 'str',
      help: scenario.targetHelp || '',
      placeholder: scenario.placeholder || '',
      pointer: { main: ctx, index: '_platformKey' },
      validate: ['required'],
      'function': function() {
        const key = getval(this) || '';
        ctx.target = scenario.targetPrefix + key;
        updateSummary();
      }
    });

    if (ctx.target && ctx.target.indexOf(scenario.targetPrefix) === 0) {
      ctx._platformKey = ctx.target.slice(scenario.targetPrefix.length);
    }
  } else if (lockedPrefixes && lockedPrefixes.length) {
    const defaultPrefix = scenario.defaultLockedPrefix || lockedPrefixes[0];

    if (!ctx._lockedPrefix) {
      ctx._lockedPrefix = defaultPrefix;
      if (ctx.target) {
        for (let p = 0; p < lockedPrefixes.length; p++) {
          if (ctx.target.indexOf(lockedPrefixes[p]) === 0) {
            ctx._lockedPrefix = lockedPrefixes[p];
            break;
          }
        }
      }
    }
    if (ctx._lockedSuffix === undefined || ctx._lockedSuffix === null) {
      ctx._lockedSuffix = '';
      if (ctx.target && ctx.target.indexOf(ctx._lockedPrefix) === 0) {
        ctx._lockedSuffix = ctx.target.slice(ctx._lockedPrefix.length);
      }
    }

    let lprefixCode = el('code', null, ctx._lockedPrefix);

    if (lockedPrefixes.length > 1) {
      elements.push({
        label: 'Protocol',
        type: 'select',
        select: lockedPrefixes.map(function(lp) { return [lp, lp]; }),
        pointer: { main: ctx, index: '_lockedPrefix' },
        'function': function() {
          lprefixCode.textContent = ctx._lockedPrefix;
          ctx.target = ctx._lockedPrefix + (ctx._lockedSuffix || '');
          ctx._connectorMatch = ps.getMatchingConnector(ctx.target);
          updateSummary();
        }
      });
    }

    const lprefixDiv = el('div', {class: 'push-platform-prefix'});
    lprefixDiv.appendChild(el('span', {class: 'push-platform-label'}, scenario.title));
    lprefixDiv.appendChild(lprefixCode);
    elements.push(lprefixDiv);

    elements.push({
      label: scenario.targetLabel || 'Address',
      type: 'str',
      help: scenario.targetHelp || '',
      placeholder: scenario.placeholder || '',
      pointer: { main: ctx, index: '_lockedSuffix' },
      validate: ['required'],
      'function': function() {
        ctx._lockedSuffix = getval(this) || '';
        ctx.target = ctx._lockedPrefix + ctx._lockedSuffix;
        ctx._connectorMatch = ps.getMatchingConnector(ctx.target);
        updateSummary();
      }
    });

    if (ctx._scenario === 'record_cloud') {
      elements.push(buildVarReference(ps.TEMPLATE_VARS));
    }

    ctx.target = ctx._lockedPrefix + (ctx._lockedSuffix || '');
  } else {
    let targetHelp = scenario.targetHelp || 'Where the stream will be pushed to.';

    if (ctx._scenario === 'record_file') {
      const exts = ps.getValidFileExtensions();
      if (exts.length) {
        targetHelp += '<br>Supported file extensions: <code>' + exts.join('</code>, <code>') + '</code>';
      }
    } else if (ctx._scenario === 'push_other') {
      const protocols = ps.getValidProtocols();
      if (protocols.length) {
        targetHelp += '<br>Supported protocols:<ul><li>' + protocols.join('</li><li>') + '</li></ul>';
      }
    }

    elements.push({
      label: scenario.targetLabel || 'Target',
      type: 'str',
      help: targetHelp,
      placeholder: scenario.placeholder || '',
      pointer: { main: ctx, index: 'target' },
      validate: ['required', function(val) {
        if (scenario.isPlatform) return false;
        const err = ps.validateTarget(val);
        if (err) return { msg: err, classes: ['red'] };
        return false;
      }],
      'function': function() {
        const t = getval(this) || '';
        ctx._connectorMatch = ps.getMatchingConnector(t);
        updateSummary();
      }
    });

    elements.push(buildVarReference(ps.TEMPLATE_VARS));
  }

  elements.push({
    label: 'Notes',
    type: 'textarea',
    rows: 3,
    help: 'Optional description or notes for this push.',
    pointer: { main: ctx, index: 'x-LSP-notes' }
  });

  const formEl = formEngine.buildUI(elements);
  $container.appendChild(formEl);

  const streamInput = formEl.querySelector('input.field[name="stream"]');
  const streamHints = buildStreamReference(allthestreams || [], streamInput);
  if (streamHints) {
    const uiElement = streamInput.closest('.UIelement');
    if (uiElement) uiElement.after(streamHints);
  }

  function updateSummary() {
    let summary = '';
    if (ctx.stream) summary = ctx.stream;
    if (ctx.target) summary += (summary ? ' \u2192 ' : '') + truncate(ctx.target, 30);
    callbacks.setStepSummary(summary);
    updateTargetPreview();
  }

  if (ctx.target) {
    ctx._connectorMatch = ps.getMatchingConnector(ctx.target);
  }
  updateSummary();
  callbacks.markValid(true);
  wrapStepContent($container, 'push-step-shell--target');

  return {
    validate: function() {
      let valid = true;
      const settings = $container.querySelectorAll('.isSetting');
      Array.from(settings).forEach(function(s) {
        const v = validateFeedback(s);
        if (v !== true) valid = false;
      });
      if (valid && ctx._dvr) assembleDvrTarget(ctx);
      return valid;
    }
  };
}

const TRACK_SELECTOR_TYPES = ['video', 'audio', 'subtitle'];
const TRACK_SELECTORS_COMMON = [
  ['', '(default - all tracks)'],
  ['none', 'None (exclude)'],
  ['all', 'All tracks'],
  ['maxbps', 'Highest bitrate'],
  ['minbps', 'Lowest bitrate']
];
const TRACK_SELECTORS_BY_TYPE = {
  video: [
    ['maxres', 'Highest resolution'],
    ['minres', 'Lowest resolution'],
    ['720p', '720p'],
    ['1080p', '1080p'],
    ['4k', '4K'],
    ['h264', 'H.264 codec'],
    ['hevc', 'HEVC / H.265 codec']
  ],
  audio: [
    ['aac', 'AAC codec'],
    ['opus', 'Opus codec'],
    ['stereo', 'Stereo'],
    ['mono', 'Mono'],
    ['surround', 'Surround']
  ],
  subtitle: []
};
const TRACK_SELECTOR_MISMATCH = {
  video: {
    aac: 'audio',
    opus: 'audio',
    stereo: 'audio',
    mono: 'audio',
    surround: 'audio'
  },
  audio: {
    h264: 'video',
    hevc: 'video',
    h265: 'video',
    av1: 'video',
    vp9: 'video',
    maxres: 'video',
    minres: 'video',
    '720p': 'video',
    '1080p': 'video',
    '4k': 'video'
  },
  subtitle: {
    h264: 'video',
    hevc: 'video',
    h265: 'video',
    av1: 'video',
    vp9: 'video',
    aac: 'audio',
    opus: 'audio'
  }
};

function getStreamInfoURL(streamName) {
  if (!streamName) return null;
  try {
    const url = new URL(mist.user.host);
    let path = url.pathname || '/';
    path = path.replace(/\/api\/?$/, '/');
    if (path.slice(-1) !== '/') path += '/';
    url.pathname = path + 'json_' + encodeURIComponent(streamName) + '.js';
    url.search = 'inclzero=1';
    return url.toString();
  } catch (err) {
    return null;
  }
}

function parseStreamMetaHints(info) {
  if (!info || !info.meta || !info.meta.tracks) return null;
  const hints = {
    trackIdsByType: {
      video: [],
      audio: [],
      subtitle: []
    },
    typeByTrackId: {},
    codecsByType: {
      video: {},
      audio: {},
      subtitle: {}
    },
    languagesByType: {
      video: {},
      audio: {},
      subtitle: {}
    },
    trackCountByType: {
      video: 0,
      audio: 0,
      subtitle: 0
    },
    timing: null
  };

  let hasTrack = false;
  let minMs = Infinity;
  let maxMs = -Infinity;
  for (const id in info.meta.tracks) {
    const track = info.meta.tracks[id];
    if (!track) continue;
    let type = String(track.type || '').toLowerCase();
    if (String(track.codec || '').toLowerCase() === 'subtitle') {
      type = 'subtitle';
    }
    if (TRACK_SELECTOR_TYPES.indexOf(type) < 0) continue;

    hasTrack = true;
    hints.trackCountByType[type]++;

    const trackId = track.trackid;
    if (trackId !== undefined && trackId !== null && trackId !== '') {
      const sid = String(trackId);
      if (hints.trackIdsByType[type].indexOf(sid) < 0) {
        hints.trackIdsByType[type].push(sid);
      }
      hints.typeByTrackId[sid] = type;
    }

    const codecRaw = String(track.codec || '').trim();
    const codec = codecRaw.toLowerCase();
    if (codec && codec !== 'subtitle') {
      hints.codecsByType[type][codec] = codecRaw;
    }

    const languageRaw = String(track.language || '').trim();
    const language = languageRaw.toLowerCase();
    if (language && language !== 'unknown') {
      hints.languagesByType[type][language] = languageRaw;
    }

    const first = Number(track.firstms);
    const lastKey = ('nowms' in track) ? 'nowms' : 'lastms';
    const last = Number(track[lastKey]);
    if (!isNaN(first) && !isNaN(last) && (last >= first)) {
      minMs = Math.min(minMs, first);
      maxMs = Math.max(maxMs, last);
    }
  }

  for (let i = 0; i < TRACK_SELECTOR_TYPES.length; i++) {
    const type = TRACK_SELECTOR_TYPES[i];
    hints.trackIdsByType[type].sort(function(a, b) {
      return Number(a) - Number(b);
    });
  }

  if (minMs !== Infinity && maxMs !== -Infinity) {
    const durationMs = Math.max(0, maxMs - minMs);
    hints.timing = {
      minMs: Math.max(0, Math.floor(minMs)),
      maxMs: Math.max(0, Math.ceil(maxMs)),
      maxDurationS: Math.floor(durationMs / 1000)
    };
  }

  return hasTrack ? hints : null;
}

function loadStreamMetaHints(ctx, done) {
  const stream = String(ctx.stream || '').trim();
  if (!stream || stream.charAt(0) === '#') {
    done(null);
    return;
  }

  if (!ctx._streamMetaCache) ctx._streamMetaCache = {};
  if (stream in ctx._streamMetaCache) {
    done(ctx._streamMetaCache[stream] || null);
    return;
  }

  const requestId = (ctx._streamMetaRequestId || 0) + 1;
  ctx._streamMetaRequestId = requestId;

  const candidates = [stream];
  if (stream.indexOf('+') >= 0) {
    const base = stream.split('+')[0];
    if (base && candidates.indexOf(base) < 0) candidates.push(base);
  }

  function finish(hints) {
    if (ctx._streamMetaRequestId !== requestId) return;
    ctx._streamMetaCache[stream] = hints || null;
    done(hints || null);
  }

  function tryCandidate(index) {
    if (index >= candidates.length) {
      finish(null);
      return;
    }
    const url = getStreamInfoURL(candidates[index]);
    if (!url) {
      tryCandidate(index + 1);
      return;
    }
    fetch(url, {cache: 'no-store'}).then(function(r) {
      if (!r.ok) throw new Error(r.statusText || ('HTTP ' + r.status));
      return r.json();
    }).then(function(info) {
      const hints = parseStreamMetaHints(info);
      if (hints) {
        finish(hints);
      } else {
        tryCandidate(index + 1);
      }
    }).catch(function() {
      tryCandidate(index + 1);
    });
  }

  tryCandidate(0);
}

function validateTrackSelectorChoice(type, value, ctx) {
  const raw = String(value == null ? '' : value).trim();
  if (!raw) return;
  if (/[,&|=!<>~]/.test(raw)) return;
  const token = raw.toLowerCase();

  const hints = ctx._streamMetaHints;
  if (hints) {
    const tokenType = hints.typeByTrackId[token];
    if (tokenType && tokenType !== type) {
      return {
        msg: 'Track ' + raw + ' is a ' + tokenType + ' track, not ' + type + '.',
        classes: ['red']
      };
    }
    if (!(token in hints.codecsByType[type])) {
      for (let i = 0; i < TRACK_SELECTOR_TYPES.length; i++) {
        const other = TRACK_SELECTOR_TYPES[i];
        if (other === type) continue;
        if (token in hints.codecsByType[other]) {
          return {
            msg: '"' + raw + '" is a ' + other + ' codec. Select it under ' + other + ' track.',
            classes: ['red']
          };
        }
      }
    }
  }

  if (TRACK_SELECTOR_MISMATCH[type] && TRACK_SELECTOR_MISMATCH[type][token]) {
    const otherType = TRACK_SELECTOR_MISMATCH[type][token];
    return {
      msg: '"' + raw + '" is usually a ' + otherType + ' selector. Select it under ' + otherType + ' track.',
      classes: ['red']
    };
  }
}

function buildTrackSelectorOptions(type, hints, currentValue) {
  const options = [];
  const seen = {};
  function add(value, label) {
    const key = String(value);
    if (seen[key]) return;
    seen[key] = true;
    options.push([key, label]);
  }

  for (let i = 0; i < TRACK_SELECTORS_COMMON.length; i++) {
    add(TRACK_SELECTORS_COMMON[i][0], TRACK_SELECTORS_COMMON[i][1]);
  }
  const typeOptions = TRACK_SELECTORS_BY_TYPE[type] || [];
  for (let j = 0; j < typeOptions.length; j++) {
    add(typeOptions[j][0], typeOptions[j][1]);
  }

  if (hints) {
    const trackIds = hints.trackIdsByType[type] || [];
    for (let ti = 0; ti < trackIds.length; ti++) {
      add(trackIds[ti], 'Track ID ' + trackIds[ti]);
    }

    const codecs = Object.keys(hints.codecsByType[type] || {}).sort();
    for (let ci = 0; ci < codecs.length; ci++) {
      const codec = codecs[ci];
      add(codec, 'Codec: ' + hints.codecsByType[type][codec]);
    }

    const languages = Object.keys(hints.languagesByType[type] || {}).sort();
    for (let li = 0; li < languages.length; li++) {
      const lang = languages[li];
      add(lang, 'Language: ' + hints.languagesByType[type][lang]);
    }
  }

  const current = String(currentValue == null ? '' : currentValue).trim();
  if (current && !seen[current]) {
    add(current, 'Custom: ' + current);
  }

  return options;
}

function setSelectOptions($field, options, selected) {
  if (!$field) return;
  const current = String(selected == null ? '' : selected);
  $field.innerHTML = '';
  let hasSelected = false;
  for (let i = 0; i < options.length; i++) {
    const opt = el('option');
    opt.value = String(options[i][0]);
    opt.textContent = options[i][1];
    if (opt.value === current) hasSelected = true;
    $field.appendChild(opt);
  }
  $field.value = hasSelected ? current : '';
}

function buildHandledParamKeyMap(ctx, isFile) {
  const handled = {
    stream: true,
    target: true,
    video: true,
    audio: true,
    subtitle: true,
    duration: true,
    recstart: true,
    recstop: true
  };

  if (isFile) {
    handled.split = true;
    handled.rate = true;
  } else {
    handled.split = true;
    handled.rate = true;
    handled.append = true;
    handled.m3u8 = true;
    handled.maxEntries = true;
    handled.targetAge = true;
    handled.noendlist = true;
    handled.unmask = true;
  }

  if (ctx && ctx._scenario === 'record_dvr') {
    handled.m3u8 = true;
    handled.maxEntries = true;
    handled.targetAge = true;
    handled.append = true;
    handled.noendlist = true;
  }

  handled.stream_key = true;
  handled.streamname = true;

  return handled;
}

function isDOMHeading(item) {
  if (typeof HTMLElement === 'undefined') return false;
  if (!(item instanceof HTMLElement)) return false;
  return /^H[1-6]$/.test(item.tagName || '');
}

function elementHasInteractiveFields(item) {
  if (!item) return false;
  if ((typeof HTMLElement !== 'undefined') && (item instanceof HTMLElement)) return false;
  if (item.type === 'group') {
    return listHasInteractiveFields(item.options || []);
  }
  if (item.type === 'help' || item.type === 'text' || item.type === 'buttons') return false;
  return true;
}

function listHasInteractiveFields(elements) {
  for (let i = 0; i < elements.length; i++) {
    if (elementHasInteractiveFields(elements[i])) return true;
  }
  return false;
}

function trimOrphanHeadings(elements) {
  const out = [];
  for (let i = 0; i < elements.length; i++) {
    const item = elements[i];
    if (!isDOMHeading(item)) {
      out.push(item);
      continue;
    }

    let keep = false;
    for (let j = i + 1; j < elements.length; j++) {
      const next = elements[j];
      if (isDOMHeading(next)) break;
      if (elementHasInteractiveFields(next)) {
        keep = true;
        break;
      }
    }
    if (keep) out.push(item);
  }
  return out;
}

function pruneConnectorFormElements(elements, handledKeys) {
  const out = [];
  for (let i = 0; i < elements.length; i++) {
    const item = elements[i];
    if (!item) continue;
    if ((typeof HTMLElement !== 'undefined') && (item instanceof HTMLElement)) {
      out.push(item);
      continue;
    }

    if (item.type === 'group') {
      const prunedOptions = pruneConnectorFormElements(item.options || [], handledKeys);
      const cleanedOptions = trimOrphanHeadings(prunedOptions);
      if (!listHasInteractiveFields(cleanedOptions)) continue;
      const group = Object.assign({}, item, {
        options: cleanedOptions,
        expand: true
      });
      out.push(group);
      continue;
    }

    const key = item.pointer && item.pointer.index;
    if (key && handledKeys[key]) continue;
    out.push(item);
  }

  return trimOrphanHeadings(out);
}

function buildParamsStep($container, ctx, callbacks) {
  const ps = pushScenarios;

  const preview = buildAssemblyPreview(callbacks, ctx);
  if (preview) $container.appendChild(preview);

  if (!ctx.params) ctx.params = {};
  if (!ctx._fullParamList) ctx._fullParamList = {};
  const target = ctx.target || '';
  const isFile = target.charAt(0) === '/';
  const handledParamKeys = buildHandledParamKeyMap(ctx, isFile);

  const universalElements = [];

  universalElements.push(el('h4', null, 'Track Selection'));
  const trackHint = el('p', {class: 'wizard-intro'}, 'Selectors are grouped by track type. Metadata-based hints load automatically for active streams.');
  universalElements.push(trackHint);
  const trackTypes = [
    { key: 'video', label: 'Video track' },
    { key: 'audio', label: 'Audio track' },
    { key: 'subtitle', label: 'Subtitle track' }
  ];
  for (let t = 0; t < trackTypes.length; t++) {
    let tk = trackTypes[t];
    ctx._fullParamList[tk.key] = true;
    universalElements.push({
      label: tk.label,
      type: 'select',
      select: buildTrackSelectorOptions(tk.key, ctx._streamMetaHints, ctx.params[tk.key]),
      help: 'Select by codec (<code>h264</code>, <code>aac</code>), language (<code>eng</code>), '
        + 'resolution (<code>1080p</code>, <code>1920x1080</code>), or bitrate (<code>maxbps</code>, <code>minbps</code>). '
        + 'Use track IDs from metadata when available.',
      pointer: { main: ctx.params, index: tk.key },
      validate: [function(val) {
        return validateTrackSelectorChoice(tk.key, val, ctx);
      }]
    });
  }

  universalElements.push(el('h4', null, 'Timing'));
  const timingHint = el('p', {class: 'wizard-intro'}, 'Media timing bounds will be filled from stream metadata when available.');
  universalElements.push(timingHint);
  ctx._fullParamList.duration = true;
  universalElements.push({
    label: 'Duration',
    type: 'int',
    unit: 's',
    min: 0,
    help: 'Push this many seconds of media, then stop. Leave empty for unlimited.',
    pointer: { main: ctx.params, index: 'duration' },
    validate: [function(val, me) {
      if (val === '' || val === null) return;
      if (ctx._streamMetaHints && ctx._streamMetaHints.timing) {
        const maxDuration = ctx._streamMetaHints.timing.maxDurationS;
        if (maxDuration >= 0 && Number(val) > maxDuration) {
          return {
            msg: 'Duration exceeds detected media window (' + maxDuration + 's).',
            classes: ['red']
          };
        }
      }
    }]
  });
  ctx._fullParamList.recstart = true;
  ctx._fullParamList.recstop = true;
  universalElements.push({
    label: 'Start at (media time)',
    type: 'int',
    unit: 'ms',
    min: 0,
    help: 'Start pushing from this media timestamp in milliseconds.',
    pointer: { main: ctx.params, index: 'recstart' },
    validate: [function(val, me) {
      if (val === '' || val === null) return;
      const container = me.closest('.input_container');
      if (!container) return;
      const stopField = container.querySelector('.field[name="recstop"]');
      const stopVal = stopField ? getval(stopField) : null;
      if (stopVal !== '' && stopVal !== null && Number(stopVal) < Number(val)) {
        return {
          msg: 'Start time must be less than or equal to stop time.',
          classes: ['red']
        };
      }
    }]
  });
  universalElements.push({
    label: 'Stop at (media time)',
    type: 'int',
    unit: 'ms',
    min: 0,
    help: 'Stop pushing at this media timestamp in milliseconds.',
    pointer: { main: ctx.params, index: 'recstop' },
    validate: [function(val, me) {
      if (val === '' || val === null) return;
      const container = me.closest('.input_container');
      if (!container) return;
      const startField = container.querySelector('.field[name="recstart"]');
      const startVal = startField ? getval(startField) : null;
      if (startVal !== '' && startVal !== null && Number(val) < Number(startVal)) {
        return {
          msg: 'Stop time must be greater than or equal to start time.',
          classes: ['red']
        };
      }
    }]
  });

  if (isFile) {
    universalElements.push(el('h4', null, 'Recording'));
    ctx._fullParamList.split = true;
    universalElements.push({
      label: 'Split interval',
      type: 'int',
      unit: 's',
      min: 0,
      help: 'Restart the recording every N seconds at keyframe boundaries. Useful for creating segments.',
      pointer: { main: ctx.params, index: 'split' }
    });
    ctx._fullParamList.rate = true;
    universalElements.push({
      label: 'Playback rate',
      type: 'select',
      select: [['', '(default - real-time)'], ['0', 'As fast as possible'], ['2', 'Double speed']],
      help: 'Speed at which media is written. Use "as fast as possible" for offline transcoding of VoD content.',
      pointer: { main: ctx.params, index: 'rate' }
    });
  }

  const universalForm = formEngine.buildUI(universalElements);
  $container.appendChild(universalForm);

  const trackFields = {
    video: universalForm.querySelector('.field[name="video"]'),
    audio: universalForm.querySelector('.field[name="audio"]'),
    subtitle: universalForm.querySelector('.field[name="subtitle"]')
  };
  const timingFields = {
    duration: universalForm.querySelector('.field[name="duration"]'),
    recstart: universalForm.querySelector('.field[name="recstart"]'),
    recstop: universalForm.querySelector('.field[name="recstop"]')
  };

  function applyMetaHints(hints) {
    ctx._streamMetaHints = hints || null;

    for (let i = 0; i < trackTypes.length; i++) {
      const type = trackTypes[i].key;
      const $field = trackFields[type];
      if (!$field) continue;
      const selected = getval($field);
      const options = buildTrackSelectorOptions(type, hints, selected);
      setSelectOptions($field, options, selected);
      validateFeedback($field);
    }

    if (hints) {
      trackHint.textContent =
        'Detected tracks: '
        + hints.trackCountByType.video + ' video, '
        + hints.trackCountByType.audio + ' audio, '
        + hints.trackCountByType.subtitle + ' subtitle.';
    } else {
      trackHint.textContent = 'No live track metadata found for this stream yet. Generic selector hints are shown.';
    }

    if (hints && hints.timing) {
      timingHint.textContent =
        'Detected media range: '
        + hints.timing.minMs + 'ms to '
        + hints.timing.maxMs + 'ms (max duration '
        + hints.timing.maxDurationS + 's).';
      if (timingFields.recstart) {
        timingFields.recstart.setAttribute('min', hints.timing.minMs);
        timingFields.recstart.setAttribute('max', hints.timing.maxMs);
      }
      if (timingFields.recstop) {
        timingFields.recstop.setAttribute('min', hints.timing.minMs);
        timingFields.recstop.setAttribute('max', hints.timing.maxMs);
      }
      if (timingFields.duration) {
        timingFields.duration.setAttribute('max', hints.timing.maxDurationS);
      }
    } else {
      timingHint.textContent = 'Media timing bounds will be filled from stream metadata when available.';
      if (timingFields.recstart) {
        timingFields.recstart.setAttribute('min', '0');
        timingFields.recstart.removeAttribute('max');
      }
      if (timingFields.recstop) {
        timingFields.recstop.setAttribute('min', '0');
        timingFields.recstop.removeAttribute('max');
      }
      if (timingFields.duration) {
        timingFields.duration.removeAttribute('max');
      }
    }

    if (timingFields.duration) validateFeedback(timingFields.duration);
    if (timingFields.recstart) validateFeedback(timingFields.recstart);
    if (timingFields.recstop) validateFeedback(timingFields.recstop);
  }

  applyMetaHints(ctx._streamMetaHints || null);
  loadStreamMetaHints(ctx, function(hints) {
    if (!$container.isConnected) return;
    applyMetaHints(hints);
  });

  let connector = ctx._connectorMatch;
  if (!connector) {
    if (ctx.target) {
      connector = ps.getMatchingConnector(ctx.target);
      ctx._connectorMatch = connector;
    }
  }

  const targetQuery = (target.split('?')[1]) || '';
  let targetKeys = {};
  if (targetQuery) {
    let tqPairs = targetQuery.split('&');
    for (let tq = 0; tq < tqPairs.length; tq++) {
      targetKeys[tqPairs[tq].split('=')[0]] = true;
    }
  }

  if (connector && mist.data.capabilities.connectors[connector]) {
    const conn = mist.data.capabilities.connectors[connector];
    let friendly = conn.friendly || conn.name || connector;
    friendly = friendly.replace('over HTTP', '');

    callbacks.setStepSummary(friendly);

    const push_parameters = Object.assign({}, conn.push_parameters);

    function processParam(param, key) {
      if (targetKeys[key]) {
        delete push_parameters[key];
        return;
      }
      if (param.prot_only && target.match && target.match(/.+:\/\/.+/) === null) {
        delete push_parameters[key];
        return;
      }
      if (param.file_only && target[0] !== '/') {
        delete push_parameters[key];
        return;
      }
      if (param.type === 'group') {
        for (const i in param.options) {
          processParam(param.options[i], i);
        }
      } else {
        if (handledParamKeys[key]) {
          delete push_parameters[key];
          return;
        }
        ctx._fullParamList[key] = param;
      }
    }

    for (const k in conn.push_parameters) {
      processParam(conn.push_parameters[k], k);
    }

    const capa = {
      desc: (conn.desc || '').replace('over HTTP', ''),
      optional: push_parameters,
      sort: 'sort'
    };
    const capaform = formEngine.convertBuildOptions(capa, ctx.params);
    if (capaform[1] && capaform[1].tagName && capaform[1].tagName.toLowerCase() === 'h4') capaform.splice(1, 1);
    const prunedCapaform = pruneConnectorFormElements(capaform, handledParamKeys);

    const classified = classifyFields(prunedCapaform);
    if (listHasInteractiveFields(classified.basic) || listHasInteractiveFields(classified.advanced)) {
      $container.appendChild(el('h4', null, friendly + ' Parameters'));
      $container.appendChild(disclosureForm({
        basic: classified.basic,
        advanced: classified.advanced,
        startExpanded: true
      }));
    }
  } else {
    callbacks.setStepSummary('');
  }

  $container.appendChild(formEngine.buildUI([
    el('h4', null, 'Custom'),
    {
      type: 'inputlist',
      label: 'Custom parameters',
      value: ctx.custom_url_params || [],
      classes: ['custom_url_parameters'],
      input: {
        type: 'str',
        placeholder: 'name=value',
        prefix: ''
      },
      help: 'Additional <code>name=value</code> parameters not covered above.',
      pointer: { main: ctx, index: 'custom_url_params' }
    }
  ]));
  wrapStepContent($container, 'push-step-shell--params');
}

function buildScheduleStep($container, ctx, callbacks) {
  const preview = buildAssemblyPreview(callbacks, ctx);
  if (preview) $container.appendChild(preview);

  if (!ctx._scheduleMode) ctx._scheduleMode = 'now';

  const optionsDiv = el('div', {class: 'push-schedule-options'});
  let fieldsDiv = el('div', {class: 'push-schedule-fields'});

  const modes = [
    { id: 'now', icon: 'play', title: 'Start now', desc: 'One-time push, starts immediately.' },
    { id: 'always', icon: 'repeat', title: 'Always active', desc: 'Runs whenever the stream is active. Retries on failure.' },
    { id: 'timed', icon: 'calendar', title: 'Scheduled', desc: 'Start and/or stop at specific times.' },
    { id: 'variable', icon: 'variable', title: 'Conditional', desc: 'Trigger based on server variable values.' }
  ];

  for (let i = 0; i < modes.length; i++) {
    (function(mode) {
      const card = el('button', {class: 'push-schedule-card'});
      const head = el('div', {class: 'push-schedule-card-head'});
      head.appendChild(el('span', {class: 'push-schedule-card-icon', 'data-icon': mode.icon}));
      head.appendChild(el('div', {class: 'push-schedule-card-title'}, mode.title));
      card.appendChild(head);
      card.appendChild(el('div', {class: 'push-schedule-card-desc'}, mode.desc));
      card.addEventListener('click', function() {
        ctx._scheduleMode = mode.id;
        Array.from(optionsDiv.querySelectorAll('.push-schedule-card')).forEach(function(c) {
          c.classList.remove('selected');
        });
        this.classList.add('selected');
        buildScheduleFields();
        callbacks.setStepSummary(mode.title);
      });

      if (ctx._scheduleMode === mode.id) {
        card.classList.add('selected');
      }
      optionsDiv.appendChild(card);
    })(modes[i]);
  }

  $container.appendChild(optionsDiv);
  $container.appendChild(fieldsDiv);
  callbacks.setStepSummary(modes.filter(function(m) { return m.id === ctx._scheduleMode; })[0].title);

  function buildScheduleFields() {
    fieldsDiv.innerHTML = '';
    const elements = [];
    let conditionVarHints = null;

    if (ctx._scheduleMode === 'now') {
      fieldsDiv.appendChild(el('p', {class: 'wizard-intro'}, 'The push will start immediately when you click Save.'));
      return;
    }

    elements.push({
      label: 'Enabled',
      type: 'checkbox',
      help: 'When disabled, the automatic push will not start new pushes but stays configured.',
      pointer: { main: ctx, index: 'enabled' }
    });

    if (ctx._scheduleMode === 'always') {
      fieldsDiv.appendChild(el('p', {class: 'wizard-intro'}, 'The push will run whenever the stream is active and will retry on failure.'));
    } else if (ctx._scheduleMode === 'timed') {
      if (!ctx.scheduletime) ctx.scheduletime = null;
      if (!ctx.completetime) ctx.completetime = null;

      elements.push({
        type: 'unix',
        label: 'Start time',
        min: 0,
        help: 'When the push will become active. Leave empty to start immediately.',
        pointer: { main: ctx, index: 'scheduletime' }
      });
      elements.push({
        type: 'unix',
        label: 'End time',
        min: 0,
        help: 'When the push will stop. Leave empty to never stop automatically.',
        pointer: { main: ctx, index: 'completetime' }
      });
    } else if (ctx._scheduleMode === 'variable') {
      if (!ctx.start_rule) ctx.start_rule = [null, null, null];
      if (!ctx.end_rule) ctx.end_rule = [null, null, null];

      conditionVarHints = buildConditionalVariableSuggestionSet(mist.data.variable_list || {});
      const varlist = conditionVarHints.varNames;
      const vallist = conditionVarHints.valueHints;

      const quickCreate = el('div', {class: 'push-var-quick-create'});
      quickCreate.appendChild(el('p', {class: 'wizard-intro'}, 'Trigger pushes based on server variable values.'
        + (conditionVarHints.customCount ? '' : ' No variables configured yet - create one to get started.')));
      const newVarBtn = el('button', null, 'New Variable');
      newVarBtn.setAttribute('data-icon', 'plus');
      newVarBtn.addEventListener('click', function() {
        variables.editPopup('', {
          onSave: function() {
            apiClient.send(function(d) {
              mist.data.variable_list = d.variable_list;
              buildScheduleFields();
            }, { variable_list: true });
          }
        });
      });
      quickCreate.appendChild(newVarBtn);
      elements.push(quickCreate);

      const operators = [
        [0, 'is true'], [1, 'is false'],
        [2, '=='], [3, '!='],
        [10, '> (numerical)'], [11, '>= (numerical)'],
        [12, '< (numerical)'], [13, '<= (numerical)'],
        [20, '> (lexical)'], [21, '>= (lexical)'],
        [22, '< (lexical)'], [23, '<= (lexical)']
      ];

      elements.push(el('h4', null, 'Start condition'));
      elements.push({
        label: 'Variable',
        type: 'str',
        classes: ['startVariable'],
        prefix: '$',
        help: 'The variable that determines if this push should start.',
        datalist: varlist,
        pointer: { main: ctx.start_rule, index: 0 }
      });
      elements.push({
        label: 'Operator',
        type: 'select',
        classes: ['startOperator'],
        select: operators,
        value: ctx.start_rule[1] !== null ? ctx.start_rule[1] : 2,
        help: 'How to compare the variable.',
        pointer: { main: ctx.start_rule, index: 1 },
        'function': function() {
          const valEl = fieldsDiv.querySelector('.startValue');
          let uiEl = valEl ? valEl.closest('.UIelement') : null;
          if (uiEl) {
            if (Number(getval(this)) < 2) { uiEl.hidden = true; } else { uiEl.style.display = ''; uiEl.hidden = false; }
          }
        }
      });
      elements.push({
        label: 'Value',
        type: 'str',
        classes: ['startValue'],
        help: 'Compare the variable with this value. You can also use another variable.',
        datalist: vallist,
        pointer: { main: ctx.start_rule, index: 2 }
      });

      elements.push(el('h4', null, 'Stop condition (optional)'));
      elements.push({
        label: 'Variable',
        type: 'str',
        classes: ['endVariable'],
        prefix: '$',
        help: 'Leave blank to not have an automatic stop condition.',
        datalist: varlist,
        pointer: { main: ctx.end_rule, index: 0 }
      });
      elements.push({
        label: 'Operator',
        type: 'select',
        classes: ['endOperator'],
        select: operators,
        value: ctx.end_rule[1] !== null ? ctx.end_rule[1] : 2,
        help: 'How to compare the variable.',
        pointer: { main: ctx.end_rule, index: 1 },
        'function': function() {
          const valEl = fieldsDiv.querySelector('.endValue');
          let uiEl = valEl ? valEl.closest('.UIelement') : null;
          if (uiEl) {
            if (Number(getval(this)) < 2) { uiEl.hidden = true; } else { uiEl.style.display = ''; uiEl.hidden = false; }
          }
        }
      });
      elements.push({
        label: 'Value',
        type: 'str',
        classes: ['endValue'],
        help: 'Compare the variable with this value. You can also use another variable.',
        datalist: vallist,
        pointer: { main: ctx.end_rule, index: 2 }
      });
    }

    elements.push({
      label: 'Inhibitor',
      type: 'inputlist',
      help: 'Optional list of tags and/or stream names: if any match the stream, this push will not start.',
      input: {
        type: 'str',
        datalist: [],
        validate: [function(val) {
          if (!val) return false;
          if (val[0] === '#') return false;
          const base = val.split('+')[0];
          if (base in mist.data.streams) return false;
          return { msg: "'" + base + "' is not a stream name.", classes: ['orange'], 'break': false };
        }]
      },
      pointer: { main: ctx, index: 'inhibit' }
    });

    const scheduleForm = formEngine.buildUI(elements);
    fieldsDiv.appendChild(scheduleForm);

    if (ctx._scheduleMode === 'timed') {
      const startInput = scheduleForm.querySelector('input.field[name="scheduletime"]');
      const endInput = scheduleForm.querySelector('input.field[name="completetime"]');
      const startHints = buildTimeReference(startInput);
      const endHints = buildTimeReference(endInput);
      if (startHints && startInput) {
        const startUi = startInput.closest('.UIelement');
        if (startUi) startUi.after(startHints);
      }
      if (endHints && endInput) {
        const endUi = endInput.closest('.UIelement');
        if (endUi) endUi.after(endHints);
      }
    }

    if (ctx._scheduleMode === 'variable' && conditionVarHints) {
      const varFieldHints = buildVarReference(conditionVarHints.allHints, {
        label: 'Variable hints',
        inputSelector: 'input.startVariable, input.endVariable',
        fallbackSelector: 'input.startVariable, input.endVariable',
        insertToken: function(v) {
          return String(v.name || '').replace(/^\$/, '');
        }
      });
      if (varFieldHints) {
        const startVarInput = scheduleForm.querySelector('input.field.startVariable');
        const startVarUi = startVarInput ? startVarInput.closest('.UIelement') : null;
        if (startVarUi) startVarUi.after(varFieldHints);
      }

      const valueFieldHints = buildVarReference(conditionVarHints.allHints, {
        label: 'Value variable hints',
        inputSelector: 'input.startValue, input.endValue',
        fallbackSelector: 'input.startValue, input.endValue',
        insertToken: function(v) {
          return v.name;
        }
      });
      if (valueFieldHints) {
        const endValueInput = scheduleForm.querySelector('input.field.endValue');
        const endValueUi = endValueInput ? endValueInput.closest('.UIelement') : null;
        if (endValueUi) endValueUi.after(valueFieldHints);
      }
    }

    const startOp = fieldsDiv.querySelector('.startOperator');
    if (startOp) startOp.dispatchEvent(new Event('change', {bubbles: true}));
    const endOp = fieldsDiv.querySelector('.endOperator');
    if (endOp) endOp.dispatchEvent(new Event('change', {bubbles: true}));
  }

  buildScheduleFields();
  callbacks.markValid(true);
  wrapStepContent($container, 'push-step-shell--schedule');
}

function buildReviewStep($container, ctx, callbacks) {
  const ps = pushScenarios;
  const scenario = ps.SCENARIOS[ctx._scenario];
  const review = el('div', {class: 'push-review'});

  const section = function(t, s) { return uiHelpers.reviewSection(t, s, callbacks); };

  function row(label, value) {
    const tr = el('tr');
    tr.appendChild(el('td', null, label));
    tr.appendChild(el('td', null, value || '(not set)'));
    return tr;
  }

  review.appendChild(section('Type', 'scenario'));
  const scenarioGrid = el('div', {class: 'cam-info-grid nolay'});
  const scenarioTable = el('table');
  scenarioTable.appendChild(row('Type', scenario ? scenario.title : 'Unknown'));
  scenarioGrid.appendChild(scenarioTable);
  review.appendChild(scenarioGrid);

  review.appendChild(section('Stream & Target', 'target'));
  const targetDisplay = ctx.target || '';
  const displayTarget = assembleTarget(ctx);

  const targetGrid = el('div', {class: 'cam-info-grid nolay'});
  const targetTable = el('table');
  targetTable.appendChild(row('Stream', ctx.stream));
  targetTable.appendChild(row('Target', displayTarget));
  if (ctx['x-LSP-notes']) targetTable.appendChild(row('Notes', ctx['x-LSP-notes']));
  targetGrid.appendChild(targetTable);
  review.appendChild(targetGrid);

  let hasParams = false;
  if (ctx.params) {
    for (const pk in ctx.params) {
      if (ctx.params[pk] !== null && ctx.params[pk] !== false && ctx.params[pk] !== undefined && ctx.params[pk] !== '') {
        hasParams = true;
        break;
      }
    }
  }
  if (!hasParams && ctx.custom_url_params && ctx.custom_url_params.length) hasParams = true;
  if (hasParams) {
    review.appendChild(section('Parameters', 'params'));
    const paramsTable = el('table');
    for (const k in ctx.params) {
      if (ctx.params[k] !== null && ctx.params[k] !== false && ctx.params[k] !== undefined && ctx.params[k] !== '') {
        paramsTable.appendChild(row(k, String(ctx.params[k])));
      }
    }
    if (ctx.custom_url_params && ctx.custom_url_params.length) {
      for (let c = 0; c < ctx.custom_url_params.length; c++) {
        paramsTable.appendChild(row('Custom', ctx.custom_url_params[c]));
      }
    }
    const paramsGrid = el('div', {class: 'cam-info-grid nolay'});
    paramsGrid.appendChild(paramsTable);
    review.appendChild(paramsGrid);
  }

  review.appendChild(section('Schedule', 'schedule'));
  const scheduleInfo = [];
  if (ctx._scheduleMode === 'now') {
    scheduleInfo.push(row('Mode', 'Start immediately'));
  } else if (ctx._scheduleMode === 'always') {
    scheduleInfo.push(row('Mode', 'Always active'));
    scheduleInfo.push(row('Enabled', ctx.enabled !== false ? 'Yes' : 'No'));
  } else if (ctx._scheduleMode === 'timed') {
    scheduleInfo.push(row('Mode', 'Scheduled'));
    if (ctx.scheduletime) {
      scheduleInfo.push(row('Start', new Date(ctx.scheduletime * 1e3).toLocaleString()));
    }
    if (ctx.completetime) {
      scheduleInfo.push(row('End', new Date(ctx.completetime * 1e3).toLocaleString()));
    }
    scheduleInfo.push(row('Enabled', ctx.enabled !== false ? 'Yes' : 'No'));
  } else if (ctx._scheduleMode === 'variable') {
    scheduleInfo.push(row('Mode', 'Conditional (variable-based)'));
    if (ctx.start_rule && ctx.start_rule[0]) {
      scheduleInfo.push(row('Start when', formatRule(ctx.start_rule)));
    }
    if (ctx.end_rule && ctx.end_rule[0]) {
      scheduleInfo.push(row('Stop when', formatRule(ctx.end_rule)));
    }
    scheduleInfo.push(row('Enabled', ctx.enabled !== false ? 'Yes' : 'No'));
  }
  if (ctx.inhibit && ctx.inhibit.length) {
    scheduleInfo.push(row('Inhibitors', ctx.inhibit.join(', ')));
  }
  const schedGrid = el('div', {class: 'cam-info-grid nolay'});
  const schedTable = el('table');
  for (let si = 0; si < scheduleInfo.length; si++) {
    schedTable.appendChild(scheduleInfo[si]);
  }
  schedGrid.appendChild(schedTable);
  review.appendChild(schedGrid);

  if (ctx.stream && ctx.stream[0] !== '#' && ctx.stream.indexOf('+') < 0) {
    const base = ctx.stream.split('+')[0];
    if (mist.data.streams && !(base in mist.data.streams)) {
      let warn1 = el('div', {class: 'review-warning'});
      warn1.textContent = 'Warning: stream "' + base + '" is not currently configured. The push may fail if this stream does not exist.';
      review.appendChild(warn1);
    }
  }
  if (ctx._scheduleMode === 'now') {
    let warn2 = el('div', {class: 'review-warning'});
    warn2.textContent = 'This is a one-time push that starts immediately. It will not automatically restart if the stream goes offline.';
    review.appendChild(warn2);
  }

  $container.appendChild(review);
  callbacks.markValid(true);
  wrapStepContent($container, 'push-step-shell--review');
}

function formatRule(rule) {
  if (!rule || !rule[0]) return '';
  return pushScenarios.formatCondition(rule[0], rule[1], rule[2]);
}

function assembleTarget(ctx) {
  let target = ctx.target || '';
  const params = ctx.params || {};
  const full_list = ctx._fullParamList || {};
  const custom = ctx.custom_url_params || [];

  const parts = [];
  for (const k in params) {
    if (params[k] === null || params[k] === false || params[k] === undefined || params[k] === '') continue;
    if (!(k in full_list)) continue;
    parts.push(k + '=' + params[k]);
  }
  for (let i = 0; i < custom.length; i++) {
    if (custom[i]) parts.push(custom[i]);
  }
  if (parts.length) {
    target += '?' + parts.join('&');
  }
  return target;
}

function truncate(str, maxLen) {
  if (!str) return '';
  if (str.length <= maxLen) return str;
  return str.substring(0, maxLen - 3) + '...';
}

function wrapStepContent($container, className) {
  if (!$container) return null;
  const existing = $container.firstElementChild;
  if ($container.children.length === 1 && existing && existing.classList && existing.classList.contains('push-step-shell')) {
    if (className) existing.classList.add(className);
    return existing;
  }
  let classes = 'push-step-shell';
  if (className) classes += ' ' + className;
  const shell = el('div', {class: classes});
  while ($container.firstChild) {
    shell.appendChild($container.firstChild);
  }
  $container.appendChild(shell);
  return shell;
}

/* ===== Main Tab Handler ===== */

registerTab('Push Config', function(tab, other, prev, $main, $pageHeader) {
  $pageHeader.classList.add('page-header--dense');
  $main.classList.add('page-body--dense', 'page-body--flex-col', 'page-body--wizard', 'push-config-page-body', 'push-config-shell', 'slab-shell');
  apiClient.send(function() {
    const ps = pushScenarios;
    ps.invalidateCache();

    let editing = false;
    let editId = null;

    let prefillStream = '';
    if (other) {
      const parts = other.split('_');
      if (parts[0] === 'auto' && parts.length >= 2) {
        editing = true;
        editId = parts.slice(1).join('_');
      } else {
        prefillStream = other;
      }
    }

    let h2 = $pageHeader.querySelector('h2');
    if (h2) h2.textContent = editing ? 'Edit Push' : 'New Push';

    let ctx = {
      stream: prefillStream || '',
      target: '',
      params: {},
      custom_url_params: [],
      'x-LSP-notes': '',
      enabled: true,
      scheduletime: null,
      completetime: null,
      start_rule: [null, null, null],
      end_rule: [null, null, null],
      inhibit: [],
      _scenario: null,
      _scheduleMode: 'now',
      _editing: editing,
      _editId: editId,
      _connectorMatch: null,
      _platformKey: null,
      _fullParamList: {}
    };

    let allthestreams = [];

    function buildWizard() {
      const steps = [
        {
          id: 'scenario', label: 'Type', icon: 'send',
          build: function($c, cx, cb) { return buildScenarioStep($c, cx, cb); }
        },
        {
          id: 'target', label: 'Target', icon: 'target',
          build: function($c, cx, cb) { return buildTargetStep($c, cx, cb, allthestreams); }
        },
        {
          id: 'params', label: 'Parameters', icon: 'sliders', optional: true,
          build: function($c, cx, cb) { return buildParamsStep($c, cx, cb); }
        },
        {
          id: 'schedule', label: 'Schedule', icon: 'clock',
          build: function($c, cx, cb) { return buildScheduleStep($c, cx, cb); }
        },
        {
          id: 'review', label: 'Review', icon: 'check',
          build: function($c, cx, cb) { return buildReviewStep($c, cx, cb); }
        }
      ];

      const uriLabel = el('label', null, 'Target:');
      const uriInput = el('input', {
        class: 'wizard-uri-input',
        type: 'text',
        placeholder: 'Target URI will appear here as you configure the push',
        spellcheck: 'false',
        autocomplete: 'off'
      });
      _uriHighlight = el('code', {class: 'wizard-uri-highlight'});
      const uriDisplay = el('div', {class: 'wizard-uri-display'}, [uriInput, _uriHighlight]);
      if (ctx.target) uriInput.value = assembleTarget(ctx);
      var _uriFromFooter = false;
      uriInput.addEventListener('input', function() {
        _uriFromFooter = true;
        ctx.target = uriInput.value;
        updateTargetPreview();
        _uriFromFooter = false;
      });
      ctx._syncUriFooter = function() {
        if (!_uriFromFooter) {
          uriInput.value = assembleTarget(ctx);
        }
        updateTargetPreview();
      };
      if (ctx.target) updateTargetPreview();
      const uriFooter = el('div', {class: 'wizard-uri-footer'}, [uriLabel, uriDisplay]);

      mountWizard({
        steps: steps,
        context: ctx,
        allowJump: true,
        markAllCompleted: editing,
        mount: $main,
        footer: uriFooter,
        onStepBuilt: function(cx) {
          uriInput.value = assembleTarget(cx);
        },
        onComplete: function() {
          performSave(ctx);
        },
        onCancel: function() {
          navto('Push');
        }
      });
    }

    ps.loadStreamList(function(streams) {
      allthestreams = streams;

      if (editing && editId) {
        apiClient.send(function(d) {
          if (editId in d.auto_push) {
            populateFromAutoPush(ctx, d.auto_push[editId], ps);
          }
          buildWizard();
        }, { push_auto_list: 1 });
      } else {
        if (other === 'auto') {
          ctx._scheduleMode = 'always';
        }
        buildWizard();
      }
    });

  }, { capabilities: true, variable_list: true, external_writer_list: true });
});

function populateFromAutoPush(ctx, push, ps) {
  ctx.stream = push.stream || '';
  ctx.enabled = true;

  if (ctx.stream.indexOf('\u{1F4A4}deactivated\u{1F4A4}_') === 0) {
    ctx.enabled = false;
    ctx.stream = ctx.stream.slice(16);
  }

  const targetParts = (push.target || '').split('?');
  ctx.target = targetParts[0];
  if (targetParts.length > 1) {
    const paramStr = targetParts.slice(1).join('?');
    const pairs = paramStr.split('&');
    for (let i = 0; i < pairs.length; i++) {
      const kv = pairs[i].split('=');
      let key = kv.shift();
      ctx.params[key] = kv.join('=');
    }
  }

  ctx['x-LSP-notes'] = push['x-LSP-notes'] || '';
  ctx.inhibit = push.inhibit || [];

  if (push.start_rule || push.end_rule) {
    ctx._scheduleMode = 'variable';
    ctx.start_rule = push.start_rule || [null, null, null];
    ctx.end_rule = push.end_rule || [null, null, null];
  } else if (push.scheduletime || push.completetime) {
    ctx._scheduleMode = 'timed';
    ctx.scheduletime = push.scheduletime || null;
    ctx.completetime = push.completetime || null;
  } else {
    ctx._scheduleMode = 'always';
  }

  ctx._scenario = null;
  for (const id in ps.SCENARIOS) {
    const s = ps.SCENARIOS[id];
    if (s.isPlatform && s.targetPrefix && ctx.target.indexOf(s.targetPrefix) === 0) {
      ctx._scenario = id;
      ctx._platformKey = ctx.target.slice(s.targetPrefix.length);
      break;
    }
  }
  if (!ctx._scenario) {
    ctx._scenario = ps.detectScenario(ctx.target) || 'push_other';
  }

  if (ctx._scenario === 'record_dvr') {
    const dvrKeys = ['m3u8', 'split', 'maxEntries', 'targetAge', 'append', 'noendlist'];
    const dvrParams = [];
    for (let dk = 0; dk < dvrKeys.length; dk++) {
      if (ctx.params[dvrKeys[dk]] !== undefined && ctx.params[dvrKeys[dk]] !== null && ctx.params[dvrKeys[dk]] !== '') {
        dvrParams.push(dvrKeys[dk] + '=' + ctx.params[dvrKeys[dk]]);
        delete ctx.params[dvrKeys[dk]];
      }
    }
    if (dvrParams.length) {
      ctx.target = ctx.target + '?' + dvrParams.join('&');
    }
  }

  ctx._connectorMatch = ps.getMatchingConnector(ctx.target);
}

function performSave(ctx) {
  const target = assembleTarget(ctx);
  const isAuto = ctx._scheduleMode !== 'now';

  if (isAuto) {
    let saveas = {
      stream: ctx.stream,
      target: target
    };

    if (ctx['x-LSP-notes']) saveas['x-LSP-notes'] = ctx['x-LSP-notes'];

    if (ctx.enabled === false) {
      saveas.stream = '\u{1F4A4}deactivated\u{1F4A4}_' + saveas.stream;
    }

    if (ctx.inhibit && ctx.inhibit.length) {
      saveas.inhibit = ctx.inhibit;
    }

    if (ctx._scheduleMode === 'timed') {
      if (ctx.scheduletime) saveas.scheduletime = ctx.scheduletime;
      if (ctx.completetime) saveas.completetime = ctx.completetime;
    } else if (ctx._scheduleMode === 'variable') {
      if (ctx.start_rule && ctx.start_rule[0]) saveas.start_rule = ctx.start_rule;
      if (ctx.end_rule && ctx.end_rule[0]) saveas.end_rule = ctx.end_rule;
      saveas.scheduletime = 0;
      saveas.completetime = 0;
    }

    let obj = {};
    if (ctx._editId) {
      obj.push_auto_add = {};
      obj.push_auto_add[ctx._editId] = saveas;
    } else {
      obj.push_auto_add = saveas;
    }

    apiClient.send(function() {
      navto('Push');
    }, obj);
  } else {
    apiClient.send(function() {
      navto('Push');
    }, {
      push_start: {
        stream: ctx.stream,
        target: target
      }
    });
  }
}
