// Type declarations for @mistserver/player-core

// ── Options ──

export interface PlayerOptions {
  target: HTMLElement;
  stream: string;
  host?: string | null;
  autoplay?: boolean;
  controls?: boolean;
  keyControls?: boolean | 'focus';
  loop?: boolean;
  poster?: string | false;
  muted?: boolean;
  callback?: ((player: Player) => void) | false;
  streaminfo?: object | false;
  startCombo?: boolean | false;
  forceType?: string | false;
  forcePlayer?: string | false;
  forceSource?: string | false;
  forcePriority?: object | false;
  monitor?: object | false;
  reloadDelay?: number | false;
  urlappend?: string | false;
  setTracks?: Record<string, number> | false;
  fillSpace?: boolean;
  width?: number | false;
  height?: number | false;
  rotate?: -1 | 1 | 2 | false;
  maxwidth?: number | false;
  maxheight?: number | false;
  ABR_resize?: boolean;
  ABR_bitrate?: boolean;
  useDateTime?: boolean;
  subscribeToMetaTrack?: [number, (msg: object) => void][] | false;
  MistVideoObject?: object | false;
  translations?: Partial<TranslationStrings> | false;
  skin?: string | SkinDefinition;
  thumbnails?: string;
  keyMap?: Partial<KeyMap>;
}

export interface TranslationStrings {
  play: string;
  pause: string;
  mute: string;
  unmute: string;
  volume: string;
  fullscreen: string;
  'exit fullscreen': string;
  pip: string;
  loop: string;
  settings: string;
  automatic: string;
  none: string;
  live: string;
  'current time': string;
  'total time': string;
  'seek bar': string;
  'reload video': string;
  'reload player': string;
  'next source': string;
  ignore: string;
  'stop casting': string;
  chromecast: string;
  'player encountered a problem': string;
  'chromecast encountered a problem': string;
  'seek forward': string;
  'seek backward': string;
  [key: string]: string;
}

export interface KeyMap {
  togglePlay: string[] | false;
  seekForward: string[] | false;
  seekBackward: string[] | false;
  volumeUp: string[] | false;
  volumeDown: string[] | false;
  toggleFullscreen: string[] | false;
  toggleMute: string[] | false;
  togglePip: string[] | false;
  cycleSubtitle: string[] | false;
  speedUp: string[] | false;
  speedDown: string[] | false;
  seekToStart: string[] | false;
  seekToEnd: string[] | false;
  seekPercent: string[] | false;
}

// ── State ──

export interface PlayerStateValues {
  paused: boolean;
  playing: boolean;
  currentTime: number;
  duration: number;
  volume: number;
  muted: boolean;
  buffered: TimeRanges | null;
  seeking: boolean;
  ended: boolean;
  loading: boolean;
  fullscreen: boolean;
  error: MediaError | null;
  playbackRate: number;
  loop: boolean;
  pip: boolean;
  tracks: ParsedTracks | null;
  streamState: string | null;
}

export type StateProperty = keyof PlayerStateValues;
export type StateCallback<K extends StateProperty = StateProperty> = (
  value: PlayerStateValues[K],
  prop: K
) => void;

export interface StateSubscription {
  on<K extends StateProperty>(prop: K, cb: StateCallback<K>): () => void;
  off<K extends StateProperty>(prop: K, cb: StateCallback<K>): void;
  get<K extends StateProperty>(prop: K): PlayerStateValues[K];
}

// ── Tracks ──

export interface TrackInfo {
  idx: number;
  type: string;
  codec: string;
  bps?: number;
  width?: number;
  height?: number;
  fpks?: number;
  language?: string;
  init?: string;
  channels?: number;
  rate?: number;
  size?: number;
}

export interface ParsedTracks {
  video: TrackInfo[];
  audio: TrackInfo[];
  subtitle: TrackInfo[];
  meta: TrackInfo[];
  [type: string]: TrackInfo[];
}

// ── Stream info ──

export interface StreamInfo {
  type: 'live' | 'vod';
  meta?: {
    tracks?: Record<string, TrackInfo>;
    [key: string]: unknown;
  };
  source?: SourceInfo[];
  [key: string]: unknown;
}

export interface SourceInfo {
  type: string;
  url: string;
  priority: number;
  relurl?: string;
  simul_tracks?: number;
  [key: string]: unknown;
}

// ── Player (createPlayer return) ──

export interface Player {
  // Queries
  readonly video: HTMLVideoElement | HTMLAudioElement | null;
  readonly info: StreamInfo | false;
  readonly source: SourceInfo | null;
  readonly playerName: string | null;
  readonly logs: string[];
  readonly options: PlayerOptions;
  readonly paused: boolean;
  readonly playbackRate: number;
  readonly loop: boolean;
  readonly buffered: TimeRanges | null;
  readonly fullscreen: boolean;
  readonly pip: boolean;
  readonly tracks: ParsedTracks | null;
  readonly size: { width: number; height: number } | null;
  readonly capabilities: Record<string, boolean>;
  readonly quality: number | null;
  readonly streamState: string | null;
  readonly raw: MistVideoInstance;

  // Read/write
  currentTime: number;
  volume: number;
  muted: boolean;
  duration: number;

  // Methods
  play(): Promise<void>;
  pause(): void;
  setTrack(type: string, trackid: number): boolean;
  getStats(): object | null;
  destroy(): void;
  reload(): void;
  nextCombo(): void;
  translate(key: string, fallback?: string): string;
  setTheme(themeOrTokens: string | ThemeTokens, mode?: 'dark' | 'light'): void;

  // Subscriptions
  state: StateSubscription;
  on(event: string, callback: EventListenerOrEventListenerObject): void;
  off(event: string, callback: EventListenerOrEventListenerObject): void;
}

// ── Themes ──

export interface ThemeTokens {
  'color-text'?: string;
  'color-text-muted'?: string;
  'color-icon-fill'?: string;
  'color-icon-stroke'?: string;
  'color-surface'?: string;
  'color-accent'?: string;
  'color-accent-hover'?: string;
  'color-progress-track'?: string;
  'color-progress-fill'?: string;
  'color-progress-buffer'?: string;
  'color-border'?: string;
  'color-link'?: string;
  [key: string]: string | undefined;
}

export interface ThemeDefinition {
  dark?: ThemeTokens;
  light?: ThemeTokens;
}

export type ThemeName =
  | 'default'
  | 'tokyo-night'
  | 'dracula'
  | 'nord'
  | 'catppuccin'
  | 'gruvbox'
  | 'one-dark'
  | 'github-dark'
  | 'rose-pine'
  | 'solarized'
  | 'ayu-mirage';

// ── Skin ──

export interface SkinDefinition {
  [key: string]: unknown;
}

// ── Events ──

export type PlayerEvent =
  | 'play'
  | 'pause'
  | 'playing'
  | 'ended'
  | 'timeupdate'
  | 'durationchange'
  | 'volumechange'
  | 'ratechange'
  | 'seeking'
  | 'seeked'
  | 'waiting'
  | 'canplay'
  | 'error'
  | 'progress'
  | 'enterpictureinpicture'
  | 'leavepictureinpicture'
  | 'fullscreenchange'
  | 'metaUpdate_tracks';

// ── Exports ──

export function createPlayer(options: PlayerOptions): Player;

export function mistPlay(streamName: string, options: Omit<PlayerOptions, 'stream'>): MistVideoInstance;

export class MistVideo {
  constructor(streamName: string, options: Omit<PlayerOptions, 'stream'>);
}

export type MistVideoInstance = InstanceType<typeof MistVideo>;

export class MistPlayer {}

export function PlayerState(mv: MistVideoInstance): void;

export function registerWrapper(name: string, wrapper: object): void;

export var mistplayers: Record<string, object>;

export var MistUtil: {
  tracks: { parse(tracks: Record<string, TrackInfo>): ParsedTracks };
  event: {
    addListener(target: EventTarget, event: string, callback: EventListenerOrEventListenerObject): object;
    removeListener(ref: object): void;
    send(event: string, detail: unknown, target: EventTarget): void;
  };
  object: {
    extend<T extends object>(target: T, ...sources: object[]): T;
    keys(obj: object): string[];
  };
  http: {
    url: { sanitizeHost(host: string): string };
  };
  class: {
    add(el: Element, className: string): void;
    remove(el: Element, className: string): void;
    has(el: Element, className: string): boolean;
  };
  controlTooltip(element: HTMLElement, label: string): HTMLDivElement;
  [key: string]: unknown;
};

export var MistThemes: Record<ThemeName | string, ThemeDefinition>;
export function resolveTheme(themeOrTokens: string | ThemeTokens, mode?: 'dark' | 'light'): ThemeTokens | null;

export function MistSkin(definition: SkinDefinition, mv: MistVideoInstance): object;
export function MistUI(mv: MistVideoInstance): object;
export var MistSkins: Record<string, SkinDefinition>;

export function ControlChannel(options: object): object;
export function DataChannel2WebSocket(options: object): object;
export function ControlChannelAPI(options: object): object;
