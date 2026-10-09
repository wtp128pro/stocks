# Desktop integration: desktop entry, AppStream metainfo and the two icons.
# data/fidelity_catalog.json and data/market_indices.json are NOT installed:
# they are compiled into the binary (src/catalog).

app_id = 'io.github.wtp128pro.Stocks'

datadir = get_option('prefix') / get_option('datadir')

# Nothing to substitute: the .in files are installed verbatim.
desktop_file = configure_file(
  input: app_id + '.desktop.in',
  output: app_id + '.desktop',
  copy: true,
)
install_data(desktop_file, install_dir: datadir / 'applications')

metainfo_file = configure_file(
  input: app_id + '.metainfo.xml.in',
  output: app_id + '.metainfo.xml',
  copy: true,
)
install_data(metainfo_file, install_dir: datadir / 'metainfo')

install_data(
  'icons/hicolor/scalable/apps' / (app_id + '.svg'),
  install_dir: datadir / 'icons/hicolor/scalable/apps',
)
install_data(
  'icons/hicolor/symbolic/apps' / (app_id + '-symbolic.svg'),
  install_dir: datadir / 'icons/hicolor/symbolic/apps',
)

# Always registered: needs only sh, grep and sed.
sh = find_program('sh')
test(
  'app-id',
  sh,
  args: [files('check-app-id.sh'), meson.project_source_root()],
  suite: 'data',
)

# The validators are optional: without the tool the test is not registered.
desktop_file_validate = find_program('desktop-file-validate', required: false)
if desktop_file_validate.found()
  test('desktop-file', desktop_file_validate, args: [desktop_file], suite: 'data')
endif

appstreamcli = find_program('appstreamcli', required: false)
if appstreamcli.found()
  test(
    'metainfo-appstreamcli',
    appstreamcli,
    args: ['validate', '--no-net', metainfo_file],
    suite: 'data',
  )
endif

appstream_util = find_program('appstream-util', required: false)
if appstream_util.found()
  test(
    'metainfo-appstream-util',
    appstream_util,
    args: ['validate-relax', '--nonet', metainfo_file],
    suite: 'data',
  )
endif
