import { containerBlueprints } from './container.js';
import { controlsBarBlueprints } from './controls-bar.js';
import { playbackBlueprints } from './playback.js';
import { settingsBlueprints } from './settings.js';
import { overlayBlueprints } from './overlay.js';
import { mediaBlueprints } from './media.js';
import { contextMenuBlueprints } from './context-menu.js';
import { idleScreenBlueprints } from './idle-screen.js';

export const allBlueprints = Object.assign({},
  containerBlueprints,
  controlsBarBlueprints,
  playbackBlueprints,
  settingsBlueprints,
  overlayBlueprints,
  mediaBlueprints,
  contextMenuBlueprints,
  idleScreenBlueprints
);
