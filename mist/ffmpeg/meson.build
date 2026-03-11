transcode_header_tgts = []

foreach header : transcode_headers
  transcode_header_tgts += custom_target('copy_ffmpeg_'+header, input: join_paths('../../lib/ffmpeg', header), output: header, command: ['cp', '@INPUT@', '@OUTPUT@'])
endforeach
