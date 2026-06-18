const DUCK_SVG = '<svg width="179.93mm" height="150.37mm" viewBox="0 0 179.93 150.37" xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink"><defs><linearGradient id="dg1"><stop style="stop-color:#ffde00" offset="0"/><stop style="stop-color:#ffb100" offset="1"/></linearGradient><linearGradient id="dg2" x1="94.899" x2="80.56" y1="137.38" y2="212.71" gradientUnits="userSpaceOnUse" xlink:href="#dg1"/><linearGradient id="dg3" x1="386.86" x2="170.96" y1="661.38" y2="973.76" gradientUnits="userSpaceOnUse" xlink:href="#dg1"/><linearGradient id="dg4" x1="32.865" x2="31.541" y1="101.57" y2="141.51" gradientUnits="userSpaceOnUse"><stop style="stop-color:#ffa100" offset="0"/><stop style="stop-color:#da5f00" offset="1"/></linearGradient><linearGradient id="dg5" x1="50.886" x2="53.22" y1="109.57" y2="105.63" gradientTransform="matrix(1.4578 0 0 1.4578 -17.408 -54.421)" gradientUnits="userSpaceOnUse"><stop offset="0"/><stop style="stop-color:#575757" offset="1"/></linearGradient></defs><g transform="translate(-7.8657 -81.297)"><path transform="scale(.26458)" d="m229.82 308.62c-52.686 8.5857-93.755 51.43-99.728 104.9-.82845 7.4149-.82455 19.563.0165 26.976 3.1814 28.042 16.139 53.842 36.191 73.063 14.308 13.715 13.476 25.064-3.3447 35.581-75.123 46.971-99.633 162.36-19.341 241 29.597 28.985 92.384 57.495 132.37 68.838 210.72 59.773 447.47-40.454 433.17-321.72-.78668-15.474-13.176-21.838-27.173-15.197-80.159 38.031-211.64 74.046-336.75 26.067-2.1766-.83471-5.6864-2.238-7.8634-3.0716-7.2202-2.7645-14.32-5.1811-21.299-7.2783-7.1432-2.1467-7.5593-6.234-1.3107-10.301 28.294-18.416 47.46-47.906 52.91-81.043 1.7462-10.619 1.7381-28.195.40004-38.878-6.4678-51.639-39.343-92.201-99.247-99.371-10.722-1.2833-28.347-1.2943-39 .44173z" style="fill:url(#dg3)"/><path d="m9.6843 118.33c11.858-2.4107 15.567-.48281 24.45-6.3787 1.2169-.80771 2.9884-.66599 4.0036.38507l4.4584 4.6161a4.5146 4.5146 76.369 011.0759 4.4368l-2.2563 7.5008c-.42092 1.3993-1.8853 2.9113-3.2867 3.3253-19.604 5.7915-26.038-.98581-30.082-10.848-.55428-1.3519.20464-2.746 1.6365-3.0372z" style="fill:url(#dg4)"/><path d="m110.34 150.49c21.746 3.312 42.195-5.6218 54.698-13.819 2.4436-1.6021 4.7728-.70704 5.1774 2.1871 8.627 61.709-37.458 83.206-77.241 67.108-2.7086-1.096-6.9776-3.1735-9.4763-4.6873-32.518-19.7-20.196-58.139 16.461-52.825 2.8912.41913 7.4935 1.5969 10.382 2.0368z" style="fill:url(#dg2)"/><g transform="matrix(.60507 0 0 .60507 18.341 45.6)"><circle cx="59.599" cy="102.31" r="13.157" style="fill:#fff"/><g transform="matrix(1.1337 0 0 1.1337 -10.577 -13.668)"><ellipse cx="57.26" cy="104.74" rx="4.9154" ry="7.5191" style="fill:url(#dg5)"/><circle cx="59.022" cy="100.78" r="1.2466" style="fill:#fff"/></g></g></g></svg>';
const DUCK_COUNT = 25;
const DURATION = 5000;
let active = false;
const kwaak = new Audio("data:audio/ogg;base64,T2dnUwACAAAAAAAAAADaEttMAAAAAJJmF14BE09wdXNIZWFkAQE4AYA+AAAAAABPZ2dTAAAAAAAAAAAAANoS20wBAAAA1iSHDgEbT3B1c1RhZ3MLAAAAbGlib3B1cyAxLjQAAAAAT2dnUwAE5jcAAAAAAADaEttMAgAAAFi/7WoPCQoOEhQTDxEUDRANDQ8KCAWOfCtJiSENCIMxqiP40EZWgAiEVuRsKA5Iklt8rfiwCL03d4ZzUNda+gAfJmiTsJmsCL09EYp43qwIsW/0L8hYXnA0YBgIvUzUE7JOYekIGOkZ+Pk35VOACL1MTwOgnC/X3Xv0YSWICL1PwX6a8JO6qLnbJW5aeUAIv/9oFr0izN2RaQ1TaArcM8xR2AiF8RzuHDa2GXI8g4AIl2Wx/2c+SPfctuaiSdBACIV8ofY7lxiF+t55HgiWd0A9snA5FMxZcfAIlZvx8pQA/m8HF3I/YmgIBnFgADYJ0uQg");

function spamDetector(el, threshold, cb) {
  let clicks = [];
  el.addEventListener('click', function() {
    const now = Date.now();
    clicks.push(now);
    clicks = clicks.filter(function(t) { return now - t < 2000; });
    if (clicks.length >= threshold) {
      clicks = [];
      cb();
    }
  });
}

const konamiSeq = [38,38,40,40,37,39,37,39,66,65];
let konamiPos = 0;
document.addEventListener('keydown', function(e) {
  if (e.keyCode === konamiSeq[konamiPos]) {
    konamiPos++;
    if (konamiPos === konamiSeq.length) {
      konamiPos = 0;
      launchDuckSwarm();
    }
  } else {
    konamiPos = 0;
  }
});

function launchDuckSwarm() {
  if (active) return;
  active = true;

  kwaak.currentTime = 0;
  kwaak.play();

  const h1 = document.querySelector('header .logo');
  const origText = h1.textContent;
  h1.textContent = 'QUACK QUACK QUACK!';
  h1.classList.add('quacking');

  const overlay = document.createElement('div');
  overlay.id = 'duck-swarm';
  document.body.appendChild(overlay);

  const W = window.innerWidth;
  const H = window.innerHeight;
  const ducks = [];

  for (let i = 0; i < DUCK_COUNT; i++) {
    const el = document.createElement('div');
    el.className = 'duck';
    el.innerHTML = DUCK_SVG.replace(/dg(\d)/g, 'dg' + i + '_$1');
    const scale = 0.8 + Math.random() * 0.6;
    el.style.width = Math.round(40 * scale) + 'px';
    el.style.height = Math.round(40 * scale) + 'px';
    el.style.cursor = 'pointer';
    el.addEventListener('click', function() {
      const q = kwaak.cloneNode();
      q.play();
    });
    overlay.appendChild(el);

    const edge = Math.floor(Math.random() * 4);
    let x, y, vx, vy;
    const speed = 2 + Math.random() * 4;
    let angle;
    switch (edge) {
      case 0: x = Math.random() * W; y = -50; angle = Math.PI * (0.25 + Math.random() * 0.5); break;
      case 1: x = W + 50; y = Math.random() * H; angle = Math.PI * (0.75 + Math.random() * 0.5); break;
      case 2: x = Math.random() * W; y = H + 50; angle = -Math.PI * (0.25 + Math.random() * 0.5); break;
      default: x = -50; y = Math.random() * H; angle = Math.PI * (-0.25 + Math.random() * 0.5); break;
    }
    vx = Math.cos(angle) * speed;
    vy = Math.sin(angle) * speed;

    ducks.push({
      el: el,
      x: x, y: y,
      vx: vx, vy: vy,
      rot: Math.random() * 360,
      rotSpeed: (Math.random() - 0.5) * 10,
      size: Math.round(40 * scale),
      entered: false
    });
  }

  const start = Date.now();
  function frame() {
    const elapsed = Date.now() - start;
    for (let i = 0; i < ducks.length; i++) {
      const d = ducks[i];
      d.x += d.vx;
      d.y += d.vy;
      d.rot += d.rotSpeed;

      if (!d.entered && d.x > 0 && d.x < W && d.y > 0 && d.y < H) {
        d.entered = true;
        kwaak.cloneNode().play();
      }
      let bounced = false;
      if (d.x < -10) { d.x = -10; d.vx = Math.abs(d.vx); bounced = true; }
      if (d.x > W - d.size + 10) { d.x = W - d.size + 10; d.vx = -Math.abs(d.vx); bounced = true; }
      if (d.y < -10) { d.y = -10; d.vy = Math.abs(d.vy); bounced = true; }
      if (d.y > H - d.size + 10) { d.y = H - d.size + 10; d.vy = -Math.abs(d.vy); bounced = true; }
      if (bounced) { kwaak.cloneNode().play(); }

      d.el.style.transform = 'translate(' + d.x.toFixed(1) + 'px,' + d.y.toFixed(1) + 'px) rotate(' + d.rot.toFixed(1) + 'deg)';
    }

    if (elapsed < DURATION) {
      requestAnimationFrame(frame);
    } else {
      exitDucks(ducks, overlay, h1, origText);
    }
  }
  requestAnimationFrame(frame);
}

function exitDucks(ducks, overlay, h1, origText) {
  let remaining = ducks.length;
  const shuffled = ducks.slice().sort(function() { return Math.random() - 0.5; });

  shuffled.forEach(function(d, i) {
    setTimeout(function() {
      kwaak.cloneNode().play();
      d.el.style.transition = 'transform 0.4s ease-in, opacity 0.4s ease-in';
      d.el.style.opacity = '0';
      const flyX = d.vx > 0 ? window.innerWidth + 100 : -100;
      const flyY = d.vy > 0 ? window.innerHeight + 100 : -100;
      d.el.style.transform = 'translate(' + flyX + 'px,' + flyY + 'px) rotate(' + (d.rot + 180) + 'deg)';
      setTimeout(function() {
        d.el.remove();
        remaining--;
        if (remaining <= 0) {
          overlay.remove();
          h1.textContent = origText;
          h1.classList.remove('quacking');
          active = false;
        }
      }, 400);
    }, i * 120);
  });
}

document.addEventListener('DOMContentLoaded', function() {
  const logo = document.querySelector('header .logo');
  if (logo) spamDetector(logo, 5, launchDuckSwarm);
});
