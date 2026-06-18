inputs = [
    {'name' : 'HLS', 'format' : 'hls'},
    {'name' : 'DTSC', 'format' : 'dtsc'},
    {'name' : 'MP3', 'format' : 'mp3'},
    {'name' : 'FLV', 'format' : 'flv'},
    {'name' : 'OGG', 'format' : 'ogg'},
    {'name' : 'Buffer', 'format' : 'buffer'},
    {'name' : 'H264', 'format' : 'h264'},
    {'name' : 'EBML', 'format' : 'ebml'},
    {'name' : 'ISMV', 'format' : 'ismv'},
    {'name' : 'MP4', 'format' : 'mp4'},
    {'name' : 'TS', 'format' : 'ts'},
    {'name' : 'Folder', 'format' : 'folder'},
    {'name' : 'Playlist', 'format' : 'playlist'},
    {'name' : 'Balancer', 'format' : 'balancer'},
    {'name' : 'RTSP', 'format' : 'rtsp'},
    {'name' : 'SubRip', 'format' : 'subrip'},
    {'name' : 'SDP', 'format' : 'sdp'},
    {'name' : 'AAC', 'format' : 'aac'},
    {'name' : 'FLAC', 'format' : 'flac'},
    {'name' : 'RawGenerator', 'format' : 'rawgenerator'},
]

#Referenced by process targets
input_ebml_cpp = files('input_ebml.cpp')
input_cpp = files('input.cpp', 'processing_profile.cpp')

if have_librist
  inputs += {'name' : 'TSRIST', 'format' : 'tsrist', 'extra' : 'with_rist'}
endif

if have_srt
  inputs += {'name' : 'TSSRT', 'format' : 'tssrt', 'extra' : 'with_srt'}
endif

if get_option('WITH_AV')
  inputs += {'name' : 'AV', 'format' : 'av', 'extra': 'with_av'}
endif

if have_v4l2
  inputs += {'name' : 'V4L2', 'format' : 'v4l2'}
endif

if get_option('WITH_DECKLINK')
  inputs += {'name' : 'DeckLink', 'format' : 'decklink', 'extra': 'with_decklink'}
endif
if have_ndi
  inputs += {'name' : 'NDI', 'format' : 'ndi', 'extra': 'with_ndi'}
endif

inputs_tgts = []

foreach input : inputs
  deps = [libmist_dep]

  if input.has_key('extra')
    if input.get('extra').contains('with_rist')
      deps += librist
    endif
    if input.get('extra').contains('with_srt')
      deps += libmist_srt_dep
      deps += libsrt
    endif
    if input.get('extra').contains('with_av')
      deps += av_libs
    endif
    if input.get('extra').contains('with_decklink')
      deps += decklink_dep
    endif
    if input.get('extra').contains('with_ndi')
      deps += ndi_lib_dep
      deps += ndi_sdk_dep
    endif
  endif
  if input.get('name').contains('AV')
    deps += av_libs
  endif

  executables += {
    'name' : 'MistIn'+input.get('name'),
    'sources' : [
      files(
        'mist_in.cpp',
        'input.cpp',
        'processing_profile.cpp',
        'input_'+input.get('format')+'.cpp',
      ),
      io_cpp,
      header_tgts
    ],
    'deps' : deps,
    'defines': [
      string_opt.format('INPUTTYPE', 'input_'+input.get('format')+'.h')
    ]
  }
endforeach

