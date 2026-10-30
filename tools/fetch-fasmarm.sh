#!/bin/sh
# fetch-fasmarm.sh: FASMARM, the flat assembler for ARM (by "revolution",
# on Tomasz Grysztar's FASM 1.73), the assembler of the programs written
# in assembler (docs/asm.md), into third_party/fasmarm (not kept in the
# repository): the full package of version 1.44 (2023-07-01), checked by
# its SHA-256. The Makefile runs it when third_party/fasmarm is missing.
#
# Its licence is FASM's (a BSD-style licence: use and redistribution in
# source and binary form with the copyright notice kept; source/armv8.inc
# has the text). It runs on Linux on x86-64 (fasmarm.x64) and x86
# (fasmarm, 32 bits); the package also has an object for linking with a
# C library elsewhere (source/libc).
#
# From arm.flatassembler.net; that host did not resolve on 2026-10-30,
# so the Internet Archive's copies are tried next: its captures of
# 2025-06-06 and 2026-10-05 are the same bytes, the checksum below. If
# the author's site ever serves other bytes under this name, the check
# fails and someone has to look.
set -eu
VERSION=1.44
SHA256=3b424490014303584c4f8bcc8a1a665cdb7f52f37186059a0ced07f4b357adb7
FILE=FASMARM_full.ZIP
URLS="https://arm.flatassembler.net/$FILE
http://web.archive.org/web/20261005025025id_/https://arm.flatassembler.net/$FILE
http://web.archive.org/web/20250606090657id_/https://arm.flatassembler.net/$FILE"
top=$(cd "$(dirname "$0")/.." && pwd)
dest=$top/third_party/fasmarm
case $(uname -m) in
x86_64) bin=fasmarm.x64 ;;
i?86) bin=fasmarm ;;
*)
	echo "fetch-fasmarm.sh: FASMARM's executables are for x86 Linux; on $(uname -m)," >&2
	echo "link its source/libc object with a C library (docs/asm.md)" >&2
	exit 1 ;;
esac
[ -x "$dest/$bin" ] && exit 0
mkdir -p "$top/third_party"
zip=$top/third_party/fasmarm-$VERSION.zip
if [ ! -f "$zip" ] || ! echo "$SHA256  $zip" | sha256sum -c --quiet - 2> /dev/null; then
	rm -f "$zip"
	for url in $URLS; do
		if curl -sSfL --connect-timeout 20 -m 300 -o "$zip.part" "$url" 2> /dev/null &&
		   echo "$SHA256  $zip.part" | sha256sum -c --quiet - 2> /dev/null; then
			mv "$zip.part" "$zip"
			break
		fi
		rm -f "$zip.part"
		echo "fetch-fasmarm.sh: not from $url" >&2
	done
fi
[ -f "$zip" ] || {
	echo "fetch-fasmarm.sh: FASMARM $VERSION not found with SHA-256 $SHA256" >&2
	exit 1
}
tmp=$top/third_party/fasmarm.unpack
rm -rf "${tmp:?}"
mkdir -p "$tmp"
unzip -q "$zip" -d "$tmp"
chmod +x "$tmp/fasmarm.x64" "$tmp/fasmarm"
rm -rf "${dest:?}"
mv "$tmp" "$dest"
echo "FASMARM $VERSION in $dest"
