# Everything-1.0.0 — meson build configuration
# SPDX-License-Identifier: MIT

project('org.voidtools.Everything', 'c',
  version : '1.0.0',
  license  : 'MIT',
  meson_version : '>=0.62.0',
  default_options : [
    'c_std=gnu11',
    'warning_level=2',
    'b_pie=true',
  ],
)

# Enable POSIX + GNU extensions needed for lstat, S_ISSOCK, S_ISVTX,
# inotify, etc. gnu11 std + the define below makes these visible.
add_project_arguments('-D_GNU_SOURCE', language : 'c')

gnome = import('gnome')
i18n  = import('i18n')

# Dependencies provided by the runtime (GNOME 47 ships all of these).
glib_dep    = dependency('glib-2.0',  version : '>=2.74')
gio_dep     = dependency('gio-2.0',   version : '>=2.74')
gtk_dep     = dependency('gtk4',      version : '>=4.12')
adwaita_dep = dependency('libadwaita-1', version : '>=1.5')
sqlite_dep  = dependency('sqlite3',   version : '>=3.42')  # FTS5 contentless delete-all

conf_data = configuration_data()
conf_data.set_quoted('APP_ID',           'org.voidtools.Everything')
conf_data.set_quoted('APP_DISPLAY_NAME', 'Everything')
conf_data.set_quoted('APP_VERSION', meson.project_version())
config_h = configure_file(
  input  : 'src/config.h.in',
  output : 'config.h',
  configuration : conf_data,
)

# GResource — compiled into the executable via the generated C source.
# We do not install the standalone .gresource bundle because the UI
# template is linked into the binary.
gresource_xml = 'src/org.voidtools.Everything.gresource.xml'
gresource = gnome.compile_resources(
  'everything-resources',
  gresource_xml,
  source_dir : 'src',
  c_name     : 'everything',
)

sources = [
  'src/main.c',
  'src/everything-config.c',
  'src/everything-index.c',
  'src/everything-search.c',
  'src/everything-result-row.c',
  'src/everything-window.c',
]

executable('everything',
  sources + gresource,
  include_directories : ['src'],
  dependencies : [glib_dep, gio_dep, gtk_dep, adwaita_dep, sqlite_dep],
  install : true,
)

# Desktop file (i18n merge — works because msgfmt can merge .desktop
# files via the simple key=value format without ITS rules).
desktop_file = i18n.merge_file(
  input  : 'data/org.voidtools.Everything.desktop.in',
  output : 'org.voidtools.Everything.desktop',
  type   : 'desktop',
  po_dir : 'po',
  install : true,
  install_dir : join_paths(get_option('datadir'), 'applications'),
)

# AppStream metainfo: msgfmt needs ITS rules to merge XML translations.
# When a recent-enough msgfmt is available, we use i18n.merge_file. As a
# fallback (older msgfmt, or msgfmt without ITS files installed), we
# install the .in file verbatim. Since we currently ship only an
# English source catalogue, the two paths produce identical output.
appstream_in = 'data/org.voidtools.Everything.appdata.xml.in'
appstream_out = 'org.voidtools.Everything.appdata.xml'
if find_program('msgfmt', required : false).found()
  r = run_command(find_program('msgfmt'), '--xml', '--template', meson.current_source_dir() / appstream_in,
                  '-d', meson.current_source_dir() / 'po', '-o', '/dev/null',
                  check : false)
  if r.returncode() == 0
    appstream_file = i18n.merge_file(
      input  : appstream_in,
      output : appstream_out,
      type   : 'xml',
      po_dir : 'po',
      install : true,
      install_dir : join_paths(get_option('datadir'), 'metainfo'),
    )
  else
    configure_file(input : appstream_in, output : appstream_out,
                   copy : true,
                   install : true,
                   install_dir : join_paths(get_option('datadir'), 'metainfo'))
  endif
else
  configure_file(input : appstream_in, output : appstream_out,
                 copy : true,
                 install : true,
                 install_dir : join_paths(get_option('datadir'), 'metainfo'))
endif

# Icons
install_data(
  'data/icons/scalable/org.voidtools.Everything.svg',
  install_dir : join_paths(get_option('datadir'), 'icons/hicolor/scalable/apps'),
)
install_data(
  'data/icons/symbolic/org.voidtools.Everything-symbolic.svg',
  install_dir : join_paths(get_option('datadir'), 'icons/hicolor/symbolic/apps'),
)

# GSchema
install_data(
  'data/org.voidtools.Everything.gschema.xml',
  install_dir : join_paths(get_option('datadir'), 'glib-2.0/schemas'),
)
meson.add_install_script('meson_post_install.py')
