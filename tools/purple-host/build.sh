#!/bin/sh
# build.sh WORKDIR [STAGE...]: purple-host, the Telegram backend of
# KolibriOS's messenger (docs/messengers.md): libpurple with tdlib-purple
# in one static AArch64 Linux program (musl, through zig cc), for the
# kernel's Linux personality.
#
# WORKDIR holds the sources (dl/, src/), the build trees (build/) and the
# libraries (sysroot/): some 6 GB, TDLib's the most; on a disk with room.
# Stages, in order (all without arguments): fetch zlib openssl td glib
# libxml2 purple fmt tdpurple host. The protocol is tdlib-purple 0.9.4 of
# UnitedMarsupials (the maintained fork of ars3niy's, GPL 2 or later) on
# TDLib 1.8.67; TDLib builds its code generators natively first.
set -eu
W=$(cd "$1" && pwd)
shift
TOOLS=$(cd "$(dirname "$0")" && pwd)
P=$W/sysroot
J=${J:-2}
export SYSROOT=$P PATH="$TOOLS:$PATH"
CC=$TOOLS/zig-cc CXX=$TOOLS/zig-c++ AR=$TOOLS/zig-ar RANLIB=$TOOLS/zig-ranlib
export PKG_CONFIG_LIBDIR=$P/lib/pkgconfig PKG_CONFIG_SYSROOT_DIR=
HOST=aarch64-linux-musl
mkdir -p "$W/dl" "$W/src" "$W/build" "$P"

TD_COMMIT=bc9c263e2     # "Update version to 1.8.67." (2026-08-24)

fetch() {
	cd "$W/dl"
	while read -r sum url; do
		f=${url##*/}
		[ "$f" != 11.0.2.tar.gz ] || f=fmt-11.0.2.tar.gz
		[ -f "$f" ] || { curl -sSfL -o "$f.part" "$url" && mv "$f.part" "$f"; }
		echo "$sum  $f" | sha256sum -c --quiet - || { echo "build.sh: $f: wrong checksum" >&2; exit 1; }
	done <<-EOF
	9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23 https://zlib.net/fossils/zlib-1.3.1.tar.gz
	23c666d0edf20f14249b3d8f0368acaee9ab585b09e1de82107c66e1f3ec9533 https://github.com/openssl/openssl/releases/download/openssl-3.0.15/openssl-3.0.15.tar.gz
	9f23a9de803c695bbfde7e37d6626b18b9a83869689dd79019bf3ae66c3e6771 https://download.gnome.org/sources/glib/2.80/glib-2.80.5.tar.xz
	59912db536ab56a3996489ea0299768c7bcffe57169f0235e7f962a91f483590 https://download.gnome.org/sources/libxml2/2.12/libxml2-2.12.9.tar.xz
	0ffc9994def10260f98a55cd132deefa8dc4a9835451cc0e982747bd458e2356 https://downloads.sourceforge.net/project/pidgin/Pidgin/2.14.14/pidgin-2.14.14.tar.bz2
	6cb1e6d37bdcb756dbbe59be438790db409cdb4868c66e888d5df9f13f7c027f https://github.com/fmtlib/fmt/archive/refs/tags/11.0.2.tar.gz
	eb145d32b28f378f194d612f7eaa878237d07ec6b2809f7f644f67eeef15da56 https://github.com/UnitedMarsupials/tdlib-purple/releases/download/v0.9.4/tdlib-purple-0.9.4.tar.xz
	EOF
	cd "$W/src"
	[ -d td ] || git clone -q --filter=blob:none https://github.com/tdlib/td.git td
	git -C td fetch -q origin master
	git -C td checkout -q -- .
	git -C td checkout -q "$TD_COMMIT"
}

unpack() {   # unpack NAME: dl/NAME.tar.* into build/NAME, afresh
	rm -rf "${W:?}/build/$1"
	tar -xf "$W"/dl/"$1".tar.* -C "$W/build"
}

zlib() {
	unpack zlib-1.3.1
	cd "$W/build/zlib-1.3.1"
	CC=$CC AR=$AR RANLIB=$RANLIB CFLAGS=-O2 ./configure --static --prefix="$P" > /dev/null
	make -j"$J" > /dev/null
	make install > /dev/null
}

openssl() {
	unpack openssl-3.0.15
	cd "$W/build/openssl-3.0.15"
	# the CPU's features from AT_HWCAP with musl too (as with glibc): else
	# OpenSSL probes for them with instructions and SIGILL, which the
	# Linux personality does not deliver
	sed -i 's|^# if defined(__GLIBC__) \&\& defined(__GLIBC_PREREQ)$|# if defined(__linux__) \&\& !defined(__GLIBC__)\n#  include <sys/auxv.h>\n#  define OSSL_IMPLEMENT_GETAUXVAL\n# elif defined(__GLIBC__) \&\& defined(__GLIBC_PREREQ)|' crypto/armcap.c
	grep -q '!defined(__GLIBC__)' crypto/armcap.c
	./Configure linux-aarch64 no-shared no-tests no-module no-dso no-engine \
	    --prefix="$P" --libdir=lib CC="$CC" AR="$AR" RANLIB="$RANLIB" > /dev/null
	make -j"$J" build_libs > /dev/null
	make install_dev > /dev/null
}

td() {
	# the generators, natively (they run during the build), then TDLib
	mkdir -p "$W/build/td-native"
	cd "$W/build/td-native"
	env -u PKG_CONFIG_LIBDIR -u SYSROOT cmake -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
	    -DCMAKE_BUILD_TYPE=Release -DOPENSSL_ROOT_DIR=/usr "$W/src/td" > /dev/null
	env -u PKG_CONFIG_LIBDIR -u SYSROOT cmake --build . --target prepare_cross_compiling -j"$J" > /dev/null
	# no shared tdjson: zig's linker does not take it (and nothing here needs it)
	sed -i 's/add_library(tdjson SHARED/add_library(tdjson STATIC/' "$W/src/td/CMakeLists.txt"
	mkdir -p "$W/build/td"
	cd "$W/build/td"
	cmake -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_TOOLCHAIN_FILE="$TOOLS/toolchain.cmake" -DCMAKE_BUILD_TYPE=Release \
	    -DCMAKE_INSTALL_PREFIX="$P" -DOPENSSL_ROOT_DIR="$P" -DOPENSSL_USE_STATIC_LIBS=ON \
	    -DZLIB_ROOT="$P" -DTD_ENABLE_JNI=OFF -DTD_ENABLE_DOTNET=OFF "$W/src/td" > /dev/null
	cmake --build . --target tdclient -j"$J"
	cmake --build . --target install -j"$J" > /dev/null
}

glib() {
	unpack glib-2.80.5
	sed "s|@TOOLS@|$TOOLS|g" "$TOOLS/meson-cross.ini" > "$W/build/cross.ini"
	cd "$W/build/glib-2.80.5"
	meson setup _b --cross-file "$W/build/cross.ini" --prefix="$P" --libdir=lib \
	    -Ddefault_library=static -Dtests=false -Dnls=disabled -Dlibmount=disabled \
	    -Dselinux=disabled -Dxattr=false -Dintrospection=disabled -Dman-pages=disabled \
	    -Ddocumentation=false -Dsysprof=disabled -Dglib_debug=disabled \
	    -Dlibelf=disabled -Dforce_fallback_for=libffi,pcre2 > /dev/null
	ninja -C _b -j"$J" > /dev/null
	ninja -C _b install > /dev/null
}

libxml2() {
	unpack libxml2-2.12.9
	cd "$W/build/libxml2-2.12.9"
	./configure --host=$HOST --prefix="$P" --disable-shared --enable-static --without-python \
	    --without-lzma --without-icu --without-http --without-ftp --without-debug \
	    --with-zlib="$P" CC="$CC" AR="$AR" RANLIB="$RANLIB" CFLAGS=-O2 > /dev/null
	make -j"$J" > /dev/null
	make install > /dev/null
}

purple() {
	# libpurple alone: no UI, no protocols of its own, no D-Bus, no NLS
	unpack pidgin-2.14.14
	cd "$W/build/pidgin-2.14.14"
	./configure --host=$HOST --prefix="$P" --disable-shared --enable-static \
	    --disable-gtkui --disable-consoleui --disable-gstreamer --disable-vv --disable-idn \
	    --disable-meanwhile --disable-avahi --disable-dbus --disable-perl --disable-tcl \
	    --disable-nm --disable-nss --disable-gnutls --disable-cyrus-sasl --disable-doxygen \
	    --disable-nls --disable-gevolution --disable-screensaver --disable-startup-notification \
	    --with-static-prpls= --with-dynamic-prpls= --disable-plugins \
	    CC="$CC" AR="$AR" RANLIB="$RANLIB" CFLAGS=-O2 > /dev/null
	make -C libpurple -j"$J" > /dev/null
	make -C libpurple install > /dev/null
}

fmt() {
	unpack fmt-11.0.2
	mkdir -p "$W/build/fmt-11.0.2/_b"
	cd "$W/build/fmt-11.0.2/_b"
	cmake -DCMAKE_TOOLCHAIN_FILE="$TOOLS/toolchain.cmake" -DCMAKE_BUILD_TYPE=Release \
	    -DCMAKE_INSTALL_PREFIX="$P" -DCMAKE_INSTALL_LIBDIR=lib -DFMT_TEST=OFF -DFMT_DOC=OFF .. > /dev/null
	cmake --build . -j"$J" > /dev/null
	cmake --build . --target install > /dev/null
}

tdpurple() {
	# tdlib-purple as a static protocol (purple_init_telegram_tdlib_plugin()),
	# without calls, animated stickers and translations
	unpack tdlib-purple-0.9.4
	cd "$W/build/tdlib-purple-0.9.4"
	sed -i 's/add_library(telegram-tdlib SHARED/add_library(telegram-tdlib STATIC/' CMakeLists.txt
	mkdir -p _b
	cd _b
	cmake -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_TOOLCHAIN_FILE="$TOOLS/toolchain.cmake" \
	    -DCMAKE_BUILD_TYPE=Release -DTd_DIR="$P/lib/cmake/Td" -DNoVoip=TRUE -DNoLottie=TRUE \
	    -DNoTranslations=TRUE -DCMAKE_CXX_FLAGS=-DPURPLE_STATIC_PRPL .. > /dev/null
	cmake --build . --target telegram-tdlib -j"$J" > /dev/null
}

host() {
	mkdir -p "$W/build/host"
	cd "$W/build/host"
	TP=$W/build/tdlib-purple-0.9.4
	# shellcheck disable=SC2046
	$CC -O2 -Wall -c "$TOOLS/purple-host.c" $(pkg-config --cflags purple glib-2.0) -o purple-host.o
	# shellcheck disable=SC2046
	$CXX -static -o purple-host purple-host.o \
	    "$TP/_b/libtelegram-tdlib.a" -L"$P/lib" -lpurple -lfmt \
	    -Wl,--start-group $(cd "$P/lib" && ls libtd*.a | sed 's/^lib/-l/; s/\.a$//' | grep -v json) -Wl,--end-group \
	    $(pkg-config --libs --static glib-2.0 gobject-2.0 gmodule-2.0 gthread-2.0 libxml-2.0) \
	    -lssl -lcrypto -lz
	llvm-strip purple-host
	ls -l purple-host
}

[ $# -gt 0 ] || set -- fetch zlib openssl td glib libxml2 purple fmt tdpurple host
for s in "$@"; do
	echo "== $s"
	"$s"
done
