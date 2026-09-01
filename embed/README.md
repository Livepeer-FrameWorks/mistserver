# @optimist-video/mistplayer-core

MistServer's streaming player as an ESM package. Supports HLS, DASH, WebRTC (WHEP), MEWS, raw WebSocket (H.265), and native HTML5 playback with automatic protocol selection.

- [Install](#install) · [Quick Start](#quick-start) · [API Reference](#api-reference) · [Options](#options)
- [Features](#features): [Accessibility](#accessibility) · [i18n](#internationalization) · [Keyboard](#keyboard-controls) · [Thumbnails](#thumbnail-previews) · [AirPlay](#airplay) · [Audio Gain](#audio-gain-volume-boost) · [Mobile](#mobile) · [Breakpoints](#responsive-breakpoints)
- [Theming](#theming) · [TypeScript](#typescript)
- [Advanced](#advanced-extending-the-player): Custom Skins · Blueprints · Wrappers · CSS Architecture · Legacy

## Install

```bash
npm install @optimist-video/mistplayer-core
```

Or use directly from your MistServer instance — no install needed:

```html
<!-- ESM -->
<script type="module">
  import { createPlayer } from 'https://myserver:8080/player.mjs';
</script>

<!-- IIFE -->
<script src="https://myserver:8080/player.js"></script>
```

## Quick Start

```js
import { createPlayer } from '@optimist-video/mistplayer-core';

const player = createPlayer({
  target: document.getElementById('player'),
  host: 'https://myserver:8080',
  stream: 'live',
});

player.on('initialized', () => console.log('Ready'));
player.play();
```

For IIFE (no build step): `mistPlay('live', { target: document.getElementById('player') })`.

## API Reference

### `createPlayer(options)` → `Player`

Creates a player instance. This is the recommended entry point.

### Queries (read state)

| Property | Type | Description |
|----------|------|-------------|
| `player.currentTime` | `number` | Current playback position (seconds) |
| `player.duration` | `number` | Stream duration (seconds, `Infinity` for live) |
| `player.volume` | `number` | Volume level (`0.0` to `1.0`) |
| `player.muted` | `boolean` | Whether audio is muted |
| `player.paused` | `boolean` | Whether playback is paused |
| `player.playbackRate` | `number` | Playback speed (`1` = normal) |
| `player.loop` | `boolean` | Whether looping is enabled |
| `player.buffered` | `TimeRanges` | Buffered time ranges |
| `player.fullscreen` | `boolean` | Whether fullscreen is active |
| `player.pip` | `boolean` | Whether picture-in-picture is active |
| `player.tracks` | `object\|null` | Available tracks: `{video, audio, subtitle}` |
| `player.size` | `object\|null` | Player dimensions: `{width, height}` |
| `player.capabilities` | `object` | What the active wrapper supports |
| `player.quality` | `number\|null` | Playback quality score (`0.0` to `1.0`) |
| `player.streamState` | `string\|null` | Stream status |
| `player.video` | `HTMLVideoElement` | The underlying `<video>` element |
| `player.info` | `object` | Stream metadata (tracks, sources, type) |
| `player.source` | `object` | Currently active source (`{url, type}`) |
| `player.playerName` | `string` | Active wrapper name (`"html5"`, `"wheprtc"`, etc.) |

### Mutations (change state)

| Method / Setter | Description |
|--------|-------------|
| `player.play()` | Start or resume playback (returns `Promise`) |
| `player.pause()` | Pause playback |
| `player.currentTime = 30` | Seek to position (seconds) |
| `player.volume = 0.5` | Set volume (`0.0` to `1.0`) |
| `player.muted = true` | Mute/unmute audio |
| `player.playbackRate = 2` | Set playback speed |
| `player.loop = true` | Enable/disable looping |
| `player.fullscreen = true` | Enter/exit fullscreen |
| `player.pip = true` | Enter/exit picture-in-picture |
| `player.setTrack(type, id)` | Select a track: `setTrack('video', '1')` |
| `player.destroy()` | Tear down the player and clean up |
| `player.reload()` | Rebuild the player from scratch |
| `player.setTheme(name, mode?)` | Apply a theme: `setTheme('dracula')` or `setTheme('catppuccin', 'light')` |
| `player.setTheme(tokens)` | Apply custom tokens: `setTheme({ 'color-accent': '#ff6600' })` |
| `player.translate(key, fallback?)` | Look up an i18n string |

### Subscriptions (react to changes)

Subscribe to state changes reactively. Callbacks fire immediately with the current value, then on every change:

```js
const unsub = player.state.on('paused', (isPaused) => {
  myButton.textContent = isPaused ? 'Play' : 'Pause';
});

player.state.get('currentTime');  // read without subscribing
unsub();                          // unsubscribe
```

**Available state properties:** `paused`, `playing`, `currentTime`, `duration`, `volume`, `muted`, `playbackRate`, `loop`, `buffered`, `seeking`, `ended`, `loading`, `fullscreen`, `pip`, `tracks`, `streamState`, `error`.

### Events

```js
player.on('initialized', () => { /* player is ready */ });
player.on('error', (e) => { /* playback error */ });
player.on('ended', () => { /* stream ended */ });
player.off('ended', myCallback);
```

| Event | When |
|-------|------|
| `initialized` | Player built and ready |
| `play`, `playing`, `pause`, `ended` | Playback state changes |
| `seeking`, `seeked` | Seek started / completed |
| `timeupdate`, `durationchange` | Time / duration changed |
| `volumechange` | Volume or mute changed |
| `waiting`, `canplay`, `error` | Buffering / ready / error |
| `playerUpdate_trackChanged` | Track selection changed |
| `player_resize` | Player container resized |

## Options

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `target` | `HTMLElement` | **required** | Container element |
| `stream` | `string` | **required** | Stream name |
| `host` | `string` | auto-detected | MistServer URL |
| `autoplay` | `boolean` | `true` | Auto-start playback |
| `controls` | `boolean\|"stock"` | `true` | Show controls. `"stock"` for native browser controls |
| `muted` | `boolean` | `false` | Start muted |
| `loop` | `boolean` | `false` | Loop playback |
| `poster` | `string` | — | Poster image URL |
| `skin` | `string\|object` | `"default"` | Skin name or custom skin object |
| `fillSpace` | `boolean` | `false` | Fill parent container |
| `width` / `height` | `number` | — | Fixed dimensions (px) |
| `maxwidth` / `maxheight` | `number` | — | Maximum dimensions (px) |
| `forcePlayer` | `string` | — | Force wrapper (`"html5"`, `"wheprtc"`, etc.) |
| `forceType` | `string` | — | Force MIME type |
| `setTracks` | `object` | — | Initial track selection: `{video: 1, audio: 2}` |
| `ABR_resize` | `boolean` | `true` | Match video resolution to player size |
| `ABR_bitrate` | `boolean` | `true` | Lower bitrate on playback issues |
| `keyControls` | `boolean\|"focus"` | `true` | Keyboard controls. `"focus"` = only when focused |
| `keyMap` | `object` | — | Override default keyboard bindings (see below) |
| `translations` | `string\|object` | — | Language code (`"nl"`) or custom strings object (see below) |
| `thumbnails` | `string` | — | URL to a WebVTT thumbnail sprite file |

## Features

### Accessibility

All interactive controls include ARIA attributes (`role`, `aria-label`, `tabindex`) for screen readers and keyboard navigation. Sliders use `role="slider"` with live value updates. Loading and error overlays use `aria-live` regions. Tooltips on every button show the label and keyboard shortcut.

### Internationalization

Use a built-in locale by passing a language code:

```js
createPlayer({ target: el, stream: 'live', translations: 'nl' });
```

**Built-in locales:** `en` (English), `nl` (Dutch), `de` (German), `fr` (French), `es` (Spanish).

Or pass a custom object to override individual strings:

```js
createPlayer({
  target: el,
  stream: 'live',
  translations: {
    play: 'Afspelen',
    pause: 'Pauzeren',
    mute: 'Dempen',
    fullscreen: 'Volledig scherm',
    'exit fullscreen': 'Volledig scherm sluiten',
  },
});
```

You can also import locale objects directly and extend them:

```js
import { createPlayer, locales } from '@optimist-video/mistplayer-core';

createPlayer({
  target: el,
  stream: 'live',
  translations: { ...locales.fr, play: 'Jouer' },
});
```

For IIFE usage, locales are available as `window.MistLocales`:

```js
mistPlay('live', { target: el, translations: MistLocales.de });
```

Read strings programmatically with `player.translate('play')`.

<details>
<summary><strong>All translation keys</strong></summary>

**Playback controls**

| Key | English default |
|-----|----------------|
| `play` | Play |
| `pause` | Pause |
| `mute` | Mute |
| `unmute` | Unmute |
| `volume` | Volume |
| `fullscreen` | Full screen |
| `exit fullscreen` | Exit full screen |
| `pip` | Picture in picture |
| `loop` | Loop |
| `settings` | Settings |
| `automatic` | Automatic |
| `none` | None |
| `live` | live |
| `current time` | Current time |
| `total time` | Total time |
| `seek bar` | Seek bar |
| `seek forward` | Seek forward |
| `seek backward` | Seek backward |
| `-10s` | -10s |
| `+10s` | +10s |
| `airplay` | AirPlay |

**Keyboard overlay feedback**

| Key | English default |
|-----|----------------|
| `speed` | Speed |
| `speed doubled` | Speed doubled |
| `muted` | Muted |
| `(muted)` | (muted) |
| `seek backward seconds` | - 10 seconds |
| `seek forward seconds` | + 10 seconds |
| `to start` | To start.. |
| `to end` | To end.. |
| `frame forward` | Frame +1 |
| `frame backward` | Frame -1 |

**Error overlay & casting**

| Key | English default |
|-----|----------------|
| `reload video` | Reload video |
| `reload player` | Reload player |
| `next source` | Next source |
| `ignore` | Ignore |
| `player encountered a problem` | The player has encountered a problem |
| `chromecast encountered a problem` | The chromecast has encountered a problem |
| `stop casting` | Stop casting |
| `chromecast` | Chromecast |
| `select cast device` | Select a device to cast to |
| `casting to` | Casting to |

**Track selection**

| Key | English default |
|-----|----------------|
| `the current track` | The current |
| `track` | Track |

**Time formatting**

| Key | English default |
|-----|----------------|
| `n sec ago` | {n} sec ago |
| `in n sec` | in {n} sec |
| `at` | at |

**Dev skin**

| Key | English default |
|-----|----------------|
| `logs` | Logs |
| `player` | Player |
| `protocol` | Protocol |
| `language` | Language |
| `theme` | Theme |
| `is playing` | is playing |
| `player control` | Player control |
| `reload` | Reload |
| `next combo` | Next combo |

**Dev diagnostics**

| Key | English default |
|-----|----------------|
| `playback score` | Playback score |
| `corrupted frames` | Corrupted frames |
| `dropped frames` | Dropped frames |
| `total frames` | Total frames |
| `decoded audio` | Decoded audio |
| `decoded video` | Decoded video |
| `nack` | Negative acknowledgements |
| `picture losses` | Picture losses |
| `packets lost` | Packets lost |
| `packets received` | Packets received |
| `bytes received` | Bytes received |
| `local latency` | Local latency |
| `messages received` | Messages received |
| `messages sent` | Messages sent |
| `current bitrate` | Current bitrate |
| `framerate in` | Framerate in |
| `framerate out` | Framerate out |

</details>

### Keyboard Controls

| Action | Default keys | Description |
|--------|-------------|-------------|
| `togglePlay` | `k`, `K`, `Space` | Toggle play/pause. Hold Space for 2x speed |
| `seekForward` / `seekBackward` | `l`/`j`, `ArrowRight`/`ArrowLeft` | Seek +/- 10 seconds |
| `volumeUp` / `volumeDown` | `ArrowUp`/`ArrowDown`, `+`/`-` | Volume +/- 10% |
| `toggleFullscreen` | `f` | Toggle fullscreen |
| `toggleMute` | `m` | Toggle mute |
| `togglePip` | `i` | Toggle picture-in-picture |
| `cycleSubtitle` | `c` / `C` | Cycle subtitles (Shift for reverse) |
| `speedUp` / `speedDown` | `>`/`<`, `.`/`,` | Playback speed (`.`/`,` when paused = frame step) |
| `seekToStart` / `seekToEnd` | `Home` / `End` | Jump to start/end |
| `seekPercent` | `0`–`9` | Jump to 0%–90% |

Override with `keyMap`. Set an action to `false` to disable it:

```js
createPlayer({
  target: el,
  stream: 'live',
  keyMap: {
    togglePlay: ['k', 'K'],      // remove Space
    seekForward: ['ArrowRight'],  // only arrow key
    togglePip: false,             // disable
  },
});
```

### Thumbnail Previews

Show thumbnail frames when hovering the progress bar. Provide a WebVTT file:

```js
createPlayer({ target: el, stream: 'myvod', thumbnails: '/thumbs.vtt' });
```

Supports both individual images and sprite sheets with `xywh=` spatial fragments:

```vtt
WEBVTT

00:00:00.000 --> 00:00:05.000
/thumbs/sprite.jpg#xywh=0,0,160,90

00:00:05.000 --> 00:00:10.000
/thumbs/sprite.jpg#xywh=160,0,160,90
```

### AirPlay

On WebKit browsers, an AirPlay button appears automatically when AirPlay devices are detected. No configuration needed.

### Audio Gain (Volume Boost)

Scroll the mouse wheel on the volume slider or speaker icon past 100% to boost volume up to 200% via the Web Audio API. The tooltip shows the boosted percentage. Dragging the slider resets gain to 1x.

### Mobile

- **Double-tap to seek**: Tap the left/right side of the video twice to seek -/+ 10 seconds
- **Orientation lock**: Entering fullscreen locks to landscape; exiting unlocks
- **Seek buttons**: +10s / -10s buttons appear on small screens (`xs`, `sm` breakpoints)

### Responsive Breakpoints

The player sets a `data-size` attribute on `.mistvideo` based on width:

| Attribute | Width |
|-----------|-------|
| `xs` | < 384px |
| `sm` | 384 – 575px |
| `md` | 576 – 767px |
| `lg` | 768 – 959px |
| `xl` | >= 960px |

At `xs`, time display and volume slider hide; seek buttons appear. Target these in CSS:

```css
.mistvideo[data-size="xs"] .my-control { display: none; }
```

## Theming

### CSS Custom Properties

The fastest way to brand the player — pure CSS:

```css
.mistvideo {
  --mist-color-accent: #e63946;
  --mist-color-surface: rgba(20, 20, 30, 0.95);
  --mist-color-text: #f1faee;
}
```

Or at runtime: `player.setTheme({ 'color-accent': '#e63946' })`.

### Preset Themes

| Theme | Dark | Light | Accent |
|-------|:----:|:-----:|--------|
| `default` | yes | yes | green (`#0f0`) |
| `tokyo-night` | yes | yes | blue (`#7aa2f7`) |
| `dracula` | yes | — | purple (`#bd93f9`) |
| `nord` | yes | — | frost (`#88c0d0`) |
| `catppuccin` | yes | yes | mauve (`#cba6f7`) |
| `gruvbox` | yes | yes | yellow (`#d79921`) |
| `one-dark` | yes | — | blue (`#61afef`) |
| `github-dark` | yes | — | blue (`#58a6ff`) |
| `rose-pine` | yes | — | iris (`#c4a7e7`) |
| `solarized` | yes | yes | cyan (`#2aa198`) |
| `ayu-mirage` | yes | — | orange (`#ffad66`) |

```js
player.setTheme('dracula');
player.setTheme('catppuccin', 'light');
```

The player auto-detects `prefers-color-scheme` for light/dark mode.

<details>
<summary><strong>Full token reference</strong></summary>

**Colors**
| Token | Default | Controls |
|-------|---------|----------|
| `--mist-color-text` | `#fff` | Text, labels |
| `--mist-color-text-muted` | `rgba(255,255,255,0.5)` | Secondary text |
| `--mist-color-icon-fill` | `#fff` | SVG icon fill |
| `--mist-color-icon-stroke` | `#fff` | SVG icon stroke |
| `--mist-color-surface` | `rgba(0,0,0,0.8)` | Control bar, menus, tooltips |
| `--mist-color-accent` | `#0f0` | Active states, progress fill |
| `--mist-color-accent-hover` | derived | Hover state (via `color-mix()`) |
| `--mist-color-progress-track` | `#333` | Progress bar background |
| `--mist-color-progress-fill` | `var(--mist-color-accent)` | Progress bar filled portion |
| `--mist-color-progress-buffer` | `var(--mist-color-text-muted)` | Buffered range |
| `--mist-color-border` | `var(--mist-color-text-muted)` | Borders |
| `--mist-color-link` | `var(--mist-color-accent)` | Links |
| `--mist-icon-stroke-width` | `1.5` | SVG stroke thickness |

**Typography**
| Token | Default |
|-------|---------|
| `--mist-font-family` | `sans-serif` |
| `--mist-font-size` | `14.5px` |
| `--mist-font-size-sm` | `0.9em` |
| `--mist-font-size-lg` | `1.5em` |

**Spacing & Sizing**
| Token | Default |
|-------|---------|
| `--mist-space-xs` | `2.5px` |
| `--mist-space-sm` | `5px` |
| `--mist-space-md` | `10px` |
| `--mist-control-height` | `42px` |
| `--mist-icon-size` | `22px` |
| `--mist-progress-height` | `2px` |
| `--mist-progress-height-hover` | `10px` |
| `--mist-radius-sm` / `md` / `lg` | `0` / `4px` / `8px` |
| `--mist-transition-fast` / `normal` / `slow` | `0.1s` / `0.25s` / `0.5s` |
</details>

## TypeScript

Type declarations ship at `dist/index.d.ts`:

```ts
import { createPlayer, type Player, type PlayerOptions } from '@optimist-video/mistplayer-core';

const player: Player = createPlayer({
  target: document.getElementById('player')!,
  stream: 'live',
  translations: { play: 'Start' },
  keyMap: { togglePlay: ['k', ' '] },
});

player.state.on('currentTime', (time: number) => console.log(time));
```

## Distribution

| Format | File | Served at | Use case |
|--------|------|-----------|----------|
| **ESM** | `dist/index.js` | `/player.mjs` | `import` in apps/bundlers |
| **IIFE** | `dist/player.iife.js` | `/player.js` | `<script>` tag, no build step |

---

## Advanced: Extending the Player

The sections below cover skin authoring, custom blueprint development, wrapper registration, and CSS architecture. Most users don't need these.

<details>
<summary><strong>Custom Skins</strong></summary>

Register a custom skin, then use it by name:

```js
import { MistSkins } from '@optimist-video/mistplayer-core';

MistSkins.mytheme = {
  inherit: 'default',
  tokens: {
    '--mist-radius-sm': '8px',
    '--mist-progress-height': '4px',
  },
  css: { skin: '/path/to/mytheme.css' },
};

createPlayer({ ..., skin: 'mytheme' });
```

**Skin object shape:**

```js
{
  inherit: 'default',           // inherit from another skin
  structure: { main: {...} },   // DOM structure tree
  blueprints: { ... },          // UI component functions
  icons: { blueprints: {...} }, // SVG icon definitions
  colors: { ... },              // legacy color tokens (auto-bridged to CSS vars)
  tokens: { ... },              // direct --mist-* overrides
  css: { skin: 'url' },        // stylesheet URL(s)
}
```

**Icon overrides:**

```js
MistSkins.mytheme = {
  inherit: 'default',
  icons: {
    blueprints: {
      play: { size: 45, svg: '<path d="M10 5 L35 22.5 L10 40 Z" class="fill" />' },
    },
  },
};
```

Icons use CSS classes: `.fill`, `.stroke`, `.semiFill`, `.toggle` — colored by `--mist-color-icon-*` tokens.

**Legacy color properties** (still supported, auto-bridged to CSS vars):

| Property | CSS custom property |
|----------|-------------------|
| `fill` | `--mist-color-icon-fill` |
| `semiFill` | `--mist-color-text-muted` |
| `stroke` | `--mist-color-icon-stroke` |
| `background` | `--mist-color-surface` |
| `accent` | `--mist-color-accent` |

</details>

<details>
<summary><strong>Writing Custom Blueprints</strong></summary>

Blueprints are factory functions that create UI components. Inside a blueprint, `this` is the `MistVideo` instance:

| Property | Description |
|----------|-------------|
| `this.video` | The `<video>` element |
| `this.api` | Normalized playback API (play/pause/seek/volume) |
| `this.playerState` | Reactive state: `.on(prop, cb)`, `.get(prop)` |
| `this.fullscreen` | `.supported`, `.active`, `.toggle()`, `.request()`, `.exit()` |
| `this.pip` | Same as fullscreen |
| `this.info` | Stream metadata |
| `this.options` | Player options |
| `this.skin.icons.build(type, size)` | Build an SVG icon |
| `this.container` | Root player DOM element |
| `this.translate(key, fallback?)` | i18n string lookup |
| `this.gain` | Audio gain: `.value` (0–2), `.init()` |
| `this.log(msg, type)` | Player logging |
| `this.timers` | Timer management (auto-cleanup) |

**Example: Skip Intro button**

```js
MistSkins.myplayer = {
  inherit: 'default',
  blueprints: {
    skipIntro: function() {
      var MistVideo = this;
      var btn = document.createElement('button');
      btn.textContent = 'Skip Intro';
      btn.style.display = 'none';
      this.playerState.on('currentTime', function(time) {
        btn.style.display = (time < 45) ? '' : 'none';
      });
      btn.addEventListener('click', function() { MistVideo.api.currentTime = 45; });
      return btn;
    }
  },
  structure: {
    main: {
      type: 'container', classes: ['mistvideo'],
      children: [
        { type: 'videocontainer' },
        { type: 'skipIntro' },
        { type: 'controls' },
        { type: 'loading' },
        { type: 'error' },
      ],
    },
  },
};
```

**Hide or replace built-in controls:**

```js
blueprints: {
  volume: function() { return null; },  // hide
  play: function() {                     // replace
    var btn = document.createElement('button');
    // ... your implementation
    return btn;
  },
}
```

**Structure descriptor:**

```js
{
  type: 'container',                      // blueprint type (required)
  classes: ['my-class'],                  // CSS classes
  children: [{ type: 'play' }, ...],     // child nodes
  if: function() { return this.info.hasVideo; },  // conditional
  then: { type: 'videocontainer' },
}
```

**Available blueprint types:**

| Category | Blueprints |
|----------|------------|
| **Layout** | `container`, `video`, `videocontainer`, `secondaryVideo`, `switchVideo` |
| **Controls** | `controls`, `submenu`, `hoverWindow`, `draggable` |
| **Playback** | `progress`, `play`, `seekBackward`, `seekForward`, `speaker`, `volume`, `currentTime`, `totalTime` |
| **Settings** | `playername`, `mimetype`, `logo`, `settings`, `loop`, `fullscreen`, `picture-in-picture`, `tracks` |
| **Overlay** | `text`, `placeholder`, `timeout`, `polling`, `loading`, `error`, `tooltip`, `button` |
| **Media** | `subtitles`, `chromecast`, `airplay`, `keyControls` |

</details>

<details>
<summary><strong>Custom Protocol Wrappers</strong></summary>

```js
import { registerWrapper } from '@optimist-video/mistplayer-core';

registerWrapper('myprotocol', {
  name: 'My Protocol',
  priority: 50,
  isMimeSupported: function(mime) { return mime === 'myprotocol/video'; },
  isBrowserSupported: function(mime, source, MistVideo) {
    if (!window.MyProtocolSupport) return false;
    return ['video', 'audio'];
  },
  player: function() {
    this.build = function(MistVideo, callback) {
      var video = document.createElement('video');
      new MyProtocolConnection(MistVideo.source.url).attachTo(video);
      callback(video);
    };
    this.api = {
      play: function() { /* ... */ },
      pause: function() { /* ... */ },
      get paused() { /* ... */ },
      get currentTime() { /* ... */ },
      set currentTime(v) { /* ... */ },
      get duration() { /* ... */ },
      get volume() { /* ... */ },
      set volume(v) { /* ... */ },
      get muted() { /* ... */ },
      set muted(v) { /* ... */ },
      unload: function() { /* cleanup */ },
    };
  },
});
```

Required API: `play()`, `pause()`, `paused`, `currentTime` (get/set), `duration`, `volume` (get/set), `muted` (get/set). Optional: `buffered`, `unload()`, `setTracks(obj)`, `ABR_resize(size)`.

</details>

<details>
<summary><strong>CSS Architecture</strong></summary>

The player CSS uses `@layer` for specificity management:

```
@layer mist.tokens  → default custom property values
@layer mist.base    → general.css (base styles)
@layer mist.skin    → default.css / dev.css (skin-specific)
```

Unlayered CSS always wins:

```css
.mistvideo { --mist-color-accent: red; }
```

Or use `@layer mist.overrides` for organized overrides.

</details>

<details>
<summary><strong>Legacy APIs & Migration</strong></summary>

**`mistPlay()` (legacy)** — returns the raw `MistVideo` instance instead of the clean facade:

```js
var mv = mistPlay('live', { target: el });  // legacy
var player = createPlayer({ target: el, stream: 'live' });  // recommended
```

**IIFE globals** — when using `/player.js`, exports are on `window`: `createPlayer`, `mistPlay`, `MistThemes`, `MistSkins`, `MistUtil`, `PlayerState`, `registerWrapper`.

**`$variable` CSS migration** — old `$variable` syntax in skin CSS still works via auto-bridge:

| Old | New |
|-----|-----|
| `$fill` | `var(--mist-color-icon-fill)` |
| `$accent` | `var(--mist-color-accent)` |
| `$background` | `var(--mist-color-surface)` |
| `$stroke` | `var(--mist-color-icon-stroke)` |
| `$progressBackground` | `var(--mist-color-progress-track)` |

</details>
