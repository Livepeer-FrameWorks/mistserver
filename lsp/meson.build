fs = import('fs')

variant_dist_files = {}

_lsp_variants = {
  'mi':         ['dist/mi/index.html',         'dist/mi/main.js',         'dist/mi/main.css'],
  'efg':        ['dist/efg/index.html',        'dist/efg/main.js',        'dist/efg/main.css'],
  'playground': ['dist/playground/index.html',  'dist/playground/main.js', 'dist/playground/main.css'],
}

foreach slug, paths : _lsp_variants
  if fs.exists(paths[0])
    variant_dist_files += {slug: files(paths[0], paths[1], paths[2])}
  endif
endforeach
