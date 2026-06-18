import { APP_NAME } from '@brand';
import { registerTab } from '../core/tab_registry.js';
import { AppShell } from '../core/appshell.js';
import { cardPicker } from '../components/card_picker.js';
import { classifyFields, disclosureForm } from '../components/disclosure_form.js';
import { inlineEditor } from '../components/inline_editor.js';
import { composerEditor, buildDesignerInline } from '../components/composer.js';
import { mountWizard } from '../components/wizard.js';
import { streamScenarios } from './stream_scenarios.js';
import { processTemplates } from './process_templates.js';
import { formEngine } from '../core/form_engine.js';
import { streamPresetHelpers, streamSave } from './stream_save.js';
import { uiHelpers } from '../core/ui_helpers.js';
import { stream } from './stream_utils.js';
import { apiClient } from '../core/api_client.js';
import { navto } from '../core/navigation.js';
import { mistHelpers } from '../core/mist_helpers.js';
import { streamHints } from './stream_hints.js';
import { el, getval, setval, validateFeedback, deepExtend } from '../core/dom_helpers.js';

const PROCESS_CATEGORIES = [
  { id: 'video',     title: 'Video Processing' },
  { id: 'audio',     title: 'Audio Processing' },
  { id: 'analytics', title: 'Analytics & AI' },
  { id: 'output',    title: 'Output' },
  { id: 'utility',   title: 'Utility' }
];

const PROCESS_CATEGORY_MAP = {
  'AV': 'video',
  'FFMPEG': 'video',
  'Livepeer': 'video',
  'Composer': 'video',
  'ONNX': 'analytics'
};

function getProcessCategory(procId) {
  if (PROCESS_CATEGORY_MAP[procId]) return PROCESS_CATEGORY_MAP[procId];
  const name = (procId || '').toLowerCase();
  if (name.indexOf('push') >= 0 || name.indexOf('record') >= 0) return 'output';
  if (name.indexOf('onnx') >= 0 || name.indexOf('detect') >= 0) return 'analytics';
  if (name.indexOf('audio') >= 0 || name.indexOf('mux') >= 0) return 'audio';
  if (name.indexOf('video') >= 0 || name.indexOf('transcode') >= 0 || name.indexOf('ffmpeg') >= 0) return 'video';
  return 'utility';
}

function getProcessHRN(procId) {
  const cap = mist.data.capabilities.processes[procId];
  if (!cap) return procId;
  return cap.hrn || cap.name || procId;
}

function isPushSource(source) {
  return source && source.indexOf('push://') === 0;
}

function truncateText(value, maxLen) {
  if (!value || value.length <= maxLen) return value || '';
  return value.slice(0, maxLen - 1) + '…';
}

function getSourceSummary(ctx) {
  let summary = ctx.name || '';
  if (ctx.source) {
    summary += (summary ? ' - ' : '') + truncateText(ctx.source, 36);
  }
  return summary;
}

function buildAssemblyPreview(callbacks, ctx) {
  const completed = callbacks.getCompletedSummaries();
  if (!completed.length) return null;

  const ss = streamScenarios;
  const bar = el('div', {class: 'assembly-preview'});

  for (let i = 0; i < completed.length; i++) {
    const step = completed[i];
    const chip = el('div', {class: 'assembly-chip completed'});
    if (step.icon) chip.setAttribute('data-icon', step.icon);

    const chipLabel = el('span', {class: 'assembly-chip-label'}, step.label);
    let chipValue = el('span', {class: 'assembly-chip-value'});

    if (step.id === 'scenario') {
      const scenario = ctx._scenarioChosen ? ss.SCENARIOS[ctx._scenarioChosen] : null;
      chipValue.textContent = scenario ? scenario.title : '';
    } else if (step.id === 'source') {
      chipValue.textContent = getSourceSummary(ctx) || '(not set)';
    } else if (step.id === 'settings') {
      chipValue.textContent = step.summary || 'defaults';
    } else if (step.id === 'processes') {
      const n = (ctx.processes || []).length;
      chipValue.textContent = n ? n + ' process' + (n !== 1 ? 'es' : '') : 'none';
    } else if (step.id === 'compose') {
      chipValue.textContent = step.summary || '';
    } else if (step.id === 'access') {
      chipValue.textContent = step.summary || '';
    } else {
      chipValue.textContent = step.summary || '';
    }

    chip.appendChild(chipLabel);
    chip.appendChild(chipValue);
    bar.appendChild(chip);
  }

  return bar;
}

/* ===== Step Builders ===== */

function buildSourceCards(ss, ctx, callbacks) {
  const cards = [];
  for (const id in ss.SCENARIOS) {
    const scenario = ss.SCENARIOS[id];
    cards.push({
      id: id,
      group: scenario.group,
      title: scenario.title,
      desc: scenario.desc,
      icon: scenario.icon,
      available: scenario.available(),
      selected: id === ctx._scenarioChosen,
      promo: scenario.promo,
      promoText: scenario.promoText,
      promoLink: scenario.promoLink,
      onClick: function(cardId) {
        const previousId = ctx._scenarioChosen;
        const changed = previousId !== cardId;
        ctx._scenarioChosen = cardId;
        const sc = ss.SCENARIOS[cardId];
        if (changed) {
          ctx._sourceSuffix = null;
        }
        if (sc.defaultSource && (changed || !ctx.source)) {
          ctx.source = sc.defaultSource;
        }
        callbacks.setStepSummary(sc.title);
        callbacks.goTo('source');
      }
    });
  }
  return cardPicker({
    intro: 'Choose how this stream will receive its media data.',
    groups: ss.GROUPS,
    cards: cards
  });
}

function buildScenarioStep($container, ctx, callbacks) {
  const ss = streamScenarios;

  if (!ctx._scenarioChosen) {
    const detectedId = ss.detectScenario(ctx.source);
    if (detectedId) {
      ctx._scenarioChosen = detectedId;
    }
  }

  if (ctx._scenarioChosen) {
    const cur = ss.SCENARIOS[ctx._scenarioChosen];
    if (cur) callbacks.setStepSummary(cur.title);
    callbacks.markValid(true);
  } else {
    callbacks.markValid(false);
  }

  $container.appendChild(buildSourceCards(ss, ctx, callbacks));
}

function buildSourceStep($container, ctx, callbacks) {
  const ss = streamScenarios;
  const preview = buildAssemblyPreview(callbacks, ctx);
  if (preview) $container.appendChild(preview);

  const scenario = ss.SCENARIOS[ctx._scenarioChosen] || null;
  if (!scenario) {
    $container.appendChild(el('p', {class: 'wizard-intro'}, 'Please select a stream type first.'));
    callbacks.markValid(false);
    return;
  }

  function updateSourceSummary() {
    callbacks.setStepSummary(getSourceSummary(ctx));
    if (ctx._syncUriFooter) ctx._syncUriFooter();
  }

  const elements = [];

  if (scenario && scenario.hint) {
    elements.push({
      type: 'help',
      classes: ['wizard-intro'],
      help: scenario.hint
    });
  }

  var _wcCallout = null;
  function updateWildcardCallout() {
    if (!_wcCallout) return;
    var n = ctx.name || ctx._originalName || 'streamname';
    _wcCallout.innerHTML = '<strong>Wildcard template</strong> - Push to <code>' + n + '+identifier</code> '
      + '(e.g.\u00a0<code>' + n + '+alice</code>, <code>' + n + '+cam2</code>) to create '
      + 'independent child streams that inherit this configuration. '
      + 'Each child has its own buffer and viewer sessions.';
  }

  elements.push({
    label: 'Stream name',
    type: 'str',
    validate: ['required', 'streamname'],
    pointer: { main: ctx, index: 'name' },
    help: 'Set the name this stream will be recognised by for players and/or stream pushing.',
    'function': function() { updateSourceSummary.call(this); updateWildcardCallout(); }
  });

  if (scenario && scenario.isCompose) {
    ctx.source = scenario.defaultSource;
  } else if (scenario && scenario.enumerate) {
    elements.push({
      label: scenario.sourceLabel || 'Source',
      type: 'str',
      pointer: { main: ctx, index: 'source' },
      help: scenario.sourceHelp || '',
      placeholder: 'Select a discovered source or type a custom value',
      'function': updateSourceSummary
    });

    const inputCap = ss.getInput(scenario.inputName);
    if (inputCap && inputCap.enumerate) {
      const enumNote = el('em', null, 'Discovering sources...');
      elements.push(enumNote);

      setTimeout(function() {
        apiClient.send(function(d) {
          enumNote.remove();
          if (d.enumerate && d.enumerate.length) {
            var inputEl = $container.querySelector('[name=source]');
            if (!inputEl) return;
            var dlId = 'enum-' + scenario.inputName + '-' + Date.now();
            var datalist = document.createElement('datalist');
            datalist.id = dlId;
            for (var i = 0; i < d.enumerate.length; i++) {
              var src = d.enumerate[i];
              var val = typeof src === 'string' ? src : src.url || src.source || src;
              var label = typeof src === 'string' ? src : src.name || src.url || src;
              var opt = document.createElement('option');
              opt.value = val;
              if (label !== val) opt.textContent = label;
              datalist.appendChild(opt);
            }
            inputEl.parentNode.appendChild(datalist);
            inputEl.setAttribute('list', dlId);
            if (ctx.source) setval(inputEl, ctx.source);
            updateSourceSummary();
          }
        }, { enumerate: scenario.inputName });
      }, 100);
    }
  } else if (scenario && scenario.useBrowse) {
    const filetypes = ss.getAllFiletypes();
    elements.push({
      label: scenario.sourceLabel || 'Source',
      type: 'browse',
      filetypes: filetypes,
      pointer: { main: ctx, index: 'source' },
      help: scenario.sourceHelp || '',
      placeholder: scenario.placeholder || '',
      'function': updateSourceSummary
    });
  } else if (scenario && scenario.lockedPrefix) {
    const prefix = scenario.lockedPrefix;
    ctx._lockedPrefix = prefix;
    if (ctx._sourceSuffix === undefined || ctx._sourceSuffix === null) {
      ctx._sourceSuffix = '';
      if (ctx.source && ctx.source.indexOf(prefix) === 0) {
        ctx._sourceSuffix = ctx.source.slice(prefix.length);
      }
    }

    const srcPrefix = el('div', {class: 'push-platform-prefix'});
    srcPrefix.appendChild(el('span', {class: 'push-platform-label'}, scenario.title));
    srcPrefix.appendChild(el('code', null, prefix));
    elements.push(srcPrefix);

    elements.push({
      label: scenario.sourceLabel || 'Address',
      type: 'str',
      help: scenario.sourceHelp || '',
      placeholder: scenario.placeholder || '',
      pointer: { main: ctx, index: '_sourceSuffix' },
      'function': function() {
        var val = getval(this) || '';
        if (val.indexOf(prefix) === 0) {
          val = val.slice(prefix.length);
          setval(this, val);
        }
        ctx._sourceSuffix = val;
        ctx.source = prefix + ctx._sourceSuffix;
        updateSourceSummary();
      }
    });

    ctx.source = prefix + (ctx._sourceSuffix || '');
  } else if (scenario) {
    elements.push({
      label: scenario.sourceLabel || 'Source',
      type: 'str',
      pointer: { main: ctx, index: 'source' },
      help: scenario.sourceHelp || '',
      placeholder: scenario.placeholder || '',
      'function': updateSourceSummary
    });
  } else {
    const filetypes = ss.getAllFiletypes();
    elements.push({
      label: 'Source',
      type: 'browse',
      filetypes: filetypes,
      pointer: { main: ctx, index: 'source' },
      help: 'Enter the stream source URI or browse for a file.',
      'function': updateSourceSummary
    });
  }

  if (scenario && scenario.showAlwaysOn !== false) {
    if (scenario.alwaysOnDefault && !ctx.always_on && !ctx._editing) {
      ctx.always_on = true;
    }
    elements.push({
      label: 'Always on',
      type: 'checkbox',
      pointer: { main: ctx, index: 'always_on' },
      help: 'Keep this stream active even when there are no viewers. '+APP_NAME+' only attempts to pull in streams when a viewer is watching. Enable this to pull the stream regardless of viewers.'
    });
  }

  const formEl = formEngine.buildUI(elements);
  $container.appendChild(formEl);

  if (isPushSource(ctx.source)) {
    _wcCallout = el('div', {class: 'wizard-info-callout'});
    updateWildcardCallout();
    const nameField = formEl.querySelector('input.field[name="name"]');
    const nameUiEl = nameField ? nameField.closest('.UIelement') : null;
    if (nameUiEl) {
      nameUiEl.after(_wcCallout);
    } else {
      formEl.insertBefore(_wcCallout, formEl.children[1] || null);
    }
  }
  updateSourceSummary();
  callbacks.markValid(true);

  return {
    validate: function() {
      let valid = true;
      const settings = $container.querySelectorAll('.isSetting');
      Array.from(settings).forEach(function(s) {
        const v = validateFeedback(s);
        if (v !== true) valid = false;
      });
      return valid;
    }
  };
}

let _dynamicCapaTimer = null;

function buildSettingsForm($container, input, ctx, callbacks) {
  callbacks.setStepSummary(input.name || '');

  const intro = el('p', {class: 'wizard-intro'}, "Configure input-specific options. Common settings are shown first; expand 'Advanced' for tuning options.");
  $container.appendChild(intro);

  const build = formEngine.convertBuildOptions(input, ctx);
  var filtered = build.filter(function(item) {
    return !(item && item.type === 'help' && item.help && item.help === input.desc);
  });
  const classified = classifyFields(filtered);

  if (!classified.basic.length && !classified.advanced.length) {
    const noSettings = el('p', {class: 'wizard-intro'}, 'No additional settings for this input type.');
    $container.appendChild(noSettings);
    return;
  }

  $container.appendChild(disclosureForm({
    basic: classified.basic,
    advanced: classified.advanced,
    startExpanded: AppShell.getMode() === 'advanced'
  }));
}

function buildSettingsStep($container, ctx, callbacks) {
  const preview = buildAssemblyPreview(callbacks, ctx);
  if (preview) $container.appendChild(preview);

  let type = null;
  if (ctx.source) {
    for (const i in mist.data.capabilities.inputs) {
      if (typeof mist.data.capabilities.inputs[i].source_match === 'undefined') continue;
      if (mistHelpers.inputMatch(mist.data.capabilities.inputs[i].source_match, ctx.source)) {
        type = i;
        break;
      }
    }
  }

  if (!type) {
    const noInput = el('p', {class: 'wizard-intro'}, 'No input-specific settings available. Set a source first.');
    $container.appendChild(noInput);
    callbacks.setStepSummary('');
    return;
  }

  let input = mist.data.capabilities.inputs[type];

  if (input.dynamic_capa && ctx.source) {
    if (!input._dynamicCapaCache) input._dynamicCapaCache = {};

    if (input._dynamicCapaCache[ctx.source]) {
      buildSettingsForm($container, input._dynamicCapaCache[ctx.source], ctx, callbacks);
      return;
    }

    let loading = el('p', {class: 'wizard-intro'}, 'Loading device options\u2026');
    $container.appendChild(loading);
    callbacks.setStepSummary(input.name || type);

    if (_dynamicCapaTimer) clearTimeout(_dynamicCapaTimer);
    _dynamicCapaTimer = setTimeout(function() {
      _dynamicCapaTimer = null;
      apiClient.send(function(d) {
        if (d && d.capabilities) {
          input._dynamicCapaCache[ctx.source] = d.capabilities;
          loading.remove();
          buildSettingsForm($container, d.capabilities, ctx, callbacks);
        } else {
          loading.textContent = 'Could not load device options. Using defaults.';
          buildSettingsForm($container, input, ctx, callbacks);
        }
      }, { capabilities: ctx.source });
    }, 1000);
    return;
  }

  buildSettingsForm($container, input, ctx, callbacks);
}

function buildProcessesStep($container, ctx, callbacks) {
  const preview = buildAssemblyPreview(callbacks, ctx);
  if (preview) $container.appendChild(preview);

  const ss = streamScenarios;
  if (!ctx.processes) ctx.processes = [];

  const caps = mist.data.capabilities.processes || {};
  let hasProcesses = false;
  for (const k in caps) { hasProcesses = true; break; }

  if (!hasProcesses) {
    const noProc = el('p', {class: 'wizard-intro'}, 'No stream processes available on this server.');
    $container.appendChild(noProc);
    return;
  }

  const sourceType = ctx._scenarioChosen ? ss.SCENARIOS[ctx._scenarioChosen] : null;
  let introText = 'Add processing steps';
  if (sourceType) introText += ' to your ' + sourceType.title.toLowerCase() + ' stream';
  introText += ' - transcode, record, run AI analytics, or push to external platforms. This is optional; you can add processes later.';
  const introEl = el('p', {class: 'wizard-intro'}, introText);
  $container.appendChild(introEl);

  callbacks.setStepSummary(ctx.processes.length ? ctx.processes.length + ' process' + (ctx.processes.length !== 1 ? 'es' : '') : '');

  let pickerArea = el('div');
  const editorArea = el('div');
  $container.appendChild(editorArea);
  $container.appendChild(pickerArea);

  function showProcessPicker(addCallback) {
    pickerArea.innerHTML = '';

    const allGroups = [];
    const allCards = [];

    const templates = processTemplates ? processTemplates.getAvailableTemplates() : [];
    if (templates.length) {
      allGroups.push({ id: 'templates', title: 'Presets' });
      for (let t = 0; t < templates.length; t++) {
        (function(tmpl) {
          allCards.push({
            id: tmpl.template.id,
            group: 'templates',
            title: tmpl.template.title,
            desc: tmpl.template.desc,
            icon: tmpl.template.icon,
            available: true,
            onClick: function() {
              pickerArea.innerHTML = '';
              const items = processTemplates.applyTemplate(tmpl.template, tmpl.resolvedProcess);
              for (let m = 0; m < items.length; m++) {
                addCallback(items[m]);
              }
            }
          });
        })(templates[t]);
      }
    }

    for (let g = 0; g < PROCESS_CATEGORIES.length; g++) {
      allGroups.push(PROCESS_CATEGORIES[g]);
    }
    for (const procId in caps) {
      const cap = caps[procId];
      allCards.push({
        id: procId,
        group: getProcessCategory(procId),
        title: cap.hrn || cap.name || procId,
        desc: cap.desc || '',
        icon: 'zap',
        available: true,
        onClick: function(id) {
          pickerArea.innerHTML = '';
          const newProc = { process: id };
          addCallback(newProc);
        }
      });
    }

    pickerArea.appendChild(
      cardPicker({
        intro: 'Choose a preset for common configurations, or pick a process type to configure manually.',
        groups: allGroups,
        cards: allCards,
        searchable: true
      })
    );
  }

  const editor = inlineEditor({
    items: ctx.processes,
    emptyMessage: 'No processes configured. Add one below.',
    addLabel: 'Add process',
    addIcon: 'plus',
    renderItem: function(item, index) {
      const procName = getProcessHRN(item.process);
      return {
        title: item['x-LSP-name'] || procName,
        subtitle: item['x-LSP-name'] ? procName : '',
        icon: 'zap'
      };
    },
    editItem: function(item, index, $area, editCallbacks) {
      const procId = item.process;
      const cap = caps[procId];
      if (!cap) {
        $area.appendChild(el('p', null, 'Unknown process type: ' + procId));
        return;
      }

      if (procId === 'Composer' && composerEditor) {
        composerEditor(item, cap, $area, editCallbacks);
        return;
      }

      const editCopy = deepExtend({}, item);
      const build = formEngine.convertBuildOptions(cap, editCopy);
      const classified = classifyFields(build);

      const form = disclosureForm({
        basic: [{
          label: 'Display name',
          type: 'str',
          pointer: { main: editCopy, index: 'x-LSP-name' },
          help: 'Optional friendly name for this process.'
        }].concat(classified.basic),
        advanced: classified.advanced,
        startExpanded: AppShell.getMode() === 'advanced'
      });

      $area.appendChild(form);

      const buttons = el('div', {class: 'inline-editor-item-buttons'});
      const cancelBtn = el('button', {class: 'cancel'}, 'Cancel');
      cancelBtn.addEventListener('click', function() {
        editCallbacks.cancel();
      });
      const saveBtn = el('button', {class: 'save'}, 'Save process');
      saveBtn.addEventListener('click', function() {
        if (!formEngine.collectFormValues(form)) return;
        editCallbacks.save(editCopy);
        updateSummary();
      });
      buttons.appendChild(cancelBtn);
      buttons.appendChild(saveBtn);
      $area.appendChild(buttons);
    },
    onAdd: function(addCallback) {
      showProcessPicker(addCallback);
    },
    onRemove: function() {
      updateSummary();
    },
    onReorder: function() {
      updateSummary();
    },
    onChange: function(items) {
      ctx.processes = items;
    }
  });

  editorArea.appendChild(editor.$el);

  function updateSummary() {
    callbacks.setStepSummary(
      ctx.processes.length ? ctx.processes.length + ' process' + (ctx.processes.length !== 1 ? 'es' : '') : ''
    );
  }
}

function buildAccessStep($container, ctx, callbacks) {
  const preview = buildAssemblyPreview(callbacks, ctx);
  if (preview) $container.appendChild(preview);

  const helpers = streamPresetHelpers;
  if (!ctx.streamkeys) ctx.streamkeys = [];
  ctx.stream_input_preset = helpers.detect(ctx);

  if (!isPushSource(ctx.source)) {
    const naMsg = el('p', {class: 'wizard-intro'}, 'This step configures stream-name/key rules for push:// ingest sources only. This stream uses a direct source, so those key rules do not apply. You can still enforce access with Automations (for example STREAM_PUSH/CONN_OPEN/USER_NEW), and for SRT-style ingest use protocol-level settings such as passphrase and network ACLs.');
    $container.appendChild(naMsg);
    callbacks.setStepSummary('Use automations/protocol auth');
    return;
  }

  const introEl = el('p', {class: 'wizard-intro'}, 'Control who can push to this stream using push:// name/key matching. You can also manage stream keys from the Stream Keys page, or use automations (triggers) for dynamic access control.');
  $container.appendChild(introEl);

  let presetHelp = el('div', {class: 'description'});
  presetHelp.textContent = helpers.presetDescriptions[ctx.stream_input_preset] || '';

  callbacks.setStepSummary(ctx.stream_input_preset.replace(/_/g, ' '));

  const generateBtn = el('button', null, 'Generate');
  generateBtn.addEventListener('click', function() {
    let inputEl = this.closest('.field_container').querySelector('input');
    inputEl.value = helpers.randomKey(32);
    inputEl.dispatchEvent(new Event('change', {bubbles: true}));
  });

  const elements = [
    {
      label: 'Input access preset',
      type: 'select',
      select: [
        ['name_only', 'Stream name only'],
        ['name_or_key', 'Stream name or key'],
        ['key_only', 'Key only (strict)']
      ],
      pointer: { main: ctx, index: 'stream_input_preset' },
      'function': function(e) {
        const args = Array.prototype.slice.call(arguments, 1);
        if (args.indexOf('preset_sync') > -1) return;
        helpers.apply(ctx, getval(this), $container);
        syncPresetUI();
        presetHelp.textContent = helpers.presetDescriptions[getval(this)] || '';
        callbacks.setStepSummary(getval(this).replace(/_/g, ' '));
      }
    },
    presetHelp,
    {
      label: 'Require stream key',
      type: 'checkbox',
      pointer: { main: ctx, index: 'streamkey_only' },
      'function': function() {
        syncPresetUI();
      }
    },
    {
      label: 'Stream keys',
      type: 'inputlist',
      pointer: { main: ctx, index: 'streamkeys' },
      'function': function() {
        syncPresetUI();
      },
      input: {
        type: 'str',
        clipboard: true,
        maxlength: 256,
        validate: [function(val) {
          if (mist.data.streamkeys && (val in mist.data.streamkeys) &&
              (mist.data.streamkeys[val] !== ctx._originalName)) {
            return { msg: 'Already in use by another stream', classes: ['red'] };
          }
        }],
        unit: generateBtn
      }
    }
  ];

  $container.appendChild(formEngine.buildUI(elements));

  function syncPresetUI() {
    ctx.stream_input_preset = helpers.detect(ctx);
    const presetEl = $container.querySelector('[name=stream_input_preset]');
    if (presetEl && getval(presetEl) !== ctx.stream_input_preset) {
      setval(presetEl, ctx.stream_input_preset, ['preset_sync']);
    }
    presetHelp.textContent = helpers.presetDescriptions[ctx.stream_input_preset] || '';
  }
}

function buildComposeStep($container, ctx, callbacks) {
  const preview = buildAssemblyPreview(callbacks, ctx);
  if (preview) $container.appendChild(preview);

  if (!ctx._composeData) {
    var extracted = null;
    if (ctx.processes && typeof ctx.processes === 'object' && !Array.isArray(ctx.processes)) {
      for (var key in ctx.processes) {
        var proc = ctx.processes[key];
        if (proc && proc.process && proc.process.toLowerCase().indexOf('composer') >= 0) {
          extracted = {
            sources: proc.sources || [],
            resolution: proc.resolution || '1920x1080',
            layout: proc.layout || 'equal'
          };
          if (proc['x-LSP-name']) extracted['x-LSP-name'] = proc['x-LSP-name'];
          break;
        }
      }
    }
    ctx._composeData = extracted || { sources: [], resolution: '1920x1080', layout: 'equal' };
  }

  var composerCap = null;
  if (mist.data.capabilities && mist.data.capabilities.processes) {
    for (var pid in mist.data.capabilities.processes) {
      if (pid.toLowerCase().indexOf('composer') >= 0) {
        composerCap = mist.data.capabilities.processes[pid];
        break;
      }
    }
  }

  var introEl = el('p', {class: 'wizard-intro'}, 'Design your multiview layout. Add input streams, choose a grid mode, and arrange sources in the preview below.');
  $container.appendChild(introEl);

  var designer = buildDesignerInline(ctx._composeData, composerCap || {}, {
    onChange: function() { updateSummary(); }
  });
  $container.appendChild(designer.$el);

  function updateSummary() {
    var n = ctx._composeData.sources.length;
    var layoutNames = { equal: 'Grid', focussed: 'Focussed', none: 'Freestyle' };
    callbacks.setStepSummary(
      n + ' source' + (n !== 1 ? 's' : '') + ', ' +
      (ctx._composeData.resolution || '1920x1080') + ', ' +
      (layoutNames[ctx._composeData.layout] || ctx._composeData.layout)
    );
  }
  updateSummary();
  callbacks.markValid(true);

  return {
    teardown: function() { designer.destroy(); }
  };
}

function buildReviewStep($container, ctx, callbacks) {
  const ss = streamScenarios;
  const scenario = ctx._scenarioChosen ? ss.SCENARIOS[ctx._scenarioChosen] : null;

  const review = el('div');

  const introEl = el('p', {class: 'wizard-intro'}, "Review your configuration before saving. Click 'Edit' on any section to make changes.");
  review.appendChild(introEl);

  const section = function(t, s) { return uiHelpers.reviewSection(t, s, callbacks); };

  review.appendChild(section('Type', 'scenario'));
  if (scenario) {
    const typeLine = el('div', {class: 'review-line'});
    const typeIcon = el('span', {class: 'source-type-icon'});
    typeIcon.setAttribute('data-icon', scenario.icon);
    typeLine.appendChild(typeIcon);
    typeLine.appendChild(document.createTextNode(scenario.title));
    review.appendChild(typeLine);
  } else {
    review.appendChild(el('em', {class: 'text-muted'}, 'Unknown'));
  }

  review.appendChild(section('Source', 'source'));
  const sourceInfo = el('div', {class: 'cam-info-grid nolay'});

  const tbl = el('table');
  const tr1 = el('tr');
  tr1.appendChild(el('td', null, 'Name'));
  tr1.appendChild(el('td', null, ctx.name || '(not set)'));
  tbl.appendChild(tr1);

  const tr2 = el('tr');
  tr2.appendChild(el('td', null, 'Source'));
  tr2.appendChild(el('td', null, ctx.source || '(not set)'));
  tbl.appendChild(tr2);

  if (ctx.always_on) {
    const tr3 = el('tr');
    tr3.appendChild(el('td', null, 'Always on'));
    tr3.appendChild(el('td', null, 'Yes'));
    tbl.appendChild(tr3);
  }

  sourceInfo.appendChild(tbl);
  review.appendChild(sourceInfo);

  if (ctx._composeData) {
    review.appendChild(section('Compose Layout', 'compose'));
    var compData = ctx._composeData;
    var compInfo = el('div', {class: 'cam-info-grid nolay'});
    var compTbl = el('table');

    var trRes = el('tr');
    trRes.appendChild(el('td', null, 'Resolution'));
    trRes.appendChild(el('td', null, compData.resolution || '1920x1080'));
    compTbl.appendChild(trRes);

    var layoutNames = { equal: 'Standard grid', focussed: 'Focussed', none: 'Freestyle' };
    var trLayout = el('tr');
    trLayout.appendChild(el('td', null, 'Layout'));
    trLayout.appendChild(el('td', null, layoutNames[compData.layout] || compData.layout));
    compTbl.appendChild(trLayout);

    var trSrc = el('tr');
    trSrc.appendChild(el('td', null, 'Sources'));
    var srcNames = (compData.sources || []).map(function(s) { return s.stream || '(unnamed)'; });
    trSrc.appendChild(el('td', null, srcNames.length ? srcNames.join(', ') : 'None'));
    compTbl.appendChild(trSrc);

    compInfo.appendChild(compTbl);
    review.appendChild(compInfo);
  }

  review.appendChild(section('Processes', 'processes'));
  if (ctx.processes && ctx.processes.length) {
    for (let i = 0; i < ctx.processes.length; i++) {
      const proc = ctx.processes[i];
      const procType = getProcessHRN(proc.process);
      const displayName = proc['x-LSP-name'] || procType;
      const procLine = el('div', {class: 'review-line'});
      procLine.appendChild(el('span', null, (i + 1) + '. ' + displayName));
      if (proc['x-LSP-name']) {
        procLine.appendChild(el('span', {class: 'review-subline'}, '(' + procType + ')'));
      }
      review.appendChild(procLine);
    }
  } else {
    const noneEl = el('em', {class: 'text-muted'}, 'None');
    review.appendChild(noneEl);
  }

  if (isPushSource(ctx.source)) {
    review.appendChild(section('Access', 'access'));
    const preset = streamPresetHelpers.detect(ctx);
    let presetLine = el('div', {class: 'review-line'});
    presetLine.textContent = 'Preset: ' + preset.replace(/_/g, ' ');
    review.appendChild(presetLine);
    if (ctx.streamkeys && ctx.streamkeys.length) {
      let keysLine = el('div', {class: 'review-line review-subline'});
      keysLine.textContent = ctx.streamkeys.length + ' stream key' + (ctx.streamkeys.length !== 1 ? 's' : '') + ' configured';
      review.appendChild(keysLine);
    } else if (!ctx.streamkey_only) {
      let warnLine = el('div', {class: 'review-warning'});
      warnLine.textContent = 'No stream keys configured - anyone who knows the stream name can push to it.';
      review.appendChild(warnLine);
    }
  }

  $container.appendChild(review);
}

/* ===== Main Tab Handler ===== */

function streamConfigTab(tab, other, prev, $main, $pageHeader) {
  $main.classList.add('page-body--flex-col', 'page-body--wizard', 'stream-config-page-body', 'stream-config');
  apiClient.send(function() {
    const editing = other && other !== '';
    const streamname = editing ? other.split('+')[0] : '';
    let saveas;
    let streamHeader = null;

    if (!editing) {
      saveas = {};
    } else {
      if (streamname in mist.data.streams) {
        saveas = deepExtend({}, mist.data.streams[streamname]);
      } else {
        saveas = { name: streamname, source: 'push://' };
      }
    }

    if (!saveas.streamkeys) {
      saveas.streamkeys = streamHints.findStreamKeys(streamname) || [];
    }
    if (saveas.processes && !Array.isArray(saveas.processes) && typeof saveas.processes === 'object') {
      var arr = [];
      for (var k in saveas.processes) {
        var p = deepExtend({}, saveas.processes[k]);
        p._key = k;
        arr.push(p);
      }
      saveas.processes = arr;
    }
    if (!saveas.processes) saveas.processes = [];

    if (saveas.source && saveas.source.slice(0, 7) === 'push://') {
      if (saveas.source.match(/push:\/\/invalid,host/)) {
        saveas.streamkey_only = true;
        saveas.source = saveas.source.replace('push://invalid,host', 'push://');
      }
    }

    saveas._editing = editing;
    saveas._originalName = streamname;

    if (editing && stream && stream.header) {
      $pageHeader.innerHTML = '';
      streamHeader = stream.header(other, 'Edit', streamname);
      streamHeader.setAttribute("data-changed","no");
      $pageHeader.appendChild(streamHeader);
      const markChanged = function(ev) {
        if (!ev.isTrusted) { return; }
        streamHeader.setAttribute("data-changed","yes");
      };
      $main.addEventListener('change', markChanged);
      $main.addEventListener('input', markChanged);
    } else {
      let h2 = $pageHeader.querySelector('h2');
      if (h2) h2.textContent = 'New Stream';
    }

    function getScenario(ctx) {
      return ctx._scenarioChosen ? streamScenarios.SCENARIOS[ctx._scenarioChosen] : null;
    }

    const steps = [
      {
        id: 'scenario', label: 'Type', icon: 'send',
        build: function($c, ctx, cb) { return buildScenarioStep($c, ctx, cb); }
      },
      {
        id: 'source', label: 'Source', icon: 'target',
        build: function($c, ctx, cb) { return buildSourceStep($c, ctx, cb); }
      },
      {
        id: 'compose', label: 'Compose', icon: 'layout',
        skip: function(ctx) { return ctx._scenarioChosen !== 'compose'; },
        build: function($c, ctx, cb) { return buildComposeStep($c, ctx, cb); }
      },
      {
        id: 'settings', label: 'Settings', icon: 'sliders',
        build: function($c, ctx, cb) { return buildSettingsStep($c, ctx, cb); }
      },
      {
        id: 'processes', label: 'Processes', icon: 'zap', optional: true,
        build: function($c, ctx, cb) { return buildProcessesStep($c, ctx, cb); }
      },
      {
        id: 'access', label: 'Access', icon: 'key', optional: true,
        skip: function(ctx) { return ctx._scenarioChosen !== 'push'; },
        build: function($c, ctx, cb) { return buildAccessStep($c, ctx, cb); }
      },
      {
        id: 'review', label: 'Review', icon: 'check',
        build: function($c, ctx, cb) { return buildReviewStep($c, ctx, cb); }
      }
    ];

    const uriLabel = el('label', null, 'Source:');
    const uriInput = el('input', {
      class: 'wizard-uri-input',
      type: 'text',
      placeholder: 'Source URI will appear here as you configure the stream',
      spellcheck: 'false',
      autocomplete: 'off'
    });
    if (saveas.source) uriInput.value = saveas.source;
    var _uriFromFooter = false;
    uriInput.addEventListener('input', function() {
      _uriFromFooter = true;
      saveas.source = uriInput.value;
      // sync back to wizard fields: parse prefix/suffix for lockedPrefix scenarios
      if (saveas._lockedPrefix) {
        var src = uriInput.value;
        var pfx = saveas._lockedPrefix;
        if (src.indexOf(pfx) === 0) {
          saveas._sourceSuffix = src.slice(pfx.length);
        } else {
          saveas._sourceSuffix = src;
        }
        var suffixField = document.querySelector('.wizard-step-content input.field[name="_sourceSuffix"]');
        if (suffixField) setval(suffixField, saveas._sourceSuffix);
      } else {
        var sourceField = document.querySelector('.wizard-step-content input.field[name="source"]');
        if (sourceField) setval(sourceField, saveas.source);
      }
      _uriFromFooter = false;
    });
    saveas._syncUriFooter = function() {
      if (!_uriFromFooter) {
        uriInput.value = saveas.source || '';
      }
    };
    const uriFooter = el('div', {class: 'wizard-uri-footer'}, [uriLabel, uriInput]);

    mountWizard({
      steps: steps,
      context: saveas,
      allowJump: true,
      markAllCompleted: editing,
      mount: $main,
      footer: uriFooter,
      onStepBuilt: function(ctx) {
        uriInput.value = ctx.source || '';
      },
      onComplete: function(ctx) {
        if (streamHeader) {
          streamHeader.setAttribute("data-changed","no");
        }
        performSave(ctx, other, 'Streams');
      },
      onCancel: function() {
        navto('Streams');
      }
    });

  }, { capabilities: true, streamkeys: true });
}

registerTab('Edit', streamConfigTab);
registerTab('Stream Config', function(tab, other, prev, $main, $pageHeader) {
  streamConfigTab('Edit', other, prev, $main, $pageHeader);
});

function performSave(saveas, other, targetTab) {
  const cleanSaveas = deepExtend({}, saveas);
  delete cleanSaveas._editing;
  delete cleanSaveas._originalName;
  delete cleanSaveas._scenarioChosen;
  delete cleanSaveas._sourceSuffix;
  delete cleanSaveas._lockedPrefix;
  delete cleanSaveas._syncUriFooter;
  delete cleanSaveas.stream_input_preset;

  if (Array.isArray(cleanSaveas.processes)) {
    var procObj = {};
    var counters = {};
    for (var i = 0; i < cleanSaveas.processes.length; i++) {
      var p = deepExtend({}, cleanSaveas.processes[i]);
      var key = p._key;
      delete p._key;
      if (!key) {
        var base = p.process || 'Proc';
        if (!counters[base]) counters[base] = 0;
        key = base + counters[base]++;
      }
      procObj[key] = p;
    }
    cleanSaveas.processes = procObj;
  }

  if (cleanSaveas._composeData) {
    var compData = cleanSaveas._composeData;
    delete cleanSaveas._composeData;
    if (!cleanSaveas.processes) cleanSaveas.processes = {};
    var composerKey = null;
    if (mist.data.capabilities && mist.data.capabilities.processes) {
      for (var pid in mist.data.capabilities.processes) {
        if (pid.toLowerCase().indexOf('composer') >= 0) {
          composerKey = pid;
          break;
        }
      }
    }
    if (composerKey) {
      var procConfig = {
        process: composerKey,
        sources: compData.sources || [],
        resolution: compData.resolution || '1920x1080',
        layout: compData.layout || 'equal'
      };
      if (compData['x-LSP-name']) procConfig['x-LSP-name'] = compData['x-LSP-name'];
      cleanSaveas.processes[composerKey + '0'] = procConfig;
    }
  } else {
    delete cleanSaveas._composeData;
  }

  streamSave(cleanSaveas, other || cleanSaveas.name, targetTab);
}
