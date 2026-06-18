import { APP_NAME } from '@brand';
import { uiHelpers } from '../core/ui_helpers.js';
import * as format from '../core/formatters.js';
import { apiClient } from '../core/api_client.js';
import { formEngine } from '../core/form_engine.js';
import { el } from '../core/dom_helpers.js';
import { defineSection } from '../core/section_registry.js';

defineSection({
  id: 'general/variables',
  page: 'General',
  title: 'Custom Variables',
  subtitle: 'Reusable values for stream sources, push targets, and file paths',
  keywords: ['variable', 'placeholder', 'substitution', 'dynamic value', 'command', 'interval', 'variable_list'],
  requires: ['config'],
  navTo: { tab: 'General', other: '' }
});

function buildEmptyState() {
  return el('div', {class: 'variables-empty-state'}, [
    el('span', {'data-icon': 'variable'}),
    el('h3', {}, 'No custom variables yet'),
    el('p', {}, 'Create one to reuse values in stream sources, push targets, and file paths.')
  ]);
}

function normalizeVariableEntry(raw) {
  const out = {
    value: '',
    target: '',
    interval: 0,
    lastunix: 0,
    waitTime: 0
  };

  if (raw && (typeof raw === 'object') && !Array.isArray(raw)) {
    if (Object.prototype.hasOwnProperty.call(raw, 'value')) {
      out.value = raw.value;
    } else {
      out.value = '';
    }
    if (typeof raw.target === 'string') { out.target = raw.target; }
    if (!isNaN(Number(raw.interval))) { out.interval = Number(raw.interval); }
    if (!isNaN(Number(raw.lastunix))) { out.lastunix = Number(raw.lastunix); }
    if (!isNaN(Number(raw.waitTime))) { out.waitTime = Number(raw.waitTime); }
    return out;
  }

  if (Array.isArray(raw)) {
    // Legacy/fallback compatibility for array-shaped variable values.
    if (raw.length > 0) { out.value = raw[0]; }
    if ((raw.length > 1) && (typeof raw[1] === 'string')) { out.target = raw[1]; }
    if ((raw.length > 2) && !isNaN(Number(raw[2]))) { out.interval = Number(raw[2]); }
    if ((raw.length > 3) && !isNaN(Number(raw[3]))) { out.lastunix = Number(raw[3]); }
    if ((raw.length > 4) && !isNaN(Number(raw[4]))) { out.waitTime = Number(raw[4]); }
    return out;
  }

  // Primitive values are treated as static variables.
  out.value = raw;
  return out;
}

function formatVariableValue(value) {
  if (typeof value === 'undefined' || value === null) { return ''; }
  if (typeof value === 'string') { return value; }
  try {
    return JSON.stringify(value);
  } catch (e) {
    return String(value);
  }
}

function renderTable($container, opts) {
  opts = opts || {};
  $container.innerHTML = 'Loading..';

  apiClient.send(function(d) {
    if (!d.variable_list || !Object.keys(d.variable_list).length) {
      $container.innerHTML = '';
      $container.appendChild(buildEmptyState());
      return;
    }

    const tbody = el('tbody');
    const table = el('table', {}, [
      el('thead', {},
        el('tr', {}, [
          el('th', {}, 'Variable'),
          el('th', {}, 'Latest value'),
          el('th', {}, 'Command or url'),
          el('th', {}, 'Check interval'),
          el('th', {}, 'Last checked'),
          el('th')
        ])
      ),
      tbody
    ]);

    $container.innerHTML = '';
    const scrollWrap = el('div', {class: 'table-scroll'});
    scrollWrap.appendChild(table);
    $container.appendChild(scrollWrap);

    let rowCount = 0;
    for (const i in d.variable_list) {
      try {
        const v = normalizeVariableEntry(d.variable_list[i]);
        const hasTarget = (typeof v.target === 'string') && (v.target.length > 0);

        const codeCell = el('td');
        codeCell.appendChild(
          el('code', {},
            hasTarget
              ? (v.lastunix > 0 ? formatVariableValue(v.value) : '')
              : formatVariableValue(v.value)
          )
        );

        const targetCell = el('td');
        if (hasTarget) {
          if (/^https?:\/\//i.test(v.target)) {
            targetCell.appendChild(
              el('a', { target: '_blank', href: v.target }, v.target)
            );
          } else {
            targetCell.textContent = v.target;
          }
        }

        const lastCheckedCell = el('td', {
          title: hasTarget
            ? (v.lastunix > 0 ? 'At ' + format.dateTime(v.lastunix, 'long') : 'Not yet')
            : ''
        });
        if (hasTarget) {
          lastCheckedCell.innerHTML = v.lastunix > 0
            ? format.duration(new Date().getTime() * 1e-3 - v.lastunix) + ' ago'
            : 'Not yet';
        }

        tbody.appendChild(
          el('tr', { class: 'variable', 'data-name': i }, [
            el('td', {}, '$' + i),
            codeCell,
            targetCell,
            el('td', {},
              hasTarget ? (v.interval == 0 ? 'Once' : format.duration(v.interval)) : 'Never'
            ),
            lastCheckedCell,
            el('td', {}, [
              el('button', {
                onclick: function() {
                  const name = this.closest('tr').getAttribute('data-name');
                  editPopup(name, {
                    onSave: function() {
                      if (opts.onMutate) opts.onMutate();
                      else renderTable($container, opts);
                    }
                  });
                }
              }, 'Edit'),
              el('button', {
                onclick: function() {
                  const name = this.closest('tr').getAttribute('data-name');
                  if (confirm('Are you sure you want to remove the custom variable $' + name + '?')) {
                    apiClient.send(function() {
                      if (opts.onMutate) opts.onMutate();
                      else renderTable($container, opts);
                    }, { variable_remove: name });
                  }
                }
              }, 'Remove')
            ])
          ])
        );
        rowCount++;
      } catch (e) {
        console.warn('Failed to render custom variable', i, e);
      }
    }

    if (!rowCount) {
      $container.innerHTML = '';
      $container.appendChild(buildEmptyState());
    }
  }, { variable_list: true });
}

function editPopup(varName, opts) {
  opts = opts || {};
  const editing = (varName != '' && varName != null);
  let modal;

  function build(saveas, saveas_dyn) {
    const dynamicinputs = el('div');
    modal = uiHelpers.openFormModal({
      size: 'sm',
      title: editing ? 'Edit Variable "$' + varName + '"' : 'New Variable',
      form: [
      {
        type: 'str',
        maxlength: 31,
        label: 'Variable name',
        prefix: '$',
        help: 'What should the variable be called? A dollar sign will automatically be prepended.',
        pointer: { main: saveas, index: 'name' },
        validate: ['required', function(val) {
          if (val.length && (val[0] == '$')) {
            return {
              msg: 'The dollar sign will automatically be prepended. You don\'t need to type it here.',
              classes: ['red']
            };
          }
          if ((val.indexOf('{') !== -1) || (val.indexOf('}') !== -1) || (val.indexOf('$') !== -1)) {
            return {
              msg: 'The following symbols are not permitted: "$ { }".',
              classes: ['red']
            };
          }
        }]
      }, {
        type: 'select',
        label: 'Type',
        help: 'It can either be a static value or a dynamic one returned by a command or url.',
        select: [
          ['value', 'Static value'],
          ['command', 'Dynamic through command or url']
        ],
        value: 'value',
        pointer: { main: saveas_dyn, index: 'type' },
        'function': function() {
          let b = [el('span', {}, 'Invalid variable type')];
          switch (this.value) {
            case 'value': {
              b = [{
                type: 'str',
                label: 'Value',
                pointer: { main: saveas_dyn, index: 'value' },
                help: 'The static value that this variable should be replaced with. There is a character limit of 63 characters.',
                validate: ['required']
              }];
              break;
            }
            case 'command': {
              b = [{
                type: 'str',
                label: 'Command',
                help: 'The command that should be executed or the url that should be downloaded to retrieve the value for this variable.<br>For example:<br><code>/usr/bin/date +%A</code><br>There is a character limit of 511 characters.',
                validate: ['required'],
                pointer: { main: saveas_dyn, index: 'target' }
              }, {
                type: 'int',
                min: 0, max: 4294967295, step: 1e-3, 'default': 0,
                label: 'Checking interval',
                unit: 's',
                help: 'At what interval, in seconds, '+APP_NAME+' should execute the command and update the value.<br>To execute the command once when '+APP_NAME+' starts up (and then never update), set the interval to 0.',
                pointer: { main: saveas_dyn, index: 'interval' }
              }, {
                type: 'int',
                min: 0, max: 4294967295, step: 1e-3, 'default': 1,
                label: 'Wait time',
                unit: 's',
                help: 'Specifies the maximum time, in seconds, '+APP_NAME+' should wait for data when executing the variable target. If set to 0 this variable takes on the same value as the interval.<br>'+APP_NAME+' only updates one variable at a time, so setting this value too high can block other variables from updating.',
                pointer: { main: saveas_dyn, index: 'waitTime' }
              }];
              break;
            }
          }
          dynamicinputs.innerHTML = '';
          dynamicinputs.appendChild(formEngine.buildUI(b));
        }
      },
      dynamicinputs
      ],
      buttons: [{
        type: 'cancel',
        label: 'Cancel'
      }, {
        type: 'save',
        label: 'Save',
        'function': function() {
          const o = { variable_add: saveas };

          switch (saveas_dyn.type) {
            case 'value': {
              saveas.value = saveas_dyn.value;
              break;
            }
            case 'command': {
              saveas.target = saveas_dyn.target;
              saveas.interval = saveas_dyn.interval;
              saveas.waitTime = saveas_dyn.waitTime;
              break;
            }
          }

          if (saveas.name != varName) {
            o.variable_remove = varName;
          }
          apiClient.send(function() {
            modal.close();
            if (opts.onSave) opts.onSave();
          }, o);
        }
      }]
    });
  }

  if (!editing) {
    build({}, {});
  } else {
    apiClient.send(function(d) {
      if (varName in d.variable_list) {
        const v = d.variable_list[varName];
        v.type = 'target' in v ? 'command' : 'value';
        build({ name: varName }, v);
      } else {
        modal = uiHelpers.openConfirm({
          size: 'sm',
          title: 'Error',
          message: 'Variable "$' + varName + '" does not exist.',
          actions: [{ type: 'cancel', label: 'Close' }]
        });
      }
    }, { variable_list: true });
  }
}

export const variables = {
  renderTable: renderTable,
  editPopup: editPopup
};
