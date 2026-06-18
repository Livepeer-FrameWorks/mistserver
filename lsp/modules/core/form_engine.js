import { APP_NAME } from '@brand';
import { uiHelpers } from './ui_helpers.js';
import * as format from './formatters.js';
import { uiCore } from './ui_core.js';
import { copy } from './clipboard.js';
import { apiClient } from './api_client.js';
import { mistHelpers } from './mist_helpers.js';
import { constants } from './constants.js';
import { el, getval, setval } from './dom_helpers.js';
import { MD5 } from '../../plugins/md5.js';
import { renderQRCode } from '../../plugins/qrcode.js';

export const formEngine = {
buildUI: function(elements){
  /*elements should be an array of objects, the objects containing the UI element options
   * (or a DOM element that will be inserted instead).

  element options:
    {
      label: 'Username',                                      //label to display in front of the field
      type: 'str',                                           //type of input field
      pointer: {main: mist.user, index: 'name'},              //pointer to the value this input field controls
      help: 'You should enter your username here.',           //what should be displayed in the integrated help balloon
      validate: ['required',function(){}]                     //how the input should be validated
    }
  */

  const c = el('div', {class: 'input_container'});
  for (const i in elements) {
    let e = elements[i];
    if ((e === null) || (e === false)) { continue; }
    if (e instanceof HTMLElement) {
      c.appendChild(e);
      continue;
    }
    if (e.type == 'help') {
      if ((typeof e.help == "undefined") || (e.help === null)) { continue; }
      if (typeof e.help == "string") {
        const tmp = el('div');
        tmp.innerHTML = e.help;
        if (tmp.textContent.trim() == "") { continue; }
      }
      if ((e.help instanceof HTMLElement) && (!e.help.parentNode)) { continue; }
      const s = el('span', {class: 'text_container'});
      const descSpan = el('span', {class: 'description'});
      if (typeof e.help === 'string') {
        descSpan.innerHTML = e.help;
      } else if (e.help instanceof HTMLElement) {
        descSpan.appendChild(e.help);
      } else {
        descSpan.innerHTML = e.help;
      }
      s.appendChild(descSpan);
        c.appendChild(s);
        if ('classes' in e) {
          for (const j in e.classes) {
            const cls = e.classes[j];
            if (cls) {
              s.classList.add(cls);
            }
          }
        }
      if ("dependent" in e) {
        for (const i in e.dependent) {
          if (typeof e.dependent[i] == "string") e.dependent[i] = [e.dependent[i]];
          s.setAttribute("data-dependent-"+i,"'"+e.dependent[i].join("' '")+"'");
        }
      }
      if ("dependent_not" in e) {
        for (const i in e.dependent_not) {
          if (typeof e.dependent_not[i] == "string") e.dependent_not[i] = [e.dependent_not[i]]
          s.setAttribute("data-dependent-not-"+i,"'"+e.dependent_not[i].join("' '")+"'");
        }
      }
      continue;
    }
    if (e.type == 'text') {
      const textSpan = el('span', {class: 'text_container'});
      const innerSpan = el('span', {class: 'text'});
      if (typeof e.text === 'string') {
        innerSpan.innerHTML = e.text;
      } else if (e.text instanceof HTMLElement) {
        innerSpan.appendChild(e.text);
      } else {
        innerSpan.innerHTML = e.text;
      }
      textSpan.appendChild(innerSpan);
      c.appendChild(textSpan);
      continue;
    }

    if (e.type == 'buttons') {
      const bc = el('span', {class: 'button_container'});
      bc.addEventListener('keydown', function(e){
        e.stopPropagation();
      });
      if ('css' in e) {
        Object.assign(bc.style, e.css);
      }
      c.appendChild(bc);
      for (const j in e.buttons) {
        const button = e.buttons[j];
        if (!button) continue;
        const b = el('button', null, button.label);
        b._opts = button;
        if ('css' in button) {
          Object.assign(b.style, button.css);
        }
        if ('classes' in button) {
          for (const k in button.classes) {
            const cls = button.classes[k];
            if (cls) {
              b.classList.add(cls);
            }
          }
        }
        bc.appendChild(b);
        switch (button.type) {
          case 'cancel':
            b.classList.add('cancel');
            b.setAttribute("data-icon","cross");
            b.addEventListener('click', button['function']);
            break;
          case 'save': {
            b.classList.add('save');
            b.setAttribute("data-icon","check");
            b._save = function(skipValidation){
              const fn = this._opts['preSave'];
              if (fn) { fn.call(this); }

              const ic = this.closest('.input_container');

              const collapsed = ic.querySelectorAll(".itemgroup:not(:has(.expanded))");
              collapsed.forEach(function(g){ g.classList.add("expanded"); });

              if (!skipValidation) {
                let error = false;
                const visibleValidate = ic.querySelectorAll('.hasValidate, input[type="hidden"].hasValidate');
                for (let vi = 0; vi < visibleValidate.length; vi++) {
                  const vEl = visibleValidate[vi];
                  if (vEl.type !== 'hidden' && !vEl.checkVisibility()) continue;
                  const vf = vEl._validate;
                  error = vf(vEl, true);
                  if (error) {
                    error = vEl;
                    break;
                  }
                }
                const fn = this._opts['failedValidate'];
                if (fn) { fn.call(this); }
                if (error) {
                  collapsed.forEach(function(g){ g.classList.remove("expanded"); });
                  const focus = error.closest(".itemgroup");
                  if (focus) { focus.classList.add("expanded") }

                  return;
                }
              }

              const visibleSettings = ic.querySelectorAll('.isSetting, input[type="hidden"].isSetting');
              for (let vi = 0; vi < visibleSettings.length; vi++) {
                const settingEl = visibleSettings[vi];
                if (settingEl.type !== 'hidden' && !settingEl.checkVisibility()) continue;
                let val = getval(settingEl);
                const pointer = settingEl._pointer;
                let opts = settingEl._opts;

                if ((val === '') || (val === null)) {
                  if ('default' in opts) {
                    val = opts['default'];
                  }
                  else if (!("keepnull" in opts) || !opts.keepnull)  {
                    pointer.main[pointer.index] = null;
                    continue;
                  }
                }

                pointer.main[pointer.index] = val;

                const fn = settingEl._opts['postSave'];
                if (fn) { fn.call(settingEl); }
              }

              collapsed.forEach(function(g){ g.classList.remove("expanded"); });

              return true;
            };
            b.addEventListener('click', function(e){
              let success;
              {
                const fn = this._save;
                if (fn) { success = fn.call(this); }
              }

              if (!success) {
                return;
              }

              const fn = this._opts['function'];
              if (fn) { fn(this); }
            });
            break;
          }
          default:
            b.addEventListener('click', button['function']);
            break;
        }
        if ("icon" in button) {
          b.setAttribute("data-icon",button.icon);
        }
      }
      continue;
    }

    const $e = el('label', {class: 'UIelement'});
    c.appendChild($e);

    if ('css' in e) {
      Object.assign($e.style, e.css);
    }

    const labelSpan = el('span', {class: 'label'});
    labelSpan.innerHTML = ('label' in e ? e.label+':' : '');
    $e.appendChild(labelSpan);
    if ('classes' in e) {
      for (const k in e.classes) {
        const cls = e.classes[k];
        if (cls) {
          $e.classList.add(cls);
        }
      }
    }

    const fc = el('span', {class: 'field_container'});
    $e.appendChild(fc);
    let $field;
    switch (e.type) {
      case 'password':
        $field = el('input', {type: 'password'});
        break;
      case 'int':
        $field = el('input', {type: 'number'});
        if ('min' in e) {
          $field.setAttribute('min', e.min);
        }
        if ('max' in e) {
          $field.setAttribute('max', e.max);
        }
        if (!('step' in e)) { e.step = 1; }
        $field.setAttribute('step', e.step);

        if ('validate' in e) {
          e.validate.push('int');
        }
        else {
          e.validate = ['int'];
        }
        break;
      case 'span':
        $field = el('span');
        break;
      case 'debug':
        e.select = [
          ['','Default'],
          [0,'0 - NONE: All debugging messages disabled'],
          [1,'1 - FAIL: Messages about failed operations'],
          [2,'2 - ERROR: Previous level, and error messages'],
          [3,'3 - WARN: Previous level, and warning messages'],
          [4,'4 - INFO: Previous level, and status messages for development'],
          [5,'5 - MEDIUM: Previous level, and more status messages for development'],
          [6,'6 - HIGH: Previous level, and verbose debugging messages'],
          [7,'7 - VERY HIGH: Previous level, and very verbose debugging messages'],
          [8,'8 - EXTREME: Report everything in extreme detail'],
          [9,'9 - INSANE: Report everything in insane detail'],
          [10,'10 - DONTEVEN: All messages enabled']
        ];
      case 'select':
        $field = el('select');
        for (const j in e.select) {
          const option = el('option');
          if (typeof e.select[j] == 'string') {
            option.textContent = e.select[j];
          }
          else {
            option.value = e.select[j][0];
            option.textContent = e.select[j][1];
          }
          $field.appendChild(option);
        }
        break;
      case 'textarea':
        $field = el('textarea');
        $field.addEventListener('keydown', function(e){
          e.stopPropagation();
        });
        break;
      case 'checkbox':
        $field = el('input', {type: 'checkbox'});
        break;
      case 'hidden':
        $field = el('input', {type: 'hidden'});
        $e.hidden = true;
        break;
      case 'email':
        $field = el('input', {type: 'email', autocomplete: 'on', required: ''});
        break;
      case 'browse':
        $field = el('input', {type: 'text'});
        if ('filetypes' in e) {
          $field._filetypes = e.filetypes;
        }
        break;
      case 'geolimited':
      case 'hostlimited':
        $field = el('input', {type: 'hidden'});
        break;
      case 'radioselect':
        $field = el('div', {class: 'radioselect'});
        for (const i in e.radioselect) {
          const radio = el('input', {type: 'radio', name: e.label});
          radio.value = e.radioselect[i][0];
          if (e.readonly) {
            radio.disabled = true;
          }
          const radioLabel = el('label');
          radioLabel.appendChild(radio);
          const radioSpan = el('span');
          radioSpan.innerHTML = e.radioselect[i][1];
          radioLabel.appendChild(radioSpan);
          $field.appendChild(radioLabel);
          if (e.radioselect[i].length > 2) {
            const sel = el('select');
            sel.addEventListener('change', function(){
              const r = this.parentNode.querySelector('input[type=radio]:enabled');
              if (r) r.checked = true;
            });
            radioLabel.appendChild(sel);
            if (e.readonly) {
              sel.disabled = true;
            }
            for (const j in e.radioselect[i][2]) {
              const opt = el('option');
              if (e.radioselect[i][2][j] instanceof Array) {
                opt.value = e.radioselect[i][2][j][0];
                opt.innerHTML = e.radioselect[i][2][j][1];
              }
              else {
                opt.innerHTML = e.radioselect[i][2][j];
              }
              sel.appendChild(opt);
            }
          }
        }
        break;
      case 'checklist':
        {$field = el('div', {class: 'checkcontainer'});
        const controls = el('div', {class: 'controls'});
        const checklist = el('div', {class: 'checklist'});
        $field.appendChild(checklist);
        for (const i in e.checklist) {
          if (typeof e.checklist[i] == 'string') {
            e.checklist[i] = [e.checklist[i], e.checklist[i]];
          }
          const checkLabel = el('label');
          checkLabel.textContent = e.checklist[i][1];
          const checkInput = el('input', {type: 'checkbox', name: e.checklist[i][0]});
          checkLabel.insertBefore(checkInput, checkLabel.firstChild);
          checklist.appendChild(checkLabel);
        }
        break;
      }case 'DOMfield':
        $field = e.DOMfield;
        break;
      case "unix":
        $field = el("input", {type: "datetime-local", step: "1"});
        e.unit = el("button");
        e.unit.textContent = "Now";
        e.unit.addEventListener('click', function(){
          const fieldEl = this.closest(".field_container").querySelector(".field");
          setval(fieldEl, (new Date()).getTime()/1e3);
        });
        break;
      case "selectinput":
        {$field = el('div', {class: 'selectinput'});
        const selInp = el("select");
        $field.appendChild(selInp);
        selInp._input = false;

        for (const i in e.selectinput) {
          const opt = el("option");
          selInp.appendChild(opt);
          if (typeof e.selectinput[i] == "string") {
            opt.textContent = e.selectinput[i];
          }
          else {
            opt.textContent = e.selectinput[i][1];
            if (typeof e.selectinput[i][0] == "string") {
              opt.value = e.selectinput[i][0];
            }
            else {
              opt.value = "CUSTOM";
              const builtChildren = formEngine.buildUI([e.selectinput[i][0]]);
              selInp._input = builtChildren[0] ? builtChildren[0].children : builtChildren.children ? Array.from(builtChildren.children) : [];
              if (selInp._input instanceof HTMLCollection) selInp._input = Array.from(selInp._input);
              for (let ci = 0; ci < selInp._input.length; ci++) {
                $field.appendChild(selInp._input[ci]);
              }
            }
          }
        }
        selInp.addEventListener('change', function(){
          let inputs = this._input;
          if (!inputs) return;
          if (inputs instanceof HTMLElement) inputs = [inputs];
          for (let ci = 0; ci < inputs.length; ci++) {
            inputs[ci].hidden = (this.value != "CUSTOM");
          }
        });
        selInp.dispatchEvent(new Event('change', {bubbles: true}));
        break;
      }case "inputlist":
        function createField(e) {
          let $field = el('div', {class: 'inputlist'});
          let context_menu = new uiCore.context_menu();
          fc.appendChild(context_menu.ele);
          const newitem = function(){
            let part;
            if ("input" in e) {
              let i = e.input;
              if (typeof i == "function") i = i();
              let built = formEngine.buildUI([i]);
              part = (built[0] || built).querySelector(".field_container");
              const fields = part.querySelectorAll(".field");
              fields.forEach(function(f){
                f._help_container = function(){ return $field._help_container; };
              });
            }
            else {
              const o = Object.assign({},e);
              delete o.validate;
              delete o.pointer;
              o.type = "str";
              let built = formEngine.buildUI([o]);
              part = (built[0] || built).querySelector(".field_container");
            }
            part.classList.remove("isSetting");
            part.classList.add("listitem");

            const keyup = function(e){
              let item = this.querySelector(".field");
              if (!this.nextElementSibling) {
                if (item && getval(item)) {
                  this.after(newitem());
                }
                else if (e.which == 8) {
                  const prev = this.previousElementSibling;
                  if (prev) {
                    const prevField = prev.querySelector(".field");
                    if (prevField) prevField.focus();
                  }
                }
              }
              else {
                if (!item || !getval(item)) {
                  let f = this.previousElementSibling;
                  if (!f) {
                    f = this.nextElementSibling;
                  }
                  const focusField = f ? f.querySelector(".field") : null;
                  if (focusField) focusField.focus();

                  let hc = item ? item._help_container : null;
                  if (hc) {
                    if (typeof hc == "function") hc = hc();
                    if (hc) {
                      const uid = item._uid;
                      const errBalloon = hc.querySelector('.err_balloon[data-uid="'+uid+'"]');
                      if (errBalloon) errBalloon.remove();
                    }
                  }

                  try {
                    if (this.parentNode) this.parentNode.removeChild(this);
                  } catch(ex) {
                  }

                  $field.querySelectorAll(".field").forEach(function(f){
                    f.dispatchEvent(new Event('change', {bubbles: true}));
                  });
                }
              }
            };

            part.addEventListener('keyup', keyup);
            part.addEventListener('change', keyup);

            part.addEventListener("contextmenu",function(e){
              let menu = [];
              let p = part;
              function findPart() {
                if (!p.parentNode) {
                  const index = p.getAttribute("data-index");
                  p = $field.querySelector('.listitem[data-index="'+index+'"]');
                }
              }
              if (part.previousElementSibling) {
                menu.push(["Move item up",function(){
                  findPart();
                  p.after(p.previousElementSibling);
                  if (p.nextElementSibling) p.nextElementSibling.dispatchEvent(new Event("change", {bubbles: true}));
                },"up"]);
              }
              if (part.nextElementSibling) {
                menu.push(["Move item down",function(){
                  findPart();
                  p.before(p.nextElementSibling);
                  p.dispatchEvent(new Event("change", {bubbles: true}));
                },"down"]);
              }
              if (menu.length) {
                menu.push(["Remove item",function(){
                  findPart();
                  p.remove();
                  const lastChild = $field.lastElementChild;
                  if (lastChild) lastChild.dispatchEvent(new Event("change", {bubbles: true}));
                },"trash"]);
              }
              if (part.nextElementSibling) {
                menu.splice(-1,0,["Add item",function(){
                  findPart();
                  p.after(newitem());
                  if (p.nextElementSibling) p.nextElementSibling.dispatchEvent(new Event("change", {bubbles: true}));
                },"plus"]);
              }
              if (menu.length) {
                e.preventDefault();

                const fieldEl = part.querySelector(".field");
                let val = fieldEl ? getval(fieldEl) : null;
                let str;
                try {
                  str = val.toString();
                }
                catch(e){
                  str = ""+val;
                }
                if (str == "[object Object]") str = JSON.stringify(val);
                else if (str == "null") str = "";
                if (str) {
                  let header = el("div", {class: "header"});
                  header.textContent = str;
                  context_menu.show([[header],menu],e);
                  return;
                }

                context_menu.show([menu],e);
              }
            });

            return part;
          };
          $field._newitem = newitem;

          $field.appendChild($field._newitem());

          return $field;
        }
        $field = createField(e);
        break;
      case "sublist": {
        $field = el("div", {class: "sublist"});
        let curvals = el("div", {class: "curvals"});
        curvals.appendChild(el("span", null, "None."));
        let itemsettings = el("div", {class: "itemsettings"});
        let newitembutton = el("button", null, "New "+e.itemLabel);
        let sublist = e.sublist;
        let local_e = e;
        let local_field = $field;
        let local_label = $e;
        let keep_null = sublist.filter((a)=>!!a.keepnull).map((a)=>a.pointer?.index);
        $field._build = function(values,index){
          const savepos = index;

          for (const i in local_e.saveas) {
            if (!(i in values)) {
              delete local_e.saveas[i];
            }
          }
          local_e.saveas = Object.assign(local_e.saveas,values);
          for (let f of sublist) {
            if (f.pointer) f.pointer.main = local_e.saveas;
          }

          let mode = "New";
          if (typeof index != "undefined") {
            mode = "Edit";
          }
          let modal;
          let modalTitle = mode+" "+local_e.itemLabel;
          modal = uiHelpers.openFormModal({
            size: "md",
            title: modalTitle,
            form: sublist,
            buttons: [{
              label: "Cancel",
              type: "cancel"
            }, {
              label: "Save "+local_e.itemLabel,
              type: "save",
              "function": function(){
                let savelist = getval(local_field);
                let save = Object.assign({},local_e.saveas);
                for (let i in save) {
                  if ((save[i] === null) && (keep_null.indexOf(i) == -1)) {
                    delete save[i];
                  }
                }
                if (typeof savepos == "undefined") {
                  savelist.push(save);
                }
                else {
                  savelist[savepos] = save;
                }
                setval(local_field, savelist);
                modal.close();
              }
            }]
          });
        };
        let sublistfield = $field;
        newitembutton.addEventListener('click', function(){
          sublistfield._build({});
        });
        if (!("hrn" in e)) {
          e.hrn = "x-LSP-name";
          sublist.unshift({
            type: "str",
            label: "Human readable name",
            placeholder: "none",
            help: "A convenient name to describe this "+e.itemLabel+". It won't be used by "+APP_NAME+".",
            pointer: {
              main: e.saveas,
              index: "x-LSP-name"
            }
          });
        }
        $field._savelist = [];
        $field.appendChild(curvals);
        $field.appendChild(newitembutton);
        break;
      }
      case "json": {
        $field = el("textarea");
        $field.addEventListener('keydown', function(e){
          e.stopPropagation();
        });
        $field.addEventListener('keyup', function(e){
          this.style.height = "";
          this.style.height = (this.scrollHeight ? this.scrollHeight + 20 : this.value.split("\n").length*14 + 20)+"px";
        });
        $field.addEventListener('change', function(e){
          this.style.height = "";
          this.style.height = (this.scrollHeight ? this.scrollHeight + 20 : this.value.split("\n").length*14 + 20)+"px";
        });
        $field.style.minHeight = "3em";
        let f = function (val,me){
          if (me.value == "") { return; }
          if (val === null) {
            return {
              msg: 'Invalid json',
              classes: ['red']
            }
          }
        };
        if ('validate' in e) {
          e.validate.push(f);
        }
        else {
          e.validate = [f];
        }
        break;
      }
      case "bitmask": {
        const bitmaskCount = Array.isArray(e.bitmask) ? e.bitmask.length : 0;
        let bitmaskClass = "bitmask";
        if (bitmaskCount > 0) {
          bitmaskClass += " bitmask--count-" + bitmaskCount;
          if (bitmaskCount % 2 === 0) {
            bitmaskClass += " bitmask--even";
          }
        }
        $field = el("div", {class: bitmaskClass});
        for (const i in e.bitmask) {
          const bmLabel = el("label");
          const bmInput = el("input", {
            type: "checkbox",
            name: "bitmask_"+("pointer" in e ? e.pointer.index : ""),
            value: e.bitmask[i][0],
            class: "field"
          });
          bmLabel.appendChild(bmInput);
          bmLabel.appendChild(el("span", null, e.bitmask[i][1]));
          $field.appendChild(bmLabel);
        }

        $e.setAttribute("for","none");
        break;
      }
      case "group":{
        let cont = el("div", {class: "itemgroup"});
        if ('classes' in e) {
          for (const j in e.classes) {
            const cls = e.classes[j];
            if (cls) {
              cont.classList.add(cls);
            }
          }
        }
        let children = e.options.slice(0);
        let summary = el("ul", {class: "summary"});
        children.unshift(summary);
        if ("help" in e) {
          children.unshift(
            el("span", {class: "description"}, e.help)
          );
        }
        if ("label" in e) {
          const groupLabel = el("b", {class: "clickable", tabindex: "0", title: "Click to show / hide these options"});
          groupLabel.textContent = e.label;
          groupLabel.addEventListener('keydown', function(e){
            e.stopPropagation();
            if (e.which == 13) {
              this.click();
            }
          });
          groupLabel.addEventListener('click', function(){
            cont.classList.toggle("expanded");
          });
          children.unshift(groupLabel);
        }
        if (e.expand || (!(e.expand === false) && Object.keys(e.options).length <= 2)) {
          cont.classList.add("expanded");
        }
        cont.addEventListener('change', function(){
          summary.innerHTML = "";
          const settings = this.querySelectorAll('.isSetting, input[type="hidden"].isSetting');
          settings.forEach(function(settingEl){
            let val = getval(settingEl);
            if (val == "") { return; }
            const opts = settingEl._opts;
            if (val != opts['default']) {
              let label = opts["label"]+": ";
              switch (opts.type) {
                case "select": {
                  val = opts.select.filter(function(v){ if (v[0] == val) return true; return false; })[0][1]; break;
                }
                case "unix": {
                  val = format.dateTime(val); break;
                }
                case "checkbox": {
                  val = "";
                  label = label.slice(0,-2);
                  break;
                }
                case "inputlist": {
                  val = val.join(", ");
                  break;
                }
              }
              const li = el("li", {class: "setting"});
              li.appendChild(el("span", {class: "label"}, label));
              li.appendChild(el("span", null, String(val)));
              li.appendChild(el("span", {class: "unit"}, typeof opts.unit  == "string" ? opts["unit"] : ""));
              summary.appendChild(li);
            }
          });
        });
        const builtChildren = formEngine.buildUI(children);
        if (builtChildren instanceof HTMLElement) {
          cont.appendChild(builtChildren);
        }
        cont.dispatchEvent(new Event("change", {bubbles: true}));
        c.appendChild(cont);
        $e.remove();
        if ("dependent" in e) {
          for (const i in e.dependent) {
            if (typeof e.dependent[i] == "string") e.dependent[i] = [e.dependent[i]]
            cont.setAttribute("data-dependent-"+i,"'"+e.dependent[i].join("' '")+"'");
          }
        }
        if ("dependent_not" in e) {
          for (const i in e.dependent_not) {
            if (typeof e.dependent_not[i] == "string") e.dependent_not[i] = [e.dependent_not[i]]
            cont.setAttribute("data-dependent-not-"+i,"'"+e.dependent_not[i].join("' '")+"'");
          }
        }
        continue;
        break;
      }
      case "custom": {
        $field = e.build();
        break;
      }
      case "str":
      default: {
        $field = el('input', {type: 'text'});
        if ("maxlength" in e) {
          $field.setAttribute("maxlength",e.maxlength);
        }
        if ("minlength" in e) {
          $field.setAttribute("minlength",e.minlength);
        }
      }
    }
    e._field = $field;
    $field.classList.add('field');
    $field._opts = e;
    $field._uid = Math.random().toString().slice(2);
    $field.addEventListener("focus",function(){
      const focusable = "button, a, input, select, textarea, [tabindex]:not([tabindex=\"-1\"])";
      if (!this.matches(focusable)) {
        const first = this.querySelector(focusable);
        if (first) first.focus();
      }
    });

    if ('pointer' in e) { $field.setAttribute('name',e.pointer.index); }
    fc.appendChild($field);
    if ('classes' in e) {
      for (const j in e.classes) {
        const cls = e.classes[j];
        if (cls) {
          $field.classList.add(cls);
        }
      }
    }
    if ('placeholder' in e) {
      $field.setAttribute('placeholder',e.placeholder);
    }
    if ('default' in e) {
      $field.setAttribute('placeholder',e['default']);
    }
    if ('unit' in e) {
      if ((e.unit === null) || (typeof e.unit == "undefined") || (e.unit === false)) {
        // Intentionally no unit.
      }
      else if (Array.isArray(e.unit)) {
        const unitSelect = el("select");
        unitSelect.addEventListener('change', function(){
          const fieldEl = this.closest(".field_container").querySelector(".field");
          const e = fieldEl._opts;
          const curval = getval(fieldEl);

          e.factor = Number(this.value);
          if ("min" in e) {
            fieldEl.setAttribute("min",e.min / e.factor);
          }
          if ("max" in e) {
            fieldEl.setAttribute("max",e.max / e.factor);
          }
          if ("step" in e) {
            fieldEl.setAttribute("step",e.step / e.factor);
          }
          if (("placeholder" in e) && !isNaN(Number(e.placeholder))) {
            fieldEl.setAttribute("placeholder",e.placeholder / e.factor);
          }

          if (Number(curval) != 0) setval(fieldEl, curval);
        });
        for (const i in e.unit) {
          unitSelect.appendChild(el("option", {value: e.unit[i][0]}, e.unit[i][1]));
        }
        const unitSpan = el('span', {class: 'unit'});
        unitSpan.appendChild(unitSelect);
        fc.appendChild(unitSpan);
        unitSelect.dispatchEvent(new Event("change", {bubbles: true}));
        $field.addEventListener('change', function(e){
          const kind = e._kind;
          if (kind == "initial") {
            let val = getval(this);
            const opts = this._opts;
            if ((opts.placeholder) && (Number(val) == 0)) {
              val = opts.placeholder;
            }
            if (Number(val) != 0) {
              let shortest = 1e9;
              let unit = false;
              for (const i in opts.unit) {
                const display = (val / opts.unit[i][0]).toString();
                if (display.length < shortest) {
                  shortest = display.length;
                  unit = opts.unit[i][0];
                }
              }
              if (unit) {
                const uSel = this.closest(".field_container").querySelector(".unit select");
                if (uSel) {
                  uSel.value = unit;
                  uSel.dispatchEvent(new Event("change", {bubbles: true}));
                }
              }
            }
          }
        });
        const initialEvt = new Event("change", {bubbles: true});
        initialEvt._kind = "initial";
        $field.dispatchEvent(initialEvt);
      }
      else {
        const unitSpan = el('span', {class: 'unit'});
        if (e.unit instanceof HTMLElement) {
          unitSpan.appendChild(e.unit);
        } else {
          unitSpan.innerHTML = e.unit;
        }
        fc.appendChild(unitSpan);
      }
    }
    if (('prefix' in e) && (e.prefix !== null) && (typeof e.prefix != "undefined") && (e.prefix !== false)) {
      const prefixSpan = el('span', {class: 'unit'});
      if (e.prefix instanceof HTMLElement) {
        prefixSpan.appendChild(e.prefix);
      } else {
        prefixSpan.innerHTML = e.prefix;
      }
      fc.insertBefore(prefixSpan, fc.firstChild);
    }
    if ('readonly' in e) {
      $field.setAttribute('readonly','readonly');
      $field.addEventListener('click', function(){
        if (this.selectionStart == this.selectionEnd) {
          this.select();
        }
      });
    }
    if ('qrcode' in e) {
      const qrButton = el('button');
      qrButton.textContent = 'QR';
      qrButton.addEventListener('keydown', function(e){
        e.stopPropagation();
      });
      qrButton.addEventListener('click', function(){
        const text = String(getval(this.closest('.field_container').querySelector('.field')));
        const qrDiv = el('div', {class: 'qrcode'});
        const qrContainer = el('div', {class: 'qr_modal'});
        qrContainer.appendChild(qrDiv);
        const uriHeader = el('code', {class: 'qr_modal_uri'}, text);
        const modal = uiHelpers.openModal({
          size: "md",
          title: 'Scan this URI',
          subtitle: uriHeader,
          body: qrContainer
        });

        let renderFrame = null;
        let resizeObserver = null;
        function renderNow(){
          if (!qrDiv || !qrDiv.isConnected) return;
          const size = Math.floor(Math.min(qrDiv.clientWidth, qrDiv.clientHeight));
          if (!isFinite(size) || (size < 32)) return;
          const qrRenderer = (typeof renderQRCode === 'function') ? renderQRCode : (typeof window.renderQRCode === 'function' ? window.renderQRCode : null);
          if (!qrRenderer) {
            qrDiv.textContent = 'QR renderer unavailable';
            return;
          }
          qrDiv.innerHTML = '';
          qrRenderer(qrDiv, {
            text: text,
            size: size
          });
        }
        function scheduleRender(){
          if (renderFrame !== null) {
            cancelAnimationFrame(renderFrame);
          }
          renderFrame = requestAnimationFrame(function(){
            renderFrame = null;
            renderNow();
          });
        }

        scheduleRender();
        if (window.ResizeObserver) {
          resizeObserver = new ResizeObserver(scheduleRender);
          resizeObserver.observe(qrDiv);
        }
        if (modal && modal.element) {
          modal.element.addEventListener('close', function(){
            if (resizeObserver) {
              resizeObserver.disconnect();
            }
            if (renderFrame !== null) {
              cancelAnimationFrame(renderFrame);
              renderFrame = null;
            }
          }, {once: true});
        }
      });
      const qrUnit = el('span', {class: 'unit'});
      qrUnit.appendChild(qrButton);
      fc.appendChild(qrUnit);
    }
    if (('clipboard' in e) && (document.queryCommandSupported('copy'))) {
      const copyButton = el('button');
      copyButton.textContent = 'Copy';
      copyButton.addEventListener('keydown', function(e){
        e.stopPropagation();
      });
      copyButton.addEventListener('click', function(){
        const field = this.closest('.field_container').querySelector('.field');
        const text = String(getval(field));
        const me = this;

        copy(text).then(function(){
          me.textContent = 'Copied!';
          setTimeout(function(){
            me.textContent = 'Copy';
          },5e3);
        }).catch(function(e){
            me.textContent = e+": manually copy instead";
            const inp = el("input", {class: "field"});
            inp.value = text;
            field.parentNode.replaceChild(inp, field);
            inp.select();
            setTimeout(function(){
              me.remove();
            },5e3);
        });
      });
      const copyUnit = el('span', {class: 'unit'});
      copyUnit.appendChild(copyButton);
      fc.appendChild(copyUnit);
    }
    if ('rows' in e) {
      $field.setAttribute('rows',e.rows);
    }
    if ("dependent" in e) {
      for (const i in e.dependent) {
        if (typeof e.dependent[i] == "string") e.dependent[i] = [e.dependent[i]]
        $e.setAttribute("data-dependent-"+i,"'"+e.dependent[i].join("' '")+"'");
      }
    }
    if ("dependent_not" in e) {
      for (const i in e.dependent_not) {
        if (typeof e.dependent_not[i] == "string") e.dependent_not[i] = [e.dependent_not[i]]
        $e.setAttribute("data-dependent-not-"+i,"'"+e.dependent_not[i].join("' '")+"'");
      }
    }

    switch (e.type) {
      case 'browse':
        {const master = el('div', {class: 'grouper'});
        master.appendChild($e);
        c.appendChild(master);


        const browseBtn = el('button');
        browseBtn.textContent = 'Browse';
        browseBtn.addEventListener('keydown', function(e){
          e.stopPropagation();
        });
        fc.appendChild(browseBtn);
        const allowfolders = e.filetypes ? e.filetypes.indexOf("/*/") >= 0 : false;
        browseBtn.addEventListener('click', function(){
          const bc = el('div', {class: 'browse_container'});
          const browseField = this.parentNode.querySelector(".field");
          let fields;
          const grouper = this.closest('.grouper');
          if (grouper) {
            fields = [browseField];
          }
          else {
            const inputlist = this.closest(".inputlist");
            if (inputlist) {
              fields = Array.from(inputlist.querySelectorAll(".field_container > .field"));
            }
            else {
              throw "Could not locate browse grouper container";
            }
          }
          fields.forEach(function(f){
            f.setAttribute('readonly','readonly');
            f.setAttribute("disabled","disabled");
            f.style.opacity = '0.5';
          });
          const thisBrowseBtn = this;

          const pathSpan = el('span', {class: 'field'});

          const folderContents = el('div', {class: 'browse_contents'});
          const folder = el('a', {tabindex: "0", class: 'folder'});
          const filetypes = browseField._filetypes;

          const browseLabel = el('label', {class: 'UIelement'});
          browseLabel.appendChild(el('span', {class: 'label'}, 'Current folder:'));
          const browseFC = el('span', {class: 'field_container'});
          browseFC.appendChild(pathSpan);
          browseLabel.appendChild(browseFC);
          bc.appendChild(browseLabel);
          bc.appendChild(folderContents);

          let value = "";
          function browse(path){
            folderContents.textContent = 'Loading..';
            apiClient.send(function(d){
              pathSpan.textContent = d.browse.path[0];
              value = d.browse.path[0]+(d.browse.path[0].slice(-1) == '/' ? '' : '/');
              folderContents.innerHTML = '';
              const upFolder = folder.cloneNode(true);
              upFolder.textContent = '..';
              upFolder.setAttribute('title','Folder up');
              upFolder.addEventListener('click', folderClickHandler);
              folderContents.appendChild(upFolder);
              if (d.browse.subdirectories) {
                d.browse.subdirectories.sort();
                for (const i in d.browse.subdirectories) {
                  const f = d.browse.subdirectories[i];
                  const subFolder = folder.cloneNode(true);
                  subFolder.setAttribute('title',pathSpan.textContent+(pathSpan.textContent.slice(-1) == '/' ? '' : '/')+f);
                  subFolder.textContent = f;
                  subFolder.addEventListener('click', folderClickHandler);
                  folderContents.appendChild(subFolder);
                }
              }
              if (d.browse.files) {
                d.browse.files.sort();
                for (const i in d.browse.files) {
                  const f = d.browse.files[i];
                  const src = pathSpan.textContent+((pathSpan.textContent.slice(-1*seperator.length) == seperator ? '' : seperator))+f;
                  const file = el('a', {tabindex: "0", class: 'file', title: src});
                  file.textContent = f;
                  folderContents.appendChild(file);

                  if (filetypes) {
                    let hide = true;
                    for (const j in filetypes) {
                      if (typeof filetypes[j] == 'undefined') {
                        continue;
                      }
                      if (mistHelpers.inputMatch(filetypes[j],src)) {
                        hide = false;
                        break;
                      }
                    }
                    if (hide) { file.hidden = true; }
                  }

                  file.addEventListener('click', function(){
                    const src = this.getAttribute('title');
                    modal.close();
                    setval(browseField, src);
                    browseField.dispatchEvent(new Event("keyup", {bubbles: true}));
                    browseField.dispatchEvent(new Event("change", {bubbles: true}));
                  });
                }
              }
              filter.value = "";
              const firstA = bc.querySelector("a");
              if (firstA) firstA.focus();
            },{browse:path});
          }

          let seperator = '/';
          if (mist.data.config.version.indexOf('indows') > -1) {
            seperator = '\\';
          }
          function folderClickHandler(){
            const path = pathSpan.textContent+((pathSpan.textContent.slice(-1*seperator.length) == seperator ? '' : seperator))+this.textContent;
            browse(path);
          }

          let path = getval(browseField);

          const protocol = path.split('://');
          if (protocol.length > 1) {
            if (protocol[0] == 'file') {
              path = protocol[1];
            }
            else {
              path = '';
            }
          }


          path = path.split(seperator);
          path.pop();
          path = path.join(seperator);

          browse(path);

          let filter = el("input");
          filter.addEventListener('keyup', function(){
            const allLinks = bc.querySelectorAll("a");
            allLinks.forEach(function(a){ a.hidden = false; a.style.display = ''; });
            let val = filter.value;
            if (val) {
              allLinks.forEach(function(a, i){
                if (i == 0 || a.textContent.indexOf(val) < 0) a.hidden = true;
              });
            }
            const visibleA = bc.querySelector("a:not([hidden])");
            if (visibleA) visibleA.focus();
          });

          bc.addEventListener('keydown', function(e){
            e.stopPropagation();

            if ((e.key.length == 1) || (e.key == "Backspace")) {
              filter.focus();
            }
            switch (e.key) {
              case "Enter": {
                e.target.click();
                break;
              }
              case "ArrowDown":
              case "ArrowRight": {
                let index = Array.from(e.target.parentNode.children).indexOf(e.target);

                function getNext(index) {
                  let next = index + 1;
                  if (next >= e.target.parentNode.children.length) next = 0;
                  if (!e.target.parentNode.children[next].checkVisibility()) return getNext(next);
                  return next;
                }
                e.target.parentNode.children[getNext(index)].focus();
                break;
              }
              case "ArrowUp": {
                let index = Array.from(e.target.parentNode.children).indexOf(e.target);
                function getNext(index) {
                  let next = index - 1;
                  if (next < 0) next = e.target.parentNode.children.length - 1;
                  if (!e.target.parentNode.children[next].checkVisibility()) return getNext(next);
                  return next;
                }
                e.target.parentNode.children[getNext(index)].focus();
                break;
              }
              case "ArrowLeft": {
                const firstA = bc.querySelector("a");
                if (firstA) firstA.click();
              }
            }
          });

          let modal;
          let chooserTitle = "Select a file"+(allowfolders ? " or folder" : "");
          modal = uiHelpers.openFormModal({
            size: "lg",
            title: chooserTitle,
            buttonPlacement: "footer",
            form: [{
              label: "Filter files and folders",
              type: "custom",
              help: "Type to filter files and folders.",
              build: function(){
                return filter;
              }
            },
            bc
            ],
            buttons: [{
              type: "cancel",
              label: "Cancel"
            }, allowfolders ? {
              type: "save",
              label: "Select folder",
              close: true,
              "function": function(){
                setval(browseField, value);
                browseField.dispatchEvent(new Event("keyup", {bubbles: true}));
                browseField.dispatchEvent(new Event("change", {bubbles: true}));
              }
            } : null]
          });
          const modalEl = modal.element instanceof HTMLElement ? modal.element : modal.element[0];
          const modalIC = modalEl.querySelector(".input_container");
          if (modalIC) modalIC.style.margin = "0";
          modalEl.addEventListener("close",function(){
            fields.forEach(function(f){
              f.removeAttribute('readonly');
              f.removeAttribute("disabled");
              f.style.opacity = '1';
            });
            browseField.dispatchEvent(new Event("change", {bubbles: true}));
          });


        });
        break;
      }case 'geolimited':
      case 'hostlimited':
        {const subUI = {
          field: $field
        };
        subUI.blackwhite = el('select');
        const bwOptBlack = el('option');
        bwOptBlack.value = '-';
        bwOptBlack.textContent = 'Blacklist';
        subUI.blackwhite.appendChild(bwOptBlack);
        const bwOptWhite = el('option');
        bwOptWhite.value = '+';
        bwOptWhite.textContent = 'Whitelist';
        subUI.blackwhite.appendChild(bwOptWhite);
        subUI.values = el('span', {class: 'limit_value_list'});
        switch (e.type) {
          case 'geolimited':
            {subUI.prototype = el('select');
            const defaultOpt = el('option');
            defaultOpt.value = '';
            defaultOpt.textContent = '[Select a country]';
            subUI.prototype.appendChild(defaultOpt);
            for (const i in constants.countrylist) {
              const cOpt = el('option');
              cOpt.value = i;
              cOpt.innerHTML = constants.countrylist[i];
              subUI.prototype.appendChild(cOpt);
            }
            break;
          }case 'hostlimited':
            subUI.prototype = el('input', {type: 'text', placeholder: 'type a host'});
            break;
        }
        subUI.prototype.addEventListener('change', function(){
          const subUI = this.closest('.field_container')._subUI;
          subUI.blackwhite.dispatchEvent(new Event('change', {bubbles: true}));
        });
        subUI.prototype.addEventListener('keyup', function(){
          const subUI = this.closest('.field_container')._subUI;
          subUI.blackwhite.dispatchEvent(new Event('change', {bubbles: true}));
        });
        subUI.blackwhite.addEventListener('change', function(){
          const subUI = this.closest('.field_container')._subUI;
          const values = [];
          let lastval = false;
          Array.from(subUI.values.children).forEach(function(child){
            lastval = child.value;
            if (lastval != '') {
              values.push(lastval);
            }
            else {
              child.remove();
            }
          });
          const clone = subUI.prototype.cloneNode(true);
          clone.addEventListener('change', function(){
            const subUI = this.closest('.field_container')._subUI;
            subUI.blackwhite.dispatchEvent(new Event('change', {bubbles: true}));
          });
          clone.addEventListener('keyup', function(){
            const subUI = this.closest('.field_container')._subUI;
            subUI.blackwhite.dispatchEvent(new Event('change', {bubbles: true}));
          });
          subUI.values.appendChild(clone);
          if (values.length > 0) {
            subUI.field.value = this.value+values.join(' ');
          }
          else {
            subUI.field.value = '';
          }
          subUI.field.dispatchEvent(new Event('change', {bubbles: true}));
        });
        const initClone = subUI.prototype.cloneNode(true);
        initClone.addEventListener('change', function(){
          const subUI = this.closest('.field_container')._subUI;
          subUI.blackwhite.dispatchEvent(new Event('change', {bubbles: true}));
        });
        initClone.addEventListener('keyup', function(){
          const subUI = this.closest('.field_container')._subUI;
          subUI.blackwhite.dispatchEvent(new Event('change', {bubbles: true}));
        });
        subUI.values.appendChild(initClone);
        fc._subUI = subUI;
        fc.classList.add('limit_list');
        fc.appendChild(subUI.blackwhite);
        fc.appendChild(subUI.values);
        break;
    }}

    if ('value' in e) {
      setval($field, e.value, ["initial"]);
    }
    if ('pointer' in e) {
      $field._pointer = e.pointer;
      $field.classList.add('isSetting');
      if (e.pointer.main) {
        const val = e.pointer.main[e.pointer.index];
        if (typeof val != 'undefined') {
          setval($field, val, ["initial"]);
        }
      }
    }

    if ('datalist' in e) {
      const r = 'datalist_'+i+MD5($field.outerHTML);
      $field.setAttribute('list',r);
      const datalist = el('datalist', {id: r});
      fc.appendChild(datalist);
      for (const i in e.datalist) {
        const dlOpt = el('option');
        dlOpt.value = e.datalist[i];
        datalist.appendChild(dlOpt);
      }
    }

    const ihc = el('span', {class: 'help_container'});
    $e.appendChild(ihc);
    $field._help_container = ihc;
    if ('help' in e) {
      let helpId = 'ih_' + Math.random().toString(36).slice(2);
      let helpLabel = e.label ? ('Show help for ' + e.label) : 'Show help';
      let balloon = el('span', {class: 'ih_balloon', id: helpId, role: 'tooltip', popover: 'auto'});
      if (typeof e.help === 'string') {
        balloon.innerHTML = e.help;
      } else if (e.help instanceof HTMLElement) {
        balloon.appendChild(e.help);
      } else {
        balloon.innerHTML = e.help;
      }
      let trigger = el('button', {class: 'ih_trigger', type: 'button', 'aria-label': helpLabel, popovertarget: helpId, 'data-icon': 'circle-help'});
      balloon.addEventListener('click', function(ev){
        ev.stopPropagation();
      });
      balloon.addEventListener('beforetoggle', function(ev) {
        if (ev.newState === 'open') {
          const rect = trigger.getBoundingClientRect();
          this.style.top = (rect.bottom + 6) + 'px';
          this.style.right = Math.max(8, window.innerWidth - rect.right) + 'px';
          this.style.left = 'auto';
        }
      });
      ihc.appendChild(trigger);
      ihc.appendChild(balloon);
    }

    if ('validate' in e) {
      const fs = [];
      for (const j in e.validate) {
        const validate = e.validate[j];
        let f;
        if (typeof validate == 'function') {
          f = validate;
        }
        else {
          switch (validate) {
            case 'required':
              f = function(val,me){
                if ((val == '') || (val == null)) {
                  return {
                    msg:'This is a required field.',
                    classes: ['red']
                  }
                }
                return false;
              }
              break;
            case 'int':
              f = function(val,me) {
                const ele = me._opts;
                if (!me.validity.valid) {
                  if ("factor" in ele && (ele.factor != 1)) {
                    let msg = 'Please enter a number';
                    const msgs = [];
                    if (me.validity.stepMismatch) {
                      msg += " divisible by "+(ele.step/ele.factor);
                    }
                    else if (me.validity.rangeUnderflow || me.validity.rangeOverflow) {
                      const unitSel = me.closest(".field_container").querySelector(".unit select option:checked");
                      const unit = unitSel ? unitSel.textContent : "";
                      if ('min' in ele) {
                        msgs.push(' greater than or equal to '+(ele.min/ele.factor)+" "+unit);
                      }
                      if ('max' in ele) {
                        msgs.push(' smaller than or equal to '+(ele.max/ele.factor)+" "+unit);
                      }
                    }
                    return {
                      msg: msg+msgs.join(' and')+'.',
                      classes: ['red']
                    };

                  }
                  else {
                    let msg = 'Please enter an integer';
                    const msgs = [];
                    if ('min' in ele) {
                      msgs.push(' greater than or equal to '+ele.min);
                    }
                    if ('max' in ele) {
                      msgs.push(' smaller than or equal to '+ele.max);
                    }
                    return {
                      msg: msg+msgs.join(' and')+'.',
                      classes: ['red']
                    };
                  }
                }
              }
              break;
            case 'streamname': {
              f = function(val,me) {
                if (val == "") { return; }

                if (val.toLowerCase() != val) {
                  return {
                    msg: 'Uppercase letters are not allowed.',
                    classes: ['red']
                  };
                }
                if (val.replace(/[^\da-z_\-\.]/g,'') != val) {
                  return {
                    msg: 'Special characters (except for underscores (_), periods (.) and dashes (-)) are not allowed.',
                    classes: ['red']
                  };
                }
                if (('streams' in mist.data) && (val in mist.data.streams)) {
                  let other = decodeURIComponent(location.hash)?.substring(1)?.split('@')?.[1]?.split('&')?.[1];
                  if (other != val) {
                    return {
                      msg: 'This streamname already exists.<br>If you want to edit an existing stream, please click edit on the the streams tab.',
                      classes: ['red']
                    };
                  }
                }
              };
              break;
            }
            case 'streamname_with_wildcard': {
              f = function(val,me) {
                if (val == "") { return; }

                let streampart = val.split("+");
                const wildpart = streampart.slice(1).join("+");
                streampart = streampart[0];

                if (streampart.toLowerCase() != streampart) {
                  return {
                    msg: 'Uppercase letters are not allowed in a stream name.',
                    classes: ['red']
                  };
                }
                if (streampart.replace(/[^\da-z_]/g,'') != streampart) {
                  return {
                    msg: 'Special characters (except for underscores) are not allowed in a stream name.',
                    classes: ['red']
                  };
                }

                if (streampart != val) {
                  if (wildpart.replace(/[\00|\0|\/]/g,'') != wildpart) {
                    return {
                      msg: 'Slashes or null bytes are not allowed in wildcards.',
                      classes: ['red']
                    };
                  }
                }
              };
              break;
            }
            case 'streamname_with_wildcard_and_variables': {
              f = function(val,me) {
                if (val == "") { return; }

                let streampart = val.split("+");
                const wildpart = streampart.slice(1).join("+");
                streampart = streampart[0];

                if (streampart.toLowerCase() != streampart) {
                  return {
                    msg: 'Uppercase letters are not allowed in a stream name.',
                    classes: ['red']
                  };
                }
                if (streampart.replace(/[^\da-z_$]/g,'') != streampart) {
                  return {
                    msg: 'Special characters (except for underscores) are not allowed in a stream name.',
                    classes: ['red']
                  };
                }

                if (streampart != val) {
                  if (wildpart.replace(/[\00|\0|\/]/g,'') != wildpart) {
                    return {
                      msg: 'Slashes or null bytes are not allowed in wildcards.',
                      classes: ['red']
                    };
                  }
                }
              };
              break;
            }
            case 'track_selector_parameter': {
              f = function(){};
              break;

            }
            case 'track_selector': {
              f = function(){};
              break;

            }
            default:
              f = function(){};
              break;
          }
        }
        fs.push(f);
      }

      if ("checkValidity" in $field) {
        fs.push(function(val,me){
          if ("checkValidity" in me) {
            if (me.checkValidity() == false) {
              return {
                msg: "validationMessage" in me ? me.validationMessage : "This value ("+val+") is invalid." ,
                classes: ["red"]
              };
            }
          }
        });
      }

      const validateOnChange = !('validate_on_change' in e) || !!e.validate_on_change;
      $field._validate_functions = fs;
      $field._validation_touched = false;
      $field._validation_user_interacted = false;
      $field._validate = function(me,validateOptions){
        if ((!me.checkVisibility()) && (me.type !== "hidden")) { return false; }
        const options = {
          focusonerror: false,
          showMessage: true,
          markTouched: false
        };
        if (typeof validateOptions == "object" && validateOptions) {
          Object.assign(options,validateOptions);
        }
        else if (typeof validateOptions != "undefined") {
          options.focusonerror = !!validateOptions;
        }
        if (options.markTouched || options.focusonerror) {
          me._validation_touched = true;
        }
        const val = getval(me);
        const fs = me._validate_functions;
        let ihcEl = me._help_container;
        const uid = me._uid;
        if (typeof ihcEl == "function") {
          ihcEl = ihcEl();
        }
        const errBalloon = ihcEl.querySelector('.err_balloon[data-uid="'+uid+'"]');
        if (errBalloon) errBalloon.remove();
        let hasError = false;
        for (const i in fs) {
          const error = fs[i](val,me);
          if (!error) { continue; }
          if (options.showMessage) {
            const err = el('span', {class: 'err_balloon', 'data-uid': uid});
            err.innerHTML = error.msg;
            for (const j in error.classes) {
              const cls = error.classes[j];
              if (cls) {
                err.classList.add(cls);
              }
            }
            ihcEl.insertBefore(err, ihcEl.firstChild);
          }
          if ((typeof error != "object") || (!("break" in error)) || (error.break)) {
            if (options.focusonerror) { me.focus(); }
            hasError = true;
            break;
          }
        }
        const touched = !!me._validation_touched;
        const hasValue = (val !== '') && (val !== null);
        const shouldShowState = options.focusonerror || touched || hasValue;
        me.classList.remove('field-validation-valid', 'field-validation-invalid');
        if (shouldShowState) {
          const stateClass = hasError ? 'field-validation-invalid' : (hasValue ? 'field-validation-valid' : '');
          if (stateClass) {
            me.classList.add(stateClass);
          }
        }
        return hasError;
      };
      $field.classList.add('hasValidate');
      $field.addEventListener('mousedown', function(ev){
        this._validation_user_interacted = true;
      });
      $field.addEventListener('keydown', function(ev){
        this._validation_user_interacted = true;
      });
      $field.addEventListener('touchstart', function(ev){
        this._validation_user_interacted = true;
      });
      $field.addEventListener('focus', function(ev){
        if (ev.isTrusted) {
          this._validation_user_interacted = true;
        }
        this.classList.add('field-validation-active');
      });
      $field.addEventListener('blur', function(){
        this.classList.remove('field-validation-active');
        if (this._validation_user_interacted) {
          const f = this._validate;
          f(this,{
            showMessage: false,
            markTouched: true
          });
        }
      });
      $field.addEventListener('change', function(ev){
        if (ev.isTrusted) {
          this._validation_user_interacted = true;
        }
        if ((!validateOnChange) && (!this._validation_touched) && (!this._validation_user_interacted)) {
          return;
        }
        const f = this._validate;
        f(this,{
          showMessage: false,
          markTouched: !!this._validation_user_interacted
        });
      });
      $field.addEventListener('keyup', function(ev){
        if (ev.isTrusted) {
          this._validation_user_interacted = true;
        }
        if ((!validateOnChange) && (!this._validation_touched) && (!this._validation_user_interacted)) {
          return;
        }
        const f = this._validate;
        f(this,{
          showMessage: false,
          markTouched: !!this._validation_user_interacted
        });
      });
      $field.addEventListener('input', function(ev){
        if (ev.isTrusted) {
          this._validation_user_interacted = true;
        }
        if ((!validateOnChange) && (!this._validation_touched) && (!this._validation_user_interacted)) {
          return;
        }
        const f = this._validate;
        f(this,{
          showMessage: false,
          markTouched: !!this._validation_user_interacted
        });
      });
    }

    if ('function' in e) {
      $field.addEventListener("change", function(ev){
        let lastval = this._lastval;
        let newval = getval(this);
        e["function"].apply(this,[ev,newval,lastval]);
        if (lastval != newval) {
          this._lastval = newval;
        }
      });
      $field.addEventListener("keyup", function(ev){
        let lastval = this._lastval;
        let newval = getval(this);
        e["function"].apply(this,[ev,newval,lastval]);
        if (lastval != newval) {
          this._lastval = newval;
        }
      });
      $field.dispatchEvent(new Event("change", {bubbles: true}));
    }
    if ("observe" in e) {
      const observer = new IntersectionObserver((ev)=>{
        const entry = ev[0];
        e.observe.call(entry.target, entry.isIntersecting && entry.target.checkVisibility());
      },{
        rootMargin: "1000%"
      });
      observer.observe($e);
      e.observe.call($e, $e.checkVisibility() && $e.offsetHeight > 0);
    }
  }
  c.addEventListener('keydown',function(e){
    let button = false;
    switch (e.which) {
      case 13:
        if (e.target.tagName != "BUTTON") {
          button = this.querySelector('button.save');
        }
        break;
      case 27:
        button = this.querySelector('button.cancel');
        break;
    }
    if (button) {
      button.click();
      e.stopPropagation();
      e.preventDefault();
    }
  });

  return c;
},

convertBuildOptions: function(input,saveas) {
  const build = [];
  const type = ['required','optional'];
  if ('desc' in input) {
    build.push({
      type: 'help',
      help: input.desc
    });
  }

  function processEle(j,i,ele) {
    function addClass(obj,name) {
      if (!("classes" in obj)) { obj.classes = []; }
      if (obj.classes.indexOf(name) < 0) { obj.classes.push(name); }
    }
    const obj = {
      label: format.capital((ele.name ? ele.name : i)),
      pointer: {
        main: saveas,
        index: i
      },
      validate: []
    };
    if (ele.display === 'hidden') {
      return null;
    }
    if (ele.display === 'advanced') {
      addClass(obj, 'advanced-only');
    } else if (ele.display === 'always') {
    } else {
      if ((type[j] == 'optional') && !ele.guided_visible) {
        addClass(obj,'advanced-only');
      }
      if (ele.mode == "advanced") {
        addClass(obj,'advanced-only');
      }
      if (ele.mode == "guided") {
        if ("classes" in obj) {
          obj.classes = obj.classes.filter(function(v){ return v != "advanced-only"; });
          if (!obj.classes.length) delete obj.classes;
        }
      }
    }
    if ((type[j] == 'required') && ((!('default' in ele)) || (ele['default'] == ''))) {
      obj.validate.push('required');
    }
    if ('default' in ele) {
      obj.placeholder = ele['default'];
      if (ele.type == "select") {
        for (const k in ele.select) {
          if (ele.select[k][0] == ele["default"]) {
            obj.placeholder = ele.select[k][1];
            break;
          }
        }
      }
    }
    if ('help' in ele) {
      obj.help = ele.help;
    }
    if ('unit' in ele) {
      obj.unit = ele.unit;
    }
    if ('display' in ele) {
      obj.display = ele.display;
    }
    if ('placeholder' in ele) {
      obj.placeholder = ele.placeholder;
    }
    if ("datalist" in ele) {
      obj.datalist = ele.datalist;
    }
    if ("type" in ele) {
      switch (ele.type) {
        case 'int':
          obj.type = 'int';
          if ("max" in ele) { obj.max = ele.max; }
          if ("min" in ele) { obj.min = ele.min; }
          break;
        case 'uint':
          obj.type = 'int';
          obj.min = 0;
          if ("max" in ele) { obj.max = ele.max; }
          if ("min" in ele) { obj.min = Math.max(obj.min,ele.min); }
          break;
        case 'radioselect':
          obj.type = 'radioselect';
          obj.radioselect = ele.radioselect;
          break;
        case 'select':
          obj.type = 'select';
          obj.select = ele.select.slice(0);
          if (obj.validate.indexOf("required") < 0) {
            obj.select.unshift(["",("placeholder" in obj ? "Default ("+obj.placeholder+")" : "" )]);
          }
          break;
        case 'sublist': {
          obj.type = 'sublist';
          obj.saveas = {};
          obj.itemLabel = ele.itemLabel;
          obj.sublist = formEngine.convertBuildOptions(ele,obj.saveas);
          break;
        }
        case 'group': {
          obj.type = "group";
          obj.label = ele.name;
          if ("dependent" in ele) obj.dependent = ele.dependent;
          if ("expand" in ele) obj.expand = ele.expand;
          let cobj = {
            optional: ele.options
          };
          if ("sort" in input) {
            cobj.sort = input.sort;
          }
          obj.options = formEngine.convertBuildOptions(cobj,saveas);
          obj.options = obj.options.slice(1);
          break;
        }
        case 'bool': {
          obj.type = 'checkbox';
          break;
        }
        case 'unixtime': {
          obj.type = 'unix';
          break;
        }
        case 'inputlist': {
          obj.type = "inputlist";
          if ("input" in ele) obj.input = ele.input;
          break;
        }
        case 'help': {
          obj.type = "help";
          obj.help = ele.help;
          if ("dependent" in ele) obj.dependent = ele.dependent;
          break;
        }
        case 'json':
        case 'debug':
        case 'inputlist':
        case 'browse': {
          obj.type = ele.type;
          break;
        }
        default:
          obj.type = 'str';
          if ("minlength" in ele) { obj.minlength = ele.minlength; }
          if ("maxlength" in ele) { obj.maxlength = ele.maxlength; }
          break;
      }
    }
    else {
      obj.type = "checkbox";
    }
    if ("format" in ele) {
      switch (ele.format) {
        case "set_or_unset": {
          obj['postSave'] = function(){
            const pointer = this._pointer;
            if (!pointer.main[pointer.index]) {
              delete pointer.main[pointer.index];
            }
          };

          break;
        }
      }
    }
    if ("influences" in ele) {
      obj["function"] = function(){
        const cont = this.closest(".UIelement");
        let style = cont.querySelector("style");
        if (!style) {
          style = document.createElement("style");
          style.classList.add("dependencies");
          cont.appendChild(style);
        }
        style.innerHTML = '[data-dependent-'+i+']:not([data-dependent-'+i+'~="\''+getval(this)+'\'"]) { display: none; }'+"\n";
        style.innerHTML += '[data-dependent-not-'+i+'~="\''+getval(this)+'\'"] { display: none; }'+"\n";

        style._content = style.innerHTML;

      };
      obj.observe = function(){
        const styles = this.querySelectorAll("style.dependencies");
        styles.forEach(function(s){
          if (s._content && (s.innerHTML !== s._content)) {
            s.innerHTML = s._content;
          }
          s.classList.remove("hidden");
        });
      }

    }
    else if ("disable" in ele) {
      obj["function"] = function(){
        const cont = this.closest(".input_container");
        const val = getval(this);
        for (let i = 0; i < ele.disable.length; i++) {
          const dependent = ele.disable[i];
          const depField = cont.querySelector('.field[name="'+dependent+'"]');
          const dependency = depField ? depField.closest(".UIelement") : null;
          if (dependency) {
            if (val == "") {
              dependency.style.display = "";
            }
            else {
              dependency.hidden = true;
            }
          }
        }
      };
    }
    if ("dependent" in ele) {
      obj.dependent = ele.dependent;
    }
    if ("dependent_not" in ele) {
      obj.dependent_not = ele.dependent_not;
    }
    if ("value" in ele) {
      obj.value = ele.value;
    }
    if ("validate" in ele) {
      obj.validate = obj.validate.concat(ele.validate);
      if (ele.validate.indexOf("track_selector_parameter") > -1) {
        obj.help = "<div>"+ele.help+"</div>"+"<p>Track selector parameters consist of a string value which may be any of the following:</p> <ul><li><code>selector,selector</code>: Selects the union of the given selectors. Any number of comma-separated selector combinations may be used, they are evaluated one by one from left to right.</li> <li><code>selector,!selector</code>: Selects the difference of the given selectors. Specifically, all tracks part of the first selector that are not part of the second selector. Any number of comma-separated selector combinations may be used, they are evaluated one by one from left to right.</li> <li><code>selector,|selector</code>: Selects the intersection of the given selectors. Any number of comma-separated selector combinations may be used, they are evaluated one by one from left to right.</li> <li><code>none</code> or <code>-1</code>: Selects no tracks of this type.</li> <li><code>all</code> or <code>*</code>: Selects all tracks of this type.</li> <li>Any positive integer: Select this specific track ID. Does not apply if the given track ID does not exist or is of the wrong type. <strong>Does</strong> apply if the given track ID is incompatible with the currently active protocol or container format.</li> <li>ISO 639-1/639-3 language code: Select all tracks marked as the given language. Case insensitive.</li> <li>Codec string (e.g. <code>h264</code>): Select all tracks of the given codec. Case insensitive.</li> <li><code>highbps</code>, <code>maxbps</code> or <code>bestbps</code>: Select the track of this type with the highest bit rate.</li> <li><code>lowbps</code>, <code>minbps</code> or <code>worstbps</code>: Select the track of this type with the lowest bit rate.</li> <li><code>Xbps</code> or <code>Xkbps</code> or <code>Xmbps</code>: Select the single of this type which has a bit rate closest to the given number X. This number is in bits, not bytes.</li> <li><code>&gt;Xbps</code> or <code>&gt;Xkbps</code> or <code>&gt;Xmbps</code>: Select all tracks of this type which have a bit rate greater than the given number X. This number is in bits, not bytes.</li> <li><code>&lt;Xbps</code> or <code>&lt;Xkbps</code> or <code>&lt;Xmbps</code>: Select all tracks of this type which have a bit rate less than the given number X. This number is in bits, not bytes.</li> <li><code>max&lt;Xbps</code> or <code>max&lt;Xkbps</code> or <code>max&lt;Xmbps</code>: Select the one track of this type which has the highest bit rate less than the given number X. This number is in bits, not bytes.</li> <li><code>highres</code>, <code>maxres</code> or <code>bestres</code>: Select the track of this type with the highest pixel surface area. Only applied when the track type is video.</li> <li><code>lowres</code>, <code>minres</code> or <code>worstres</code>: Select the track of this type with the lowest pixel surface area. Only applied when the track type is video.</li> <li><code>XxY</code>: Select all tracks of this type with the given pixel surface area in X by Y pixels. Only applied when the track type is video.</li> <li><code>~XxY</code>: Select the single track of this type closest to the given pixel surface area in X by Y pixels. Only applied when the track type is video.</li> <li><code>&gt;XxY</code>: Select all tracks of this type with a pixel surface area greater than X by Y pixels. Only applied when the track type is video.</li> <li><code>&lt;XxY</code>: Select all tracks of this type with a pixel surface area less than X by Y pixels. Only applied when the track type is video.</li> <li><code>720p</code>, <code>1080p</code>, <code>1440p</code>, <code>2k</code>, <code>4k</code>, <code>5k</code>, or <code>8k</code>: Select all tracks of this type with the given pixel surface area. Only applied when the track type is video.</li> <li><code>surround</code>, <code>mono</code>, <code>stereo</code>, <code>Xch</code>: Select all tracks of this type with the given channel count. The 'Xch' variant can use any positive integer for 'X'. Only applied when the track type is audio.</li></ul>";
      }
      if (ele.validate.indexOf("track_selector") > -1) {
        obj.help = "<div>"+ele.help+"</div>"+"<p>A track selector is at least one track type (audio, video or subtitle) combined with a track selector parameter. For example: <code>audio=none&video=maxres</code>.<p>Track selector parameters consist of a string value which may be any of the following:</p> <ul><li><code>selector,selector</code>: Selects the union of the given selectors. Any number of comma-separated selector combinations may be used, they are evaluated one by one from left to right.</li> <li><code>selector,!selector</code>: Selects the difference of the given selectors. Specifically, all tracks part of the first selector that are not part of the second selector. Any number of comma-separated selector combinations may be used, they are evaluated one by one from left to right.</li> <li><code>selector,|selector</code>: Selects the intersection of the given selectors. Any number of comma-separated selector combinations may be used, they are evaluated one by one from left to right.</li> <li><code>none</code> or <code>-1</code>: Selects no tracks of this type.</li> <li><code>all</code> or <code>*</code>: Selects all tracks of this type.</li> <li>Any positive integer: Select this specific track ID. Does not apply if the given track ID does not exist or is of the wrong type. <strong>Does</strong> apply if the given track ID is incompatible with the currently active protocol or container format.</li> <li>ISO 639-1/639-3 language code: Select all tracks marked as the given language. Case insensitive.</li> <li>Codec string (e.g. <code>h264</code>): Select all tracks of the given codec. Case insensitive.</li> <li><code>highbps</code>, <code>maxbps</code> or <code>bestbps</code>: Select the track of this type with the highest bit rate.</li> <li><code>lowbps</code>, <code>minbps</code> or <code>worstbps</code>: Select the track of this type with the lowest bit rate.</li> <li><code>Xbps</code> or <code>Xkbps</code> or <code>Xmbps</code>: Select the single of this type which has a bit rate closest to the given number X. This number is in bits, not bytes.</li> <li><code>&gt;Xbps</code> or <code>&gt;Xkbps</code> or <code>&gt;Xmbps</code>: Select all tracks of this type which have a bit rate greater than the given number X. This number is in bits, not bytes.</li> <li><code>&lt;Xbps</code> or <code>&lt;Xkbps</code> or <code>&lt;Xmbps</code>: Select all tracks of this type which have a bit rate less than the given number X. This number is in bits, not bytes.</li> <li><code>max&lt;Xbps</code> or <code>max&lt;Xkbps</code> or <code>max&lt;Xmbps</code>: Select the one track of this type which has the highest bit rate less than the given number X. This number is in bits, not bytes.</li> <li><code>highres</code>, <code>maxres</code> or <code>bestres</code>: Select the track of this type with the highest pixel surface area. Only applied when the track type is video.</li> <li><code>lowres</code>, <code>minres</code> or <code>worstres</code>: Select the track of this type with the lowest pixel surface area. Only applied when the track type is video.</li> <li><code>XxY</code>: Select all tracks of this type with the given pixel surface area in X by Y pixels. Only applied when the track type is video.</li> <li><code>~XxY</code>: Select the single track of this type closest to the given pixel surface area in X by Y pixels. Only applied when the track type is video.</li> <li><code>&gt;XxY</code>: Select all tracks of this type with a pixel surface area greater than X by Y pixels. Only applied when the track type is video.</li> <li><code>&lt;XxY</code>: Select all tracks of this type with a pixel surface area less than X by Y pixels. Only applied when the track type is video.</li> <li><code>720p</code>, <code>1080p</code>, <code>1440p</code>, <code>2k</code>, <code>4k</code>, <code>5k</code>, or <code>8k</code>: Select all tracks of this type with the given pixel surface area. Only applied when the track type is video.</li> <li><code>surround</code>, <code>mono</code>, <code>stereo</code>, <code>Xch</code>: Select all tracks of this type with the given channel count. The 'Xch' variant can use any positive integer for 'X'. Only applied when the track type is audio.</li></ul>";
      }
    }

    return obj;
  }

  for (const j in type) {
    if (input[type[j]] && Object.keys(input[type[j]]).length) {
      const heading = el('h4');
      heading.textContent = format.capital(type[j])+' parameters';
      build.push(heading);
      const list = Object.keys(input[type[j]]);
      if ("sort" in input) {
        function getStr(key) {
          let opts = input[type[j]][key];
          if (Array.isArray(opts)) {
            if (opts.length) opts = opts[0]
            else return "";
          }
          return ""+(input.sort in opts ? opts[input.sort] : key);
        }
        list.sort(function(a,b){
          return getStr(a).localeCompare(getStr(b));
        });
      }
      else {
        list.sort(function(a,b){
          return input[type[j]][a].type == "group" ? 1 : -1;
        });
      }
      for (const n in list) {
        const i = list[n];
        const ele = input[type[j]][i];
        if (Array.isArray(ele)) {
          for (const m in ele) {
            const item = processEle(j,i,ele[m]);
            if (item) build.push(item);
          }
        }
        else {
          const item = processEle(j,i,ele);
          if (item) build.push(item);
        }
      }
    }
  }
  return build;
},

collectFormValues: function(container) {
  const fields = container.querySelectorAll('.hasValidate');
  for (let i = 0; i < fields.length; i++) {
    const f = fields[i];
    if (f.type !== 'hidden' && !f.checkVisibility()) continue;
    if (f._validate && f._validate(f, true)) return false;
  }
  const settings = container.querySelectorAll('.isSetting, input[type="hidden"].isSetting');
  for (let i = 0; i < settings.length; i++) {
    const s = settings[i];
    if (s.type !== 'hidden' && !s.checkVisibility()) continue;
    let val = getval(s);
    const pointer = s._pointer;
    if (!pointer) continue;
    const opts = s._opts || {};
    if ((val === '') || (val === null)) {
      if ('default' in opts) { val = opts['default']; }
      else if (!opts.keepnull) { pointer.main[pointer.index] = null; continue; }
    }
    pointer.main[pointer.index] = val;
    if (opts.postSave) opts.postSave.call(s);
  }
  return true;
}
};
