// @mistserver/player-core — IIFE entry point
// Assigns all exports to window for backward compatibility

import {
  MistUtil,
  MistVideo,
  MistPlayer,
  mistPlay,
  mistplayers,
  MistSkins,
  MistSkin,
  MistUI,
  PlayerState,
  ControlChannel,
  DataChannel2WebSocket,
  ControlChannelAPI,
  createPlayer,
  registerWrapper,
  MistThemes,
  resolveTheme,
  locales
} from './index.js';

// Backward-compat globals
window.MistUtil = MistUtil;
window.MistVideo = MistVideo;
window.MistPlayer = MistPlayer;
window.mistPlay = mistPlay;
window.mistplayers = mistplayers;
window.MistSkins = MistSkins;
window.MistSkin = MistSkin;
window.MistUI = MistUI;
window.PlayerState = PlayerState;
window.createPlayer = createPlayer;
window.registerWrapper = registerWrapper;
window.MistThemes = MistThemes;
window.MistLocales = locales;

// Attach shared utilities to MistUtil for backward compat
MistUtil.shared = { ControlChannel, DataChannel2WebSocket, ControlChannelAPI };
