#!/bin/sh
# fetch-lwip.sh: lwIP, the kernel's TCP/IP stack, into third_party/lwip
# (not kept in the repository): the release from savannah, checked by
# its SHA-256. The Makefile runs it when third_party/lwip is missing.
set -eu
VERSION=2.2.1
SHA256=7b622662dba2383d71f874f2e494b54ae948531559c17acbe75797966d646878
URL=https://download.savannah.nongnu.org/releases/lwip/lwip-$VERSION.zip
top=$(cd "$(dirname "$0")/.." && pwd)
dest=$top/third_party/lwip
[ -f "$dest/src/core/init.c" ] && exit 0
mkdir -p "$top/third_party"
zip=$top/third_party/lwip-$VERSION.zip
if [ ! -f "$zip" ]; then
	curl -sSfL -o "$zip.part" "$URL"
	mv "$zip.part" "$zip"
fi
echo "$SHA256  $zip" | sha256sum -c --quiet - || {
	echo "fetch-lwip.sh: $zip does not match its checksum" >&2
	exit 1
}
tmp=$top/third_party/lwip.unpack
rm -rf "${tmp:?}"
mkdir -p "$tmp"
unzip -q "$zip" -d "$tmp"
mv "$tmp/lwip-$VERSION" "$dest"
rmdir "$tmp"
echo "lwIP $VERSION in $dest"
