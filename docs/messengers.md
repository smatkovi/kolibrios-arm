# Messengers: WhatsApp, Telegram and Signal under KolibriOS

The messengers on the Pro1X under KolibriOS use the same backends as on
Maemo Leste where there is one, and an open Signal client library for
Signal:

- **WhatsApp**: Yappari's daemon `yappari-wa`
  ([smatkovi/yappari-leste](https://github.com/smatkovi/yappari-leste)),
  Go with whatsmeow, the multi-device protocol, linked to the main phone by
  QR code or phone number;
- **Telegram**: the libpurple plugin tdlib-purple (Leste's
  `purple-telegram`) on TDLib, in a small libpurple host program;
- **Signal**: [presage](https://github.com/whisperfish/presage), the Rust
  Signal client library of whisperfish and flare, on Signal's own
  libsignal, in a small program of its own (`tools/signal-host`), linked
  to the main phone as a secondary device by QR code.

All three run unchanged as Linux programs on a Linux personality of the kernel;
the user interfaces are KolibriOS programs in the Hildon look. Below: why
that way, the pieces, the protocol between them, the Telegram build, and
what is done. The network under them: [pro1x-network.md](pro1x-network.md).

## Why a Linux personality

- `yappari-wa` is Go. Go's runtime exists for real operating systems
  only; a KolibriOS port of it would be a new `GOOS` (runtime, syscall
  package, net poller), kept up with every Go release. A static
  `linux/arm64` program (`CGO_ENABLED=0`) instead needs some 40 Linux
  system calls (measured below), and whatsmeow, its SQLite (modernc, pure
  Go), TLS and DNS come with it unchanged.
- The Telegram plugin is C++17 on TDLib, libpurple and GLib: threads, a
  C++ runtime, a libc. Statically linked with musl, it too only needs the
  Linux system calls, the same layer.

So one piece in the kernel serves both, and both backends stay what Leste
runs (and what gets their fixes). The KolibriOS programs (ARMv7, AArch32)
stay as they are; Linux programs are AArch64 at EL0 next to them.

## The pieces

```
 KolibriOS (AArch32 programs)            Linux personality (AArch64 EL0)
 +----------------------------+          +-------------------------------+
 | chat (Hildon look):        |  pipes   | yappari-wa -stdio  (WhatsApp)  |
 |  accounts, chats, a chat,  | <------> | purple-host -stdio (Telegram:  |
 |  QR code, login prompts    |          |   libpurple + tdlib-purple)    |
 |                            |          | signal-host -stdio (Signal:    |
 |                            |          |   presage + libsignal)         |
 +----------------------------+          +-------------------------------+
              | function 101                  | Linux system calls
 +-----------------------------------------------------------------------+
 | kernel: tasks, windows | kernel/linux: ELF, memory, threads, futexes, |
 |                        | files, sockets, epoll | kernel/net: lwIP      |
 +-----------------------------------------------------------------------+
```

### The protocol: JSON lines over a pipe

A backend's standard input and output are a pair of kernel pipes; the
UI program starts the backend and reads and writes them through function
101 (below). One JSON object per line, shaped after Yappari's D-Bus
interface, so the D-Bus methods and signals map one to one:

```
UI -> backend   {"id": 7, "call": "SendText", "args": ["", "4366...@s.whatsapp.net", "hello"]}
backend -> UI   {"id": 7, "result": ["3EB0..."]}
                {"id": 8, "error": "not connected"}
backend -> UI   {"signal": "QRCode", "args": ["2@..."]}
                {"signal": "Message", "args": ["id", "chat", "sender", "pushname", 1761750000, false, "text", "hi", "", 0]}
```

- `yappari-wa -stdio`: the same methods and signals as on the session
  bus (the list is in `backend/main.go` of yappari-leste); D-Bus stays the
  default on Leste. The questions it asks the bus (is the UI running,
  desktop notifications) are answered by the pipe: the UI is the other end.
- `purple-host -stdio`: the same message shapes for libpurple's side:
  `Login` (the account's fields), `Status`, `SendText`, `GetContacts`,
  `GetChats`; signals `StateChanged`, `Message`, `Receipt`, `ChatState`,
  and `Input` for what the plugin asks the user (Telegram's login code, the
  2FA password), answered with `InputReply`.

The chat program keeps one model for both: accounts, chats, messages;
the Hildon look as the Leste programs have it (the conversation list,
bubbles, the text field at the bottom; the QR code drawn as a picture).

### Function 101: Linux programs (KolibriOS for ARM)

| ebx | |
| --- | --- |
| 1 | start: ecx = path, edx = arguments (one string, space separated); eax = handle or -1 |
| 2 | write to its standard input: ecx = handle, edx = buffer, esi = length; eax = bytes or -1 |
| 3 | read its standard output: ecx = handle, edx = buffer, esi = size; eax = bytes (0: none now), -1 when it ended |
| 4 | status: eax = 0 running, 1 ended (and ebx its exit code) |
| 5 | stop it |

Output arriving posts event 8 (`EV_NETWORK`, KolibriOS's event for data)
to the program that started it, so the UI waits with function 10 as usual.

## The Linux personality (`kernel/linux/`)

What a static Go program does at start and while running, measured with
`qemu-aarch64 -strace` on a Go 1.27 hello world and on `yappari-wa`:

| | calls | here |
| --- | --- | --- |
| memory | `mmap` (anonymous, large `PROT_NONE` reservations: two of 512 MiB, 64 MiB, 8 MiB at start; then `MAP_FIXED` read-write pieces), `munmap`, `mprotect`, `madvise`, `brk` | 4 KiB pages, three-level tables of the process's own, pages allocated on first touch (data abort -> a zeroed page), reservations cost only address space. The address space: the first GiB (the program's ELF, from 0x10000) and 64-256 GiB (mmap) |
| threads | `clone(CLONE_VM \| CLONE_THREAD \| ...)`, `gettid`, `set_tid_address`, `exit`, `exit_group`, `sched_yield`, `sched_getaffinity` (one CPU) | a thread is a kernel task sharing the process's tables; x0-x30, SP_EL0, TPIDR_EL0, the FP/SIMD registers saved per thread |
| waiting | `futex` (WAIT, WAKE, private, with timeouts), `nanosleep`, `clock_gettime` (realtime, monotonic: the generic timer), `epoll_create1`/`epoll_ctl`/`epoll_pwait`, `eventfd2` | the kernel keeps no stack per task: a waiting call is a wait state, re-run or answered when the thread wakes |
| signals | `rt_sigaction`, `rt_sigprocmask`, `sigaltstack` (recorded), `tgkill` (Go's preemption by `SIGURG`: dropped; Go then preempts at function entries) | no delivery yet; a fault ends the process with a register dump |
| files | `openat`, `read`, `write`, `pread64`, `pwrite64`, `close`, `fstat`/`newfstatat`, `lseek`, `fcntl` (locks always granted: one process per file), `getdents64`, `mkdirat`, `unlinkat`, `renameat`, `ftruncate`, `fsync`, `readlinkat`, `getcwd` | the ramdisk read-only under `/`, a RAM file system for `/tmp`, `/var` and `$HOME`; `/dev/null`, `/dev/urandom`; `/etc/resolv.conf` from the DHCP lease; the CA bundle `/etc/ssl/certs/ca-certificates.crt` on the ramdisk |
| network | `socket` (TCP, UDP, v4), `connect` (non-blocking: `EINPROGRESS`), `getsockopt(SO_ERROR)`, `setsockopt`, `getsockname`, `getpeername`, `sendto`, `recvfrom`, `read`, `write`, `shutdown` | sockets on lwIP's raw API, readiness to epoll |
| other | `getrandom`, `uname`, `prlimit64`, `getpid`, `getuid` and friends | a ChaCha20 generator seeded from virtio-rng in QEMU, on the phone from the SoC's PRNG (to do: until then the seed is weak and linking must not be trusted) |

Not planned: `fork`/`exec` (neither backend runs programs), shared
memory between processes, `ptrace`.

## WhatsApp: yappari-wa

- Built as on Leste: `CGO_ENABLED=0 GOOS=linux GOARCH=arm64 go build` in
  yappari-leste's `backend/` (21 MiB), put on the ramdisk.
- Its state (`$XDG_DATA_HOME/yappari-wa/session.db`, the linked device's
  keys) is in the RAM file system: **lost at every reboot** until KolibriOS
  can write somewhere. Options: `tools/pro1x-kexec.sh` brings Leste's
  `session.db` along (the same linked device; Leste is gone while
  KolibriOS runs, but the Signal sessions move on under KolibriOS, and
  Leste's copy is stale afterwards); or a native UFS driver (large).
- Its clock must be right (TLS): KolibriOS's real-time clock, which the
  modem sets on the phone.

## Telegram: the purple host

`purple-host` (`tools/purple-host/`) is a C program of its own, linked
statically with musl: libpurple's core (no UI), tdlib-purple as a static
protocol (`purple_init_telegram_tdlib_plugin()`), GLib's main loop, the
JSON lines of `yappari-wa -stdio` on stdin/stdout. Methods `Status`,
`Login(phone)` (tdlib-purple's account name is the phone number),
`InputReply(id, text)`, `InputCancel(id)`, `SendText(who, text)`, `Logout`;
signals `StateChanged`, `Input` (what the plugin asks: the login code, the
password), `Notice`, `Message`, `Error`.

The protocol is [UnitedMarsupials' tdlib-purple](https://github.com/UnitedMarsupials/tdlib-purple)
0.9.4 (2026-10-27), the maintained fork of ars3niy's tdlib-purple (which
stopped at TDLib 1.7.9: Telegram refuses that one now with
`UPDATE_APP_TO_LOGIN`, tried). Its licence: GPL 2 or later, as upstream
(the `LICENSE` file; GitHub shows NOASSERTION only because it does not
recognise the file). TDLib is 1.8.67 (commit `bc9c263e2`); the fork
detects the version and probes the API.

`tools/purple-host/build.sh WORKDIR` builds all of it with `zig cc -target
aarch64-linux-musl` (Arch's `zig`), the sources pinned by SHA-256 and
commit: zlib 1.3.1, OpenSSL 3.0.15 (the CPU's features from `AT_HWCAP` with
musl too: else it probes with instructions and SIGILL, which the Linux
personality does not deliver), GLib 2.80.5 (meson, static), libxml2
2.12.9, libpurple 2.14.14 (no UI, no protocols of its own, no D-Bus, no
NLS), fmt 11.0.2, TDLib (its code generators natively first; its `tdjson`
static, which zig's linker needs), tdlib-purple without calls, animated
stickers and translations. TDLib takes some 1.5 h on 2 cores; the work
directory some 5 GB. The result: one static program of 35 MB.

`make run-telegram PURPLE_HOST=WORKDIR/build/host/purple-host
TG_NUMBER=+...`: QEMU with the messenger in Telegram mode (`kolibri.args=
telegram,NUMBER`), logging in with that number at once.

TDLib's database (the login) is in the RAM file system, the same
persistence question as WhatsApp's.

## Signal: the signal host

`signal-host` (`tools/signal-host/`) is a Rust program of its own on
[presage](https://github.com/whisperfish/presage) (the library under
flare and whisperfish), which is built on whisperfish's
[libsignal-service-rs](https://github.com/whisperfish/libsignal-service-rs)
(Signal's service: the REST calls, the websockets, the provisioning) and
Signal's own [libsignal](https://github.com/signalapp/libsignal) (the
protocol, zkgroup, the account keys). It links as a **secondary device**
(as Signal Desktop does): GetQR opens Signal's provisioning websocket,
the server hands out a provisioning address, and the program shows it as
a QR code; the phone's Signal (Settings, Linked devices, Link new device)
scans it and sends the account's keys over that socket. The store is
presage's SQLite one (`$XDG_DATA_HOME/signal-host/store.db`, in the RAM
file system: lost at a reboot, as WhatsApp's).

The protocol is `yappari-wa -stdio`'s, so the messenger's WhatsApp path
serves it unchanged (`kolibri.args=signal`: the title, the linking text,
`/bin/signal-host`):

```
UI -> host   {"id": 1, "call": "Status", "args": []}
host -> UI   {"id": 1, "result": ["logged-out"]}               (or "connecting", "connected")
UI -> host   {"id": 2, "call": "GetQR", "args": []}
host -> UI   {"signal": "QRCode", "args": ["sgnl://linkdevice?uuid=...&pub_key=..."]}
             {"id": 2, "result": ["sgnl://linkdevice?uuid=...&pub_key=..."]}
UI -> host   {"id": 3, "call": "SendText", "args": ["<ACI uuid> or group:<master key, hex>", "hello"]}
host -> UI   {"id": 3, "result": [1761750000000]}
             {"signal": "StateChanged", "args": ["connected"]}
             {"signal": "Message", "args": ["<timestamp ms>", "<chat>", "<sender>", "", 1761750000, false, "text", "hi", "", 0]}
             {"signal": "Error", "args": ["..."]}
```

The server closes an unscanned provisioning socket after about 90 s
(websocket close 1000, "Timeout"; seen in a 7-minute run on the PC: a new
address every 91 s); linking starts again and a new QRCode signal
follows (an Error signal only when no address came at all). After linking (and at a start with
a linked store) the host receives: StateChanged `connecting`, then
`connected` once the server's queue is empty; text messages (their own,
sent from the phone, too: `fromMe` true) as Message signals; SendText to a
contact (its ACI) or a group (its master key). Received messages, the
contacts and the groups are in presage's store as well. Only linking up
to the QR code has been tried (below); receiving and sending have not run
against Signal yet, since no account is linked in tests.

**Versions** (`tools/signal-host/Cargo.toml`, all of them in its
`Cargo.lock`, built with `--locked`):

| | |
| --- | --- |
| presage, presage-store-sqlite | `aeaedf99b` (2026-10-27, "feat(manager): sync groups from the storage service") |
| libsignal-service-rs | `72a09876` (2026-10-25, presage's pin), without `cdsi` (no libsignal-net, no BoringSSL) |
| libsignal (libsignal-protocol, -core, -account-keys, signal-crypto, zkgroup, usernames) | tag `v0.99.0` (`439f035d9`) |
| SQLite | libsqlite3-sys 0.36.0 (whisperfish's rusqlite fork `2a42b335`, as presage pins it), plain SQLite compiled in (`bundled`: not presage's default SQLCipher) |
| TLS | rustls 0.23.45 with ring 0.17.14 (libsignal-service installs it as the provider; Signal's servers are checked against the certificates libsignal-service carries, `certs/`), and AWS-LC (aws-lc-sys 0.46.0) through reqwest's `rustls` feature (presage's link previews; it is linked, not used for Signal's servers) |
| Rust | 1.99.1 (Arch's `rust`, `rust-aarch64-musl`: Rust's own musl, self-contained) |

**Licences:** presage, presage-store-sqlite, libsignal-service-rs and
libsignal are AGPL-3.0-only (their `LICENSE` files and `Cargo.toml`), so
`signal-host` is AGPL-3.0-only too (its `Cargo.toml`); whoever runs it
for others must offer its source. It is a program of its own, started
through function 101, not linked with KolibriOS (GPL 2). The rest: ring
Apache-2.0 AND ISC, AWS-LC ISC/Apache-2.0 (with MIT and BSD parts),
rustls Apache-2.0/MIT/ISC, SQLite public domain, tokio and the other
crates MIT/Apache-2.0.

**The build:** `tools/signal-host/build.sh WORKDIR`: `cargo build
--release --locked --target aarch64-unknown-linux-musl`, the C parts
(SQLite, AWS-LC, ring's assembly) through `zig cc -target
aarch64-linux-musl` (wrappers that drop the cc crate's `--target` and
zig's sanitizer for code built without -O: AWS-LC's jitterentropy), the
link by clang with lld against Rust's self-contained musl
(`+crt-static`). The target directory some 1.2 GB; the result one
static program of 17 MB.

`make run-signal SIGNAL_HOST=WORKDIR/signal-host`: QEMU with the
messenger in Signal mode (`kolibri.args=signal`). No CA bundle is needed:
the host trusts only the certificates it carries for Signal.

**What it needed of the Linux personality** (measured with
`qemu-aarch64 -strace` on the PC, then in QEMU): 47 system calls, all
there before but `pwritev2`; and one answered wrongly:

- `pwritev2` (287): musl 1.2.5's `pwrite` tries it first with
  `RWF_NOAPPEND` (POSIX's pwrite on an `O_APPEND` file), and falls back to
  `pwrite64` on `ENOSYS`; SQLite writes with `pwrite`, so every write
  logged "system call 287 not there". Now `preadv`, `pwritev`, `preadv2`
  and `pwritev2` are there (`kernel/linux/file.c`, offset -1: the file's
  position; `RWF_APPEND`, `RWF_NOAPPEND`).
- `mkdirat` of a directory whose parent is missing answered `EROFS`
  instead of `ENOENT`, so Rust's `create_dir_all` gave up at once
  (`~/.local/share/signal-host`); `EROFS` is now only for a parent on the
  ramdisk (`/bin/...`), as for `openat` with `O_CREAT`.

SQLite's WAL index (`store.db-shm`) is a `MAP_SHARED` mapping, which the
personality gives as a private copy. That is enough here: SQLite maps the
index once per process and shares it between its connections (one
`mmap` in the trace for presage's three), and no other process uses the
file. A real shared mapping (the RAM file's pages themselves) would be
needed for two processes on one database.

The rest was there: threads (`clone`, futexes), `epoll` and `eventfd`
(tokio), TCP with keep-alive options, UDP and `bind` (musl's resolver),
`ppoll`, `sendmsg`/`recvmsg`, file locks (`F_SETLK`), `fsync`,
`ftruncate`, `getrandom`.

## Status

- [x] the design (this document, [pro1x-network.md](pro1x-network.md))
- [x] the network stack in QEMU (virtio-net, lwIP, DHCP, DNS: [pro1x-network.md](pro1x-network.md))
- [x] the Linux personality (`kernel/linux/`): `tests/linux/hello` (Go: goroutines, timers,
  the GC), `tests/linux/nettest` (files, Go's resolver, three HTTPS requests at once, TLS 1.3)
- [x] `yappari-wa -stdio` in yappari-leste (`backend/stdio.go`)
- [x] function 101 and `messenger`, its first part: the daemon started, its state, linking by
  QR code (`lib/qr.h`, checked with zbarimg against the daemon's code)
- [ ] the chats in `messenger`: the conversation list, a conversation, sending (the daemon has
  all of it: `GetContacts`, `Message`, `SendText`, ...)
- [x] the purple host's build and program (`tools/purple-host/`); in QEMU (`make run-telegram`)
  the messenger's Telegram mode logs in with a number that is not anyone's, and Telegram's
  question for the login code shows in its window (sendmsg/recvmsg were missing before)
- [x] the signal host (`tools/signal-host/`, presage): on the PC under `qemu-aarch64` and in
  QEMU (`make run-signal`), the messenger's Signal mode shows the linking QR code from Signal's
  server (below)
- [ ] the phone: Wi-Fi ([pro1x-network.md](pro1x-network.md)), randomness, persistence

In QEMU (`make run-messenger YAPPARI_WA=<yappari-wa for linux/arm64>`): `messenger` starts the
daemon, which connects to WhatsApp through lwIP and Go's TLS and hands over its QR codes (a new
one every 20 seconds); a screenshot of QEMU's screen decodes with `zbarimg` to the daemon's code
of that moment. Linking itself was not tried (no account is used in tests), and must not be
until the random numbers come from hardware (`getrandom`, above).

Signal, the same way (`tests/linux/qrcheck.py`, which waits for the backend's code on the
serial console, takes QEMU's screenshot and decodes it with `zbarimg`):

```
make PLATFORM=arm64 LINUX_FILES="bin/signal-host=WORKDIR/signal-host" all
python3 tests/linux/qrcheck.py build/arm64/kernel.bin 200 sgnl://linkdevice -- \
    -append "kolibri.run=/sys/messenger kolibri.args=signal"
qrcheck: the backend's code: sgnl://linkdevice?uuid=X7dj6ZeuXXyfNNkinUIKmw%3D%3D&pub_key=Bdwl...
qrcheck: on the screen:       sgnl://linkdevice?uuid=X7dj6ZeuXXyfNNkinUIKmw%3D%3D&pub_key=Bdwl...
qrcheck: the same
```

The address is a real one from Signal's server (`chat.signal.org`'s provisioning socket needs
no account). It was not scanned: linking a real account waits for the phone's hardware
random numbers and the user.
