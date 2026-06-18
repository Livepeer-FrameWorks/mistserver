/**
 * el(tag, attrs, children) - terse native DOM element factory.
 *
 *   el('div', {class: 'foo', onclick: handler}, [
 *     el('span', null, 'Hello'),
 *     el('button', {'data-icon': 'plus'}, 'Add')
 *   ])
 *
 * Returns HTMLElement. Accepts:
 *   attrs.class / attrs.className - sets className
 *   attrs.on* - addEventListener (onclick, onchange, etc.)
 *   attrs.style (object) - merges into el.style
 *   attrs.* - setAttribute for everything else
 *   children - string | number | Node | Array | falsy (skipped)
 */
export function appendChildren(node, children) {
  if (children == null || children === false) return;
  if (Array.isArray(children)) {
    for (let i = 0; i < children.length; i++) appendChildren(node, children[i]);
  } else if (typeof children === 'string') {
    node.appendChild(document.createTextNode(children));
  } else if (typeof children === 'number') {
    node.appendChild(document.createTextNode(String(children)));
  } else if (children instanceof Node) {
    node.appendChild(children);
  }
}

export function el(tag, attrs, children) {
  const node = document.createElement(tag);
  if (attrs) {
    for (const key in attrs) {
      if (!attrs.hasOwnProperty(key)) continue;
      const val = attrs[key];
      if (val == null || val === false) continue;
      if (key === 'class' || key === 'className') {
        node.className = val;
      } else if (key.length > 2 && key.slice(0, 2) === 'on' && typeof val === 'function') {
        node.addEventListener(key.slice(2), val);
      } else if (key === 'style' && typeof val === 'object') {
        for (const s in val) {
          if (val.hasOwnProperty(s)) node.style[s] = val[s];
        }
      } else {
        node.setAttribute(key, val);
      }
    }
  }
  if (children != null) appendChildren(node, children);
  return node;
}


/**
 * deepExtend(target, src1, src2, ...) - recursive merge of plain objects.
 * Recursive merge of plain objects. Does NOT clone arrays or non-plain objects.
 */
export function deepExtend(target) {
  for (let i = 1; i < arguments.length; i++) {
    const src = arguments[i];
    if (src == null) continue;
    for (const key in src) {
      if (!src.hasOwnProperty(key)) continue;
      const val = src[key];
      if (val && typeof val === 'object' && !Array.isArray(val)
          && !(val instanceof Node) && !(val instanceof RegExp)
          && Object.getPrototypeOf(val) === Object.prototype) {
        if (typeof target[key] !== 'object' || target[key] === null
            || Array.isArray(target[key])) {
          target[key] = {};
        }
        deepExtend(target[key], val);
      } else {
        target[key] = val;
      }
    }
  }
  return target;
}


/** q(selector, context) - querySelector shortcut */
export function q(sel, ctx) { return (ctx || document).querySelector(sel); }

/** qa(selector, context) - querySelectorAll shortcut, returns Array */
export function qa(sel, ctx) { return Array.from((ctx || document).querySelectorAll(sel)); }


/** Identity pass-through */
function _raw(o) {
  return o;
}

/**
 * getval(domEl) - extract the value from a buildUI-created form element.
 * Reads from ._opts, ._savelist etc. (set by buildUI).
 */
export function getval(domEl) {
  domEl = _raw(domEl);
  if (!domEl) return undefined;

  const opts = domEl._opts;
  let val = domEl.value;
  if ((opts) && ('type' in opts)) {
    const type = opts.type;
    switch (type) {
      case 'int':
        if (val != "") { val = Number(val); }
        break;
      case 'span':
        val = domEl.innerHTML;
        break;
      case 'debug':
        val = domEl.value == "" ? null : Number(domEl.value);
        break;
      case 'checkbox':
        val = domEl.checked;
        break;
      case 'radioselect': {
        const checked = domEl.querySelector('label > input[type=radio]:checked');
        if (checked) {
          const parentLabel = checked.parentElement;
          val = [];
          val.push(checked.value);
          const sel = parentLabel.querySelector('select');
          if (sel) {
            val.push(sel.value);
          }
        }
        else {
          val = '';
        }
        break;
      }
      case 'checklist':
        {val = [];
        const checkboxes = domEl.querySelectorAll('.checklist input[type=checkbox]:checked');
        for (let ci = 0; ci < checkboxes.length; ci++) {
          val.push(checkboxes[ci].getAttribute('name'));
        }
        break;
      }case "unix":
        if (val != "") {
          val = Math.round(new Date(domEl.value) / 1e3);
        }
        break;
      case "selectinput":
        {const firstSelect = domEl.querySelector(':scope > select');
        val = firstSelect ? firstSelect.value : '';
        if (val == "CUSTOM") {
          const firstLabel = domEl.querySelector(':scope > label');
          const fc = firstLabel ? firstLabel.querySelector('.field_container') : null;
          const firstChild = fc ? fc.children[0] : null;
          val = firstChild ? getval(firstChild) : '';
        }
        break;
      }case "inputlist":
        {val = [];
        const items = domEl.querySelectorAll(':scope > .listitem > .field');
        for (let ii = 0; ii < items.length; ii++) {
          val.push(getval(items[ii]));
        }
        if (val.length) {
          const last = val[val.length-1];
          if ((last === "") || (last === null)) {
            val.pop();
          }
        }
        break;
      }case "sublist":
        val = domEl._savelist;
        break;
      case "json":
        try {
          val = JSON.parse(domEl.value);
        }
        catch (e) {
          val = null;
        }
        break;
      case "bitmask": {
        val = 0;
        const bmInputs = domEl.querySelectorAll("input");
        for (let bi = 0; bi < bmInputs.length; bi++) {
          if (bmInputs[bi].checked) {
            val += Number(bmInputs[bi].value);
          }
        }
        break;
      }
    }

    if ("getval" in opts) {
      val = opts.getval(val, domEl);
    }

    if (("factor" in opts) && (val !== null) && (val != "")) {
      val *= opts.factor;
    }
  }

  return val;
}

/**
 * validateFeedback(domEl, focusonerror) - trigger validation on a buildUI form element.
 * Returns true if valid, false if validation error found.
 */
export function validateFeedback(domEl, focusonerror) {
  domEl = _raw(domEl);
  if (!domEl) return true;
  const vf = domEl._validate;
  if (typeof vf == 'function') {
    if (vf(domEl, focusonerror)) {
      return false;
    }
  }
  return true;
}

/**
 * setval(domEl, val, extraParameters) - set the value on a buildUI-created form element.
 * Writes to native DOM properties; reads from ._opts, ._savelist etc.
 */
export function setval(domEl, val, extraParameters) {
  domEl = _raw(domEl);
  if (!domEl) return;

  const oldval = getval(domEl);
  const opts = domEl._opts;
  if (opts && ("factor" in opts) && (Number(val) != 0)) {
    val /= opts.factor;
  }

  if ((opts) && ('type' in opts)) {
    const type = opts.type;
    switch (type) {
      case 'span':
        domEl.innerHTML = val;
        domEl.setAttribute("title", typeof val == "string" ? val : "");
        break;
      case 'checkbox':
        domEl.checked = val;
        break;
      case 'select': {
        if (Array.from(domEl.options).filter(function(a){ return a.value == val; }).length) {
          domEl.value = val;
        }
        break;
      }
      case 'geolimited':
      case 'hostlimited': {
        const subUI = domEl.closest('.field_container')._subUI;
        if ((typeof val == 'undefined') || (val.length == 0)) {
          val = '-';
        }
        const bw = _raw(subUI.blackwhite);
        bw.value = val.charAt(0);
        val = val.substr(1).split(' ');
        const valuesEl = _raw(subUI.values);
        for (let si = 0; si < val.length; si++) {
          const proto = _raw(subUI.prototype);
          const clone = proto.cloneNode(true);
          clone.addEventListener('change', function(){
            const sub = this.closest('.field_container')._subUI;
            _raw(sub.blackwhite).dispatchEvent(new Event('change', {bubbles: true}));
          });
          clone.addEventListener('keyup', function(){
            const sub = this.closest('.field_container')._subUI;
            _raw(sub.blackwhite).dispatchEvent(new Event('change', {bubbles: true}));
          });
          clone.value = val[si];
          valuesEl.appendChild(clone);
        }
        bw.dispatchEvent(new Event('change', {bubbles: true}));
        break;
      }
      case 'radioselect':
        {if (typeof val == 'undefined') { return; }
        const radio = domEl.querySelector('label > input[type=radio][value="'+val[0]+'"]');
        if (radio) {
          radio.checked = true;
          if (val.length > 1) {
            const rSel = radio.parentElement.querySelector('select');
            if (rSel) rSel.value = val[1];
          }
        }
        break;
      }case 'checklist': {
        const clInputs = domEl.querySelectorAll('.checklist input[type=checkbox]');
        for (let cli = 0; cli < clInputs.length; cli++) { clInputs[cli].checked = false; }
        for (let vi = 0; vi < val.length; vi++) {
          const cb = domEl.querySelector('.checklist input[type=checkbox][name="'+val[vi]+'"]');
          if (cb) cb.checked = true;
        }
        break;
      }
      case "unix":
        if ((typeof val != "undefined") && (val != "") && (val !== null)) {
          let datetime = new Date(Math.round(val) * 1e3);
          datetime.setMinutes(datetime.getMinutes() - datetime.getTimezoneOffset());
          datetime = datetime.toISOString();
          domEl.value = datetime.split("Z")[0];
        }

        break;
      case "selectinput": {
        let localval = val;
        if (localval === null) {
          localval = "";
        }
        let found = false;
        for (let sii = 0; sii < opts.selectinput.length; sii++) {
          let compare;
          if (typeof opts.selectinput[sii] == "string") {
            compare = opts.selectinput[sii];
          }
          else if (typeof opts.selectinput[sii][0] == "string") {
            compare = opts.selectinput[sii][0];
          }
          if (compare == localval) {
            domEl.querySelector(':scope > select').value = localval;
            found = true;
            break;
          }
        }
        if (!found) {
          const siLabel = domEl.querySelector(':scope > label');
          const siFC = siLabel ? siLabel.querySelector('.field_container') : null;
          const siField = siFC ? siFC.children[0] : null;
          if (siField) {
            setval(siField, localval);
            siField.dispatchEvent(new Event('change', {bubbles: true}));
          }
          const siSelect = domEl.querySelector(':scope > select');
          if (siSelect) {
            siSelect.value = "CUSTOM";
            siSelect.dispatchEvent(new Event('change', {bubbles: true}));
          }
        }
        break;
      }
      case "inputlist": {
        if (typeof val == "string") { val = [val]; }
        const oldChildren = Array.from(domEl.children);
        for (let ili = 0; ili < val.length; ili++) {
          const newitem = domEl._newitem();
          const newitemEl = _raw(newitem);
          newitemEl.setAttribute("data-index", ili);
          const newField = newitemEl.querySelector(".field");
          if (newField) setval(newField, val[ili]);
          domEl.appendChild(newitemEl);
        }
        const emptyItem = _raw(domEl._newitem());
        domEl.appendChild(emptyItem);
        for (let ri = 0; ri < oldChildren.length; ri++) {
          if (oldChildren[ri].parentNode) oldChildren[ri].parentNode.removeChild(oldChildren[ri]);
        }
        break;
      }
      case "sublist": {
        const field = domEl;
        const curvals = domEl.querySelector(':scope > .curvals');
        curvals.innerHTML = "";
        if (val && val.length) {
          for (let sli = 0; sli < val.length; sli++) {
            (function(idx) {
              const v = deepExtend({}, val[idx]);

              function removeNull(v) {
                for (const j in v) {
                  if (j.slice(0,6) == "x-LSP-") {
                    delete v[j];
                  }
                  else if (typeof v[j] == "object") {
                    removeNull(v[j]);
                  }
                }
              }
              removeNull(v);

              curvals.appendChild(
                el("div", {"class": "subitem"}, [
                  el("span", {"class": "itemdetails", title: JSON.stringify(v,null,2)},
                    (val[idx][opts.hrn] ? val[idx][opts.hrn] : JSON.stringify(v))
                  ),
                  el("button", {"class": "move", title: "Move item up", onclick: function(){
                    const i = Array.from(this.parentElement.parentElement.children).indexOf(this.parentElement);
                    if (i == 0) { return; }
                    const savelist = getval(field);
                    savelist.splice(i - 1, 0, savelist.splice(i, 1)[0]);
                    setval(field, savelist);
                  }}, "^"),
                  el("button", {onclick: function(){
                    const index = Array.from(this.parentElement.parentElement.children).indexOf(this.parentElement);
                    field._build(Object.assign({}, getval(field)[index]), index);
                  }}, "Edit"),
                  el("button", {title: "Remove item", onclick: function(e){
                    const i = Array.from(this.parentElement.parentElement.children).indexOf(this.parentElement);
                    const savelist = field._savelist;
                    savelist.splice(i, 1);
                    setval(field, savelist);
                    e.preventDefault();
                  }}, "x")
                ])
              );
            })(sli);
          }
        }
        else {
          curvals.appendChild(document.createTextNode("None."));
        }
        field._savelist = val;
        break;
      }
      case "json": {
        domEl.value = val === null ? "" : JSON.stringify(val, null, 2);
        break;
      }
      case "bitmask": {
        const map = domEl._opts.bitmask;
        const bmInputs = domEl.querySelectorAll("input");
        for (let bmi = 0; bmi < map.length; bmi++) {
          if (bmi < bmInputs.length) {
            if ((val & map[bmi][0]) == map[bmi][0]) {
              bmInputs[bmi].setAttribute("checked", "checked");
            }
            else {
              bmInputs[bmi].removeAttribute("checked");
            }
          }
        }
        break;
      }
      case "custom": {
        break;
      }
      default: {
        domEl.value = val;
      }
    }
  }
  else {
    domEl.value = val;
  }

  if (opts && ("setval" in opts)) {
    opts.setval(val, extraParameters, domEl);
  }

  domEl.dispatchEvent(new Event('change', {bubbles: true}));

  if (extraParameters && extraParameters.indexOf("initial") < -1) {
    const newval = getval(domEl);
    if (oldval != newval) {
      if (JSON.stringify(oldval) != JSON.stringify(newval)) {
        const label = domEl.closest("label.UIelement");
        if (label) {
          label.classList.add("animate_changed");
          setTimeout(function(){
            label.classList.remove("animate_changed");
          }, 100);
        }
      }
    }
  }
}


export function toNode(node) {
  if (!node) return null;
  if (node.nodeType) return node;
  if (node[0] && node[0].nodeType) return node[0];
  return null;
}

