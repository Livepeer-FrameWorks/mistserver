// LSP theme registry - named color palettes for the management UI.
// Each theme has dark and/or light mode palettes with CSS variable values.
// Player theme IDs match keys in MistThemes (embed/src/core/themes.js).

var THEMES = [
  {
    id: 'tokyo-night',
    label: 'Tokyo Night',
    icon: 'moon',
    modes: ['dark', 'light'],
    palettes: {
      dark: {
        colorScheme: 'dark',
        backgroundColor: '#1a1b26',
        secondaryBackgroundColor: '#1f2335',
        secondaryBackgroundColorDim: '#1f233580',
        tertiaryBackgroundColor: '#15161e',
        textColor: '#c0caf5',
        secondaryTextColor: '#a9b1d6',
        highlightedColor: '#e0e0e0',
        accentColor: '#7aa2f7',
        accentTextColor: '#1a1b26',
        accentDarkTextColor: '#ffffff',
        shadowColor: 'rgba(0,0,0,0.4)',
        inputBackgroundColor: '#24283b',
        inputActiveBackgroundColor: '#1a1b26',
        inputActiveOutlineColor: '#565f89',
        inputTextColor: '#c0caf5',
        changedColor: '#e0af68',
        scrollbarBackgroundColor: '#1a1b26',
        scrollbarMainColor: '#414868',
        red: '#f7768e',
        orange: '#ff9e64',
        green: '#9ece6a',
        'border-color': '#414868',
        'hover-bg': '#292e42',
      },
      light: {
        colorScheme: 'light',
        backgroundColor: '#e1e2e7',
        secondaryBackgroundColor: '#d5d6db',
        secondaryBackgroundColorDim: '#d5d6db80',
        tertiaryBackgroundColor: '#c8c9ce',
        textColor: '#3b4261',
        secondaryTextColor: '#4e5772',
        highlightedColor: '#1a1b26',
        accentColor: '#2e7de9',
        accentTextColor: '#ffffff',
        accentDarkTextColor: '#ffffff',
        shadowColor: 'rgba(0,0,0,0.12)',
        inputBackgroundColor: '#c8c9ce',
        inputActiveBackgroundColor: '#b4b5b9',
        inputActiveOutlineColor: '#6172b0',
        inputTextColor: '#3b4261',
        changedColor: '#8c6c3e',
        scrollbarBackgroundColor: '#d5d6db',
        scrollbarMainColor: '#b4b5b9',
        red: '#f52a65',
        orange: '#b15c00',
        green: '#587539',
        'border-color': '#b4b5b9',
        'hover-bg': '#c8c9ce',
      }
    }
  },
  {
    id: 'dracula',
    label: 'Dracula',
    icon: 'shield',
    modes: ['dark'],
    palettes: {
      dark: {
        colorScheme: 'dark',
        backgroundColor: '#282a36',
        secondaryBackgroundColor: '#44475a',
        secondaryBackgroundColorDim: '#44475a80',
        tertiaryBackgroundColor: '#1e1f29',
        textColor: '#f8f8f2',
        secondaryTextColor: '#bd93f9',
        highlightedColor: '#ffffff',
        accentColor: '#bd93f9',
        accentTextColor: '#282a36',
        accentDarkTextColor: '#ffffff',
        shadowColor: 'rgba(0,0,0,0.5)',
        inputBackgroundColor: '#383a4a',
        inputActiveBackgroundColor: '#282a36',
        inputActiveOutlineColor: '#6272a4',
        inputTextColor: '#f8f8f2',
        changedColor: '#f1fa8c',
        scrollbarBackgroundColor: '#282a36',
        scrollbarMainColor: '#6272a4',
        red: '#ff5555',
        orange: '#ffb86c',
        green: '#50fa7b',
        'border-color': '#6272a4',
        'hover-bg': '#383a4a',
      }
    }
  },
  {
    id: 'nord',
    label: 'Nord',
    icon: 'sparkles',
    modes: ['dark'],
    palettes: {
      dark: {
        colorScheme: 'dark',
        backgroundColor: '#2e3440',
        secondaryBackgroundColor: '#3b4252',
        secondaryBackgroundColorDim: '#3b425280',
        tertiaryBackgroundColor: '#272c36',
        textColor: '#eceff4',
        secondaryTextColor: '#d8dee9',
        highlightedColor: '#ffffff',
        accentColor: '#88c0d0',
        accentTextColor: '#2e3440',
        accentDarkTextColor: '#ffffff',
        shadowColor: 'rgba(0,0,0,0.4)',
        inputBackgroundColor: '#434c5e',
        inputActiveBackgroundColor: '#3b4252',
        inputActiveOutlineColor: '#4c566a',
        inputTextColor: '#eceff4',
        changedColor: '#ebcb8b',
        scrollbarBackgroundColor: '#2e3440',
        scrollbarMainColor: '#4c566a',
        red: '#bf616a',
        orange: '#d08770',
        green: '#a3be8c',
        'border-color': '#4c566a',
        'hover-bg': '#434c5e',
      }
    }
  },
  {
    id: 'catppuccin',
    label: 'Catppuccin',
    icon: 'layers',
    modes: ['dark', 'light'],
    palettes: {
      dark: {
        colorScheme: 'dark',
        backgroundColor: '#1e1e2e',
        secondaryBackgroundColor: '#313244',
        secondaryBackgroundColorDim: '#31324480',
        tertiaryBackgroundColor: '#181825',
        textColor: '#cdd6f4',
        secondaryTextColor: '#bac2de',
        highlightedColor: '#ffffff',
        accentColor: '#cba6f7',
        accentTextColor: '#1e1e2e',
        accentDarkTextColor: '#ffffff',
        shadowColor: 'rgba(0,0,0,0.4)',
        inputBackgroundColor: '#45475a',
        inputActiveBackgroundColor: '#313244',
        inputActiveOutlineColor: '#585b70',
        inputTextColor: '#cdd6f4',
        changedColor: '#f9e2af',
        scrollbarBackgroundColor: '#1e1e2e',
        scrollbarMainColor: '#585b70',
        red: '#f38ba8',
        orange: '#fab387',
        green: '#a6e3a1',
        'border-color': '#585b70',
        'hover-bg': '#45475a',
      },
      light: {
        colorScheme: 'light',
        backgroundColor: '#eff1f5',
        secondaryBackgroundColor: '#ccd0da',
        secondaryBackgroundColorDim: '#ccd0da80',
        tertiaryBackgroundColor: '#dce0e8',
        textColor: '#4c4f69',
        secondaryTextColor: '#5c5f77',
        highlightedColor: '#1e1e2e',
        accentColor: '#8839ef',
        accentTextColor: '#ffffff',
        accentDarkTextColor: '#ffffff',
        shadowColor: 'rgba(0,0,0,0.1)',
        inputBackgroundColor: '#bcc0cc',
        inputActiveBackgroundColor: '#ccd0da',
        inputActiveOutlineColor: '#7287fd',
        inputTextColor: '#4c4f69',
        changedColor: '#df8e1d',
        scrollbarBackgroundColor: '#eff1f5',
        scrollbarMainColor: '#9ca0b0',
        red: '#d20f39',
        orange: '#fe640b',
        green: '#40a02b',
        'border-color': '#9ca0b0',
        'hover-bg': '#ccd0da',
      }
    }
  },
  {
    id: 'gruvbox',
    label: 'Gruvbox',
    icon: 'sun',
    modes: ['dark', 'light'],
    palettes: {
      dark: {
        colorScheme: 'dark',
        backgroundColor: '#282828',
        secondaryBackgroundColor: '#3c3836',
        secondaryBackgroundColorDim: '#3c383680',
        tertiaryBackgroundColor: '#1d2021',
        textColor: '#ebdbb2',
        secondaryTextColor: '#d5c4a1',
        highlightedColor: '#fbf1c7',
        accentColor: '#d79921',
        accentTextColor: '#282828',
        accentDarkTextColor: '#ffffff',
        shadowColor: 'rgba(0,0,0,0.5)',
        inputBackgroundColor: '#504945',
        inputActiveBackgroundColor: '#3c3836',
        inputActiveOutlineColor: '#665c54',
        inputTextColor: '#ebdbb2',
        changedColor: '#fabd2f',
        scrollbarBackgroundColor: '#282828',
        scrollbarMainColor: '#665c54',
        red: '#fb4934',
        orange: '#fe8019',
        green: '#b8bb26',
        'border-color': '#665c54',
        'hover-bg': '#504945',
      },
      light: {
        colorScheme: 'light',
        backgroundColor: '#fbf1c7',
        secondaryBackgroundColor: '#ebdbb2',
        secondaryBackgroundColorDim: '#ebdbb280',
        tertiaryBackgroundColor: '#f2e5bc',
        textColor: '#3c3836',
        secondaryTextColor: '#504945',
        highlightedColor: '#282828',
        accentColor: '#d79921',
        accentTextColor: '#282828',
        accentDarkTextColor: '#282828',
        shadowColor: 'rgba(0,0,0,0.12)',
        inputBackgroundColor: '#d5c4a1',
        inputActiveBackgroundColor: '#ebdbb2',
        inputActiveOutlineColor: '#a89984',
        inputTextColor: '#3c3836',
        changedColor: '#b57614',
        scrollbarBackgroundColor: '#fbf1c7',
        scrollbarMainColor: '#a89984',
        red: '#cc241d',
        orange: '#d65d0e',
        green: '#98971a',
        'border-color': '#a89984',
        'hover-bg': '#ebdbb2',
      }
    }
  },
  {
    id: 'one-dark',
    label: 'One Dark',
    icon: 'code',
    modes: ['dark'],
    palettes: {
      dark: {
        colorScheme: 'dark',
        backgroundColor: '#282c34',
        secondaryBackgroundColor: '#2c313a',
        secondaryBackgroundColorDim: '#2c313a80',
        tertiaryBackgroundColor: '#21252b',
        textColor: '#abb2bf',
        secondaryTextColor: '#9da5b4',
        highlightedColor: '#e6e6e6',
        accentColor: '#61afef',
        accentTextColor: '#282c34',
        accentDarkTextColor: '#ffffff',
        shadowColor: 'rgba(0,0,0,0.5)',
        inputBackgroundColor: '#3a3f4b',
        inputActiveBackgroundColor: '#2c313a',
        inputActiveOutlineColor: '#4d78cc',
        inputTextColor: '#abb2bf',
        changedColor: '#e5c07b',
        scrollbarBackgroundColor: '#282c34',
        scrollbarMainColor: '#4b5263',
        red: '#e06c75',
        orange: '#d19a66',
        green: '#98c379',
        'border-color': '#3e4451',
        'hover-bg': '#3a3f4b',
      }
    }
  },
  {
    id: 'github-dark',
    label: 'GitHub Dark',
    icon: 'globe',
    modes: ['dark'],
    palettes: {
      dark: {
        colorScheme: 'dark',
        backgroundColor: '#0d1117',
        secondaryBackgroundColor: '#161b22',
        secondaryBackgroundColorDim: '#161b2280',
        tertiaryBackgroundColor: '#010409',
        textColor: '#e6edf3',
        secondaryTextColor: '#8b949e',
        highlightedColor: '#ffffff',
        accentColor: '#58a6ff',
        accentTextColor: '#0d1117',
        accentDarkTextColor: '#ffffff',
        shadowColor: 'rgba(0,0,0,0.6)',
        inputBackgroundColor: '#21262d',
        inputActiveBackgroundColor: '#161b22',
        inputActiveOutlineColor: '#388bfd',
        inputTextColor: '#e6edf3',
        changedColor: '#d29922',
        scrollbarBackgroundColor: '#0d1117',
        scrollbarMainColor: '#30363d',
        red: '#f85149',
        orange: '#d29922',
        green: '#3fb950',
        'border-color': '#30363d',
        'hover-bg': '#21262d',
      }
    }
  },
  {
    id: 'rose-pine',
    label: 'Rosé Pine',
    icon: 'target',
    modes: ['dark'],
    palettes: {
      dark: {
        colorScheme: 'dark',
        backgroundColor: '#191724',
        secondaryBackgroundColor: '#1f1d2e',
        secondaryBackgroundColorDim: '#1f1d2e80',
        tertiaryBackgroundColor: '#13111e',
        textColor: '#e0def4',
        secondaryTextColor: '#908caa',
        highlightedColor: '#ffffff',
        accentColor: '#c4a7e7',
        accentTextColor: '#191724',
        accentDarkTextColor: '#ffffff',
        shadowColor: 'rgba(0,0,0,0.5)',
        inputBackgroundColor: '#26233a',
        inputActiveBackgroundColor: '#1f1d2e',
        inputActiveOutlineColor: '#524f67',
        inputTextColor: '#e0def4',
        changedColor: '#f6c177',
        scrollbarBackgroundColor: '#191724',
        scrollbarMainColor: '#524f67',
        red: '#eb6f92',
        orange: '#f6c177',
        green: '#31748f',
        'border-color': '#524f67',
        'hover-bg': '#26233a',
      }
    }
  },
  {
    id: 'solarized',
    label: 'Solarized',
    icon: 'zap',
    modes: ['dark', 'light'],
    palettes: {
      dark: {
        colorScheme: 'dark',
        backgroundColor: '#002b36',
        secondaryBackgroundColor: '#073642',
        secondaryBackgroundColorDim: '#07364280',
        tertiaryBackgroundColor: '#00212b',
        textColor: '#839496',
        secondaryTextColor: '#93a1a1',
        highlightedColor: '#fdf6e3',
        accentColor: '#2aa198',
        accentTextColor: '#002b36',
        accentDarkTextColor: '#fdf6e3',
        shadowColor: 'rgba(0,0,0,0.4)',
        inputBackgroundColor: '#0a4050',
        inputActiveBackgroundColor: '#073642',
        inputActiveOutlineColor: '#586e75',
        inputTextColor: '#839496',
        changedColor: '#b58900',
        scrollbarBackgroundColor: '#002b36',
        scrollbarMainColor: '#586e75',
        red: '#dc322f',
        orange: '#cb4b16',
        green: '#859900',
        'border-color': '#586e75',
        'hover-bg': '#0a4050',
      },
      light: {
        colorScheme: 'light',
        backgroundColor: '#fdf6e3',
        secondaryBackgroundColor: '#eee8d5',
        secondaryBackgroundColorDim: '#eee8d580',
        tertiaryBackgroundColor: '#f5efdc',
        textColor: '#657b83',
        secondaryTextColor: '#586e75',
        highlightedColor: '#002b36',
        accentColor: '#2aa198',
        accentTextColor: '#fdf6e3',
        accentDarkTextColor: '#002b36',
        shadowColor: 'rgba(0,0,0,0.1)',
        inputBackgroundColor: '#e6dfca',
        inputActiveBackgroundColor: '#eee8d5',
        inputActiveOutlineColor: '#93a1a1',
        inputTextColor: '#657b83',
        changedColor: '#b58900',
        scrollbarBackgroundColor: '#fdf6e3',
        scrollbarMainColor: '#93a1a1',
        red: '#dc322f',
        orange: '#cb4b16',
        green: '#859900',
        'border-color': '#93a1a1',
        'hover-bg': '#eee8d5',
      }
    }
  },
  {
    id: 'ayu-mirage',
    label: 'Ayu Mirage',
    icon: 'eye',
    modes: ['dark'],
    palettes: {
      dark: {
        colorScheme: 'dark',
        backgroundColor: '#242936',
        secondaryBackgroundColor: '#1f2430',
        secondaryBackgroundColorDim: '#1f243080',
        tertiaryBackgroundColor: '#1a1e29',
        textColor: '#cccac2',
        secondaryTextColor: '#707a8c',
        highlightedColor: '#f0f0f0',
        accentColor: '#ffad66',
        accentTextColor: '#242936',
        accentDarkTextColor: '#ffffff',
        shadowColor: 'rgba(0,0,0,0.5)',
        inputBackgroundColor: '#2d3441',
        inputActiveBackgroundColor: '#1f2430',
        inputActiveOutlineColor: '#565b70',
        inputTextColor: '#cccac2',
        changedColor: '#ffd580',
        scrollbarBackgroundColor: '#242936',
        scrollbarMainColor: '#565b70',
        red: '#f28779',
        orange: '#ffad66',
        green: '#bae67e',
        'border-color': '#565b70',
        'hover-bg': '#2d3441',
      }
    }
  }
];

// CSS var name mapping - palette key → CSS custom property
var VAR_MAP = {
  backgroundColor: '--backgroundColor',
  secondaryBackgroundColor: '--secondaryBackgroundColor',
  secondaryBackgroundColorDim: '--secondaryBackgroundColorDim',
  tertiaryBackgroundColor: '--tertiaryBackgroundColor',
  textColor: '--textColor',
  secondaryTextColor: '--secondaryTextColor',
  highlightedColor: '--highlightedColor',
  accentColor: '--accentColor',
  accentTextColor: '--accentTextColor',
  accentDarkTextColor: '--accentDarkTextColor',
  shadowColor: '--shadowColor',
  inputBackgroundColor: '--inputBackgroundColor',
  inputActiveBackgroundColor: '--inputActiveBackgroundColor',
  inputActiveOutlineColor: '--inputActiveOutlineColor',
  inputTextColor: '--inputTextColor',
  changedColor: '--changedColor',
  scrollbarBackgroundColor: '--scrollbarBackgroundColor',
  scrollbarMainColor: '--scrollbarMainColor',
  red: '--red',
  orange: '--orange',
  green: '--green',
  'border-color': '--border-color',
  'hover-bg': '--hover-bg',
};

var THEME_MAP = {};
for (var i = 0; i < THEMES.length; i++) { THEME_MAP[THEMES[i].id] = THEMES[i]; }

function getTheme(id) {
  return THEME_MAP[id] || null;
}

function getActiveThemeId() {
  return localStorage.getItem('mist-theme-id') || 'tokyo-night';
}

function getActiveMode() {
  var stored = localStorage.getItem('mist-theme-mode');
  if (stored === 'dark' || stored === 'light') return stored;
  return window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light';
}

function getAvailableModes(themeId) {
  var theme = getTheme(themeId);
  return theme ? theme.modes : ['dark'];
}

function applyTheme(themeId, mode) {
  var theme = getTheme(themeId);
  if (!theme) { theme = THEME_MAP['tokyo-night']; themeId = 'tokyo-night'; }

  if (!mode || theme.modes.indexOf(mode) === -1) {
    var prefersDark = window.matchMedia('(prefers-color-scheme: dark)').matches;
    if (prefersDark && theme.modes.indexOf('dark') !== -1) { mode = 'dark'; }
    else if (!prefersDark && theme.modes.indexOf('light') !== -1) { mode = 'light'; }
    else { mode = theme.modes[0]; }
  }

  var palette = theme.palettes[mode];
  if (!palette) return;

  // Apply CSS variables
  for (var key in palette) {
    if (key === 'colorScheme') continue;
    var cssVar = VAR_MAP[key];
    if (cssVar) {
      document.body.style.setProperty(cssVar, palette[key]);
    }
  }

  // Derived variables
  document.body.style.setProperty('--textColorFaded',
    'color-mix(in srgb,var(--textColor),transparent 35%)');
  document.body.style.setProperty('--accentColorDim',
    'color-mix(in srgb,var(--accentColor),transparent 50%)');
  document.body.style.setProperty('--accentColorDark',
    'color-mix(in srgb,var(--accentColor),black 50%)');
  document.body.style.setProperty('--logoColor', 'var(--textColor)');
  document.body.style.setProperty('--logoHighlightColor', 'var(--highlightedColor)');
  document.body.style.setProperty('--headerBackgroundColor', 'var(--backgroundColor)');
  document.body.style.setProperty('--navBackgroundColor', 'var(--secondaryBackgroundColor)');

  // Select arrow SVG with themed stroke color
  var arrowColor = encodeURIComponent(palette.secondaryTextColor);
  document.body.style.setProperty('--selectArrowImage',
    "url(\"data:image/svg+xml;utf8,<svg xmlns='http://www.w3.org/2000/svg' width='1.5em' height='1em' viewBox='0 0 20 10'><path d='M 1,3 6.5,7.5 12,3' style='stroke-width:2;stroke-linecap:round;stroke-linejoin:round;fill:none;stroke:" + arrowColor + "' /></svg>\")");

  // Color scheme for native UI elements
  document.body.style.setProperty('color-scheme', palette.colorScheme || mode);

  // data-theme attribute (for existing CSS toggle selectors)
  document.body.setAttribute('data-theme', mode);

  // Persist
  localStorage.setItem('mist-theme-id', themeId);
  localStorage.setItem('mist-theme-mode', mode);

  // Notify listeners
  document.dispatchEvent(new CustomEvent('themechange', {
    detail: { themeId: themeId, mode: mode }
  }));
}

function toggleMode() {
  var themeId = getActiveThemeId();
  var theme = getTheme(themeId);
  if (!theme || theme.modes.length < 2) return;

  var current = getActiveMode();
  var next = (current === 'dark') ? 'light' : 'dark';
  if (theme.modes.indexOf(next) === -1) return;

  applyTheme(themeId, next);
}

function getPlayerThemeId() {
  return getActiveThemeId();
}

// Migrate old localStorage format
function migrate() {
  if (localStorage.getItem('mist-theme-id')) return;
  var old = localStorage.getItem('mist-theme');
  if (old) {
    localStorage.setItem('mist-theme-id', 'tokyo-night');
    localStorage.setItem('mist-theme-mode', old);
    localStorage.removeItem('mist-theme');
  }
}
migrate();

export {
  THEMES,
  getTheme,
  getActiveThemeId,
  getActiveMode,
  getAvailableModes,
  applyTheme,
  toggleMode,
  getPlayerThemeId
};
