#!/bin/sh
# build.sh WORKDIR: signal-host, the Signal backend of KolibriOS's
# messenger (docs/messengers.md), one static AArch64 Linux program (musl)
# for the kernel's Linux personality: presage and libsignal (Rust, the
# versions pinned in Cargo.toml and Cargo.lock), SQLite and AWS-LC (C,
# through zig cc), linked by clang with lld against Rust's own musl.
#
# Needs cargo with the aarch64-unknown-linux-musl target (Arch:
# rust-aarch64-musl), zig, clang, lld, cmake. WORKDIR holds the build
# tree (target/, some 3 GB): on a disk with room.
set -eu
W=$(mkdir -p "$1" && cd "$1" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)
T=aarch64-unknown-linux-musl
export CARGO_TARGET_DIR="$W/target"
# C: zig's compiler for musl (the cc crate's --target dropped by the wrapper)
export CC_aarch64_unknown_linux_musl="$HERE/zig-cc" AR_aarch64_unknown_linux_musl="$HERE/zig-ar"
export CFLAGS_aarch64_unknown_linux_musl=-O2
export CMAKE_TOOLCHAIN_FILE_aarch64_unknown_linux_musl="$HERE/toolchain.cmake"
# the link: clang drives lld; Rust brings musl's crt and libc (self-contained)
export CARGO_TARGET_AARCH64_UNKNOWN_LINUX_MUSL_LINKER=clang
export CARGO_TARGET_AARCH64_UNKNOWN_LINUX_MUSL_RUSTFLAGS="-C link-arg=--target=$T -C link-arg=-fuse-ld=lld -C target-feature=+crt-static"
cd "$HERE"
cargo build --release --locked --target $T -j"${J:-3}"
cp "$CARGO_TARGET_DIR/$T/release/signal-host" "$W/signal-host"
ls -l "$W/signal-host"
