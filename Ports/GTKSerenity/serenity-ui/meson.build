project('serenity-ui', 'c',
  version: '1.0.0',
  license: 'BSD-2-Clause',
  default_options: ['c_std=c11', 'warning_level=2'])

cc = meson.get_compiler('c')
cairo_dep = dependency('cairo', required: true)
threads_dep = dependency('threads', required: true)
m_dep = cc.find_library('m', required: false)

# Built as a STATIC library on purpose: the GTK forks absorb it, so
# libgtk-3.so.0 / libgtk-4.so.1 / libadwaita-1.so.0 keep exactly the same
# SONAME, exported symbol table and DT_NEEDED entries. Existing applications
# do not need to be recompiled or relinked.
serenity_ui = static_library('serenity-ui',
  ['serenity-ui.c'],
  dependencies: [cairo_dep, threads_dep, m_dep],
  c_args: ['-D_GNU_SOURCE', '-fPIC', '-fvisibility=hidden',
           '-DSERENITY_UI_STATIC'],
  pic: true,
  install: false,
  build_by_default: true)

serenity_ui_dep = declare_dependency(
  link_with: serenity_ui,
  dependencies: [cairo_dep, threads_dep, m_dep],
  include_directories: include_directories('.'))

# Headless reference renderer: shows every Serenity widget without a display
# server, and doubles as a visual regression target for theme changes.
executable('serenity-widget-preview',
  ['preview.c'],
  dependencies: [serenity_ui_dep, cairo_dep],
  c_args: ['-D_GNU_SOURCE'],
  build_by_default: true)

# Small CLI used by check.sh and by the session to dump the active palette.
executable('serenity-theme-dump',
  ['theme-dump.c'],
  dependencies: [serenity_ui_dep],
  c_args: ['-D_GNU_SOURCE'],
  build_by_default: true)
