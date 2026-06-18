import { MistUtil } from '../../core/util.js';

var MIST_LOGO_SVG = '<svg viewBox="0 0 202.52 150" xmlns="http://www.w3.org/2000/svg" preserveAspectRatio="xMidYMid meet">'
  + '<path d="m53.406 10.557-5.0488 3.3223 42.061 63.951c-1.0569 1.2973-1.8786 2.7896-2.418 4.4121h-70.115v6.0449h69.498c0.81169 6.9508 6.713 12.344 13.879 12.344 7.1658 0 13.064-5.393 13.875-12.344h69.5v-6.0449h-70.117c-0.53924-1.6225-1.3594-3.1148-2.416-4.4121l42.061-63.951-5.0508-3.3223-41.777 63.518c-1.8391-0.89026-3.894-1.4043-6.0742-1.4043-2.1814 0-4.238 0.51505-6.0781 1.4062z" class="mist-logo-highlight"/>'
  + '<path d="m49.875 0c-7.0953 0-12.848 5.7519-12.848 12.846 0 3.7198 1.5899 7.0604 4.1152 9.4062l-20.535 46.447c-1.0469-0.19686-2.1225-0.31055-3.2266-0.31055-9.5991 0-17.381 7.783-17.381 17.381 0 9.5997 7.7817 17.381 17.381 17.381 5.3445 0 10.122-2.4174 13.311-6.2129l54.49 29.096c-0.83369 2.0328-1.3008 4.2547-1.3008 6.5879 0 9.5997 7.7811 17.379 17.379 17.379 9.5997 0 17.381-7.7792 17.381-17.379 0-2.3333-0.46899-4.5551-1.3027-6.5879l54.492-29.098c3.1877 3.796 7.963 6.2148 13.309 6.2148 9.5997 0 17.381-7.7812 17.381-17.381 0-9.5978-7.7812-17.381-17.381-17.381-1.1042 0-2.1796 0.11369-3.2266 0.31055l-20.535-46.447c2.5256-2.3459 4.1152-5.6861 4.1152-9.4062 0-7.0938-5.7519-12.846-12.846-12.846-5.9165 0-10.885 4.0056-12.377 9.4473h-78.018c-1.4922-5.4418-6.4613-9.4473-12.377-9.4473zm13.406 15.492h75.957l-24.789 11.568c-3.1173-3.8293-7.8647-6.2793-13.188-6.2793-5.3228 0-10.072 2.45-13.189 6.2793zm-3.498 5.5312 24.967 12.77c-0.30913 1.2822-0.49218 2.6131-0.49218 3.9902 0 1.6082 0.23907 3.1582 0.65625 4.6328l-54.004 32.453c-1.2919-1.6017-2.8674-2.9575-4.6426-4.0176l20.174-45.635c1.0944 0.30322 2.2426 0.47851 3.4336 0.47851 3.9887 0 7.552-1.819 9.9082-4.6719zm82.953 0c2.3561 2.8528 5.9208 4.6719 9.9102 4.6719 1.1903 0 2.3377-0.17551 3.4316-0.47851l20.176 45.635c-1.7754 1.0602-3.3507 2.4157-4.6426 4.0176l-54.004-32.453c0.41706-1.4746 0.6543-3.0247 0.6543-4.6328 0-1.377-0.18122-2.7081-0.49024-3.9902zm-54.041 28.186c2.2766 2.5025 5.2868 4.3206 8.6894 5.1211v61.355c-3.5044 0.80386-6.605 2.6644-8.9492 5.2324l-54.734-29.225c0.67236-1.8515 1.0605-3.8399 1.0605-5.9238 0-1.8275-0.28752-3.5875-0.81055-5.2422zm25.131 2e-3 54.742 31.316c-0.52293 1.6547-0.80859 3.4147-0.80859 5.2422 0 2.0838 0.38643 4.0724 1.0586 5.9238l-54.734 29.225c-2.3448-2.5682-5.4441-4.4287-8.9492-5.2324v-61.354c3.4038-0.79961 6.4143-2.6181 8.6914-5.1211z" class="mist-logo-base"/>'
  + '</svg>';

var DVD_ASPECT = 202.52 / 150;

var PARTICLE_COLORS_DEFAULT = [
  '#7aa2f7', '#bb9af7', '#9ece6a', '#73daca',
  '#7dcfff', '#f7768e', '#e0af68', '#2ac3de'
];

var BUBBLE_COLORS_DEFAULT = [
  'rgba(122,162,247,0.2)', 'rgba(187,154,247,0.2)',
  'rgba(158,206,106,0.2)', 'rgba(115,218,202,0.2)',
  'rgba(125,207,255,0.2)', 'rgba(247,118,142,0.2)',
  'rgba(224,175,104,0.2)', 'rgba(42,195,222,0.2)'
];

var HITMARKER_AUDIO_DATA_URL = 'data:audio/mpeg;base64,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';

function createSyntheticHitmarkerSound() {
  try {
    var Ctor = window.AudioContext || window.webkitAudioContext;
    if (!Ctor) return;
    var ctx = new Ctor();
    var osc1 = ctx.createOscillator();
    var osc2 = ctx.createOscillator();
    var noiseBuf = ctx.createBuffer(1, ctx.sampleRate * 0.1, ctx.sampleRate);
    var noiseSrc = ctx.createBufferSource();
    var data = noiseBuf.getChannelData(0);
    for (var i = 0; i < data.length; i++) data[i] = Math.random() * 2 - 1;
    noiseSrc.buffer = noiseBuf;
    var g1 = ctx.createGain(), g2 = ctx.createGain(), ng = ctx.createGain(), mg = ctx.createGain();
    osc1.connect(g1); osc2.connect(g2); noiseSrc.connect(ng);
    g1.connect(mg); g2.connect(mg); ng.connect(mg); mg.connect(ctx.destination);
    var t = ctx.currentTime;
    osc1.frequency.setValueAtTime(1800, t);
    osc1.frequency.exponentialRampToValueAtTime(900, t + 0.08);
    osc2.frequency.setValueAtTime(3600, t);
    osc2.frequency.exponentialRampToValueAtTime(1800, t + 0.04);
    osc1.type = 'triangle'; osc2.type = 'sine';
    g1.gain.setValueAtTime(0, t); g1.gain.linearRampToValueAtTime(0.4, t + 0.002);
    g1.gain.exponentialRampToValueAtTime(0.001, t + 0.12);
    g2.gain.setValueAtTime(0, t); g2.gain.linearRampToValueAtTime(0.3, t + 0.001);
    g2.gain.exponentialRampToValueAtTime(0.001, t + 0.06);
    ng.gain.setValueAtTime(0, t); ng.gain.linearRampToValueAtTime(0.2, t + 0.001);
    ng.gain.exponentialRampToValueAtTime(0.001, t + 0.01);
    mg.gain.setValueAtTime(0.5, t);
    osc1.start(t); osc2.start(t); noiseSrc.start(t);
    osc1.stop(t + 0.15); osc2.stop(t + 0.15); noiseSrc.stop(t + 0.02);
  } catch(e) {}
}

function playHitmarkerSound() {
  try {
    var audio = new Audio(HITMARKER_AUDIO_DATA_URL);
    audio.volume = 0.3;
    audio.play().catch(function() { createSyntheticHitmarkerSound(); });
  } catch(e) { createSyntheticHitmarkerSound(); }
}

function makeStatusIconSVG(type) {
  if (type === 'loading') {
    return '<svg class="status-icon spin" fill="none" viewBox="0 0 24 24">'
      + '<circle style="opacity:0.25" cx="12" cy="12" r="10" stroke="var(--mist-color-warning)" stroke-width="4"/>'
      + '<path style="opacity:0.75" fill="var(--mist-color-warning)" d="M4 12a8 8 0 018-8V0C5.373 0 0 5.373 0 12h4zm2 5.291A7.962 7.962 0 014 12H0c0 3.042 1.135 5.824 3 7.938l3-2.647z"/>'
      + '</svg>';
  }
  if (type === 'offline') {
    return '<svg class="status-icon" fill="none" viewBox="0 0 24 24" stroke="var(--mist-color-danger)">'
      + '<path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M18.364 5.636a9 9 0 010 12.728m0 0l-2.829-2.829m2.829 2.829L21 21M15.536 8.464a5 5 0 010 7.072m0 0l-2.829-2.829m-4.243 2.829a4.978 4.978 0 01-1.414-2.83m-1.414 5.658a9 9 0 01-2.167-9.238m7.824 2.167a1 1 0 111.414 1.414m-1.414-1.414L3 3m8.293 8.293l1.414 1.414"/>'
      + '</svg>';
  }
  if (type === 'error') {
    return '<svg class="status-icon" fill="none" viewBox="0 0 24 24" stroke="var(--mist-color-danger)">'
      + '<path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 9v2m0 4h.01m-6.938 4h13.856c1.54 0 2.502-1.667 1.732-3L13.732 4c-.77-1.333-2.694-1.333-3.464 0L3.34 16c-.77 1.333.192 3 1.732 3z"/>'
      + '</svg>';
  }
  // default spinner
  return '<svg class="status-icon spin" fill="none" viewBox="0 0 24 24">'
    + '<circle style="opacity:0.25" cx="12" cy="12" r="10" stroke="var(--mist-color-accent)" stroke-width="4"/>'
    + '<path style="opacity:0.75" fill="var(--mist-color-accent)" d="M4 12a8 8 0 018-8V0C5.373 0 0 5.373 0 12h4zm2 5.291A7.962 7.962 0 014 12H0c0 3.042 1.135 5.824 3 7.938l3-2.647z"/>'
    + '</svg>';
}

function classifyState(state) {
  if (!state) return 'default';
  var s = state.toLowerCase();
  if (s.indexOf('offline') >= 0) return 'offline';
  if (s.indexOf('initializing') >= 0 || s.indexOf('booting') >= 0 || s.indexOf('waiting') >= 0 || s.indexOf('shutting') >= 0) return 'loading';
  if (s.indexOf('invalid') >= 0) return 'error';
  if (s.indexOf('online') >= 0) return 'online';
  return 'default';
}

export var idleScreenBlueprints = {
  idleScreen: function() {
    var MistVideo = this;
    var container = document.createElement('div');
    container.className = 'mistvideo-idle';
    container.setAttribute('role', 'status');
    container.setAttribute('aria-label', MistVideo.translate('stream status'));

    // ── Hitmarker container ──
    var hitmarkerLayer = document.createElement('div');
    hitmarkerLayer.className = 'mistvideo-idle-hitmarkers';
    container.appendChild(hitmarkerLayer);

    // ── Particles ──
    var particlesEl = document.createElement('div');
    particlesEl.className = 'mistvideo-idle-particles';
    for (var i = 0; i < 12; i++) {
      var p = document.createElement('div');
      p.className = 'particle';
      var pLeft = Math.random() * 100;
      var pSize = Math.random() * 4 + 2;
      var pDur = 8 + Math.random() * 4;
      var pDelay = Math.random() * 8;
      var pColor = PARTICLE_COLORS_DEFAULT[i % PARTICLE_COLORS_DEFAULT.length];
      p.style.cssText = '--p-left:' + pLeft + '%;--p-size:' + pSize + 'px;--p-dur:' + pDur + 's;--p-delay:' + pDelay + 's;--p-color:' + pColor;
      particlesEl.appendChild(p);
    }
    container.appendChild(particlesEl);

    // ── Bubbles ──
    var bubblesEl = document.createElement('div');
    bubblesEl.className = 'mistvideo-idle-bubbles';
    var bubbleEls = [];
    var bubbleTimers = [];
    for (var i = 0; i < 8; i++) {
      var b = document.createElement('div');
      b.className = 'bubble';
      b.style.top = (Math.random() * 80 + 10) + '%';
      b.style.left = (Math.random() * 80 + 10) + '%';
      b.style.width = (Math.random() * 60 + 30) + 'px';
      b.style.height = b.style.width;
      b.style.background = BUBBLE_COLORS_DEFAULT[i % BUBBLE_COLORS_DEFAULT.length];
      b.style.opacity = '0';
      bubblesEl.appendChild(b);
      bubbleEls.push(b);
    }
    container.appendChild(bubblesEl);

    function animateBubble(index) {
      var el = bubbleEls[index];
      if (!el) return;
      el.style.opacity = '0.15';
      var visDur = 4000 + Math.random() * 3000;
      bubbleTimers.push(MistVideo.timers.start(function() {
        el.style.opacity = '0';
        bubbleTimers.push(MistVideo.timers.start(function() {
          el.style.top = (Math.random() * 80 + 10) + '%';
          el.style.left = (Math.random() * 80 + 10) + '%';
          var sz = (Math.random() * 60 + 30) + 'px';
          el.style.width = sz;
          el.style.height = sz;
          bubbleTimers.push(MistVideo.timers.start(function() {
            animateBubble(index);
          }, 200));
        }, 1500));
      }, visDur));
    }

    function startBubbles() {
      for (var i = 0; i < bubbleEls.length; i++) {
        (function(idx) {
          bubbleTimers.push(MistVideo.timers.start(function() {
            animateBubble(idx);
          }, idx * 500));
        })(i);
      }
    }

    // ── Center logo ──
    var logoWrap = document.createElement('div');
    logoWrap.className = 'mistvideo-idle-logo';

    var logoPulse = document.createElement('div');
    logoPulse.className = 'logo-pulse';
    logoWrap.appendChild(logoPulse);

    var logoBtn = document.createElement('div');
    logoBtn.className = 'logo-button';
    logoBtn.setAttribute('role', 'button');
    logoBtn.setAttribute('tabindex', '0');
    logoBtn.setAttribute('aria-label', MistVideo.translate('mistserver logo'));
    logoBtn.innerHTML = MIST_LOGO_SVG;
    logoWrap.appendChild(logoBtn);

    container.appendChild(logoWrap);

    var logoSize = 100;
    var logoOffsetX = 0, logoOffsetY = 0;

    function updateLogoSize() {
      var rect = container.getBoundingClientRect();
      var min = Math.min(rect.width, rect.height);
      if (!isFinite(min) || min <= 0) return;
      logoSize = min * 0.2;
      logoBtn.style.width = logoSize + 'px';
      logoBtn.style.height = logoSize + 'px';
      logoPulse.style.width = (logoSize * 1.4) + 'px';
      logoPulse.style.height = (logoSize * 1.4) + 'px';
    }

    function updateLogoTransform() {
      logoWrap.style.transform = 'translate(calc(-50% + ' + logoOffsetX + 'px), calc(-50% + ' + logoOffsetY + 'px))';
    }

    // ── Mouse interaction ──
    function onMouseMove(e) {
      var rect = container.getBoundingClientRect();
      if (rect.width === 0 || rect.height === 0) return;
      var cx = rect.left + rect.width / 2;
      var cy = rect.top + rect.height / 2;
      var dx = e.clientX - cx;
      var dy = e.clientY - cy;
      var dist = Math.sqrt(dx * dx + dy * dy);
      var maxDist = logoSize * 1.5;
      if (dist < maxDist && dist > 0) {
        var strength = (maxDist - dist) / maxDist;
        var push = 50 * strength;
        logoOffsetX = -(dx / dist) * push;
        logoOffsetY = -(dy / dist) * push;
      } else {
        logoOffsetX = 0;
        logoOffsetY = 0;
      }
      updateLogoTransform();
    }

    function onMouseLeave() {
      logoOffsetX = 0;
      logoOffsetY = 0;
      updateLogoTransform();
    }

    MistUtil.event.addListener(container, 'mousemove', onMouseMove, container);
    MistUtil.event.addListener(container, 'mouseleave', onMouseLeave, container);

    // ── Hitmarker click ──
    MistUtil.event.addListener(logoBtn, 'click', function(e) {
      e.stopPropagation();
      var rect = container.getBoundingClientRect();
      var hx = e.clientX - rect.left;
      var hy = e.clientY - rect.top;

      var hm = document.createElement('div');
      hm.className = 'hitmarker';
      hm.style.left = hx + 'px';
      hm.style.top = hy + 'px';
      hm.style.transform = 'translate(-50%, -50%)';
      var lines = ['tl', 'tr', 'bl', 'br'];
      for (var j = 0; j < 4; j++) {
        var line = document.createElement('div');
        line.className = 'hitmarker-line ' + lines[j];
        hm.appendChild(line);
      }
      hitmarkerLayer.appendChild(hm);
      playHitmarkerSound();

      MistVideo.timers.start(function() {
        if (hm.parentNode) hm.parentNode.removeChild(hm);
      }, 600);
    }, container);

    // ── DVD bouncing logo ──
    var dvdEl = document.createElement('div');
    dvdEl.className = 'mistvideo-idle-dvd';
    dvdEl.innerHTML = MIST_LOGO_SVG;
    container.appendChild(dvdEl);

    var dvdPos = { top: 0, left: 0 };
    var dvdVel = { x: 1.8, y: 1.6 };
    var dvdDims = { w: 60, h: 60 / DVD_ASPECT };
    var dvdColor = PARTICLE_COLORS_DEFAULT[0];
    var dvdColorIdx = 0;
    var dvdFrame = null;
    var dvdLastTime = 0;

    function dvdResize() {
      var pw = container.clientWidth;
      var ph = container.clientHeight;
      if (pw <= 0 || ph <= 0) return;
      var maxW = pw * 0.08;
      var maxH = ph * 0.08;
      var w = maxW;
      var h = w / DVD_ASPECT;
      if (h > maxH) { h = maxH; w = h * DVD_ASPECT; }
      dvdDims.w = Math.max(20, w);
      dvdDims.h = Math.max(20, h);
      dvdEl.style.width = dvdDims.w + 'px';
      dvdEl.style.height = dvdDims.h + 'px';
      dvdPos.top = Math.random() * Math.max(0, ph - dvdDims.h);
      dvdPos.left = Math.random() * Math.max(0, pw - dvdDims.w);
      var baseSpeed = Math.max(1.2, Math.min(dvdDims.w, dvdDims.h) / 70);
      dvdVel.x = baseSpeed * (Math.random() > 0.5 ? 1 : -1);
      dvdVel.y = baseSpeed * (Math.random() > 0.5 ? 1 : -1);
    }

    function dvdPickColor() {
      dvdColorIdx = (dvdColorIdx + 1) % PARTICLE_COLORS_DEFAULT.length;
      dvdColor = PARTICLE_COLORS_DEFAULT[dvdColorIdx];
      var paths = dvdEl.querySelectorAll('svg path');
      for (var i = 0; i < paths.length; i++) {
        paths[i].style.fill = dvdColor;
      }
    }

    function dvdTick(ts) {
      if (!dvdLastTime) dvdLastTime = ts;
      var dt = ts - dvdLastTime;
      dvdLastTime = ts;
      var mult = Math.min(dt / 16, 2);

      var pw = container.clientWidth;
      var ph = container.clientHeight;
      var maxTop = ph - dvdDims.h;
      var maxLeft = pw - dvdDims.w;

      dvdPos.top += dvdVel.y * mult;
      dvdPos.left += dvdVel.x * mult;

      var bounced = false;
      if (dvdPos.top <= 0 || dvdPos.top >= maxTop) {
        dvdVel.y = -dvdVel.y;
        dvdPos.top = Math.max(0, Math.min(maxTop, dvdPos.top));
        bounced = true;
      }
      if (dvdPos.left <= 0 || dvdPos.left >= maxLeft) {
        dvdVel.x = -dvdVel.x;
        dvdPos.left = Math.max(0, Math.min(maxLeft, dvdPos.left));
        bounced = true;
      }
      if (bounced) dvdPickColor();

      dvdEl.style.top = dvdPos.top + 'px';
      dvdEl.style.left = dvdPos.left + 'px';

      dvdFrame = requestAnimationFrame(dvdTick);
    }

    function dvdStart() {
      if (dvdFrame) return;
      dvdLastTime = 0;
      dvdFrame = requestAnimationFrame(dvdTick);
    }

    function dvdStop() {
      if (dvdFrame) {
        cancelAnimationFrame(dvdFrame);
        dvdFrame = null;
      }
    }

    // ── Status overlay ──
    var statusEl = document.createElement('div');
    statusEl.className = 'mistvideo-idle-status';

    var indicatorEl = document.createElement('div');
    indicatorEl.className = 'status-indicator';
    statusEl.appendChild(indicatorEl);

    var statusIconWrap = document.createElement('span');
    statusIconWrap.innerHTML = makeStatusIconSVG('default');
    indicatorEl.appendChild(statusIconWrap);

    var statusTextEl = document.createElement('span');
    statusTextEl.textContent = MistVideo.translate('waiting for stream');
    indicatorEl.appendChild(statusTextEl);

    var progressBar = document.createElement('div');
    progressBar.className = 'progress-bar';
    progressBar.style.display = 'none';
    var progressFill = document.createElement('div');
    progressFill.className = 'progress-fill';
    progressFill.style.width = '0%';
    progressBar.appendChild(progressFill);
    statusEl.appendChild(progressBar);

    var retryBtn = document.createElement('button');
    retryBtn.type = 'button';
    retryBtn.className = 'retry-btn';
    retryBtn.textContent = MistVideo.translate('retry');
    retryBtn.style.display = 'none';
    MistUtil.event.addListener(retryBtn, 'click', function() {
      if (MistVideo.player && MistVideo.player.api && MistVideo.player.api.load) {
        MistVideo.player.api.load();
      } else {
        MistVideo.reload('Retry button clicked');
      }
    }, container);
    statusEl.appendChild(retryBtn);

    container.appendChild(statusEl);

    // ── Texture overlay ──
    var texture = document.createElement('div');
    texture.className = 'mistvideo-idle-texture';
    texture.style.background = 'radial-gradient(circle at 20% 80%, color-mix(in srgb, var(--mist-color-accent) 3%, transparent) 0%, transparent 50%),'
      + 'radial-gradient(circle at 80% 20%, color-mix(in srgb, var(--mist-color-link, var(--mist-color-accent)) 3%, transparent) 0%, transparent 50%),'
      + 'radial-gradient(circle at 40% 40%, color-mix(in srgb, var(--mist-color-warning) 2%, transparent) 0%, transparent 50%)';
    container.appendChild(texture);

    // ── State management ──
    var isShowing = false;

    function updateStatus(state, percentage) {
      var type = classifyState(state);

      statusIconWrap.innerHTML = makeStatusIconSVG(type);

      if (state) {
        statusTextEl.textContent = state;
      } else {
        statusTextEl.textContent = MistVideo.translate('waiting for stream');
      }

      if (type === 'loading' && typeof percentage === 'number') {
        progressBar.style.display = '';
        progressFill.style.width = Math.min(100, Math.max(0, percentage)) + '%';
      } else {
        progressBar.style.display = 'none';
      }

      retryBtn.style.display = (type === 'error') ? '' : 'none';
    }

    function show() {
      if (isShowing) return;
      isShowing = true;
      MistUtil.class.add(container, 'show');
      dvdResize();
      dvdStart();
      updateLogoSize();
      startBubbles();
    }

    function hide() {
      if (!isShowing) return;
      isShowing = false;
      MistUtil.class.remove(container, 'show');
      dvdStop();
      for (var i = 0; i < bubbleTimers.length; i++) {
        MistVideo.timers.stop(bubbleTimers[i]);
      }
      bubbleTimers = [];
    }

    // ── Public API on MistVideo ──
    MistVideo.showIdleScreen = function(state, percentage) {
      updateStatus(state, percentage);
      show();
      MistVideo.clearError();
    };

    MistVideo.hideIdleScreen = function() {
      hide();
    };

    // ── ResizeObserver ──
    if (typeof ResizeObserver !== 'undefined') {
      var ro = new ResizeObserver(function() {
        if (isShowing) {
          updateLogoSize();
          dvdResize();
        }
      });
      // observe once inserted into DOM
      var checkInserted = function() {
        if (MistVideo.destroyed) return;
        if (container.parentNode) {
          ro.observe(container);
          return;
        }
        setTimeout(checkInserted, 100);
      };
      checkInserted();
    }

    // ── Auto-hide when video plays ──
    if (MistVideo.video) {
      var autoHideEvents = ['playing', 'timeupdate'];
      for (var i = 0; i < autoHideEvents.length; i++) {
        MistUtil.event.addListener(MistVideo.video, autoHideEvents[i], function(e) {
          if (!isShowing) return;
          if (e.type === 'timeupdate' && MistVideo.player && MistVideo.player.api && MistVideo.player.api.currentTime === 0) return;
          if (MistVideo.state === 'Stream is online') {
            hide();
          }
        }, container);
      }
    }

    return container;
  }
};
