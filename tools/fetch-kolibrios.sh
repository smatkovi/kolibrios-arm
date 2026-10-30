#!/bin/sh
# fetch-kolibrios.sh: the sources of KolibriOS's own programs that are
# ported here (shell, terminal, kfar, t_edit with t_edit_synt) and of the
# libraries they use (console, box_lib, libini, msgbox, proc_lib), into
# third_party/kolibrios (not kept in the repository), each file at the
# commit below and checked by its SHA-256 (tools/kolibrios-files.txt).
# The Makefile runs it when third_party/kolibrios is missing.
#
# KolibriOS is GPL-2.0 (COPYING.TXT, fetched with them); the authors are
# named in each source and in docs/apps.md. The files come byte for byte
# as they are in KolibriOS's repository: CR LF line ends, comments in
# CP866 (Russian).
#
# Gitea's API serves single files of a commit: GET /api/v1/repos/
# KolibriOS/kolibrios/raw/PATH?ref=COMMIT (the web UI's raw links answer
# 403 to scripts). git.kolibrios.org is KolibriOS's own host.
set -eu
COMMIT=855f02d2950bdb254423d84e4678524c6bb94604     # main, 2026-10-28
API=https://git.kolibrios.org/api/v1/repos/KolibriOS/kolibrios/raw
top=$(cd "$(dirname "$0")/.." && pwd)
list=$top/tools/kolibrios-files.txt
dest=$top/third_party/kolibrios
[ -f "$dest/.complete" ] && [ "$dest/.complete" -nt "$list" ] && exit 0
mkdir -p "$dest"
n=0
while read -r sum path; do
	[ -n "$sum" ] || continue
	f=$dest/$path
	if [ ! -f "$f" ] || ! echo "$sum  $f" | sha256sum -c --quiet - 2> /dev/null; then
		mkdir -p "$(dirname "$f")"
		url=$API/$(printf '%s' "$path" | sed 's/ /%20/g')?ref=$COMMIT
		curl -sSfL --connect-timeout 20 -m 300 -o "$f.part" "$url"
		echo "$sum  $f.part" | sha256sum -c --quiet - || {
			echo "fetch-kolibrios.sh: $path does not match its checksum" >&2
			rm -f "$f.part"
			exit 1
		}
		mv "$f.part" "$f"
	fi
	n=$((n + 1))
done < "$list"
echo "$COMMIT" > "$dest/.complete"
echo "KolibriOS $COMMIT: $n files in $dest"
