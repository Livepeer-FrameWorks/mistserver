transcode_headers = [
  'audio_decoder_node.h',
  'audio_encoder_node.h',
  'audio_frame_context.h',
  'audio_transformer_node.h',
  'hw_context_manager.h',
  'node_pipeline.h',
  'packet_context.h',
  'packet_egress.h',
  'processing_node.h',
  'utils.h',
  'video_decoder_node.h',
  'video_encoder_node.h',
  'video_frame_context.h',
  'video_transformer_node.h',
]
install_headers(transcode_headers, subdir: 'mist/ffmpeg')

if get_option('WITH_AV')
  # Add MediaToolbox framework for macOS
  if host_machine.system() == 'darwin'
    mediatoolbox_dep = dependency(
      'appleframeworks',
      modules: ['VideoToolbox', 'CoreMedia'],
      required: true,
    )
    av_libs += mediatoolbox_dep
  endif

  libmist_transcode = library(
    'mist_transcode',
    'audio_decoder_node.cpp',
    'audio_encoder_node.cpp',
    'audio_frame_context.cpp',
    'audio_transformer_node.cpp',
    'hw_context_manager.cpp',
    'node_pipeline.cpp',
    'packet_context.cpp',
    'packet_egress.cpp',
    'utils.cpp',
    'video_decoder_node.cpp',
    'video_encoder_node.cpp',
    'video_frame_context.cpp',
    'video_transformer_node.cpp',
    dependencies: [libmist_dep, av_libs],
    install: true,
  )
  libmist_transcode_dep = declare_dependency(link_with: [libmist_transcode])
endif
