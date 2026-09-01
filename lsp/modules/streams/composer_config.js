function parseResolution(value) {
  const parts = String(value || '1920x1080').split('x');
  return [parseInt(parts[0], 10) || 1920, parseInt(parts[1], 10) || 1080];
}

export function calculateComposerPositions(sourceCount, resolution, layout) {
  if (!sourceCount) return [];
  const size = parseResolution(resolution);
  const width = size[0];
  const height = size[1];
  const positions = [];

  if (layout === 'lasagna') {
    for (let index = 0; index < sourceCount; index++) {
      positions.push({x: 0, y: 0, w: width, h: height});
    }
    return positions;
  }

  if (layout === 'focussed' && sourceCount > 1) {
    const cellCount = sourceCount + 3;
    const columns = Math.ceil(Math.sqrt(cellCount));
    const rows = Math.ceil(cellCount / columns);
    const cellWidth = Math.floor(width / columns);
    const cellHeight = Math.floor(height / rows);
    const largeCellSlots = rows * columns - sourceCount + 1;
    const largeCellHeight = Math.floor(Math.sqrt(largeCellSlots));
    const largeCellWidth = Math.floor(largeCellSlots / largeCellHeight);

    positions.push({
      x: 0,
      y: 0,
      w: cellWidth * largeCellWidth,
      h: cellHeight * largeCellHeight
    });

    let index = 1;
    for (let row = 0; row < rows && index < sourceCount; row++) {
      for (let column = 0; column < columns && index < sourceCount; column++) {
        if (row < largeCellHeight && column < largeCellWidth) continue;
        positions.push({
          x: column * cellWidth,
          y: row * cellHeight,
          w: cellWidth,
          h: cellHeight
        });
        index++;
      }
    }
    return positions;
  }

  const columns = Math.ceil(Math.sqrt(sourceCount));
  const rows = Math.ceil(sourceCount / columns);
  const cellWidth = Math.floor(width / columns);
  const cellHeight = Math.floor(height / rows);
  for (let index = 0; index < sourceCount; index++) {
    positions.push({
      x: (index % columns) * cellWidth,
      y: Math.floor(index / columns) * cellHeight,
      w: cellWidth,
      h: cellHeight
    });
  }
  return positions;
}

function normalizeSources(sources) {
  if (!Array.isArray(sources)) return [];
  return sources.map(function(source) {
    if (source && typeof source === 'object') {
      const clean = {};
      for (const key in source) {
        if (key.charAt(0) !== '_') clean[key] = source[key];
      }
      return clean;
    }
    return { stream: String(source || '') };
  });
}

function isComposerProcess(process) {
  return !!(process && process.process && String(process.process).toLowerCase().indexOf('composer') >= 0);
}

function findLegacyComposerProcess(processes) {
  if (Array.isArray(processes)) {
    for (let i = 0; i < processes.length; i++) {
      if (isComposerProcess(processes[i])) return processes[i];
    }
    return null;
  }
  if (processes && typeof processes === 'object') {
    for (const key in processes) {
      if (isComposerProcess(processes[key])) return processes[key];
    }
  }
  return null;
}

function positionsMatch(sources, expected) {
  if (sources.length !== expected.length) return false;
  for (let i = 0; i < sources.length; i++) {
    const source = sources[i];
    const position = expected[i];
    if (!source || typeof source !== 'object') return false;
    if (Number(source.x) !== position.x || Number(source.y) !== position.y ||
        Number(source.w) !== position.w || Number(source.h) !== position.h) {
      return false;
    }
  }
  return true;
}

export function inferComposerLayout(sources, resolution) {
  if (!sources.length || sources.every(function(source) {
    return source.x === undefined && source.y === undefined && source.w === undefined && source.h === undefined;
  })) {
    return 'equal';
  }
  if (positionsMatch(sources, calculateComposerPositions(sources.length, resolution, 'equal'))) return 'equal';
  if (positionsMatch(sources, calculateComposerPositions(sources.length, resolution, 'focussed'))) return 'focussed';
  return 'none';
}

export function extractComposerConfig(streamConfig) {
  const legacy = findLegacyComposerProcess(streamConfig.processes);
  const resolution = streamConfig.resolution || (legacy && legacy.resolution) || '1920x1080';
  const sources = normalizeSources(
    Array.isArray(streamConfig.sources) ? streamConfig.sources : (legacy && legacy.sources)
  );
  return {
    sources: sources,
    resolution: resolution,
    layout: streamConfig.layout || (legacy && legacy.layout) || inferComposerLayout(sources, resolution)
  };
}

export function removeComposerProcesses(processes) {
  if (Array.isArray(processes)) {
    return processes.filter(function(process) { return !isComposerProcess(process); });
  }
  if (processes && typeof processes === 'object') {
    for (const key in processes) {
      if (isComposerProcess(processes[key])) delete processes[key];
    }
  }
  return processes;
}

export function applyComposerConfig(streamConfig, composerConfig) {
  streamConfig.sources = normalizeSources(composerConfig.sources);
  streamConfig.resolution = composerConfig.resolution || '1920x1080';
  delete streamConfig.layout;
  streamConfig.processes = removeComposerProcesses(streamConfig.processes);
  return streamConfig;
}

function cloneConfig(config) {
  return JSON.parse(JSON.stringify(config));
}

function processList(config) {
  if (!config.processes || typeof config.processes !== 'object') config.processes = {};
  if (Array.isArray(config.processes)) {
    const converted = {};
    for (let i = 0; i < config.processes.length; i++) converted['Proc' + i] = config.processes[i];
    config.processes = converted;
  }
  return config.processes;
}

function hasAVCodec(config, kind, codec) {
  const processes = config && config.processes;
  if (!processes || typeof processes !== 'object') return false;
  for (const key in processes) {
    const process = processes[key];
    if (!process || process.process !== 'AV') continue;
    if (process['x-LSP-kind'] === kind && String(process.codec).toUpperCase() === codec.toUpperCase()) return true;
  }
  return false;
}

function addAVProcess(config, keyBase, process) {
  const processes = processList(config);
  if (hasAVCodec(config, process['x-LSP-kind'], process.codec)) return false;
  let key = keyBase;
  let suffix = 1;
  while (key in processes) key = keyBase + suffix++;
  processes[key] = process;
  return true;
}

function resolveConfiguredSource(name, configuredStreams) {
  if (configuredStreams[name]) return name;
  const base = String(name || '').split('+')[0];
  return configuredStreams[base] ? base : null;
}

export function findComposerOutputCodec(processes) {
  if (!processes || typeof processes !== 'object') return null;
  for (const key in processes) {
    const process = processes[key];
    if (!process || process.process !== 'AV' || process['x-LSP-kind'] !== 'video') continue;
    const codec = String(process.codec || '').toUpperCase();
    if (codec && codec !== 'UYVY' && codec !== 'YUYV' && codec !== 'NV12') return process.codec;
  }
  return null;
}

export function prepareComposerPipeline(streamConfig, composerConfig, configuredStreams) {
  const relatedStreams = {};
  const warnings = [];
  const outputCodec = composerConfig._outputCodec || '';

  if (composerConfig._outputCodecChanged && streamConfig.processes) {
    for (const key in streamConfig.processes) {
      const process = streamConfig.processes[key];
      if (process && process.process === 'AV' && process['x-LSP-name'] === 'Composer output encoder') {
        delete streamConfig.processes[key];
      }
    }
  }

  if (outputCodec && outputCodec !== 'raw') {
    addAVProcess(streamConfig, 'AV_composer_output', {
      process: 'AV',
      'x-LSP-name': 'Composer output encoder',
      'x-LSP-kind': 'video',
      codec: outputCodec,
      track_select: 'video=UYVY&audio=none'
    });
  }

  if (!composerConfig._prepareInputs) return {relatedStreams: relatedStreams, warnings: warnings};

  const rawSources = composerConfig._rawSources || [];
  const sources = composerConfig.sources || [];
  const decoderTargets = {};
  for (let i = 0; i < sources.length; i++) {
    const name = String(sources[i].stream || '');
    if (!name || /\.png(?:$|\?)/i.test(name) || rawSources.indexOf(name) >= 0) continue;
    const target = resolveConfiguredSource(name, configuredStreams);
    if (!target) {
      warnings.push('Could not prepare unconfigured Composer source "' + name + '".');
      continue;
    }
    decoderTargets[target] = decoderTargets[target] || {video: false, audio: false};
    decoderTargets[target].video = true;
  }

  const audioSource = String(streamConfig.copyaudio || '');
  if (audioSource) {
    const audioTarget = resolveConfiguredSource(audioSource, configuredStreams);
    if (audioTarget) {
      decoderTargets[audioTarget] = decoderTargets[audioTarget] || {video: false, audio: false};
      decoderTargets[audioTarget].audio = true;
    } else {
      warnings.push('Could not prepare unconfigured Composer audio source "' + audioSource + '".');
    }
  }

  for (const target in decoderTargets) {
    const prepared = cloneConfig(configuredStreams[target]);
    let changed = false;
    if (decoderTargets[target].video) {
      changed = addAVProcess(prepared, 'AV_composer_decode_video', {
        process: 'AV',
        'x-LSP-name': 'Composer raw video decoder',
        'x-LSP-kind': 'video',
        codec: 'UYVY',
        track_select: 'video=all&audio=none'
      }) || changed;
    }
    if (decoderTargets[target].audio) {
      changed = addAVProcess(prepared, 'AV_composer_decode_audio', {
        process: 'AV',
        'x-LSP-name': 'Composer raw audio decoder',
        'x-LSP-kind': 'audio',
        codec: 'PCM',
        track_select: 'audio=all&video=none'
      }) || changed;
    }
    if (changed) relatedStreams[target] = prepared;
  }

  return {relatedStreams: relatedStreams, warnings: warnings};
}
