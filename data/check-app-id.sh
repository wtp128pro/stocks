#!/bin/sh
# Checks that the application id is spelled the same everywhere it matters.
#
# Usage: check-app-id.sh [SOURCE_ROOT]     (default: the parent of this script)
#
# The id is STK_APP_ID in src/ui/stk-app.h. It must also be
#   - the base name of the desktop file and of the metainfo file, the Icon= of
#     the desktop file, the <id> and <launchable> of the metainfo file,
#   - the base name of the two installed icons (and the only icons shipped),
#   - the GResource prefix of both .gresource.xml files and the path constants
#     in stk-app.c / stk-catalog.c (the id with '.' turned into '/'),
# and the retired id (the one under the org. domain, in its dotted and its
# path form) must not appear in any of those files, the README or the spec.
# Needs only POSIX sh, grep and sed.

set -u

root=${1:-$(dirname "$0")/..}
cd "$root" || exit 2

fail=0
bad() {
  echo "check-app-id: $*" >&2
  fail=1
}

header=src/ui/stk-app.h
id=$(sed -n 's/^#define STK_APP_ID "\([^"]*\)".*/\1/p' "$header" 2>/dev/null)
if [ -z "$id" ]; then
  echo "check-app-id: no STK_APP_ID in $header" >&2
  exit 1
fi
path=/$(echo "$id" | sed 's|\.|/|g')

desktop=data/$id.desktop.in
metainfo=data/$id.metainfo.xml.in
scalable=data/icons/hicolor/scalable/apps/$id.svg
symbolic=data/icons/hicolor/symbolic/apps/$id-symbolic.svg

for f in "$desktop" "$metainfo" "$scalable" "$symbolic"; do
  [ -f "$f" ] || bad "missing file $f"
done

# All comparisons are literal (grep -F / string equality): the id contains dots.
[ -f "$desktop" ] && ! grep -qxF "Icon=$id" "$desktop" && bad "$desktop: Icon= is not $id"
if [ -f "$metainfo" ]; then
  meta_id=$(sed -n 's|^[[:space:]]*<id>\(.*\)</id>[[:space:]]*$|\1|p' "$metainfo")
  [ "$meta_id" = "$id" ] || bad "$metainfo: <id> is not $id"
  meta_launch=$(sed -n 's|^[[:space:]]*<launchable type="desktop-id">\(.*\)</launchable>[[:space:]]*$|\1|p' "$metainfo")
  [ "$meta_launch" = "$id.desktop" ] || bad "$metainfo: <launchable> is not $id.desktop"
fi

# Only the two icons may exist under data/icons.
for f in $(find data/icons -type f | sort); do
  [ "$f" = "$scalable" ] || [ "$f" = "$symbolic" ] || bad "unexpected icon file $f"
done

grep -qF "<gresource prefix=\"$path/ui\">" src/ui/stocks.gresource.xml ||
  bad "src/ui/stocks.gresource.xml: prefix is not $path/ui"
grep -qF "<gresource prefix=\"$path/catalog\">" src/catalog/catalog.gresource.xml ||
  bad "src/catalog/catalog.gresource.xml: prefix is not $path/catalog"
grep -qxF "#define CSS_RESOURCE_PATH \"$path/ui/style.css\"" src/ui/stk-app.c ||
  bad "src/ui/stk-app.c: CSS_RESOURCE_PATH is not $path/ui/style.css"
grep -qxF "#define CATALOG_RESOURCE_FIDELITY \"$path/catalog/fidelity_catalog.json\"" src/catalog/stk-catalog.c ||
  bad "src/catalog/stk-catalog.c: CATALOG_RESOURCE_FIDELITY is not under $path/catalog"
grep -qxF "#define CATALOG_RESOURCE_INDICES \"$path/catalog/market_indices.json\"" src/catalog/stk-catalog.c ||
  bad "src/catalog/stk-catalog.c: CATALOG_RESOURCE_INDICES is not under $path/catalog"

for f in "$header" "$desktop" "$metainfo" src/ui/stocks.gresource.xml src/ui/stk-app.c \
  src/catalog/catalog.gresource.xml src/catalog/stk-catalog.c README.md packaging/rpm/stocks.spec data/meson.build; do
  [ -f "$f" ] || continue
  if grep -nE 'org[./]stocks' "$f" >&2; then
    bad "$f mentions the retired application id"
  fi
done

[ "$fail" -eq 0 ] && echo "check-app-id: $id is consistent"
exit "$fail"
