/**
 * Multi-step wizard framework.
 * @param {Object} options
 * @param {Array} options.steps     Array of step descriptors
 * @param {Object} options.context  Shared state object passed to every step
 * @param {Function} options.onComplete  Called with context when final step is saved
 * @param {Function=} options.onCancel   Called when wizard is cancelled
 * @param {number=} options.startAt     Index of initial step (default 0)
 * @param {boolean=} options.allowJump  Allow clicking completed steps to jump (default true)
 * @return {Object} Wizard controller with $el property (native DOM element)
 *
 * Each step: { id, label, icon, build(container, ctx, callbacks), optional, skip }
 * build() may return { validate: fn, teardown: fn }
 *   validate - called before advancing (return true to allow)
 *   teardown - called before navigating away (clean up event listeners, etc.)
 * skip(ctx) - if provided and returns true, the step is hidden and skipped during navigation
 */
import { el, getval, toNode } from '../core/dom_helpers.js';

export function wizard(options) {
  const steps = options.steps || [];
  const ctx = options.context || {};
  let currentIndex = options.startAt || 0;
  const allowJump = options.allowJump !== false;
  const stepStates = [];
  const stepValidators = [];
  const stepSummaries = [];
  let highestVisited = currentIndex;
  let _activeStepTeardown = null;

  function isSkipped(idx) {
    return steps[idx].skip && steps[idx].skip(ctx);
  }

  for (let i = 0; i < steps.length; i++) {
    stepStates.push(i < currentIndex ? 'completed' : (i === currentIndex ? 'active' : 'upcoming'));
    stepValidators.push(null);
    stepSummaries.push('');
  }

  const progress = el('div', {class: 'wizard-progress'});
  const content = el('div', {class: 'wizard-step-content'});
  const btnBack = el('button', {'data-icon': 'return'}, 'Back');
  const btnNext = el('button', {class: 'save'}, 'Next');
  const btnSkip = el('button', null, 'Skip');
  const btnCancel = el('button', {class: 'cancel'}, 'Cancel');
  const nav = el('div', {class: 'wizard-nav'}, [btnCancel, btnBack, btnSkip, btnNext]);

  const children = [progress, content];
  if (options.footer) children.push(options.footer);
  children.push(nav);
  const container = el('div', {class: 'wizard-container'}, children);

  let stepCircles = [];
  let stepLabelEls = [];
  let stepSummaryEls = [];
  let connectors = [];

  function buildProgressBar() {
    progress.innerHTML = '';
    stepCircles = [];
    stepLabelEls = [];
    stepSummaryEls = [];
    connectors = [];

    for (let i = 0; i < steps.length; i++) {
      if (i > 0) {
        const conn = el('div', {class: 'wizard-connector'});
        if (stepStates[i - 1] === 'completed' || stepStates[i] === 'completed' || stepStates[i] === 'active') {
          conn.classList.add('completed');
        }
        connectors.push(conn);
        progress.appendChild(conn);
      }

      const circle = el('div', {class: 'wizard-step-circle'});
      if (stepStates[i] === 'completed') {
        circle.setAttribute('data-icon', 'check');
      } else if (steps[i].icon) {
        circle.setAttribute('data-icon', steps[i].icon);
      } else {
        circle.textContent = i + 1;
      }

      const label = el('div', {class: 'wizard-step-label'}, steps[i].label);
      const summary = el('div', {class: 'wizard-step-summary'}, stepSummaries[i]);
      const step = el('div', {class: 'wizard-step ' + stepStates[i]}, [circle, label, summary]);

      stepCircles.push(circle);
      stepLabelEls.push(label);
      stepSummaryEls.push(summary);

      (function(idx) {
        step.addEventListener('click', function() {
          if (allowJump && canJumpTo(idx)) {
            goToStep(idx);
          }
        });
      })(i);

      progress.appendChild(step);
    }
  }

  function updateProgressStates() {
    for (let i = 0; i < steps.length; i++) {
      const stepEl = stepCircles[i].parentElement;
      const skipped = isSkipped(i);
      stepEl.style.display = skipped ? 'none' : '';

      stepEl.classList.remove('completed', 'active', 'upcoming', 'reachable');
      stepEl.classList.add(stepStates[i]);
      if (allowJump && stepStates[i] !== 'active' && canJumpTo(i)) {
        stepEl.classList.add('reachable');
      }

      stepCircles[i].removeAttribute('data-icon');
      stepCircles[i].innerHTML = '';
      if (stepStates[i] === 'completed') {
        stepCircles[i].setAttribute('data-icon', 'check');
      } else if (steps[i].icon) {
        stepCircles[i].setAttribute('data-icon', steps[i].icon);
      } else {
        stepCircles[i].textContent = i + 1;
      }

      stepSummaryEls[i].textContent = stepSummaries[i];
    }

    // Hide all connectors, then show one between each pair of consecutive visible steps
    for (let j = 0; j < connectors.length; j++) {
      connectors[j].style.display = 'none';
    }
    var prevVisible = -1;
    for (let i = 0; i < steps.length; i++) {
      if (isSkipped(i)) continue;
      if (prevVisible >= 0 && connectors[prevVisible]) {
        connectors[prevVisible].style.display = '';
        var shouldComplete = stepStates[prevVisible] === 'completed' || stepStates[i] === 'completed' || stepStates[i] === 'active';
        connectors[prevVisible].classList.toggle('completed', shouldComplete);
      }
      prevVisible = i;
    }
  }

  function commitPointers() {
    try {
      const settings = content.querySelectorAll('.isSetting');
      for (let i = 0; i < settings.length; i++) {
        const setting = settings[i];
        const pointer = setting._pointer;
        if (!pointer || !pointer.main) continue;
        const val = getval(setting);
        if (val === '' || val === null) {
          const opts = setting._opts;
          pointer.main[pointer.index] = (opts && 'default' in opts) ? opts['default'] : null;
        } else {
          pointer.main[pointer.index] = val;
        }
      }
    } catch(e) {
      console.warn('commitPointers:', e);
    }
  }

  function buildStep(idx) {
    commitPointers();
    if (_activeStepTeardown) { _activeStepTeardown(); _activeStepTeardown = null; }
    content.innerHTML = '';
    stepValidators[idx] = null;

    let nextEnabled = true;
    const callbacks = {
      markValid: function(valid) {
        nextEnabled = valid;
        btnNext.disabled = !valid;
      },
      setStepSummary: function(text) {
        stepSummaries[idx] = text || '';
        if (stepSummaryEls[idx]) stepSummaryEls[idx].textContent = stepSummaries[idx];
      },
      goTo: function(stepId) {
        for (let s = 0; s < steps.length; s++) {
          if (steps[s].id === stepId) {
            goToStep(s);
            return;
          }
        }
      },
      getCompletedSummaries: function() {
        const result = [];
        for (let s = 0; s < steps.length && s < idx; s++) {
          if (isSkipped(s)) continue;
          result.push({
            id: steps[s].id,
            label: steps[s].label,
            icon: steps[s].icon,
            summary: stepSummaries[s] || ''
          });
        }
        return result;
      }
    };

    const result = steps[idx].build(content, ctx, callbacks);
    if (result && typeof result.validate === 'function') {
      stepValidators[idx] = result.validate;
    }
    if (result && typeof result.teardown === 'function') {
      _activeStepTeardown = result.teardown;
    }

    updateNav();
    btnNext.disabled = !nextEnabled;
    if (options.onStepBuilt) options.onStepBuilt(ctx, steps[idx], idx);
  }

  function updateNav() {
    btnBack.hidden = currentIndex <= 0;
    btnSkip.hidden = !(steps[currentIndex] && steps[currentIndex].optional);

    let isLast = currentIndex === steps.length - 1;
    if (!isLast) {
      let allAfterSkipped = true;
      for (let i = currentIndex + 1; i < steps.length; i++) {
        if (!isSkipped(i)) { allAfterSkipped = false; break; }
      }
      if (allAfterSkipped) isLast = true;
    }
    btnNext.textContent = isLast ? 'Save' : 'Next';
    btnCancel.hidden = currentIndex !== 0;
  }

  function canJumpTo(idx) {
    if (isSkipped(idx)) return false;
    if (idx === currentIndex) return true;
    if (idx < currentIndex) return true;
    if (idx <= highestVisited) return true;
    for (let i = 0; i < idx; i++) {
      if (i === currentIndex) continue;
      if (isSkipped(i)) continue;
      if (!steps[i].optional && stepStates[i] !== 'completed') return false;
    }
    return true;
  }

  function validateCurrentStep() {
    commitPointers();
    const validator = stepValidators[currentIndex];
    if (validator && !validator()) return false;
    return true;
  }

  function goToStep(idx) {
    if (idx < 0 || idx >= steps.length) return;
    if (isSkipped(idx)) return;
    if (idx > currentIndex && !validateCurrentStep()) return;

    stepStates[currentIndex] = 'completed';
    currentIndex = idx;
    if (idx > highestVisited) highestVisited = idx;

    for (let i = 0; i < steps.length; i++) {
      if (i < currentIndex && stepStates[i] !== 'completed') {
        stepStates[i] = 'completed';
      } else if (i === currentIndex) {
        stepStates[i] = 'active';
      } else if (i > currentIndex && stepStates[i] !== 'completed') {
        stepStates[i] = 'upcoming';
      }
    }

    updateProgressStates();
    buildStep(currentIndex);
  }

  function advance() {
    if (!validateCurrentStep()) return;

    stepStates[currentIndex] = 'completed';

    if (currentIndex === steps.length - 1) {
      if (options.onComplete) options.onComplete(ctx);
      return;
    }

    let next = currentIndex + 1;
    while (next < steps.length - 1 && isSkipped(next)) {
      stepStates[next] = 'completed';
      next++;
    }
    currentIndex = next;
    if (currentIndex > highestVisited) highestVisited = currentIndex;
    stepStates[currentIndex] = 'active';
    updateProgressStates();
    buildStep(currentIndex);
  }

  function goBack() {
    if (currentIndex <= 0) return;
    stepStates[currentIndex] = 'completed';
    let prev = currentIndex - 1;
    while (prev > 0 && isSkipped(prev)) {
      prev--;
    }
    currentIndex = prev;
    stepStates[currentIndex] = 'active';
    updateProgressStates();
    buildStep(currentIndex);
  }

  btnNext.addEventListener('click', advance);
  btnBack.addEventListener('click', goBack);
  btnSkip.addEventListener('click', function() {
    if (currentIndex < steps.length - 1) {
      stepStates[currentIndex] = 'completed';
      let next = currentIndex + 1;
      while (next < steps.length - 1 && isSkipped(next)) {
        stepStates[next] = 'completed';
        next++;
      }
      currentIndex = next;
      if (currentIndex > highestVisited) highestVisited = currentIndex;
      stepStates[currentIndex] = 'active';
      updateProgressStates();
      buildStep(currentIndex);
    }
  });
  btnCancel.addEventListener('click', function() {
    if (options.onCancel) options.onCancel();
  });

  content.addEventListener('keydown', function(e) {
    if (e.key === 'Enter' && e.target.tagName !== 'BUTTON' && e.target.tagName !== 'TEXTAREA' && !e.target.closest('.card-picker')) {
      if (!btnNext.disabled) {
        e.preventDefault();
        e.stopPropagation();
        advance();
      }
    }
  });

  buildProgressBar();
  updateProgressStates();
  buildStep(currentIndex);

  return {
    $el: container,
    goTo: function(stepId) {
      for (let s = 0; s < steps.length; s++) {
        if (steps[s].id === stepId) {
          goToStep(s);
          return;
        }
      }
    },
    getCurrentStep: function() { return steps[currentIndex]; },
    getCurrentIndex: function() { return currentIndex; },
    markAllCompleted: function() {
      for (let i = 0; i < steps.length; i++) {
        if (i !== currentIndex) stepStates[i] = 'completed';
      }
      highestVisited = steps.length - 1;
      updateProgressStates();
    }
  };
}

/**
 * Shared bootstrap for mounting wizards inline or in a modal.
 * @param {Object} options
 * @param {Array} options.steps
 * @param {Object} options.context
 * @param {Function} options.onComplete
 * @param {Function=} options.onCancel
 * @param {boolean=} options.allowJump
 * @param {boolean=} options.markAllCompleted
 * @param {Element=} options.mount  Mount element (native element or wrapped collection)
 * @param {string=} options.wrapperClass
 * @param {Object=} options.modal  Result from uiHelpers.openFormModal/openModal
 * @return {{wizard:Object, $el:Element}}
 */
export function mountWizard(options) {
  options = options || {};



  const wiz = wizard({
    steps: options.steps || [],
    context: options.context || {},
    allowJump: options.allowJump,
    startAt: options.startAt || 0,
    onComplete: options.onComplete,
    onCancel: options.onCancel,
    footer: options.footer || null,
    onStepBuilt: options.onStepBuilt || null
  });

  if (options.markAllCompleted) {
    wiz.markAllCompleted();
  }

  if (options.modal) {
    options.modal.setBody(wiz.$el);
    const progressEl = options.modal.element.querySelector('.wizard-progress');
    const closeEl = options.modal.element.querySelector(':scope > .close');
    if (progressEl && closeEl) {
      closeEl.classList.add('wizard-popup-close');
      progressEl.appendChild(closeEl);
    }
  } else if (options.mount) {
    const mountEl = toNode(options.mount);
    if (mountEl) {
      if (options.wrapperClass) {
        const wrapper = el('div', {class: options.wrapperClass}, wiz.$el);
        mountEl.appendChild(wrapper);
      } else {
        mountEl.appendChild(wiz.$el);
      }
    }
  }

  return { wizard: wiz, $el: wiz.$el };
}
