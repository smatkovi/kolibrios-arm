#!/usr/bin/env python3
"""tests/storage: the storage on the PC and in QEMU (make -C tests/storage).

    run.py [TEST...]      host, ufs, virtio, ufs-qemu, emmc, realview, tamper, kfar (all: none given)

Disks like the phones' (mkdisk.py) in /tmp/kolibri-storage-test. Every
test that writes checks afterwards, on the host, that the image is
unchanged outside the container's ranges (sha256 of everything else),
that e2fsck -fn and fsck.fat -n find the file systems clean (the ext4
partitions, MyDocs, the container), and what was written is in the
container (mtools). The QEMU tests boot the kernel two or three times on
the same image: kolibri.home=ro (the volumes listed and compared with
the files put there; every write refused), kolibri.home=rw (storetest rw
on /hd0/1), and again (storetest keep: still there after the reset).
"""
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.dirname(os.path.dirname(HERE))
OUT = "/tmp/kolibri-storage-test"
sys.path.insert(0, HERE)
sys.dont_write_bytecode = True
import mkdisk  # noqa: E402

failed = []
ENV = dict(os.environ, MTOOLS_SKIP_CHECK="1", LANG="C.UTF-8", LC_ALL="C.UTF-8")


def check(ok, what):
    print(("ok   " if ok else "FAIL ") + what)
    if not ok:
        failed.append(what)
    return ok


def ranges_of(img):
    return [tuple(int(x) for x in l.split()) for l in open(img + ".ranges")]


def outside_hash(img):
    """sha256 of the image without the container's ranges"""
    h = hashlib.sha256()
    rs = sorted(ranges_of(img))
    with open(img, "rb") as f:
        pos = 0
        for off, ln in rs + [(os.path.getsize(img), 0)]:
            f.seek(pos)
            left = off - pos
            while left > 0:
                b = f.read(min(left, 4 << 20))
                h.update(b)
                left -= len(b)
            pos = off + ln
    return h.hexdigest()


def whole_hash(img):
    h = hashlib.sha256()
    with open(img, "rb") as f:
        for b in iter(lambda: f.read(4 << 20), b""):
            h.update(b)
    return h.hexdigest()


def extract(img, ranges, out):
    with open(img, "rb") as f, open(out, "wb") as o:
        for off, ln in ranges:
            f.seek(off)
            o.write(f.read(ln))


def part_table(img, bs):
    """(start, size) in bytes of each partition: GPT (bs) or MBR"""
    with open(img, "rb") as f:
        mbr = f.read(512)
        f.seek(bs)
        h = f.read(92)
        if h[:8] == b"EFI PART":
            ent, n, esz = struct.unpack_from("<QII", h, 72)
            out = {}
            for i in range(n):
                f.seek(ent * bs + i * esz)
                e = f.read(esz)
                if e[:16] == bytes(16):
                    continue
                s, l = struct.unpack_from("<QQ", e, 32)
                out[i + 1] = (s * bs, (l - s + 1) * bs)
            return out
    out = {}
    for i in range(4):
        typ = mbr[446 + 16 * i + 4]
        s, n = struct.unpack_from("<II", mbr, 446 + 16 * i + 8)
        if typ:
            out[i + 1] = (s * 512, n * 512)
    return out


def fs_checks(img, bs, ext4_parts, fat_parts, what):
    parts = part_table(img, bs)
    tmp = img + ".part"
    for num in ext4_parts + fat_parts:
        s, l = parts[num]
        with open(img, "rb") as f, open(tmp, "wb") as o:
            f.seek(s)
            left = l
            while left:
                b = f.read(min(left, 4 << 20))
                o.write(b)
                left -= len(b)
        if num in ext4_parts:
            r = subprocess.run(["e2fsck", "-fn", tmp], capture_output=True, text=True, env=ENV)
        else:
            r = subprocess.run(["fsck.fat", "-n", tmp], capture_output=True, text=True, env=ENV)
        check(r.returncode == 0, "%s: partition %d clean (%s)" % (what, num, "e2fsck" if num in ext4_parts else "fsck.fat"))
        if r.returncode:
            print(r.stdout[-2000:])
    os.unlink(tmp)


def container_checks(img, what, expect_files=()):
    rs = ranges_of(img)
    cont = img + ".container"
    extract(img, rs, cont)
    r = subprocess.run(["fsck.fat", "-n", cont], capture_output=True, text=True, env=ENV)
    check(r.returncode == 0 and "lost" not in r.stdout.lower(), "%s: the container's FAT clean" % what)
    if r.returncode:
        print(r.stdout[-2000:])
    for path, want in expect_files:
        r = subprocess.run(["mtype", "-i", cont, "::" + path], capture_output=True, env=ENV)
        check(r.returncode == 0 and r.stdout.startswith(want), "%s: the container has %s" % (what, path))
    os.unlink(cont)


# --- on the PC ---------------------------------------------------------------

def build_host():
    subprocess.run(["make", "-C", HERE, "-s", "bins"], check=True)


def test_host():
    build_host()
    os.makedirs(OUT, exist_ok=True)
    cases = [("pro1x", {}, 4096, "ufs0", [13, 2], []), ("n950", {"home": "mydocs"}, 512, "mmc0", [2, 4], [1]),
             ("n950", {"home": "leste"}, 512, "mmc0", [2, 4], [1])]
    for kind, kw, bs, name, e4, fat in cases:
        img = os.path.join(OUT, "host-%s-%s.img" % (kind, kw.get("home", "")))
        getattr(mkdisk, kind)(img, **kw)
        before = outside_hash(img)
        label = "host %s %s" % (kind, kw.get("home", ""))
        r = subprocess.run([os.path.join(HERE, "host"), img, str(bs), name, "home", "ro"], capture_output=True, text=True)
        check(r.returncode == 0, label + ": read only, the container found, nothing written")
        if r.returncode:
            print(r.stdout[-3000:])
        check(outside_hash(img) == before, label + ": unchanged")
        r = subprocess.run([os.path.join(HERE, "host"), img, str(bs), name, "home", "rw"], capture_output=True, text=True)
        check(r.returncode == 0, label + ": read-write through FAT, every write inside the container")
        if r.returncode:
            print(r.stdout[-3000:])
        check(outside_hash(img) == before, label + ": nothing changed outside the container")
        fs_checks(img, bs, e4, fat, label)
        container_checks(img, label, [("/hosttest/small.txt", b"hello")])
        os.unlink(img)
    for t in ("shift", "size", "id", "mutable", "uuid"):
        for kind, kw, bs, name in (("pro1x", {}, 4096, "ufs0"), ("n950", {"home": "mydocs"}, 512, "mmc0")):
            img = os.path.join(OUT, "host-tamper.img")
            getattr(mkdisk, kind)(img, tamper=t, **kw)
            before = whole_hash(img)
            r = subprocess.run([os.path.join(HERE, "host"), img, str(bs), name, "nohome", "rw"], capture_output=True, text=True)
            check(r.returncode == 0 and whole_hash(img) == before,
                  "host %s, map tampered (%s): refused, the disk unchanged" % (kind, t))
            if r.returncode:
                print(r.stdout[-2000:])
            os.unlink(img)


def test_ufs():
    build_host()
    os.makedirs(OUT, exist_ok=True)
    img = os.path.join(OUT, "ufs-sim.img")
    mkdisk.pro1x(img)
    before = outside_hash(img)
    r = subprocess.run([os.path.join(HERE, "ufs_sim"), img], capture_output=True, text=True)
    print("\n".join(l for l in r.stdout.splitlines() if "FAIL" in l or "VIOLATION" in l or l.startswith("ufs_sim")))
    check(r.returncode == 0, "UFS model: the link kept, the LUNs, the container written through it")
    check(outside_hash(img) == before, "UFS model: nothing changed outside the container")
    fs_checks(img, 4096, [13, 2], [], "UFS model")
    container_checks(img, "UFS model")
    os.unlink(img)


# --- in QEMU -------------------------------------------------------------------

def make_kernel(platform, autorun="autorun.storage"):
    args = ["make", "-C", TOP, "-j4", "AUTORUN=" + os.path.join(HERE, autorun)]
    if platform != "realview":
        args.append("PLATFORM=" + platform)
    subprocess.run(args, check=True, stdout=subprocess.DEVNULL)


def qemu(platform, name, disk_args, script, disk, home, extra_append="", wait=40, steps=None):
    """one boot: storetest's script on disk (or the autorun's programs,
    driven by steps); its log"""
    out = os.path.join(OUT, name)
    shutil.rmtree(out, ignore_errors=True)
    env = dict(os.environ)
    if platform == "arm64":
        env["QEMU_MACHINE"] = "virt"
        kernel = os.path.join(TOP, "build/arm64/kernel.bin")
        extra = "-global virtio-mmio.force-legacy=false " + disk_args
    else:
        kernel = os.path.join(TOP, "build/realview/kernel.bin")      # the raw image: -append in the ATAGs
        extra = disk_args
    env["QEMU_EXTRA"] = extra
    run = "kolibri.run=/sys/storetest kolibri.args=script,%s,%s" % (script, disk) if script else ""
    # no fake modem: its lines would come between the program's
    env["QEMU_APPEND"] = ("kolibri.home=%s kolibri.nomodem %s %s" % (home, run, extra_append)).strip()
    subprocess.run([sys.executable, os.path.join(TOP, "scripts/qemu-shot.py"), kernel, out] +
                   (steps or ["wait:%d" % wait]), env=env, check=True, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
    return open(os.path.join(out, "serial.log"), "rb").read().decode("utf-8", "replace")


def summary(log, what):
    fails = [l for l in log.splitlines() if "storetest: FAIL" in l]
    for l in fails[:20]:
        print("     " + l.strip())
    done = [l for l in log.splitlines() if "storetest: script" in l]
    ok = bool(done) and not fails and " 0 failed" in done[-1]
    check(ok, what + (": " + done[-1].split("storetest: ")[1].strip() if done else ": no end"))
    return ok


def tree_matches(log, manifest_vol, disk_vol, img, what):
    """storetest tree's F lines for disk_vol against the manifest's for manifest_vol"""
    want = {}
    for l in open(img + ".manifest"):
        vol, rest = l.split(" ", 1)
        if vol == manifest_vol:
            p, size, crc = rest.rsplit(" ", 2)
            want[p] = (int(size), crc.strip())
    # the F lines come before their "tree PATH: ..." line: split by order
    blocks, cur = [], {}
    for l in log.splitlines():
        # the program writes its lines a character at a time: a kernel
        # line may come into one (before its "F ", as seen)
        m = re.search(r"F (.+) (\d+) ([0-9a-f]{8})\s*$", l)
        if m:
            cur[m.group(1)] = (int(m.group(2)), m.group(3))
            continue
        if "storetest: " not in l:
            continue
        l = l.split("storetest: ", 1)[1]
        if l.startswith("tree "):
            blocks.append((l.split()[1].rstrip(":"), cur))
            cur = {}
    for path, files in blocks:
        if path == "/" + disk_vol:
            # the container's own folder (kolibri/) is in the tree too
            files = {p: v for p, v in files.items() if not p.lower().startswith("kolibri/")}
            ok = files == want
            if not ok:
                for p in sorted(set(want) | set(files)):
                    if want.get(p) != files.get(p):
                        print("     %s: want %s, got %s" % (p, want.get(p), files.get(p)))
                        break
            return check(ok, "%s: /%s's %d files as put there" % (what, disk_vol, len(want)))
    return check(False, "%s: no tree of /%s" % (what, disk_vol))


def boots(platform, name, img, bs, disk_args, disk, e4, fat, scripts=("ro", "rw", "keep"), extra_append="",
          vols=(("ufs0/13", "13"), ("ufs0/2", "2"))):
    before = outside_hash(img)
    log = qemu(platform, name + "-ro", disk_args, scripts[0], disk, "ro", extra_append)
    summary(log, "%s: kolibri.home=ro" % name)
    check("home: " in log and "checked" in log, "%s: the container checked" % name)
    for mvol, num in vols:
        tree_matches(log, mvol, "%s/%s" % (disk, num), img, name)
    check(outside_hash(img) == before, "%s: read only: unchanged" % name)
    log = qemu(platform, name + "-rw", disk_args, scripts[1], disk, "rw", extra_append, wait=60)
    summary(log, "%s: kolibri.home=rw, storetest rw on /hd0/1" % name)
    check("outside the container" not in log, "%s: no write refused by the fence" % name)
    log = qemu(platform, name + "-keep", disk_args, scripts[2], disk, "rw", extra_append)
    summary(log, "%s: after the reset, storetest keep" % name)
    check(outside_hash(img) == before, "%s: nothing changed outside the container" % name)
    fs_checks(img, bs, e4, fat, name)
    container_checks(img, name, [("/keep/a.txt", b"persistent hello"), ("/keep/sub/deep/c.txt", b"deep")])


def test_virtio():
    make_kernel("arm64")
    img = os.path.join(OUT, "virtio.img")
    mkdisk.pro1x(img)
    boots("arm64", "virtio", img, 4096,
          "-drive if=none,id=d0,file=%s,format=raw -device virtio-blk-device,drive=d0,logical_block_size=4096,"
          "physical_block_size=4096" % img, "vd0", [13, 2], [])
    os.unlink(img)


def test_ufs_qemu():
    make_kernel("arm64")
    img = os.path.join(OUT, "ufs-qemu.img")
    mkdisk.pro1x(img, fat32=True)
    boots("arm64", "ufs-qemu", img, 4096,
          "-drive if=none,id=u0,file=%s,format=raw -device ufs,serial=kolibri -device ufs-lu,drive=u0,lun=0" % img,
          "ufs0", [13, 2], [], extra_append="kolibri.ufstest=keeplink")
    os.unlink(img)


def test_emmc():
    make_kernel("arm64")
    img = os.path.join(OUT, "emmc.img")
    mkdisk.n950(img, home="mydocs")
    boots("arm64", "emmc", img, 512,
          "-drive if=none,id=e0,file=%s,format=raw -device sdhci-pci -device emmc,drive=e0" % img,
          "mmc0", [2, 4], [1], scripts=("n950-ro", "n950-rw", "n950-keep"),
          vols=(("mmc0/1", "1"), ("mmc0/2", "2"), ("mmc0/4", "4")))
    os.unlink(img)


def test_realview():
    make_kernel("realview")
    img = os.path.join(OUT, "realview.img")
    mkdisk.n950(img, home="leste")
    boots("realview", "realview", img, 512,
          "-drive if=none,id=e0,file=%s,format=raw -device emmc,drive=e0,bus=sd-bus" % img,
          "mmc0", [2, 4], [1], scripts=("n950-ro", "n950-rw", "n950-keep"),
          vols=(("mmc0/1", "1"), ("mmc0/2", "2"), ("mmc0/4", "4")))
    os.unlink(img)


def test_tamper():
    make_kernel("arm64")
    for t in ("shift", "mutable"):
        img = os.path.join(OUT, "tamper.img")
        mkdisk.pro1x(img, tamper=t)
        before = whole_hash(img)
        log = qemu("arm64", "tamper-" + t,
                   "-drive if=none,id=d0,file=%s,format=raw -device virtio-blk-device,drive=d0,logical_block_size=4096,"
                   "physical_block_size=4096" % img, "none", "vd0", "rw")
        check("home: no container" in log, "tamper %s: the container refused" % t)
        check(whole_hash(img) == before, "tamper %s: kolibri.home=rw, storetest rw: the disk unchanged" % t)
        os.unlink(img)


def keys(*a):
    return subprocess.run([sys.executable, os.path.join(TOP, "tests/apps/keys.py"), *a], check=True,
                          capture_output=True, text=True).stdout.split()


def test_apps():
    """kfar and t_edit (realview, the N950's eMMC with the container on
    Leste's partition, kolibri.home=rw): kfar's right panel starts in
    /hd0/1, F5 copies board and example there, F10 ends it (kfar.ini
    written, kept in /hd0/1/sys); t_edit saves a text as /hd0/1/t.txt;
    after a reset storetest finds all of it"""
    img = os.path.join(OUT, "apps.img")
    mkdisk.n950(img, home="leste")
    before = outside_hash(img)
    disk = "-drive if=none,id=e0,file=%s,format=raw -device emmc,drive=e0,bus=sd-bus" % img
    make_kernel("realview", "autorun.kfar")
    qemu("realview", "apps-kfar", disk, None, "mmc0", "rw",
         steps=keys("wait:4", "alt-b", "f5", "wait:1", "ret", "wait:3", "alt-e", "f5", "wait:1", "ret", "wait:3",
                    "f10", "wait:3"))
    make_kernel("realview", "autorun.t_edit")
    qemu("realview", "apps-t_edit", disk, None, "mmc0", "rw",
         steps=keys("wait:5", "persistent text", "wait:1", "ctrl-shift-s", "wait:2", "tab", "ctrl-a", "/hd0/1",
                    "ret", "wait:1", "tab", "ctrl-a", "t.txt", "wait:0.5", "ret", "wait:4"))
    make_kernel("realview")
    log = qemu("realview", "apps-check", disk, "apps", "mmc0", "rw")
    summary(log, "kfar's copies, kfar.ini and t_edit's text in /hd0/1 after a reset")
    check(outside_hash(img) == before, "apps: nothing changed outside the container")
    fs_checks(img, 512, [2, 4], [1], "apps")
    container_checks(img, "apps", [("/t.txt", b"persistent text")])
    os.unlink(img)


def test_messenger():
    """the messenger's chats on the container: the messenger against the
    fake backend (tests/messenger/fakepeer) with kolibri.home=rw; after a
    reset storetest finds its chats.json in /hd0/1/messenger"""
    fp = os.path.join(TOP, "tests/messenger/fakepeer/fakepeer")
    subprocess.run(["make", "-C", TOP, "tests/messenger/fakepeer/fakepeer"], check=True, stdout=subprocess.DEVNULL)
    files = " ".join("bin/%s=%s" % (b, fp) for b in ("yappari-wa", "purple-host", "signal-host"))
    subprocess.run(["make", "-C", TOP, "-j4", "PLATFORM=arm64", "AUTORUN=" + os.path.join(HERE, "autorun.storage"),
                    "LINUX_FILES=" + files], check=True, stdout=subprocess.DEVNULL)
    img = os.path.join(OUT, "messenger.img")
    mkdisk.pro1x(img)
    before = outside_hash(img)
    disk = ("-drive if=none,id=d0,file=%s,format=raw -device virtio-blk-device,drive=d0,logical_block_size=4096,"
            "physical_block_size=4096" % img)
    log = qemu("arm64", "messenger-run", disk, None, "vd0", "rw",
               extra_append="kolibri.run=/sys/messenger kolibri.args=whatsapp", wait=30)
    check("chats kept in /hd0/1/messenger/whatsapp/chats.json" in log, "messenger: its chats written to /hd0/1")
    log = qemu("arm64", "messenger-check", disk, "messenger", "vd0", "rw")
    summary(log, "messenger: chats.json in /hd0/1 after a reset")
    check(outside_hash(img) == before, "messenger: nothing changed outside the container")
    container_checks(img, "messenger", [("/messenger/whatsapp/chats.json", b'["messenger",1,"whatsapp"]')])
    os.unlink(img)


def sudo(*a, check=True):
    return subprocess.run(["sudo", "-n", *a], check=check, capture_output=True, text=True)


def test_helper():
    """tools/kolibri-home itself, as on the phones: a disk image on a loop
    device with its partitions (losetup -P), the partition mounted, the
    container made there with FIEMAP and chattr +i (ext4) or the FAT
    attributes (vfat), its status; then KolibriOS on that disk writes into
    it and keeps it (virtio). Needs sudo without a password; skipped
    otherwise"""
    if subprocess.run(["sudo", "-n", "true"], capture_output=True).returncode:
        print("skip helper: no sudo without a password")
        return
    make_kernel("arm64")
    for fs in ("ext4", "vfat"):
        w = os.path.join(OUT, "helper-" + fs)
        os.makedirs(w, exist_ok=True)
        img, src = w + ".img", os.path.join(w, "tree")
        files = mkdisk.make_tree(src, 9)
        part = w + "/part.img"
        if fs == "ext4":
            mkdisk.ext4_part(part, 96 * mkdisk.MiB, src, label="userdata")
        else:
            mkdisk.fat_part(part, 96 * mkdisk.MiB, src, label="MYDOCS")
        names = ["p%d" % i for i in range(1, 13)]
        parts = [(n, None, mkdisk.MiB) for n in names] + [("userdata", part, 96 * mkdisk.MiB)]
        starts = mkdisk.gpt_disk(img, parts, 512)
        os.unlink(part)
        shutil.rmtree(src)
        dev = sudo("losetup", "-P", "-f", "--show", img).stdout.strip()
        mnt = os.path.join(w, "mnt")
        os.makedirs(mnt, exist_ok=True)
        try:
            sudo("mount", dev + "p13", mnt)
            r = sudo(os.path.join(TOP, "tools/kolibri-home"), "create", "--host", mnt, "--size", "40M", check=False)
            check(r.returncode == 0, "helper %s: kolibri-home create" % fs)
            print("     " + (r.stdout + r.stderr).strip().replace("\n", "\n     "))
            r = sudo(os.path.join(TOP, "tools/kolibri-home"), "status", "--host", mnt, check=False)
            check(r.returncode == 0 and "NOT" not in r.stdout and "MOVED" not in r.stdout,
                  "helper %s: kolibri-home status" % fs)
            print("     " + r.stdout.strip().replace("\n", "\n     "))
            # the container's ranges on the disk, for the checks below
            mp = os.path.join(mnt, "kolibri/kolibri.map")
            m = kh_map(open(mp).read()) if os.path.exists(mp) else None
        finally:
            sudo("umount", mnt, check=False)
            sudo("losetup", "-d", dev, check=False)
        if not m:
            continue
        with open(img + ".ranges", "w") as f:
            for o, l in m["ranges"]:
                f.write("%d %d\n" % (starts["userdata"] + o, l))
        with open(img + ".manifest", "w") as f:
            for p, (size, crc) in sorted(files.items()):
                f.write("vd0/13 %s %d %08x\n" % (p, size, crc & 0xffffffff))
        boots("arm64", "helper-" + fs, img, 512,
              "-drive if=none,id=d0,file=%s,format=raw -device virtio-blk-device,drive=d0" % img,
              "vd0", [13] if fs == "ext4" else [], [] if fs == "ext4" else [13], vols=(("vd0/13", "13"),),
              scripts=("helper-ro", "helper-rw", "keep"))
        os.unlink(img)
        shutil.rmtree(w, ignore_errors=True)


def kh_map(text):
    return mkdisk.kh.parse_map(text)


TESTS = {"host": test_host, "ufs": test_ufs, "virtio": test_virtio, "ufs-qemu": test_ufs_qemu,
         "emmc": test_emmc, "realview": test_realview, "tamper": test_tamper, "apps": test_apps, "messenger": test_messenger,
         "helper": test_helper}


def main():
    names = sys.argv[1:] or list(TESTS)
    os.makedirs(OUT, exist_ok=True)
    st = shutil.disk_usage("/tmp")
    if st.free < (2 << 30):
        raise SystemExit("/tmp: less than 2 GB free")
    for n in names:
        TESTS[n]()
    print("%d failed" % len(failed))
    if not failed:
        shutil.rmtree(OUT, ignore_errors=True)       # kept for a look when something failed
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
