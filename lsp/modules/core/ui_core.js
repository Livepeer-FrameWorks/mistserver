import { uiHelpers } from './ui_helpers.js';
import { navto } from './navigation.js';
import { mistHelpers } from './mist_helpers.js';
import { el } from './dom_helpers.js';

export const uiCore = {
context_menu: function(){
  const ele = el("section", {class: "context_menu"});
  ele.addEventListener("click", function(e){ e.stopPropagation(); });
  ele.style.display = "none";
  this.ele = ele;

  UI.elements.context_menu.push(this);

  this.pos = function(pos){
    const parent = ele.offsetParent;
    const parpos = parent.getBoundingClientRect();

    const mh = parent.clientHeight - ele.offsetHeight;
    const mw = parent.clientWidth - ele.offsetWidth;

    ele.style.left = Math.min(pos.pageX - parpos.x + parent.scrollLeft, mw) + "px";
    ele.style.top = Math.min(pos.pageY - parpos.y + parent.scrollTop, mh) + "px";
  };
  this.show = function(html,pos){
    /*
     Expects something like:
     menu.show([
      [ el("div", {class: "header"}, "My header") ],
      [
        [ "Entry 1", onClick, "icon", "This is the first menu entry of this section" ],
        {
          text: "Entry 2",
          icon: "copy",
          title: "This is the second menu entry of this section",
          "function": function(e){
            this._setText("Loading..");
            let me = this;
            doSomething.then(function(){
              me._setText("Complete!");
              setTimeout(function(){ context_menu.hide(); },300);
            }).catch(function(){
              me._setText("Failed!");
              setTimeout(function(){ context_menu.hide(); },1e3);
            });

            return false; //do not hide the context menu after this function has returned
          },

        }
      ]
     ],PointerEvent);

     *  */



    if (typeof html == "string") {
      ele.innerHTML = html;
    }
    else if (html instanceof HTMLElement) {
      ele.innerHTML = "";
      ele.appendChild(html);
    }
    else if (typeof html == "object") {
      ele.innerHTML = "";
      if (!Array.isArray(html)) {
        html = [html];
      }
      for (const i in html) {
        const section = html[i];
        if (section instanceof HTMLElement) {
          const lastChild = ele.lastElementChild;
          if (lastChild) lastChild.remove(); //remove previous <hr>
          ele.appendChild(section);
          ele.appendChild(el("hr")); //so that finishing code removes this <hr> and not the section we just inserted

          continue;
        }

        for (const j in section) {
          const entry = section[j];
          const entryEl = el("div");
          if (typeof entry == "string") {
            entryEl.textContent = entry;
          }
          else if (entry instanceof HTMLElement) {
            ele.appendChild(entry);
            continue;
          }
          else {
            function createEntry(opts) {
              if (Array.isArray(opts)) {
                const obj = {};
                obj.text = opts[0];
                if ((opts.length >= 2) && (typeof opts[1] == "function")) {
                  obj["function"] = opts[1];
                }
                if (opts.length >=3) {
                  obj.icon = opts[2];
                  if (opts.length >= 4) {
                    obj.title = opts[3];
                  }
                }
                opts = obj;
              }

              if ("function" in opts) {
                entryEl.addEventListener("click", function(e){
                  const returnValue = opts["function"].apply(this,arguments);
                  if (returnValue !== false) {
                    ele.style.display = "none";
                  }
                });
                entryEl.addEventListener("keydown",function(e){
                  switch (e.key) {
                    case "Enter": {
                      this.dispatchEvent(new Event("click", {bubbles: true}));
                      break;
                    }
                  }
                });
                entryEl.setAttribute("tabindex","0");
              }

              if ("icon" in opts) {
                const iconEl = el("div", {class: "icon"});
                iconEl.setAttribute("data-icon", opts.icon);
                entryEl.appendChild(iconEl);
              }

              if ("title" in opts) {
                (function(tip) {
                  entryEl.addEventListener("mouseenter", function(e) {
                    UI.tooltip.show(e, tip);
                  });
                  entryEl.addEventListener("mouseleave", function() {
                    UI.tooltip.hide();
                  });
                })(opts.title);
              }

              if (typeof opts.text == "string") {
                entryEl._text = document.createTextNode(opts.text);
                entryEl.appendChild(entryEl._text);
                entryEl._setText = function(str) {
                  this._text.nodeValue = str;
                }
              }
              else {
                entryEl.appendChild(opts.text);
                entryEl._setText = function(str) {
                  this.innerHTML = str;
                }
              }
            }
            createEntry(entry);

          }

          ele.appendChild(entryEl);
        }
        ele.appendChild(el("hr"));
      }
      const lastChild = ele.lastElementChild;
      if (lastChild) lastChild.remove();
      const firstTabindex = ele.querySelector("[tabindex]");
      if (firstTabindex) firstTabindex.focus();
    }

    if (!ele.parentElement) { document.body.appendChild(ele); }
    ele.style.display = "";
    if (pos) { this.pos(pos); }

    const firstTabindex = ele.querySelector("[tabindex]");
    if (firstTabindex) firstTabindex.focus();
  };
  this.hide = function(){
    ele.style.display = "none";
  };
  this.remove = function(){
    delete UI.element.context_menu;
    ele.remove();
  };

  ele.addEventListener("keydown",function(e){
    function getFocussed(dir) {
      const focussed = ele.querySelector(":focus");
      if (!focussed) {
        const first = ele.querySelector("[tabindex]");
        if (first) first.focus();
        return;
      }
      if (dir == "down") {
        let next = null;
        let sibling = focussed.nextElementSibling;
        while (sibling) {
          if (sibling.hasAttribute("tabindex")) { next = sibling; break; }
          sibling = sibling.nextElementSibling;
        }
        if (!next) {
          const first = ele.querySelector("[tabindex]");
          if (first) first.focus();
        }
        else {
          next.focus();
        }
      }
      else {
        let prev = null;
        let sibling = focussed.previousElementSibling;
        while (sibling) {
          if (sibling.hasAttribute("tabindex")) { prev = sibling; break; }
          sibling = sibling.previousElementSibling;
        }
        if (!prev) {
          const all = ele.querySelectorAll("[tabindex]");
          if (all.length) all[all.length - 1].focus();
        }
        else {
          prev.focus();
        }
      }
    }
    switch (e.key) {
      case "ArrowDown": {
        getFocussed("down");
        break;
      }
      case "ArrowUp": {
        getFocussed("up");
        break;
      }
    }
  });

  this.hide();
},
pagecontrol: function(page_ele,default_page_size){
  //takes table / div of elements (page_ele)
  //take into account hidden elements (class = hidden)
  //add page-functions to paged element
  //keep track of total amount of (non hidden) items
  //update control buttons
  //returns controls container

  if (!page_ele || !page_ele.classList) {
    const emptyCtrl = el("div", {class: "page_control"});
    emptyCtrl.hidden = true;
    return emptyCtrl;
  }

  const controls = el("div", {class: "page_control"});

  const prevBtn = el("button");
  prevBtn.textContent = "Previous";
  prevBtn.addEventListener("click", function(){
    show_page("previous");
  });

  const nextBtn = el("button");
  nextBtn.textContent = "Next";
  nextBtn.addEventListener("click", function(){
    show_page("next");
  });

  const pageBtnCont = el("div", {class: "page_numbers"});

  const pagelength = el("select");
  [5, 10, 25, 50, 100].forEach(function(n){
    const opt = el("option");
    opt.textContent = n;
    pagelength.appendChild(opt);
  });
  pagelength.addEventListener("change", function(){
    controls.vars.page_size = this.value;
    show_page();
  });

  controls.elements = {
    prev: prevBtn,
    next: nextBtn,
    page_buttons: {},
    page_button_cont: pageBtnCont,
    pagelength: pagelength,
    summary: document.createElement("span"),
    style: document.createElement("style")
  };
  controls.vars = {
    currentpage: 1,
    page_size: default_page_size || 25,
    entries: 0,
    uid: "paginated_"+(Math.random()+"").slice(2) //used for the css rule that controls which items are shown/hidden
  };
  pagelength.value = controls.vars.page_size;
  controls.elements.summary.className = "summary";
  controls.elements.summary.elements = {
    start: document.createTextNode(""),
    end: document.createTextNode(""),
    total: document.createTextNode("")
  };
  controls.elements.summary.appendChild(document.createTextNode("Showing "));
  controls.elements.summary.appendChild(controls.elements.summary.elements.start);
  controls.elements.summary.appendChild(document.createTextNode("-"));
  controls.elements.summary.appendChild(controls.elements.summary.elements.end);
  controls.elements.summary.appendChild(document.createTextNode(" of "));
  controls.elements.summary.appendChild(controls.elements.summary.elements.total);
  controls.elements.summary.appendChild(document.createTextNode(" items."));
  controls.elements.summary.update = function(){
    this.elements.start.nodeValue = Math.max(0,(controls.vars.currentpage - 1) * controls.vars.page_size + 1);
    this.elements.end.nodeValue = Math.min(controls.vars.currentpage * controls.vars.page_size,controls.vars.entries);
    this.elements.total.nodeValue = controls.vars.entries;
  };
  page_ele.classList.add(controls.vars.uid);

  function createPageButton(pagenumber) {
    const button = document.createElement("button");
    button.appendChild(document.createTextNode(pagenumber));
    button.addEventListener("click",function(){
      show_page(pagenumber);
    });
    controls.elements.page_buttons[pagenumber] = button;
    controls.elements.page_button_cont.appendChild(button);
    return button;
  }

  let default_display_value = false;
  function show_page(page){
    let perpage = controls.vars.page_size;
    let maxpage;
    if (!page) page = controls.vars.currentpage;
    if (page == "next") { page = controls.vars.currentpage + 1; }
    if (page == "previous") { page = controls.vars.currentpage - 1; }

    //count current elements (exclude both text-filter .hidden and status-filter .status-hidden)
    let l = page_ele.querySelectorAll(":scope > :not(.hidden):not(.status-hidden):not(.wild-collapsed)");
    controls.vars.entries = l.length;

    if (!default_display_value && (controls.vars.entries > 0)) {
      let css = false;
      if (controls.elements.style.textContent != "") {
        //it's possible that the new entr(y/ies) is being hidden by our own pagination code: temporarily disable it
        css = controls.elements.style.textContent;
        controls.elements.style.textContent = "";
      }
      default_display_value = getComputedStyle(l[0]).getPropertyValue("display");
      if (default_display_value == "none") default_display_value = false; //if the calculated value is none, it's useless
      if (!default_display_value) {
        //fallback for detached/invisible nodes where computed style can be empty
        switch (l[0].tagName) {
          case "TR":
            default_display_value = "table-row";
            break;
          case "LI":
            default_display_value = "list-item";
            break;
          default:
            default_display_value = "block";
        }
      }
      if (css) {
        controls.elements.style.textContent = css;
      }
    }
    l = l.length;

    if (page instanceof HTMLElement) {
      const n = Array.from(page_ele.children).indexOf(page);
      page = Math.floor(n / perpage)+1;
    }


    page = Math.max(1,page);
    //clamp to last page with content if above total entries
    maxpage = Math.max(1,Math.floor((l-1)/perpage)+1);
    page = Math.min(page,maxpage);

    //check if we need to add page buttons
    //always show first page, last page, and pages around the current one
    for (let i = 1; i <= maxpage; i++) {
      if (!(i in controls.elements.page_buttons)) {
        createPageButton(i);
      }
      //hide buttons except first, last and around current page
      switch(i) {
        case 1:
        //case page-2:
        case page-1:
        case page:
        case page+1:
        //case page+2:
        case maxpage: {
          controls.elements.page_buttons[i].classList.remove("hidden");
          break;
        }
        default: {
          controls.elements.page_buttons[i].classList.add("hidden");
        }
      }
    }
    //hide buttons beyond the current page range
    //NB: assumes buttons are created in order and their index matches their key-1
    const button_indexes = Object.keys(controls.elements.page_buttons);
    for (let i = maxpage; i < button_indexes.length; i++) {
      controls.elements.page_buttons[button_indexes[i]].classList.add("hidden");
    }

    if (controls.vars.currentpage in controls.elements.page_buttons) controls.elements.page_buttons[controls.vars.currentpage].classList.remove("active");

    //disable prev/next buttons if applicable
    if (page == 1) { controls.elements.prev.setAttribute("disabled",""); }
    else { controls.elements.prev.removeAttribute("disabled"); }
    if (page == maxpage) { controls.elements.next.setAttribute("disabled",""); }
    else { controls.elements.next.removeAttribute("disabled"); }

    controls.vars.currentpage = page;
    if (controls.vars.currentpage in controls.elements.page_buttons) controls.elements.page_buttons[page].classList.add("active");

    //create css rules
    const vis = "*:not(.hidden):not(.status-hidden):not(.wild-collapsed)";
    //hide everything
    let css = "."+controls.vars.uid+" > * { display: none !important; }\n";
    //show everything but the pages before the current one
    css += "."+controls.vars.uid+" > "+vis+" ";
    css += ("~ "+vis+" ").repeat(Math.max(0,perpage*(page-1)));
    css += "{ display: "+(default_display_value ? default_display_value : "block")+" !important; }\n";

    //hide everthing after the pages including the current one
    css += "."+controls.vars.uid+"> "+vis+" ";
    css += ("~ "+vis+" ").repeat(perpage*page);
    css += "{ display: none !important; }\n";

    controls.elements.style.textContent = css;
    controls.elements.summary.update();

  }
  page_ele.show_page = show_page;
  show_page();

  const pagesDiv = el("div", {class: "pages"});
  pagesDiv.appendChild(controls.elements.prev);
  pagesDiv.appendChild(controls.elements.page_button_cont);
  pagesDiv.appendChild(controls.elements.next);
  controls.appendChild(pagesDiv);
  controls.appendChild(controls.elements.summary);

  const labelEl = el("label", {class: "input_container"});
  const labelSpan = el("span");
  labelSpan.textContent = "Items per page:";
  labelEl.appendChild(labelSpan);
  labelEl.appendChild(controls.elements.pagelength);
  controls.appendChild(labelEl);
  controls.appendChild(controls.elements.style);

  return controls;
},
sortableItems: function(item_container,getVal,options){
  options = Object.assign({
    controls: false, //container of header-like elements that will be clickable to sort the index it belongs to
    sortby: false, //initial index to sort by
    sortdir: 1, //initial sorting direction
    container: item_container,
    sortsave: false, //name of the mist.stored variable where the last used sorting should be stored
    //secondary_sortby: when the values of the sortby column are equavalent, sort by this column instead. False: do not use secondary sortby; Omitted: use options.sortby or controls.getAttribute("data-sortby")
    secondary_sortdir: false //the direction the secondary sorting should be in: 1 or -1. False: use options.sortdir or controls.getAttribute("data-sortdir")
  },options);

  options.sortdir = Math.sign(options.sortdir);
  options.secondary_sortdir = Math.sign(options.secondary_sortdir);

  let lastsortby = options.sortby;
  let lastsortdir = options.sortdir;
  if (options.controls) {
    if (options.controls.getAttribute("data-sortby")) {
      lastsortby = options.controls.getAttribute("data-sortby");
    }
    if (options.controls.getAttribute("data-sortdir")) {
      lastsortdir = options.controls.getAttribute("data-sortdir");
    }
    for (let i = 0; i < options.controls.children.length; i++) {
      if (options.controls.children[i].hasAttribute("data-index")) {
        options.controls.children[i].addEventListener("click",function(){
          const v = this.getAttribute("data-index");
          item_container.sort(v);
        });
      }
    }

    const resizeKey = options.sortsave ? "colwidths_"+options.sortsave : null;
    let savedWidths = {};
    let hasResized = false;
    if (resizeKey) {
      try { savedWidths = JSON.parse(sessionStorage.getItem(resizeKey)) || {}; } catch(e) {}
    }
    function applyTableLayout(tbl, fixed) {
      if (!tbl) return;
      tbl.style.tableLayout = fixed ? "fixed" : "";
      if (!fixed) {
        tbl.style.width = "";
        tbl.style.minWidth = "";
      }
    }
    // A column should not be draggable narrower than its own header label,
    // otherwise the header text clips and the column becomes unidentifiable.
    function headerMinWidth(th) {
      let min = 40;
      try {
        const cs = getComputedStyle(th);
        const ctx = headerMinWidth._ctx || (headerMinWidth._ctx = document.createElement("canvas").getContext("2d"));
        ctx.font = (cs.fontWeight || "400") + " " + cs.fontSize + " " + cs.fontFamily;
        let label = th.getAttribute("data-index") || th.textContent || "";
        if ((cs.textTransform || "").indexOf("uppercase") >= 0) { label = label.toUpperCase(); }
        let textW = ctx.measureText(label).width;
        const ls = parseFloat(cs.letterSpacing);
        if (!isNaN(ls)) { textW += ls * label.length; }
        const fs = parseFloat(cs.fontSize) || 14;
        // horizontal padding + room for the sort-direction arrow pseudo-element
        min = textW + (parseFloat(cs.paddingLeft) || 0) + (parseFloat(cs.paddingRight) || 0) + fs * 1.5;
      } catch (e) { /* fall back to default below */ }
      return Math.max(40, Math.ceil(min));
    }
    function colMin(cell) {
      // Measured lazily on first drag, when the table is attached and themed.
      if (!cell._colMinWidth) { cell._colMinWidth = headerMinWidth(cell); }
      return cell._colMinWidth;
    }
    function startResize(handle, th, colId, startX) {
      const idx = Array.prototype.indexOf.call(options.controls.children, th);
      const table = th.closest("table");
      const ths = options.controls.children;
      const widths = [];
      let totalColWidth = 0;
      for (let j = 0; j < ths.length; j++) {
        widths[j] = ths[j].offsetWidth;
        totalColWidth += widths[j];
      }
      table.style.minWidth = "0";
      table.style.width = totalColWidth + "px";
      table.style.tableLayout = "fixed";
      for (let j = 0; j < ths.length; j++) ths[j].style.width = widths[j] + "px";
      const rightCols = [];
      const rightWidths = [];
      const rightIds = [];
      const rightMins = [];
      for (let j = idx + 1; j < ths.length; j++) {
        if (ths[j].hasAttribute("data-index") && widths[j] > 0) {
          rightCols.push(ths[j]);
          rightWidths.push(widths[j]);
          rightIds.push(ths[j].getAttribute("data-index"));
          rightMins.push(colMin(ths[j]));
        }
      }
      handle.classList.add("col-resize-active");
      return {rightCols: rightCols, rightWidths: rightWidths, rightIds: rightIds, rightMins: rightMins, minW: colMin(th), startA: widths[idx], startX: startX, totalColWidth: totalColWidth, table: table};
    }
    function doResize(state, currentX, th) {
      let delta = currentX - state.startX;
      if (!state.rightCols.length) {
        const newW = Math.max(state.minW, state.startA + delta);
        th.style.width = newW + "px";
        state.table.style.width = (state.totalColWidth - state.startA + newW) + "px";
        return;
      }
      delta = Math.max(-(state.startA - state.minW), delta);
      let remaining = delta;
      for (let i = 0; i < state.rightCols.length; i++) {
        if (remaining > 0) {
          const available = state.rightWidths[i] - state.rightMins[i];
          const taken = Math.min(remaining, available);
          state.rightCols[i].style.width = (state.rightWidths[i] - taken) + "px";
          remaining -= taken;
        } else if (i === 0 && remaining < 0) {
          state.rightCols[i].style.width = (state.rightWidths[i] - remaining) + "px";
          remaining = 0;
        } else {
          state.rightCols[i].style.width = state.rightWidths[i] + "px";
        }
      }
      th.style.width = (state.startA + delta - remaining) + "px";
    }
    function endResize(handle, th, colId, state) {
      document.body.classList.remove("col-resizing");
      handle.classList.remove("col-resize-active");
      if (resizeKey) {
        let w = {};
        try { w = JSON.parse(sessionStorage.getItem(resizeKey)) || {}; } catch(e) {}
        w[colId] = th.offsetWidth;
        for (let i = 0; i < state.rightCols.length; i++) {
          w[state.rightIds[i]] = state.rightCols[i].offsetWidth;
        }
        sessionStorage.setItem(resizeKey, JSON.stringify(w));
      }
    }
    for (let i = 0; i < options.controls.children.length; i++) {
      const th = options.controls.children[i];
      if (!th.hasAttribute("data-index")) continue;
      th.style.position = "relative";
      const colId = th.getAttribute("data-index");
      if (savedWidths[colId]) {
        th.style.width = savedWidths[colId]+"px";
        hasResized = true;
      }
      const handle = document.createElement("div");
      handle.className = "col-resize-handle";
      handle.addEventListener("mousedown", (function(handle, th, colId) {
        return function(e) {
          e.preventDefault();
          e.stopPropagation();
          const state = startResize(handle, th, colId, e.clientX);
          function onMove(ev) { doResize(state, ev.clientX, th); }
          function onUp() {
            document.removeEventListener("mousemove", onMove);
            document.removeEventListener("mouseup", onUp);
            endResize(handle, th, colId, state);
          }
          document.addEventListener("mousemove", onMove);
          document.addEventListener("mouseup", onUp);
          document.body.classList.add("col-resizing");
        };
      })(handle, th, colId));
      handle.addEventListener("touchstart", (function(handle, th, colId) {
        return function(e) {
          if (e.touches.length !== 1) return;
          e.preventDefault();
          e.stopPropagation();
          const state = startResize(handle, th, colId, e.touches[0].clientX);
          function onTouchMove(ev) {
            ev.preventDefault();
            if (ev.touches.length !== 1) return;
            doResize(state, ev.touches[0].clientX, th);
          }
          function onTouchEnd() {
            document.removeEventListener("touchmove", onTouchMove);
            document.removeEventListener("touchend", onTouchEnd);
            document.removeEventListener("touchcancel", onTouchEnd);
            endResize(handle, th, colId, state);
          }
          document.addEventListener("touchmove", onTouchMove, {passive: false});
          document.addEventListener("touchend", onTouchEnd);
          document.addEventListener("touchcancel", onTouchEnd);
          document.body.classList.add("col-resizing");
        };
      })(handle, th, colId), {passive: false});
      handle.addEventListener("click", function(e) { e.stopPropagation(); });
      handle.addEventListener("dblclick", (function(th) {
        return function(e) {
          e.stopPropagation();
          const table = th.closest("table");
          applyTableLayout(table, false);
          const ths = options.controls.children;
          for (let j = 0; j < ths.length; j++) ths[j].style.width = "";
          if (resizeKey) sessionStorage.removeItem(resizeKey);
        };
      })(th));
      th.appendChild(handle);
    }
    if (hasResized) {
      const tbl = options.controls.closest("table");
      if (tbl) {
        let tw = 0;
        for (let j = 0; j < options.controls.children.length; j++) tw += options.controls.children[j].offsetWidth;
        tbl.style.minWidth = "0";
        tbl.style.width = tw + "px";
        tbl.style.tableLayout = "fixed";
      }
    }

  }
  if (!("secondary_sortby" in options)) {
    options.secondary_sortby = lastsortby;
    if (!options.secondary_sortdir) {
      options.secondary_sortdir = lastsortdir;
    }
  }


  if (options.sortsave) {
    const stored = mistHelpers.stored.get();
    if (!(options.sortsave in stored)) {
      stored[options.sortsave] = {};
    }
    const result = Object.assign({by: lastsortby, dir: lastsortdir},stored[options.sortsave]);
    lastsortby = result.by;
    lastsortdir = result.dir;
  }

  item_container.sort = function(sortby,sortdir){
    if (!sortdir) {
      sortdir = lastsortdir;
      if (sortby == lastsortby) {
        sortdir *= -1;
      }
    }
    sortby = (sortby || (sortby == "")) ? sortby : lastsortby;

    lastsortby = sortby;
    lastsortdir = sortdir;
    if (options.sortsave) {
      mistHelpers.stored.set(options.sortsave,{by: sortby, dir: sortdir});
    }
    if (options.controls) {
      options.controls.setAttribute("data-sortby",sortby);
      options.controls.setAttribute("data-sortdir",sortdir);
      const old = options.controls.querySelector("[data-sorting]");
      if (old) { old.removeAttribute("data-sorting"); }
      const a = options.controls.querySelector("[data-index=\""+sortby+"\"]");
      if (a) a.setAttribute("data-sorting","");
    }

    try {
      const compare = new Intl.Collator('en',{numeric:true, sensitivity:'accent'}).compare;
      for (let i = 0; i < options.container.children.length-1; i++) {
        const row = options.container.children[i];
        const next = options.container.children[i+1];
      let comparisonResult = compare(getVal.call(row,sortby),getVal.call(next,sortby)) * sortdir;
      if (options.secondary_sortby && (options.secondary_sortby != sortby) && (comparisonResult == 0)) {
        comparisonResult = compare(getVal.call(row,options.secondary_sortby),getVal.call(next,options.secondary_sortby)) * options.secondary_sortdir;
      }
      if (comparisonResult > 0) {
          //the next row should be before the current one
          //put it before and then check if it needs to go up further
          options.container.insertBefore(next,row);
          if (i > 0) {
            i = i-2;
          }
        }
      }
    }
    catch (e) {
      //something went wrong, sorting failed. It's possible we tried sorting for a column that doesn't exist
      const msg = ["Failed to sort items in ",item_container," by '"+sortby+"'"];
      if (sortby != options.sortby) { //if the column we tried to sort on is not the default as passed from initialization, fall back to that
        msg.push(", falling back to '"+options.sortby+"'");
        console.warn.apply(this,msg);
        this.sort(options.sortby);
      }
      else {
        console.warn.apply(this,msg);
      }
    }

  };

  if (lastsortby !== false) { item_container.sort(); }
},
humanMime: function (type) {
  let human = false;
  switch (type) {
    case 'html5/application/vnd.apple.mpegurl':
      human = 'HLS (TS)';
      break;
    case "html5/application/vnd.apple.mpegurl;version=7":
      human = "HLS (CMAF)";
      break;
    case "html5/application/sdp":
      human = "SDP";
      break;
    case 'html5/video/webm':
      human = 'WebM';
      break;
    case 'html5/video/raw':
      human = 'Raw';
      break;
    case 'html5/video/mp4':
      human = 'MP4';
      break;
    case 'ws/video/mp4':
      human = 'MP4 (websocket)';
      break;
    case 'ws/video/raw':
      human = 'Raw (websocket)';
      break;
    case 'dtsc':
      human = 'DTSC';
      break;
    case 'html5/audio/aac':
      human = 'AAC';
      break;
    case 'html5/audio/flac':
      human = 'FLAC';
      break;
    case 'html5/image/jpeg':
      human = 'JPG';
      break;
    case 'dash/video/mp4':
      human = 'DASH';
      break;
    case 'flash/11':
      human = 'HDS';
      break;
    case 'flash/10':
      human = 'RTMP';
      break;
    case 'flash/7':
      human = 'Progressive';
      break;
    case 'html5/audio/mp3':
      human = 'MP3';
      break;
    case 'html5/audio/wav':
      human = 'WAV';
      break;
    case 'html5/video/mp2t':
    case 'html5/video/mpeg':
      human = 'TS';
      break;
    case "html5/application/vnd.ms-sstr+xml":
    case 'html5/application/vnd.ms-ss':
      human = 'Smooth Streaming';
      break;
    case 'html5/text/vtt':
      human = 'VTT Subtitles';
      break;
    case 'html5/text/plain':
      human = 'SRT Subtitles';
      break;
    case 'html5/text/javascript':
      human = 'JSON Subtitles';
      break;
    case 'rtsp':
      human = 'RTSP';
      break;
    case 'srt':
      human = 'SRT';
      break;
    case 'webrtc':
      human = "WebRTC (websocket)";
      break;
    case 'whep':
      human = "WebRTC (WHEP)";
      break;
  }
  return human;
},
popup: function(html){
  const popupEl = el('dialog', {class: 'popup'});
  const popup = {
    element: popupEl,
    show: function(content) {
      let me = this;
      this.element.setAttribute("data-changed","no");
      const closeBtn = el('button', {class: 'close'});
      closeBtn.setAttribute("data-icon", "cross");
      closeBtn.addEventListener("click", function(){
        me.closeWithConfirm();
      });
      this.element.innerHTML = "";
      this.element.appendChild(closeBtn);
      this.element.appendChild(content);
      if (!this.element.open) {
        document.body.appendChild(this.element);
        popup.element.showModal();
      }
      const fields = popup.element.querySelectorAll('.field');
      if (fields.length) fields[0].focus();
      else {
        const buttons = popup.element.querySelectorAll("button");
        if (buttons.length) buttons[buttons.length - 1].focus();
      }
    },
    close: function(){
      this.element.close();
    },
    closeWithConfirm: function(){
      if (this.element.getAttribute("data-changed") == "yes") {
        //if there is a save button in the dialog, ask for confirmation before closing
        let savebuttons = this.element.querySelectorAll(".input_container button.save");
        if (savebuttons.length) {
          let me = this;
          let actions = [{
            icon: "return",
            label: "Don't close",
            type: "cancel",
            close: true
          }, {
            icon: "trash",
            label: "Close and discard",
            close: true,
            "function": function(){
              me.close();
            }
          }];
          if (savebuttons.length == 1) {
            actions.push({
              icon: "disk",
              label: "Save and close",
              type: "save",
              close: true,
              "function": function(){
                savebuttons[0].dispatchEvent(new Event("click"));
              }
            });
          }
          uiHelpers.openConfirm({
            size: "sm",
            message: "Are you sure you want to close this window? Your settings have not been saved yet.",
            actions: actions
          });
          return;
        }
      }

      //no need to ask for confirmation, just close
      this.close();
    }
  };

  //close modal when clicking on the modal backdrop
  popup.element.addEventListener("mouseup",function(e){ //use mouseup: click does not trigger for right or middle mouse clicks. mousedown fires before blur triggers an onchange event (to detect form changes)
    if (e.target == this) {
      //no children were clicked, but it might be on the padding
      let rect = this.getBoundingClientRect();
      let isInDialog = (rect.top <= e.clientY && e.clientY <= rect.top + rect.height &&
  rect.left <= e.clientX && e.clientX <= rect.left + rect.width);
      if (!isInDialog) {
        popup.closeWithConfirm();
        return;
      }
    }
    //if context menu, hide
    if (!e.defaultPrevented && !e.target.closest(".context_menu")) {
      for (let menu of UI.elements.context_menu) {
        menu.hide();
      }
    }
  });

  popup.element.addEventListener("change",function(){
    this.setAttribute("data-changed","yes");
  });

  popup.element.addEventListener("close",function(){
    setTimeout(function(){
      popup.element.remove();
      popup.element = null;
    },1e3);
  });
  if (html) {
    popup.show(html);
  }
  return popup;
},
buildMenu: function(){
  function createButton(j,button) {
    const btnEl = el('a', {class: 'button'});
    btnEl.setAttribute('data-tab', j);

    const plainSpan = el('span', {class: 'plain'});
    plainSpan.textContent = j;
    const highlightedSpan = el('span', {class: 'highlighted'});
    highlightedSpan.textContent = j;

    btnEl.appendChild(plainSpan);
    btnEl.appendChild(highlightedSpan);

    if (button.icon) {
      const iconSpan = el('span', {class: 'nav-icon'});
      iconSpan.setAttribute('data-icon', button.icon);
      btnEl.insertBefore(iconSpan, btnEl.firstChild);
    }
    for (const k in button.classes) {
      const cls = button.classes[k];
      if (cls) {
        btnEl.classList.add(cls);
      }
    }
    if ('link' in button) {
      btnEl.setAttribute('href', button.link);
      btnEl.setAttribute('target', '_blank');
      btnEl.classList.add('external');
      const extIcon = el('span', {class: 'external-icon'});
      extIcon.setAttribute('data-icon', 'external-link');
      btnEl.appendChild(extIcon);
    }
    else if (!('submenu' in button)) {
      btnEl.addEventListener("click", function(e){
        if (this.closest('.menu').classList.contains('hide')) { return; }
        let other;
        const subEl = this.closest('.hiddenmenu[data-param]');
        if (subEl) {
          other = subEl.getAttribute("data-param");
        }
        navto(j,other);
        e.stopPropagation();
      });
    }
    return btnEl;
  }

  const menu = UI.elements.menu;
  for (const i in UI.menu) {
    if (i > 0) {
      const hr = el('hr', {class: 'nav-separator'});
      menu.appendChild(hr);
    }
    for (const j in UI.menu[i]) {
      const button = UI.menu[i][j];
      const btnEl = createButton(j,button);
      menu.appendChild(btnEl);
      if ('submenu' in button) {
        const sub = el('span', {class: 'submenu'});
        btnEl.classList.add('arrowdown');
        btnEl.appendChild(sub);
        for (const k in button.submenu) {
          sub.appendChild(createButton(k,button.submenu[k]));
        }
      }
      else if ('hiddenmenu' in button) {
        const sub = el('span', {class: 'hiddenmenu'});
        if (button.keepParam) { sub.setAttribute("data-param",""); }
        btnEl.appendChild(sub);
        for (const k in button.hiddenmenu) {
          sub.appendChild(createButton(k,button.hiddenmenu[k]));
        }
      }
    }
  }
}
};
