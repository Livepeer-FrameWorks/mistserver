export function preloadImg() {
  const parent = document.createElement("div");
  parent.className = "thumbnail";
  parent.style.display = "contents";

  let src;
  parent.setSrc = function(s) {
    src = new URL(s);
    src.searchParams.set("t", (Math.round(new Date().getTime() * 1e-3)));
    src = src.toString();
    if (this.className == "thumbnail") {
      parent.querySelector(".preloader").setAttribute("src", src);
    }
  };
  function moveJPG() {
    parent.querySelector("img.preloader").setAttribute("src", src.replace(".jpg", ".mjpg"));
    parent.className = "thumbnail moving";
  }
  function stopJPG() {
    parent.querySelector("img.preloader").setAttribute("src", src);
    parent.className = "thumbnail";
  }
  parent.addEventListener("mouseenter", function() {
    moveJPG();
  });
  parent.addEventListener("mouseleave", function() {
    stopJPG();
  });

  const img = document.createElement('img');
  parent.appendChild(img);
  const img2 = document.createElement('img');
  img2.className = "preloader";
  parent.appendChild(img2);
  const onload = function(myself, other) {
    return function() {
      this.classList.remove("preloader");
      other.classList.add("preloader");
      other.removeAttribute("src");
    };
  };
  img.addEventListener("load", onload(img, img2));
  img2.addEventListener("load", onload(img2, img));
  return parent;
}

export function formatBitrate(bps) {
  const base = 1000;
  const suffix = ['bps', 'kbps', 'Mbps', 'Gbps', 'Tbps', 'Pbps', 'Ebps', 'Zbps'];

  let newval = bps;
  let unit;
  if (newval == 0) {
    unit = suffix[0];
  } else {
    const exponent = Math.floor(Math.log(Math.abs(bps)) / Math.log(base));
    if (exponent < 0) {
      unit = suffix[0];
    } else {
      newval = newval / Math.pow(base, exponent);
      unit = suffix[exponent];
    }
  }

  newval = new Intl.NumberFormat(undefined, {
    maximumSignificantDigits: 3,
    useGrouping: "min2"
  }).format(newval);

  return [newval, unit];
}

export function Tooltip(activator, content_func, container) {
  const element = document.createElement("div");
  element.classList.add("tooltip");
  this.element = element;

  this.pos = function(pos) {
    if (!element.offsetParent) return;
    const parpos = element.offsetParent.getBoundingClientRect();

    const mh = parpos.height - element.clientHeight;
    const mw = parpos.width - element.clientWidth;

    element.style.left = Math.min(pos.pageX - parpos.x, mw) + "px";
    element.style.top = Math.min(pos.pageY - parpos.y, mh) + "px";
  };
  this.show = function(position) {
    const content = content_func();

    if (typeof content == "string") element.innerHTML = content;
    else {
      element.innerHTML = "";
      element.appendChild(content);
    }

    element.style.display = "";
    if (position) this.pos(position);
  };
  this.hide = function() {
    element.style.display = "none";
    element.innerHTML = "";
    element.style.left = "";
    element.style.top = "";
  };
  this.remove = function() {
    element.parentNode?.removeChild(element);
    this.element = null;
  };

  this.hide();
  container.appendChild(element);

  activator.addEventListener("mouseover", (e) => {
    this.show(e);
  });
  activator.addEventListener("mousemove", (e) => {
    element.style.display = "";
    this.pos(e);
  });
  activator.addEventListener("mouseout", () => {
    this.hide();
  });
}
