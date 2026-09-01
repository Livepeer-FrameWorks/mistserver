import { MistUtil } from '../../core/util.js';

export function createMenu(MistVideo, options) {
  // options: { type, items: [{ value, label, selected }], onChange, anchorElement }
  var container = document.createElement("div");
  container.className = "mistvideo-menu";
  container.setAttribute("role","listbox");
  container.setAttribute("aria-label",options.type || "menu");
  container.setAttribute("tabindex","-1");

  var currentValue = options.selected || "";
  var itemElements = [];

  function buildItems() {
    MistUtil.empty(container);
    itemElements = [];
    for (var i = 0; i < options.items.length; i++) {
      var item = options.items[i];
      var el = document.createElement("div");
      el.className = "mistvideo-menu-item";
      el.setAttribute("role","option");
      el.setAttribute("tabindex","0");
      el.setAttribute("data-value",item.value);
      if (item.value === currentValue) {
        el.setAttribute("aria-selected","true");
        el.setAttribute("data-selected","");
      }

      var indicator = document.createElement("span");
      indicator.className = "mistvideo-menu-indicator";
      el.appendChild(indicator);

      var label = document.createElement("span");
      label.className = "mistvideo-menu-label";
      label.textContent = item.label;
      el.appendChild(label);

      el._value = item.value;
      itemElements.push(el);
      container.appendChild(el);

      (function(el, val){
        el.addEventListener("click",function(e){
          e.stopPropagation();
          setValue(val);
          if (options.onChange) options.onChange(val);
          hide();
        });
        el.addEventListener("keydown",function(e){
          var idx = itemElements.indexOf(el);
          if (e.key === "ArrowDown" && idx < itemElements.length - 1) {
            itemElements[idx+1].focus();
            e.preventDefault();
          } else if (e.key === "ArrowUp" && idx > 0) {
            itemElements[idx-1].focus();
            e.preventDefault();
          } else if (e.key === "Enter" || e.key === " ") {
            el.click();
            e.preventDefault();
          } else if (e.key === "Escape") {
            hide();
            e.preventDefault();
          }
        });
      })(el, item.value);
    }
  }

  function setValue(val) {
    currentValue = val;
    for (var i = 0; i < itemElements.length; i++) {
      if (itemElements[i]._value === val) {
        itemElements[i].setAttribute("aria-selected","true");
        itemElements[i].setAttribute("data-selected","");
      } else {
        itemElements[i].removeAttribute("aria-selected");
        itemElements[i].removeAttribute("data-selected");
      }
    }
  }

  function show() {
    // Close other open menus in the same player
    var allMenus = MistVideo.container.querySelectorAll(".mistvideo-menu[data-open]");
    for (var i = 0; i < allMenus.length; i++) {
      if (allMenus[i] !== container) {
        allMenus[i].style.display = "none";
        allMenus[i].removeAttribute("data-open");
      }
    }
    container.style.display = "";
    container.setAttribute("data-open","");
    for (var i = 0; i < itemElements.length; i++) {
      if (itemElements[i].hasAttribute("data-selected")) {
        itemElements[i].focus();
        return;
      }
    }
    if (itemElements.length) itemElements[0].focus();
  }

  function hide() {
    container.style.display = "none";
    container.removeAttribute("data-open");
  }

  container.style.display = "none";
  buildItems();

  container.setValue = setValue;
  container.getValue = function(){ return currentValue; };
  container.show = show;
  container.hide = hide;
  container.toggle = function(){ container.style.display === "none" ? show() : hide(); };
  container.setItems = function(items){
    options.items = items;
    buildItems();
  };

  // Close on outside click
  document.addEventListener("click",function(e){
    if (!container.contains(e.target) && container.style.display !== "none") {
      hide();
    }
  });

  return container;
}
