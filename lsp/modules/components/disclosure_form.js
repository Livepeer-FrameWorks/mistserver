/**
 * Progressive disclosure form wrapper around formEngine.buildUI().
 * Splits fields into basic (always visible) and advanced (behind toggle).
 *
 * @param {Object} options
 * @param {Array} options.basic       UI.buildUI element descriptors (always shown)
 * @param {Array=} options.advanced   UI.buildUI element descriptors (behind toggle)
 * @param {string=} options.advancedLabel  Toggle text (default "Show advanced settings")
 * @param {boolean=} options.startExpanded  Start with advanced visible (default false)
 * @param {Array=} options.buttons    Button descriptors appended at the bottom
 * @return {HTMLElement}
 */
import { el, toNode } from '../core/dom_helpers.js';
import { formEngine } from '../core/form_engine.js';

export function disclosureForm(options) {
  let basic = options.basic || [];
  let advanced = options.advanced || [];
  const advancedLabel = options.advancedLabel || 'Show advanced settings';
  const collapsedLabel = advancedLabel;
  const expandedLabel = options.advancedLabelExpanded || advancedLabel.replace('Show', 'Hide');
  const startExpanded = options.startExpanded || false;

  const container = el('div', {class: 'disclosure-form'});

  if (!basic.length && advanced.length) {
    basic = advanced;
    advanced = [];
  }

  if (basic.length) {
    const basicUI = toNode(formEngine.buildUI(basic));
    if (basicUI) {
      container.appendChild(basicUI);
    }
  }

  if (advanced.length) {
    const chevron = el('span', {class: 'disclosure-chevron'});
    const toggleText = el('span');
    const toggle = el('button', {class: 'disclosure-toggle'});
    toggle.appendChild(chevron);
    toggle.appendChild(toggleText);

    const advInner = el('div', {class: 'disclosure-advanced-inner'});
    const advancedUI = toNode(formEngine.buildUI(advanced));
    if (advancedUI) {
      advInner.appendChild(advancedUI);
    }
    const advContainer = el('div', {class: 'disclosure-advanced'});
    advContainer.appendChild(advInner);

    function setExpanded(expanded) {
      if (expanded) {
        advContainer.classList.add('expanded');
        toggle.classList.add('expanded');
        toggleText.textContent = expandedLabel;
      } else {
        advContainer.classList.remove('expanded');
        toggle.classList.remove('expanded');
        toggleText.textContent = collapsedLabel;
      }
    }

    toggle.addEventListener('click', function() {
      setExpanded(!advContainer.classList.contains('expanded'));
    });

    setExpanded(startExpanded);
    container.appendChild(toggle);
    container.appendChild(advContainer);
  }

  if (options.buttons && options.buttons.length) {
    const buttonsUI = toNode(formEngine.buildUI([{type: 'buttons', buttons: options.buttons}]));
    if (buttonsUI) {
      container.appendChild(buttonsUI);
    }
  }

  return container;
}

/**
 * Auto-classify UI.buildUI elements from server capabilities
 * into basic and advanced arrays based on guided_visible / mode properties.
 *
 * @param {Array} elements  Array of UI.buildUI element descriptors
 * @return {{ basic: Array, advanced: Array }}
 */
export function classifyFields(elements) {
  const basic = [];
  const advanced = [];

  function stripAdvancedOnly(el) {
    if (el.classes) {
      el.classes = el.classes.filter(function(c) { return c !== 'advanced-only'; });
      if (!el.classes.length) delete el.classes;
    }
  }

  for (let i = 0; i < elements.length; i++) {
    const el = elements[i];
    if (!el) continue;

    if (el.type === 'buttons' || el.type === 'help' || el.type === 'text') {
      basic.push(el);
      continue;
    }

    stripAdvancedOnly(el);

    if (el.display === 'hidden')   { continue; }
    if (el.display === 'advanced') { advanced.push(el); continue; }
    if (el.display === 'always')   { basic.push(el);    continue; }

    if (el.guided_visible === true || el.mode === 'guided') {
      basic.push(el);
    } else if (el.mode === 'advanced') {
      advanced.push(el);
    } else if (el.guided_visible === false) {
      advanced.push(el);
    } else {
      basic.push(el);
    }
  }

  return { basic: basic, advanced: advanced };
}
