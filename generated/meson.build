
# Build tools for embedding files into the binary
sourcery = executable('sourcery', '../src/sourcery.cpp', native: true)
embedfs = executable('embedfs', '../src/embedfs.cpp', native: true)

# If requested, use local versions instead of building our own
if get_option('LOCAL_GENERATORS')
  sourcery = find_program('sourcery', required:true, native:true, dirs: [meson.project_source_root()])
  embedfs = find_program('embedfs', required:true, native:true, dirs: [meson.project_source_root()])
endif

# --- LSP variants: embed each variant's dist/ files as a VFS ---
lsp_variants = get_option('LSP_VARIANTS')
variant_vfs_targets = []
includes_str = ''
entries_str = ''
is_first = true
actual_variant_count = 0

foreach slug : lsp_variants
  if slug not in variant_dist_files
    warning('LSP variant "' + slug + '" skipped (dist files not found)')
  else
    prefix = 'lsp_' + slug

    vfs = custom_target('vfs_lsp_' + slug,
      output: prefix + '.vfs.h',
      input: variant_dist_files[slug],
      command: [embedfs, '--output', '@OUTPUT@', '--prefix', prefix,
                '--vpath', '/index.html', '@INPUT0@',
                '--vpath', '/main.js', '@INPUT1@',
                '--vpath', '/main.css', '@INPUT2@']
    )

    variant_vfs_targets += vfs
    includes_str += '#include "' + prefix + '.vfs.h"\n'

    if is_first
      entries_str += '  {"' + slug + '", ' + prefix + '_vfs, ' + prefix + '_vfs_count, true},\n'
      is_first = false
    else
      entries_str += '  {"' + slug + '", ' + prefix + '_vfs, ' + prefix + '_vfs_count, false},\n'
    endif
    actual_variant_count += 1
  endif
endforeach

variant_conf = configuration_data()
variant_conf.set('VARIANT_INCLUDES', includes_str)
variant_conf.set('VARIANT_ENTRIES', entries_str)
variant_conf.set('VARIANT_COUNT', actual_variant_count.to_string())

configure_file(
  input: 'server_variants.h.in',
  output: 'server_variants.h',
  configuration: variant_conf
)

# --- Embed player: VFS via --vpath (same pattern as LSP) ---
embed_files = files(
  '../embed/dist/player.iife.js',
  '../embed/dist/player.min.mjs',
  '../embed/players/video.min.js',
  '../embed/players/dash.js.license.js',
  '../embed/players/dash.all.min.js',
  '../embed/players/flv.min.js',
  '../embed/players/hls.js',
  '../embed/players/libde265.min.js',
  '../embed/dist/skins/default.css',
  '../embed/dist/skins/dev.css',
  '../embed/skins/video-js.css',
  '../embed/players/webcodecsworker.js',
)

embed_vfs = custom_target('embed_vfs',
  output: 'embed.vfs.h',
  input: embed_files,
  command: [embedfs, '--output', '@OUTPUT@', '--prefix', 'embed',
            '--vpath', '/player.js',          '@INPUT0@',
            '--vpath', '/player.mjs',         '@INPUT1@',
            '--vpath', '/videojs.js',         '@INPUT2@',
            '--vpath', '/dashjs.license.js',  '@INPUT3@',
            '--vpath', '/dashjs.js',          '@INPUT4@',
            '--vpath', '/flv.js',             '@INPUT5@',
            '--vpath', '/hlsjs.js',           '@INPUT6@',
            '--vpath', '/libde265.js',        '@INPUT7@',
            '--vpath', '/skins/default.css',  '@INPUT8@',
            '--vpath', '/skins/dev.css',      '@INPUT9@',
            '--vpath', '/skins/videojs.css',  '@INPUT10@',
            '--vpath', '/webcodecsworker.js', '@INPUT11@']
)

# Legacy alias: sourcery is still used for icon.h and potentially other one-off embeds
embed_tgts = [embed_vfs]
