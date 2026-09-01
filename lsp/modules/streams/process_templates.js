const TEMPLATES = [
  {
    id: 'h264_1080p',
    title: '1080p H264',
    desc: 'Transcode video to 1080p H.264 with hardware acceleration.',
    icon: 'video',
    process: 'AV',
    fallback: 'FFMPEG',
    defaults: {
      'x-LSP-kind': 'video',
      codec: 'H264',
      resolution: '1920x1080',
      preset: 'faster',
      tune: 'zerolatency'
    }
  },
  {
    id: 'h264_720p',
    title: '720p H264',
    desc: 'Transcode video to 720p H.264 with hardware acceleration.',
    icon: 'video',
    process: 'AV',
    fallback: 'FFMPEG',
    defaults: {
      'x-LSP-kind': 'video',
      codec: 'H264',
      resolution: '1280x720',
      preset: 'faster',
      tune: 'zerolatency'
    }
  },
  {
    id: 'h264_480p',
    title: '480p H264',
    desc: 'Transcode video to 480p H.264 with hardware acceleration.',
    icon: 'video',
    process: 'AV',
    fallback: 'FFMPEG',
    defaults: {
      'x-LSP-kind': 'video',
      codec: 'H264',
      resolution: '854x480',
      preset: 'fast',
      tune: 'zerolatency'
    }
  },
  {
    id: 'abr_ladder',
    title: 'ABR Ladder (1080/720/480)',
    desc: 'Add three H.264 video transcode processes for adaptive bitrate streaming.',
    icon: 'list',
    process: 'AV',
    fallback: 'FFMPEG',
    multi: [
      {
        'x-LSP-name': '1080p H264',
        'x-LSP-kind': 'video',
        codec: 'H264',
        resolution: '1920x1080',
        preset: 'faster',
        tune: 'zerolatency'
      },
      {
        'x-LSP-name': '720p H264',
        'x-LSP-kind': 'video',
        codec: 'H264',
        resolution: '1280x720',
        preset: 'faster',
        tune: 'zerolatency'
      },
      {
        'x-LSP-name': '480p H264',
        'x-LSP-kind': 'video',
        codec: 'H264',
        resolution: '854x480',
        preset: 'fast',
        tune: 'zerolatency'
      }
    ]
  },
  {
    id: 'mjpeg',
    title: 'MJPEG Snapshots',
    desc: 'Encode video frames as MJPEG. Useful for thumbnail generation and camera snapshot streams.',
    icon: 'image',
    process: 'AV',
    defaults: {
      'x-LSP-kind': 'video',
      codec: 'JPEG',
      quality: 15,
      gopsize: 30
    }
  },
  {
    id: 'aac_to_opus',
    title: 'Audio to Opus',
    desc: 'Transcode audio to Opus for WebRTC delivery.',
    icon: 'music',
    process: 'AV',
    fallback: 'FFMPEG',
    defaults: {
      'x-LSP-kind': 'audio',
      codec: 'opus',
      bitrate: 128000
    }
  },
  {
    id: 'audio_aac',
    title: 'Audio to AAC',
    desc: 'Transcode audio to AAC for broad compatibility.',
    icon: 'music',
    process: 'AV',
    fallback: 'FFMPEG',
    defaults: {
      'x-LSP-kind': 'audio',
      codec: 'AAC',
      bitrate: 128000
    }
  }
];

function getAvailableTemplates() {
  const caps = (mist.data.capabilities && mist.data.capabilities.processes) || {};
  const out = [];
  for (let i = 0; i < TEMPLATES.length; i++) {
    const t = TEMPLATES[i];
    let procId = null;
    if (caps[t.process]) {
      procId = t.process;
    } else if (t.fallback && caps[t.fallback]) {
      procId = t.fallback;
    }
    if (procId) {
      out.push({
        template: t,
        resolvedProcess: procId
      });
    }
  }
  return out;
}

function applyTemplate(template, resolvedProcess) {
  if (template.multi) {
    const items = [];
    for (let i = 0; i < template.multi.length; i++) {
      let item = Object.assign({}, template.multi[i]);
      item.process = resolvedProcess;
      items.push(item);
    }
    return items;
  }
  let item = Object.assign({}, template.defaults);
  item.process = resolvedProcess;
  return [item];
}

export const processTemplates = {
  TEMPLATES: TEMPLATES,
  getAvailableTemplates: getAvailableTemplates,
  applyTemplate: applyTemplate
};
