import { APP_NAME } from '@brand';
import { registerTab, getTabHandler } from '../core/tab_registry.js';
import { mountWizard } from '../components/wizard.js';
import { cardPicker } from '../components/card_picker.js';
import { disclosureForm, classifyFields } from '../components/disclosure_form.js';
import { inlineEditor } from '../components/inline_editor.js';
import { accordionTree } from '../components/accordion_tree.js';
import { uiHelpers } from '../core/ui_helpers.js';
import { apiClient } from '../core/api_client.js';
import { formEngine } from '../core/form_engine.js';
import { navto } from '../core/navigation.js';
import { tabView } from '../core/tab_view.js';
import { mistHelpers } from '../core/mist_helpers.js';
import { el, getval, setval } from '../core/dom_helpers.js';
import { triggerRewrite } from '../core/app_state.js';
import { registerDynamicProvider } from '../core/section_registry.js';

const TAB_AUTOMATIONS = 'Automations';
const TAB_AUTOMATIONS_LEGACY = 'Triggers';
const TAB_EDIT_AUTOMATION = 'Edit Automation';
const TAB_EDIT_AUTOMATION_LEGACY = 'Edit Trigger';

// --- Static metadata ---

const CATEGORIES = [
  { id: 'access',     title: 'Access Control',      icon: 'shield' },
  { id: 'lifecycle',  title: 'Stream Lifecycle',     icon: 'layers' },
  { id: 'routing',    title: 'Routing & Rewrites',   icon: 'route' },
  { id: 'monitoring', title: 'Monitoring & Events',  icon: 'bar-chart-2' },
  { id: 'system',     title: 'System',               icon: 'server' }
];

const TRIGGER_CARDS = {
  USER_NEW:      { category: 'access', title: 'Authenticate Viewers', desc: 'Check with your auth server before allowing a new viewer session. Deny or allow based on tokens, geo-location, or account status.', icon: 'users' },
  CONN_OPEN:     { category: 'access', title: 'Gate Connections', desc: 'Allow or deny individual connections as they come in, before any stream interaction occurs.', icon: 'door-open' },
  CONN_PLAY:     { category: 'access', title: 'Gate Playback', desc: 'Allow or deny a connection when it attempts to start playing a stream.', icon: 'play' },
  STREAM_PUSH:   { category: 'access', title: 'Gate Incoming Pushes', desc: 'Accept or reject incoming stream pushes based on the source address, stream name, or custom logic.', icon: 'upload' },
  LIVE_BANDWIDTH:{ category: 'access', title: 'Enforce Bandwidth Limits', desc: 'Shut down a live stream if its bitrate exceeds a threshold. Protects your server from runaway encoders.', icon: 'gauge' },

  STREAM_ADD:    { category: 'lifecycle', title: 'Approve New Streams', desc: 'Intercept stream creation and approve or reject new configurations before they take effect.', icon: 'plus-circle' },
  STREAM_CONFIG: { category: 'lifecycle', title: 'Approve Config Changes', desc: 'Intercept stream configuration changes and approve, reject, or modify them.', icon: 'sliders' },
  STREAM_REMOVE: { category: 'lifecycle', title: 'Approve Stream Removal', desc: 'Prevent accidental stream deletion by requiring approval before a stream is removed.', icon: 'trash-2' },
  STREAM_SOURCE: { category: 'lifecycle', title: 'Override Stream Source', desc: 'Dynamically change where a stream pulls its media from. Useful for failover or multi-source switching.', icon: 'refresh-cw' },
  STREAM_LOAD:   { category: 'lifecycle', title: 'Approve Stream Loading', desc: 'Allow or prevent a stream input from loading. Useful for scheduling or capacity management.', icon: 'loader' },
  STREAM_READY:  { category: 'lifecycle', title: 'On Stream Ready', desc: 'React when a stream finishes loading and is ready for playback. Can shut down the input if rejected.', icon: 'check-circle' },
  STREAM_UNLOAD: { category: 'lifecycle', title: 'Prevent Stream Unloading', desc: 'Keep streams alive beyond their normal idle timeout by preventing automatic unloading.', icon: 'pause-circle' },

  PUSH_REWRITE:       { category: 'routing', title: 'Rewrite Push Stream Names', desc: 'Parse incoming push URLs on any protocol to extract a custom stream name.', icon: 'pen-line' },
  RTMP_PUSH_REWRITE:  { category: 'routing', title: 'Rewrite RTMP Push URLs', desc: 'Rewrite incoming RTMP push URLs to custom formatting (e.g., stream key to stream name).', icon: 'pen-line' },
  PUSH_OUT_START:     { category: 'routing', title: 'Override Push Target', desc: 'Change where an outgoing push goes right before it starts. Useful for dynamic recording paths.', icon: 'external-link' },
  PLAY_REWRITE:       { category: 'routing', title: 'Redirect Playback', desc: 'Redirect a viewer to a different stream than the one they requested. Useful for A/B testing or load distribution.', icon: 'corner-down-right' },
  DEFAULT_STREAM:     { category: 'routing', title: 'Fallback for Missing Streams', desc: 'When a viewer requests a stream that doesn\'t exist or is offline, redirect them to a fallback.', icon: 'life-buoy' },

  STREAM_BUFFER:    { category: 'monitoring', title: 'Buffer State Changes', desc: 'Get notified when a live stream buffer changes state (empty, full, dry, or recovering).', icon: 'activity' },
  STREAM_END:       { category: 'monitoring', title: 'Stream Ended', desc: 'Get notified when a stream ends after a period of activity. Includes total bytes and viewer counts.', icon: 'square' },
  CONN_CLOSE:       { category: 'monitoring', title: 'Connection Closed', desc: 'Get notified when a viewer connection is closed. Useful for analytics and session tracking.', icon: 'log-out' },
  USER_END:         { category: 'monitoring', title: 'Session Ended', desc: 'Get notified when a viewer session ends. Includes duration, bytes transferred, and session tags.', icon: 'user-x' },
  RECORDING_END:    { category: 'monitoring', title: 'Recording Finished', desc: 'Get notified when a recording (push to file) finishes. Includes file size, duration, and timestamps.', icon: 'hard-drive' },
  OUTPUT_END:       { category: 'monitoring', title: 'Output Finished', desc: 'Get notified when any output finishes. Includes bytes, duration, and timestamps.', icon: 'log-out' },
  PUSH_END:         { category: 'monitoring', title: 'Push Stopped', desc: 'Get notified when an outgoing push stops for any reason. Includes logs and push status.', icon: 'upload-cloud' },
  LIVE_TRACK_LIST:  { category: 'monitoring', title: 'Track List Updated', desc: 'Get notified when the list of tracks in a live stream changes.', icon: 'list' },
  INPUT_ABORT:      { category: 'monitoring', title: 'Input Error', desc: 'Get notified when a stream input process exits with an error. Includes reason and source URI.', icon: 'alert-triangle' },

  SYSTEM_START:     { category: 'system', title: 'Server Boot', desc: 'React to server startup. Can prevent boot if the handler returns false.', icon: 'power' },
  SYSTEM_STOP:      { category: 'system', title: 'Server Shutdown', desc: 'React before the server shuts down. Can abort the shutdown.', icon: 'power' },
  OUTPUT_START:     { category: 'system', title: 'Connector Started', desc: 'Get notified when a protocol connector starts listening for connections.', icon: 'radio' },
  OUTPUT_STOP:      { category: 'system', title: 'Connector Stopped', desc: 'Get notified when a protocol connector stops listening.', icon: 'radio' },
  LIVEPEER_SEGMENT_REJECTED: { category: 'system', title: 'Livepeer Segment Rejected', desc: 'Notified when a segment is rejected by the Livepeer transcoding network.', icon: 'x-circle' }
};

// --- Helpers ---

function ensureConfig() {
  if (!mist.data || typeof mist.data !== 'object') {
    mist.data = {};
  }
  if (!mist.data.config || typeof mist.data.config !== 'object') {
    mist.data.config = {};
  }
  if (!mist.data.config.triggers || typeof mist.data.config.triggers !== 'object') {
    mist.data.config.triggers = {};
  }
}

function getCardInfo(event) {
  if (TRIGGER_CARDS[event]) return TRIGGER_CARDS[event];
  const cap = (mist.data.capabilities && mist.data.capabilities.triggers) ? mist.data.capabilities.triggers[event] : null;
  return { category: 'system', title: event, desc: cap ? cap.when : '', icon: 'zap' };
}

function getCategoryTitle(catId) {
  for (let i = 0; i < CATEGORIES.length; i++) {
    if (CATEGORIES[i].id === catId) return CATEGORIES[i].title;
  }
  return catId;
}

registerDynamicProvider(function() {
  var caps = mist.data.capabilities && mist.data.capabilities.triggers;
  if (!caps) return [];
  var entries = [];
  for (var event in TRIGGER_CARDS) {
    if (!caps[event]) continue;
    var card = TRIGGER_CARDS[event];
    var cap = caps[event];
    entries.push({
      id: 'automation/' + event.toLowerCase(),
      page: 'Automations',
      title: card.title,
      subtitle: card.desc,
      keywords: [event.toLowerCase(), card.category, cap.when || ''],
      requires: ['capabilities'],
      render: (function(ev, c, cp) {
        return function($container) {
          var info = el('div');
          info.appendChild(el('p', null, c.desc));
          if (cp.when) {
            info.appendChild(el('p', {style: 'color: var(--text-muted, #999); font-size: 13px;'}, 'Fires when: ' + cp.when));
          }
          if (cp.response && cp.response !== 'ignored') {
            info.appendChild(el('p', {style: 'font-size: 13px;'}, 'Response: ' + humanifyResponse(cp.response)));
          }
          ensureConfig();
          var rules = mist.data.config.triggers[ev];
          if (rules && rules.length) {
            info.appendChild(el('p', {style: 'font-weight: 600; margin-top: 8px;'}, rules.length + ' rule' + (rules.length === 1 ? '' : 's') + ' configured'));
          }
          $container.appendChild(info);
        };
      })(event, card, cap),
      navTo: { tab: TAB_AUTOMATIONS, other: '' }
    });
  }
  return entries;
});

function humanifyResponse(response) {
  switch (response) {
    case 'ignored': return 'No. The handler response is ignored.';
    case 'always': return 'Yes. The automation must receive a response to continue.';
    case 'when-blocking': return 'Only when this rule is set to blocking.';
    default: return response || '';
  }
}

function uniqueNonEmpty(arr) {
  const seen = {};
  const out = [];
  for (let i = 0; i < arr.length; i++) {
    const item = (arr[i] || '').trim();
    if (!item || seen[item]) continue;
    seen[item] = true;
    out.push(item);
  }
  return out;
}

function getAppliesToStreams(ctx, capInfo) {
  if (!capInfo || !capInfo.stream_specific) return [];
  return uniqueNonEmpty(ctx.streams || []);
}

function buildTriggerPayload(ctx, capInfo) {
  return {
    handler: ctx.url,
    sync: ctx.blocking ? true : false,
    streams: getAppliesToStreams(ctx, capInfo),
    params: ctx.params || '',
    'default': ctx['default'] || ''
  };
}

function getAllRules() {
  ensureConfig();
  const rules = [];
  const triggers = mist.data.config.triggers;
  for (const event in triggers) {
    if (!triggers[event]) continue;
    for (let j = 0; j < triggers[event].length; j++) {
      const t = triggerRewrite(triggers[event][j]);
      rules.push({ event: event, index: j, data: t });
    }
  }
  return rules;
}

function deleteRule(event, index, callback) {
  ensureConfig();
  if (!mist.data.config.triggers[event]) return;
  mist.data.config.triggers[event].splice(index, 1);
  if (mist.data.config.triggers[event].length === 0) {
    delete mist.data.config.triggers[event];
  }
  apiClient.send(function() { if (callback) callback(); }, { config: mist.data.config });
}

function saveRule(event, triggerObj, callback) {
  ensureConfig();
  if (!(event in mist.data.config.triggers)) {
    mist.data.config.triggers[event] = [];
  }
  mist.data.config.triggers[event].push(triggerObj);
  apiClient.send(function() { if (callback) callback(); }, { config: mist.data.config });
}

// --- Configure form builder (shared between wizard step 2 and edit modal) ---

function buildStreamPicker(ctx, streamNames) {
  const selected = ctx.streams;

  const wrapper = el('div', {class: 'automation-stream-picker'});

  const labelRow = el('div', {class: 'automation-stream-picker-header'});
  labelRow.appendChild(el('span', {class: 'automation-stream-picker-label'}, 'Applies to'));
  const allBtn = el('button', {type: 'button', class: 'automation-stream-all'}, 'All streams');
  labelRow.appendChild(allBtn);
  wrapper.appendChild(labelRow);

  const pillsRow = el('div', {class: 'automation-stream-picker-pills'});
  wrapper.appendChild(pillsRow);

  const input = el('input', {
    type: 'text',
    class: 'automation-stream-picker-input',
    placeholder: 'Type a stream name, wildcard (name+), or tag (#tag)\u2026'
  });
  wrapper.appendChild(input);

  const matchHelp = el('div', {class: 'automation-stream-match-help'});
  matchHelp.innerHTML = '<code>live</code> exact match only \u2022 <code>live+</code> all wildcard children \u2022 <code>#tag</code> any stream with this tag';
  wrapper.appendChild(matchHelp);

  const hintsRow = el('div', {class: 'var-reference stream-reference automation-stream-hints'});
  hintsRow.appendChild(el('span', {class: 'var-reference-label'}, 'Streams'));
  const emptyHint = el('span', {class: 'stream-reference-empty'}, 'No matching streams');
  const hintBtns = [];

  const allNames = streamNames.slice().sort();
  for (let i = 0; i < allNames.length; i++) {
    (function(name) {
      const btn = el('button', {type: 'button', class: 'var-reference-item stream-reference-item'});
      btn.appendChild(el('code', null, name));
      btn.addEventListener('click', function(e) {
        e.preventDefault();
        toggleStream(name);
      });
      hintBtns.push({name: name, el: btn});
      hintsRow.appendChild(btn);
    })(allNames[i]);
  }
  hintsRow.appendChild(emptyHint);
  if (allNames.length) wrapper.appendChild(hintsRow);

  function toggleStream(name) {
    const idx = selected.indexOf(name);
    if (idx >= 0) selected.splice(idx, 1);
    else selected.push(name);
    ctx.streams = selected;
    render();
  }

  function addStream(name) {
    name = (name || '').trim();
    if (!name || selected.indexOf(name) >= 0) return;
    selected.push(name);
    ctx.streams = selected;
    render();
  }

  function removeStream(name) {
    const idx = selected.indexOf(name);
    if (idx >= 0) selected.splice(idx, 1);
    ctx.streams = selected;
    render();
  }

  function clearAll() {
    selected.length = 0;
    ctx.streams = selected;
    input.value = '';
    render();
  }

  function render() {
    const isEmpty = selected.length === 0;
    allBtn.classList.toggle('active', isEmpty);

    pillsRow.innerHTML = '';
    for (let i = 0; i < selected.length; i++) {
      (function(name) {
        const pill = el('span', {class: 'automation-stream-pill'});
        pill.appendChild(el('code', null, name));
        const x = el('button', {type: 'button', 'data-icon': 'x', class: 'automation-stream-pill-x'});
        x.addEventListener('click', function() { removeStream(name); });
        pill.appendChild(x);
        pillsRow.appendChild(pill);
      })(selected[i]);
    }
    pillsRow.hidden = isEmpty;

    for (let j = 0; j < hintBtns.length; j++) {
      hintBtns[j].el.classList.toggle('selected', selected.indexOf(hintBtns[j].name) >= 0);
    }
    updateFilter();
  }

  function updateFilter() {
    const query = (input.value || '').toLowerCase().trim();
    let shown = 0;
    for (let k = 0; k < hintBtns.length; k++) {
      const name = hintBtns[k].name.toLowerCase();
      const match = !query || name.indexOf(query) >= 0;
      if (match && shown < 15) {
        hintBtns[k].el.hidden = false;
        shown++;
      } else {
        hintBtns[k].el.hidden = true;
      }
    }
    emptyHint.hidden = (shown > 0);
    hintsRow.classList.toggle('empty', shown === 0);
  }

  allBtn.addEventListener('click', clearAll);
  input.addEventListener('keydown', function(e) {
    if (e.key === 'Enter') {
      e.preventDefault();
      addStream(input.value);
      input.value = '';
      updateFilter();
    }
  });
  input.addEventListener('input', updateFilter);

  render();
  return wrapper;
}

function injectStreamPicker(container, ctx, streamNames) {
  const picker = buildStreamPicker(ctx, streamNames);
  const uiElements = container.querySelectorAll('.UIelement');
  let target = null;
  for (let i = 0; i < uiElements.length; i++) {
    const lbl = uiElements[i].querySelector('label');
    if (lbl && lbl.textContent.trim() === 'Default response') {
      target = uiElements[i];
      break;
    }
  }
  if (target) target.before(picker);
  else container.appendChild(picker);
}

function buildConfigForm(ctx, capInfo) {
  const basic = [];
  const advanced = [];
  const streamNames = mist.data.streams ? Object.keys(mist.data.streams).sort() : [];
  let afterBuild = null;

  basic.push({
    label: 'Handler (URL or executable)',
    help: 'An HTTP URL to call, or a full path to an executable on the server.',
    pointer: { main: ctx, index: 'url' },
    validate: ['required'],
    type: 'str'
  });

  if (capInfo && capInfo.response !== 'ignored') {
    const blockHelp = capInfo.response === 'always'
      ? 'Processing pauses until your handler responds.'
      : 'When enabled, the response overrides the default behavior.';
    basic.push({
      label: 'Blocking',
      type: 'checkbox',
      help: blockHelp,
      pointer: { main: ctx, index: 'blocking' }
    });
  }

  if (capInfo && capInfo.stream_specific) {
    if (!ctx.streams) ctx.streams = [];
    afterBuild = function(container) {
      injectStreamPicker(container, ctx, streamNames);
    };
  }

  basic.push({
    label: 'Default response',
    type: 'str',
    help: 'Fallback response when the handler fails or this rule is non-blocking.',
    placeholder: 'true',
    pointer: { main: ctx, index: 'default' }
  });

  if (capInfo && capInfo.argument) {
    advanced.push({
      label: 'Parameters',
      type: 'str',
      help: capInfo.argument,
      pointer: { main: ctx, index: 'params' }
    });
  }

  if (capInfo && capInfo.payload) {
    basic.push({
      label: 'Payload sent to handler',
      type: 'textarea',
      value: capInfo.payload,
      rows: Math.min(capInfo.payload.split('\n').length, 8),
      readonly: true,
      help: 'Data fields sent to the handler (read-only reference).'
    });
  }

  return { basic: basic, advanced: advanced, afterBuild: afterBuild };
}

// --- Creation wizard ---

function openWizard(onDone) {
  const caps = mist.data.capabilities && mist.data.capabilities.triggers;
  if (!caps) {
    apiClient.send(function() { openWizard(onDone); }, { capabilities: true });
    return;
  }

  const modal = uiHelpers.openFormModal({
    size: 'lg',
    popupClasses: ['popup--wizard']
  });

  const ctx = {};

  mountWizard({
    modal: modal,
    steps: [
      { id: 'event', label: 'Choose Event', icon: 'list', build: function($c, cx, cb) { return buildEventStep($c, cx, cb, caps); } },
      { id: 'configure', label: 'Configure', icon: 'sliders', build: function($c, cx, cb) { return buildConfigureStep($c, cx, cb, caps); } },
      { id: 'review', label: 'Review', icon: 'check', build: function($c, cx, cb) { return buildReviewStep($c, cx, cb); } }
    ],
    context: ctx,
    onComplete: function(cx) {
      saveRule(cx.triggeron, buildTriggerPayload(cx, caps[cx.triggeron]), function() {
        modal.close();
        if (onDone) onDone();
      });
    },
    onCancel: function() { modal.close(); }
  });
}

function buildEventStep($container, ctx, callbacks, caps) {
  const groups = [];
  for (let g = 0; g < CATEGORIES.length; g++) {
    groups.push({ id: CATEGORIES[g].id, title: CATEGORIES[g].title });
  }
  groups.push({ id: '_other', title: 'Other' });

  const cards = [];
  const seen = {};
  for (const event in TRIGGER_CARDS) {
    if (!caps[event]) continue;
    seen[event] = true;
    const card = TRIGGER_CARDS[event];
    cards.push({
      id: event,
      group: card.category,
      title: card.title,
      desc: card.desc,
      icon: card.icon,
      available: true,
      onClick: function(cardId) {
        ctx.triggeron = cardId;
        const info = getCardInfo(cardId);
        callbacks.setStepSummary(info.title);
        const capInfo = caps[cardId];
        if (capInfo && capInfo.response === 'always') ctx.blocking = true;
        else if (capInfo && capInfo.response === 'when-blocking') ctx.blocking = true;
        callbacks.goTo('configure');
      }
    });
  }

  for (const ev in caps) {
    if (seen[ev]) continue;
    cards.push({
      id: ev,
      group: '_other',
      title: ev,
      desc: caps[ev].when || '',
      icon: 'zap',
      available: true,
      onClick: function(cardId) {
        ctx.triggeron = cardId;
        callbacks.setStepSummary(cardId);
        callbacks.goTo('configure');
      }
    });
  }

  const $picker = cardPicker({
    intro: 'What should this automation react to?',
    groups: groups,
    cards: cards,
    searchable: true
  });

  $container.append($picker);

  callbacks.markValid(false);
  return {
    validate: function() { return !!ctx.triggeron; }
  };
}

function buildConfigureStep($container, ctx, callbacks, caps) {
  const cardInfo = getCardInfo(ctx.triggeron);
  const capInfo = caps[ctx.triggeron];

  const header = el('div', {class: 'automation-config-header'});
  header.appendChild(el('h3', {}, cardInfo.title));
  if (capInfo && capInfo.when) {
    header.appendChild(el('p', {class: 'automation-config-when'}, capInfo.when));
  }
  $container.append(header);

  if (capInfo && capInfo.response !== 'ignored') {
    const responseBehavior = el('div', {class: 'automation-response-notice'});
    let responseText = capInfo.response === 'always'
      ? 'This event <b>requires</b> a handler response. Processing pauses until your handler responds.'
      : 'When set to <b>blocking</b>, the handler response controls what happens next.';
    if (capInfo.response_action) {
      responseText += ' ' + capInfo.response_action;
    }
    responseBehavior.innerHTML = responseText;
    $container.append(responseBehavior);
  }

  if (typeof ctx.blocking === 'undefined') {
    if (capInfo && capInfo.response === 'always') ctx.blocking = true;
    else if (capInfo && capInfo.response === 'when-blocking') ctx.blocking = true;
  }

  const fields = buildConfigForm(ctx, capInfo);
  const formEl = disclosureForm({
    basic: fields.basic,
    advanced: fields.advanced
  });
  $container.append(formEl);
  if (fields.afterBuild) fields.afterBuild(formEl);

  callbacks.markValid(true);

  return {
    validate: function() {
      if (!ctx.url || !ctx.url.trim()) {
        alert('Handler URL or path is required.');
        return false;
      }
      return true;
    }
  };
}

function buildReviewStep($container, ctx, callbacks) {
  const cardInfo = getCardInfo(ctx.triggeron);
  const capInfo = (mist.data.capabilities && mist.data.capabilities.triggers) ? mist.data.capabilities.triggers[ctx.triggeron] : null;

  const items = [{
    type: 'span',
    label: 'Event',
    value: cardInfo.title + ' (' + ctx.triggeron + ')'
  }, {
    type: 'span',
    label: 'Handler',
    value: ctx.url || '(none)'
  }];

  if (capInfo && capInfo.response !== 'ignored') {
    items.push({
      type: 'span',
      label: 'Blocking',
      value: ctx.blocking ? 'Yes' : 'No'
    });
  }

  if (capInfo && capInfo.stream_specific) {
    const appliesTo = getAppliesToStreams(ctx, capInfo);
    const streams = appliesTo.length ? appliesTo.join(', ') : '(all streams)';
    items.push({
      type: 'span',
      label: 'Applies to',
      value: streams
    });
  }

  if (ctx['default']) {
    items.push({
      type: 'span',
      label: 'Default response',
      value: ctx['default']
    });
  }

  if (ctx.params) {
    items.push({
      type: 'span',
      label: 'Parameters',
      value: ctx.params
    });
  }

  $container.append(el('h3', {}, 'Review your automation rule'));
  $container.append(formEngine.buildUI(items));

  callbacks.markValid(true);
}

// --- Edit modal ---

function showEditModal(event, ruleIndex, onDone) {
  const caps = mist.data.capabilities && mist.data.capabilities.triggers;
  if (!caps) {
    apiClient.send(function() { showEditModal(event, ruleIndex, onDone); }, { capabilities: true });
    return;
  }

  ensureConfig();
  if (!mist.data.config.triggers[event] || !mist.data.config.triggers[event][ruleIndex]) {
    if (onDone) onDone();
    return;
  }
  const source = triggerRewrite(mist.data.config.triggers[event][ruleIndex]);
  const cardInfo = getCardInfo(event);
  const capInfo = caps[event];

  const ctx = {
    url: source.handler,
    blocking: source.sync ? true : false,
    streams: source.streams || [],
    params: source.params || '',
    'default': source['default'] || ''
  };

  const content = el('div');
  const modal = uiHelpers.openFormModal({
    size: 'md',
    title: cardInfo.title,
    subtitle: el('span', {class: 'automation-config-when'}, event + (capInfo ? ' - ' + capInfo.when : '')),
    body: content
  });

  const fields = buildConfigForm(ctx, capInfo);
  const formEl = disclosureForm({
    basic: fields.basic,
    advanced: fields.advanced,
    buttons: [
      { type: 'cancel', label: 'Cancel', 'function': function() { modal.close(); } },
      { type: 'save', label: 'Save', 'function': function() {
        if (!ctx.url || !ctx.url.trim()) { alert('Handler is required.'); return; }
        deleteRule(event, ruleIndex, function() {
          saveRule(event, buildTriggerPayload(ctx, capInfo), function() {
            modal.close();
            if (onDone) onDone();
          });
        });
      }},
      { label: 'Delete', 'function': function() {
        if (!confirm('Delete this ' + event + ' automation rule?')) return;
        deleteRule(event, ruleIndex, function() {
          modal.close();
          if (onDone) onDone();
        });
      }}
    ]
  });
  content.appendChild(formEl);
  if (fields.afterBuild) fields.afterBuild(formEl);

}

// --- Helpers for inline rule rendering ---

function buildRuleMeta(rule) {
  const meta = el('span', {class: 'automation-rule-meta'});
  if (rule.data.sync) {
    meta.appendChild(el('span', {class: 'automation-rule-badge automation-rule-badge--blocking'}, 'Blocking'));
  }
  const hasStreams = rule.data.streams && rule.data.streams.length;
  const streamText = hasStreams ? rule.data.streams.join(', ') : 'All streams';
  const streamEl = el('span', {class: 'automation-rule-streams'}, streamText);
  if (!hasStreams) streamEl.classList.add('automation-rule-streams--all');
  meta.appendChild(streamEl);
  return meta;
}

function groupByCategory(rules) {
  const grouped = {};
  for (let i = 0; i < CATEGORIES.length; i++) grouped[CATEGORIES[i].id] = [];
  grouped._other = [];
  for (let r = 0; r < rules.length; r++) {
    const card = getCardInfo(rules[r].event);
    let cat = card.category;
    if (!grouped[cat]) cat = '_other';
    grouped[cat].push(rules[r]);
  }
  return grouped;
}

function buildCategoryEditor(catRules, caps, refreshRules) {
  return inlineEditor({
    items: catRules,
    reorderable: false,
    showAdd: false,
    emptyMessage: 'No rules in this category.',
    renderItem: function(rule) {
      const cardInfo = getCardInfo(rule.event);
      let handler = rule.data.handler || '';
      if (handler.length > 50) handler = handler.slice(0, 47) + '...';
      return {
        icon: cardInfo.icon,
        title: cardInfo.title,
        subtitle: rule.event + ' - ' + handler,
        meta: buildRuleMeta(rule)
      };
    },
    editItem: function(rule, index, $editArea, callbacks) {
      const capInfo = caps[rule.event];
      const ctx = {
        url: rule.data.handler,
        blocking: rule.data.sync ? true : false,
        streams: (rule.data.streams || []).slice(),
        params: rule.data.params || '',
        'default': rule.data['default'] || ''
      };
      const fields = buildConfigForm(ctx, capInfo);
      const cardInfo = getCardInfo(rule.event);
      const header = el('div', {class: 'automation-config-header'});
      header.appendChild(el('h3', {}, cardInfo.title));
      if (capInfo && capInfo.when) {
        header.appendChild(el('p', {class: 'automation-config-when'}, capInfo.when));
      }
      $editArea.append(header);
      const formEl = disclosureForm({
        basic: fields.basic,
        advanced: fields.advanced,
        buttons: [
          { type: 'cancel', label: 'Cancel', 'function': function() { callbacks.cancel(); } },
          { type: 'save', label: 'Save', 'function': function() {
            if (!ctx.url || !ctx.url.trim()) { alert('Handler is required.'); return; }
            deleteRule(rule.event, rule.index, function() {
              saveRule(rule.event, buildTriggerPayload(ctx, capInfo), function() {
                apiClient.send(function() { refreshRules(); });
              });
            });
          }}
        ]
      });
      $editArea.append(formEl);
      if (fields.afterBuild) fields.afterBuild(formEl);
    },
    onRemove: function(rule) {
      deleteRule(rule.event, rule.index, refreshRules);
    }
  });
}

// --- Main tab handler ---

let _activeAccordion = null;

registerTab(TAB_AUTOMATIONS, function(tab, other, prev, $main, $pageHeader) {
  $main.classList.add('page-body--flex-col', 'automation-page-body', 'automation-shell', 'slab-shell', 'slab-shell--seamed');
  if (!mist.data.capabilities) {
    apiClient.send(function() { navto(TAB_AUTOMATIONS); }, { capabilities: true });
    $main.append('Loading..');
    return;
  }
  if (!mist.data.capabilities.triggers || typeof mist.data.capabilities.triggers !== 'object') {
    mist.data.capabilities.triggers = {};
  }

  const caps = mist.data.capabilities.triggers;
  ensureConfig();

  let accordion = null;
  const stored = mistHelpers.stored.get();
  let categoryFilter = stored.automation_category || 'all';
  let textFilter = '';
  let emptyStateIcon = null;
  let emptyStateTitle = null;
  let emptyStateText = null;
  let emptyStateAction = null;
  let emptyState = null;
  let ruleArea = null;
  let dangerZone = null;

  // --- Page header ---
  uiHelpers.appendPageActions($pageHeader, el('button', {
    'data-icon': 'plus',
    onclick: function() { openWizard(refreshRules); }
  }, 'New rule'));

  const shell = $main;

  // --- Intro ---
  shell.append(formEngine.buildUI([{
    type: 'help',
    classes: ['page-intro'],
    help: 'Event-driven rules that react to '+APP_NAME+' events. Use them to authenticate viewers, redirect streams, track activity, and more.'
  }]));

  // --- Filter bar ---
  const filterBar = el('div', {class: 'automation-filter-bar'});

  const $textFilter = formEngine.buildUI([{
    label: 'Filter rules',
    classes: ['filter'],
    help: 'Search by event name, title, handler, or stream name.',
    'function': function() {
      textFilter = getval(this).toLowerCase();
      applyFilters();
    }
  }]);
  filterBar.appendChild($textFilter);

  const categoryChips = el('span', {class: 'automation-category-filter'});
  const allChips = [{ id: 'all', title: 'All' }];
  for (let ci = 0; ci < CATEGORIES.length; ci++) {
    allChips.push({ id: CATEGORIES[ci].id, title: CATEGORIES[ci].title });
  }
  for (let ch = 0; ch < allChips.length; ch++) {
    (function(chip) {
      const btn = el('button', {
        onclick: function() {
          categoryFilter = chip.id;
          mistHelpers.stored.set('automation_category', categoryFilter);
          Array.from(categoryChips.querySelectorAll('button')).forEach(function(b) { b.classList.remove('active'); });
          btn.classList.add('active');
          applyFilters();
        }
      }, chip.title);
      if (chip.id === categoryFilter) btn.classList.add('active');
      categoryChips.appendChild(btn);
    })(allChips[ch]);
  }
  const filterField = filterBar.querySelector('.field.filter');
  if (filterField && filterField.after) {
    filterField.after(categoryChips);
  } else {
    filterBar.appendChild(categoryChips);
  }
  shell.append(filterBar);

  function setCategoryChip(catId) {
    Array.from(categoryChips.querySelectorAll('button')).forEach(function(b) { b.classList.remove('active'); });
    let idx = 0;
    for (let i = 0; i < allChips.length; i++) {
      if (allChips[i].id === catId) { idx = i; break; }
    }
    const buttons = categoryChips.querySelectorAll('button');
    if (buttons[idx]) buttons[idx].classList.add('active');
  }

  function clearFilters() {
    categoryFilter = 'all';
    textFilter = '';
    mistHelpers.stored.set('automation_category', categoryFilter);
    setCategoryChip(categoryFilter);
    const textField = filterBar.querySelector('.field.filter');
    if (textField) {
      setval(textField, '');
    }
  }

  // --- Empty states ---
  emptyStateIcon = el('div', {'data-icon': 'zap'});
  emptyStateTitle = el('h3', {}, 'No automation rules configured');
  emptyStateText = el('p', {}, 'Automation rules let you react to server events - authenticate viewers, redirect streams, track activity, and more.');
  emptyStateAction = el('button', {
    'data-icon': 'plus',
    onclick: function() { openWizard(refreshRules); }
  }, 'Create your first automation');

  emptyState = el('div', {class: 'automation-empty-state'});
  emptyState.appendChild(emptyStateIcon);
  emptyState.appendChild(emptyStateTitle);
  emptyState.appendChild(emptyStateText);
  emptyState.appendChild(emptyStateAction);
  shell.append(emptyState);

  // --- Rule area (accordion with inline editors) ---
  ruleArea = el('div', {class: 'automation-rule-area'});
  shell.append(ruleArea);

  // --- Danger zone ---
  dangerZone = el('div', {class: 'automation-danger-zone'});
  dangerZone.appendChild(el('button', {
    onclick: function() {
      if (!confirm('Delete all configured automation rules? This cannot be undone.')) return;
      mist.data.config.triggers = {};
      apiClient.send(function() { refreshRules(); }, { config: mist.data.config });
    }
  }, 'Delete all automation rules'));
  shell.append(dangerZone);

  // --- Build / rebuild the accordion ---
  let activeSectionCatIds = [];

  function buildAccordion(grouped) {
    const sections = [];
    activeSectionCatIds = [];
    const allCats = CATEGORIES.slice();
    allCats.push({ id: '_other', title: 'Other', icon: 'zap' });

    for (let c = 0; c < allCats.length; c++) {
      const catId = allCats[c].id;
      const catRules = grouped[catId];
      if (!catRules || !catRules.length) continue;
      activeSectionCatIds.push(catId);
      (function(catId, catRules, catDef) {
        sections.push({
          id: catId,
          title: catDef.title,
          icon: catDef.icon,
          subtitle: catRules.length + (catRules.length === 1 ? ' rule' : ' rules'),
          expanded: true,
          content: function($inner) {
            const editor = buildCategoryEditor(catRules, caps, refreshRules);
            $inner.append(editor.$el);
          }
        });
      })(catId, catRules, allCats[c]);
    }

    accordion = accordionTree({
      sections: sections,
      allowMultiple: true
    });
    _activeAccordion = accordion;
    return accordion.$el;
  }

  function refreshRules() {
    const allRules = getAllRules();
    const grouped = groupByCategory(allRules);

    ruleArea.innerHTML = '';
    if (allRules.length > 0) {
      ruleArea.appendChild(buildAccordion(grouped));
    } else {
      accordion = null;
      _activeAccordion = null;
      activeSectionCatIds = [];
    }

    applyFilters(allRules);
  }

  function updateEmptyState(totalRules, visibleRules) {
    if (!emptyStateIcon || !emptyStateTitle || !emptyStateText || !emptyStateAction || !emptyState || !filterBar || !ruleArea || !dangerZone) {
      return;
    }
    if (totalRules === 0) {
      emptyStateIcon.setAttribute('data-icon', 'zap');
      emptyStateTitle.textContent = 'No automation rules configured';
      emptyStateText.textContent = 'Automation rules let you react to server events - authenticate viewers, redirect streams, track activity, and more.';
      emptyStateAction.setAttribute('data-icon', 'plus');
      emptyStateAction.textContent = 'Create your first automation';
      emptyStateAction.onclick = function() { openWizard(refreshRules); };

      emptyState.hidden = false;
      filterBar.hidden = true;
      ruleArea.hidden = true;
      dangerZone.hidden = true;
      return;
    }

    if (visibleRules === 0) {
      const noun = totalRules === 1 ? 'rule' : 'rules';
      emptyStateIcon.setAttribute('data-icon', 'filter');
      emptyStateTitle.textContent = 'No automation rules match the current filters';
      emptyStateText.textContent = 'You have ' + totalRules + ' automation ' + noun + ' configured. Clear filters to view them.';
      emptyStateAction.setAttribute('data-icon', 'x');
      emptyStateAction.textContent = 'Clear filters';
      emptyStateAction.onclick = function() { clearFilters(); applyFilters(); };

      emptyState.hidden = false;
      filterBar.hidden = false;
      ruleArea.hidden = true;
      dangerZone.hidden = false;
    } else {
      emptyState.hidden = true;
      filterBar.hidden = false;
      ruleArea.hidden = false;
      dangerZone.hidden = false;
    }
  }

  function applyFilters(allRules) {
    if (!allRules) allRules = getAllRules();
    if (!accordion) {
      updateEmptyState(allRules.length, 0);
      return;
    }

    const accordionGroup = ruleArea.querySelector('.accordion-group');
    if (!accordionGroup) {
      updateEmptyState(allRules.length, 0);
      return;
    }
    const sections = Array.from(accordionGroup.querySelectorAll(':scope > .accordion-section'));
    const grouped = groupByCategory(allRules);
    let visibleRuleCount = 0;

    for (let s = 0; s < activeSectionCatIds.length; s++) {
      const section = sections[s];
      if (!section) continue;
      const catId = activeSectionCatIds[s];
      const categoryMatch = (categoryFilter === 'all' || categoryFilter === catId);
      if (!categoryMatch) {
        section.hidden = true;
        Array.from(section.querySelectorAll('.inline-editor-item')).forEach(function(item) {
          item.classList.remove('filter-hidden');
        });
        continue;
      }

      const catRules = grouped[catId] || [];
      let sectionVisibleCount = 0;
      const items = Array.from(section.querySelectorAll('.inline-editor-item'));
      items.forEach(function(item, itemIdx) {
        let match = true;
        if (textFilter) {
          if (itemIdx >= catRules.length) {
            match = false;
          } else {
            const rule = catRules[itemIdx];
            const cardInfo = getCardInfo(rule.event);
            const searchStr = [
              rule.event,
              cardInfo.title,
              rule.data.handler || '',
              (rule.data.streams || []).join(' ')
            ].join(' ').toLowerCase();
            match = searchStr.indexOf(textFilter) >= 0;
          }
        }
        if (match) {
          item.classList.remove('filter-hidden');
          sectionVisibleCount++;
        } else {
          item.classList.add('filter-hidden');
        }
      });

      if (sectionVisibleCount > 0) {
        section.hidden = false;
        visibleRuleCount += sectionVisibleCount;
      } else {
        section.hidden = true;
      }
    }

    updateEmptyState(allRules.length, visibleRuleCount);
  }

  // --- Initial render ---
  refreshRules();
});

// External API for other modules (e.g., stream Status page)
export function editAutomation(other, onSave) {
  const defaultCallback = function() { navto(TAB_AUTOMATIONS); };
  if (other && other !== '') {
    const parts = other.split(',');
    showEditModal(parts[0], parseInt(parts[1], 10), onSave || defaultCallback);
  } else {
    openWizard(onSave || defaultCallback);
  }
}

// Legacy redirects
registerTab(TAB_AUTOMATIONS_LEGACY, function(tab, other) {
  navto(TAB_AUTOMATIONS, other);
});
registerTab(TAB_EDIT_AUTOMATION, function(tab, other) {
  if (other && other !== '') {
    const parts = other.split(',');
    tabView.showTab(TAB_AUTOMATIONS);
    showEditModal(parts[0], parseInt(parts[1], 10), function() { navto(TAB_AUTOMATIONS); });
  } else {
    navto(TAB_AUTOMATIONS);
  }
});
registerTab(TAB_EDIT_AUTOMATION_LEGACY, function(tab, other) {
  getTabHandler(TAB_EDIT_AUTOMATION)(tab, other);
});
