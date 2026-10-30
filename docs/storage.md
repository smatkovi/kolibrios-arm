# Storage: the phones' UFS and eMMC, read only; a container written

KolibriOS for ARM reads the internal storage of the F(x)tec Pro1X (UFS)
and the Nokia N950 (eMMC): their partitions, the ext2/3/4 and FAT file
systems on them, through function 70 as KolibriOS's partitions
(`/ufs0/13/...`, `/mmc0/1/...`). It writes nothing there, with one
exception: a container file prepared from Linux, `kolibri/kolibri.img`,
with a FAT file system inside, shown as `/hd0/1` and kept across resets
(the settings, the messenger's chats, t_edit's and kfar's files).

The internal storage holds every other system of the phones (Leste,
Sailfish, FreeBSD, Arch, MeeGo, Harmattan, the firmware). A write bug in
a new kernel must not be able to touch them: the layers below are built
so that it cannot (the fence, below), and nothing at all is written
unless the container was found, checked and `kolibri.home=rw` given.

## The parts

| File | What |
| --- | --- |
| `kernel/storage/storage.h` | the interfaces: disks, volumes, file systems, the fence |
| `kernel/storage/blk.c` | the block layer: the disks, their partitions (GPT; MBR with its logical partitions; a disk without a table as one volume), a block cache (4 KiB pages, write through), the fence |
| `kernel/storage/ext4.c` | ext2, ext3, ext4, read only (no write code at all) |
| `kernel/storage/fat.c` | FAT12, FAT16, FAT32 with long names, read and write |
| `kernel/storage/home.c` | the container: found, checked against the file system's own metadata, mounted |
| `kernel/storage/dev.c` | the command line, DMA memory, the start (`main.c`) |
| `kernel/storage/ufs.c` | UFS host controllers (UFSHCI 2.0+): the Pro1X's on the link Linux left up; QEMU's |
| `kernel/storage/mmc.c` | MMC, eMMC, SD cards |
| `kernel/storage/sdhci.c` | SDHCI-like hosts: the N950's OMAP3 HSMMC (MMC2), QEMU's sdhci-pci |
| `kernel/storage/pl181.c` | ARM's PL181 (QEMU realview's card slot: the ARMv7 kernel's tests) |
| `kernel/storage/virtio_blk.c`, `pci.c` | QEMU virt's virtio-blk disks and PCI Express (its UFS and SDHCI) |
| `tools/kolibri-home` | on Linux: the container made, checked, removed, mounted |
| `apps/storetest` | the test program (function 70 on the volumes) |
| `tests/storage` | the tests: on the PC and in QEMU |

The ext4 and FAT code is new C, written from the on-disk formats, not a
translation of KolibriOS's `kernel/trunk/fs` (FASM); the behaviour seen
through function 70 is KolibriOS's (its error codes, attributes, BDFE).

## Paths

| Path | What | Written |
| --- | --- | --- |
| `/ufs0` ... `/ufs7` | the Pro1X's UFS logical units by LUN (`ufs0` is Linux's `sda`) | never |
| `/ufs0/13` | LUN 0's 13th GPT entry, Linux's `sda13`: `userdata` (ext4) | never |
| `/mmc0/1`, `/mmc0/2`, `/mmc0/4` | the N950's eMMC by its MBR: MyDocs (FAT32), Harmattan's root, Leste's (ext4) | never |
| `/vd0/...` | QEMU's virtio-blk disks (tests) | never |
| `/hd0/1` | the container (FAT): KolibriOS's own hard disk, as in KolibriOS | with `kolibri.home=rw` |

`/` lists the disks that have a partition with a known file system, a
disk lists those partitions (its numbers), `/hd0` the container's `1`.
The partitions keep the numbers Linux gives them (GPT entries, MBR 1-4,
logical ones from 5). Names: ext4's as they are (case sensitive), FAT's
without regard to case (long names in UTF-8, short ones with their case
flags). A symbolic link on ext4 shows as a file whose contents are its
target; it is not followed. Every subfunction of function 70 that writes
returns 10 (access denied) on the disks' volumes, and on `/hd0/1` unless
it is writable. Programs start from any of them (70.7).

## The container

`tools/kolibri-home` makes it on Linux (as root, KolibriOS not running;
a Python 3 script that needs `mkfs.fat` (dosfstools) and `chattr`, copied
to the phone: `scp tools/kolibri-home root@PHONE:/usr/local/sbin/`):

```sh
kolibri-home create --host /host --size 512M      # the Pro1X: userdata at /host
kolibri-home create --host / --size 256M          # the N950 under Leste: p4
kolibri-home create --host /home/user/MyDocs      # or MyDocs (FAT)
kolibri-home status --host /host
kolibri-home remove --host /host
kolibri-home mount /mnt --host /host [--rw]      # its files from Linux; umount
```

What `create` does, after the FreeBSD port's writable pool
(pro1x-maemo-leste's `tools/pro1x-freebsd-pool`):

1. `HOST` must be the root of an ext4 or FAT file system on a partition
   of the internal storage (no loop image, no device mapper: KolibriOS
   sees only partitions).
2. `HOST/kolibri/kolibri.img` is written in full with zeros (no holes,
   no unwritten extents: Linux would read zeros there whatever KolibriOS
   wrote; `fallocate` alone would leave exactly that).
3. A FAT file system inside (`mkfs.fat -S 4096 -s 1 -R 32`: 4096-byte
   sectors and clusters, the disk's blocks; FAT32 from 260 MiB, FAT16
   below), the volume label KOLIBRI, its serial the UUID's first word.
4. The header at byte 65536 (its 17th reserved sector): `KOLIBRI-HOME 1`,
   a random UUID, the size, a CRC.
5. Its blocks on the partition by FIEMAP (every extent plain and written),
   into `HOST/kolibri/kolibri.map` (written atomically):

       kolibri-home-map 1
       fs ext4 id 1234567 size 536870912 uuid 5f0c...
       65536000 134217728
       ...                                   (bytes on the partition)

   `id` is the inode (FAT: the first cluster), at most 1024 ranges
   (ext4 with `e4defrag` first when there are more).
6. ext4: `chattr +i` (nothing on Linux writes, renames, deletes or moves
   it; e4defrag leaves it alone). FAT: read only and system.

KolibriOS, at the start, with every file system only read (`home.c`):

1. On every ext4 and FAT volume it found: `kolibri/kolibri.map`, parsed.
2. `kolibri/kolibri.img` looked up by the file system itself, and its
   place as the file system's own metadata gives it: ext4's extent tree
   (every extent initialised, no hole, every block marked used in its
   group's bitmap, none on a group's superblock, descriptors, bitmaps or
   inode table; the journal must not need recovery, which KolibriOS does
   not do), FAT's cluster chain.
3. That place must be the map's, range for range; the inode (first
   cluster) and the size the map's; the file immutable (ext4), read only
   and system (FAT).
4. The header at 65536 of its first range: the magic, the map's UUID and
   size. Then the FAT inside is mounted.

Any mismatch is said on the console (`home: ufs0/13: ...`) and the
container is not used; nothing is written. Found and checked, it is
`/hd0/1`, read only by default.

### The fence

With `kolibri.home=rw` the container's ranges, in the disk's blocks,
become the fence's (`blk.c`), and the container's volume is the only one
`vol_wr` writes, through them. The fence is what decides:

- A driver hands its write entry to the block layer once (`blk_add`);
  it is kept in `blk.c` and called from one function only, which checks
  every block range against the fence first. Every other volume, and the
  file systems on them (ext4 has no write code; FAT's is refused below it
  as well), cannot reach it.
- The drivers ask the fence again themselves before they send a write
  command (`ufs.c`'s WRITE(16), `mmc.c`'s CMD24/CMD25, virtio-blk's OUT).
- Without `kolibri.home=rw` the drivers get no write entry at all.

The UFS driver sends nothing that changes the device but WRITE(16) inside
the fence: TEST UNIT READY, INQUIRY, READ CAPACITY(16), READ(16); no UIC
command, no reset, no query on the Pro1X. The eMMC is started anew (CMD0,
its identification, the bus width with CMD6: a setting the card forgets
when its power goes), then CMD17/18 and, inside the fence, CMD24/25.

### What is kept there

| What | Where |
| --- | --- |
| the ramdisk's settings (`/sys/*.ini`, `/sys/settings/...`: kfar's panel modes, t_edit's) | written to `/hd0/1/sys/...` as they are written, put over the ramdisk's at the start |
| the messenger's chats | `/hd0/1/messenger/SERVICE/chats.json` (else the Linux tree's RAM) |
| kfar | its right panel starts in `/hd0/1` (the original's), so F5 copies there |
| t_edit, the shell, every program | any file under `/hd0/1` |

The backends' own data (Yappari's, Signal's, Telegram's sessions in the
Linux personality's `/home/user`) is still in RAM: not yet kept, so a
messenger has to be linked again after a reset, its chats are there.

## The command line

| | |
| --- | --- |
| `kolibri.storage=off` | no storage driver touches its controller |
| `kolibri.home=off` | no container looked for |
| `kolibri.home=ro` | (the default) the container checked, `/hd0/1` read only: nothing is written |
| `kolibri.home=rw` | the container writable, its blocks the only ones written |

## The Pro1X: UFS

Linux, starting KolibriOS with kexec, leaves the UFS controller running
with the link up and the device initialised (pro1x-maemo-leste's kernel
patch 0004 keeps it powered across kexec; Qualcomm's PHY, clocks and
regulators need drivers this kernel does not have). `ufs.c` takes it
over as FreeBSD's port does (`freebsd/patches/0005`): HCE and the
device present required, its interrupts off, the request lists stopped,
pointed at our memory (uncached, below 4 GiB) and started again, the link
and its power mode and Auto-Hibernate left as they are. The controller is
behind the SMMU (stream 0x100); its context bank's translation is turned
off (SCTLR 0, as for the display): the controller then reaches physical
addresses. The boot menu's identity rebind (done for FreeBSD) is not
needed. LUNs 0-7 are asked (TEST UNIT READY, a UNIT ATTENTION retried),
the blocks are 4096 bytes; userdata is found by its GPT name, the 13th
entry of LUN 0 (Linux's 472583832 512-byte sectors are 59072979 blocks).

## The N950: eMMC

MMC2 (0x480b4000, an OMAP3 HSMMC, 4 bits wide). ubiboot's Linux 2.6.32
puts the eMMC to sleep (CMD5) after a second without requests and turns
its supply (VEMMC, GPIO 157) off after eight more, so the kernel switches
MMC2's clocks and VEMMC on, resets the controller (1.8 V, 400 kHz, the
initialisation stream), and starts the card from CMD0 (open drain for
its identification, as Linux does), then 4 bits at 24 MHz. MyDocs is
`/mmc0/1` (FAT32), Harmattan's root `/mmc0/2`, Leste's `/mmc0/4` (ext4).

## Tests

```sh
make -C tests/storage              # all of run.py's: host ufs virtio ufs-qemu emmc realview tamper apps messenger helper
make -C tests/storage host ufs     # on the PC only
make -C tests/storage ext4-test fat-test    # the file systems' own tests
python3 tests/storage/run.py emmc  # one of them
```

`tests/storage/mkdisk.py` makes disks like the phones' without root: a
GPT disk of 4096-byte blocks with `userdata` as entry 13 (ext4, a tree
of files, the container in two pieces placed with debugfs, immutable,
its map; FAT32 inside with `--fat32`, in three pieces), an ext4
`persist`; an MBR disk of 512-byte blocks with MyDocs
(FAT32), Harmattan's and Leste's partitions (ext4 with Harmattan's
features), the container on MyDocs or on Leste's. `run.py` then:

- **host**: the block layer, the file systems and the container's checks
  on the PC: read only (found, nothing written), read-write (FAT through
  the fence; every write the disk sees inside the container's ranges;
  the fence's edges: the block before and after each range, across its
  ends, two ranges at once, another disk), maps made wrong (a range moved
  by 4 KiB, the size, the inode, not immutable, another UUID): refused,
  the disk unchanged.
- **ufs**: `ufs.c` against a model of a UFS controller (`ufs_sim.c`) left
  as Linux leaves the Pro1X's: no UIC command, no HCE write, no
  Auto-Hibernate change, no list moved while it runs, only the five SCSI
  commands; the LUNs, a UNIT ATTENTION, the container written through it.
- **virtio**, **ufs-qemu**, **emmc**, **realview**: the kernel in QEMU on
  those disks (virtio-blk with 4096-byte blocks; QEMU's UFS model, first
  left running as Linux leaves it, `kolibri.ufstest=keeplink`; QEMU's
  eMMC on sdhci-pci; the ARMv7 kernel with QEMU's eMMC on realview's
  PL181), booted three times on the same image: `kolibri.home=ro`
  (storetest lists every volume, each file's size and CRC compared with
  what mkdisk.py put there; every writing subfunction refused),
  `kolibri.home=rw` (storetest rw on `/hd0/1`: files of 0 bytes to 3 MiB,
  long and UTF-8 names, gaps, cutting and growing, renaming, moving
  folders, deleting, 200 files in a folder, a program started from
  there), and again (storetest keep: what rw left is there). Then on the
  host: the image unchanged outside the container's ranges (sha256),
  `e2fsck -fn` and `fsck.fat -n` clean on every partition and on the
  container, the files in the container read with mtools.
- **tamper**: a map moved by 4 KiB, a container that is not immutable:
  refused with `kolibri.home=rw`, the image unchanged to the byte.
- **apps**: kfar (its right panel in `/hd0/1`, F5 copies there, F10 writes
  `kfar.ini`, kept in `/hd0/1/sys`) and t_edit (a text saved as
  `/hd0/1/t.txt` through its dialog), on the ARMv7 kernel with the N950's
  disk; after a reset storetest finds all of it.
- **messenger**: the messenger against the fake backend
  (`tests/messenger/fakepeer`) with `kolibri.home=rw`; after a reset its
  `chats.json` is in `/hd0/1/messenger/whatsapp`.
- **helper**: `tools/kolibri-home` itself, as on the phones (needs `sudo`
  without a password, skipped otherwise): a disk image on a loop device
  with its partitions, `kolibri-home create` and `status` on its ext4 and
  on its vfat partition (FIEMAP, `chattr +i`, the FAT attributes), then
  the kernel on that disk, read only, read-write, after a reset.

The ext4 and FAT drivers have their own tests on the PC (`ext4_test.c`,
`fat_test.c` with `fat_test.py`): images made by `mkfs.ext2/3/4 -d`
(64bit, metadata_csum, Harmattan's features, 1 and 4 KiB blocks, inline
data, htree folders, a 300 GB sparse volume) and by `mkfs.fat` with
mtools (FAT12/16/32, 512 and 4096-byte sectors), every file and folder
compared, `map()` against debugfs's and the FAT's view, corrupted images
(no crash, no hang); FAT's writes checked with `fsck.fat` and mtools, a
random stress test against a model, the disk full, and writes cut off at
every point (a reset) followed by `fsck.fat`.

## On the devices (from 2026-10-31)

Nothing of this has run on the phones yet. In steps, so that the first
runs write nothing at all:

### The Pro1X

1. **Read only, no container** (nothing written: there is no container,
   and `kolibri.home=ro` is the default):

       tools/pro1x-kexec.sh /host/kolibri/kernel.bin "kolibri.lifetime=90 kolibri.run=/sys/board"

   Back in Linux: `tools/pro1x-read.py` (the RAM console). That nothing
   was written: every `blk: ufs0..` line ends in `read only` (no driver
   got a write entry), no `write` line of `blk:` or `ufs:`, `home: no
   container: nothing on the storage is written`. What else to look
   for: `pro1x: stream 0x100: context bank N, SCTLR 0x... -> 0`, `ufs:
   UFSHCI 3.0, ... HCE 1, HCS 0x..f` (the device present, the lists
   ready), `ufs: keeping the link that is up`, six `blk: ufs0..ufs5`
   lines with 4096-byte blocks, `blk: ufs0: GPT, N partitions`, `blk:
   ufs0/13 (userdata): ext4, 230753 MiB` and its features line, `home: no
   container`. If a `kolibri.lifetime` run hangs at the UFS (the
   watchdog's reset, the console ending before `ufs:`), `kolibri.storage=off`
   is the way around it until it is understood.
2. **The listing**: the same with `kolibri.run=/sys/kfar` (or the shell:
   `ls /ufs0/13`): userdata's folders (`maemo-leste.img`, `halium-rootfs`,
   `freebsd`, ...), a file read (F3 on a text file). Compare with `ls
   /host` on Leste.
3. **The container, read only**: on Leste, `tools/kolibri-home create
   --host /host --size 512M` (writes 512 MB to userdata: only with the
   user's go), `kolibri-home status`. Then KolibriOS again without
   `kolibri.home`: `home: ufs0/13's kolibri/kolibri.img checked: 512 MiB
   in N range(s), hd0/1 read only`. If `home:` refuses it with ext4's
   reason that the journal needs recovery or the file system is not
   clean (`ext4 ufs0/13: no map: ...`), Leste did not leave userdata
   cleanly (`pro1x-kexec.sh`'s emergency remount): start KolibriOS from
   the boot menu's entry instead, which unmounts userdata first. The
   listing works either way.
4. **Writing**: `kolibri.home=rw` (in `/host/kolibri/cmdline` for the
   boot menu): kfar, F5 from `/sys` into `/hd0/1`, a text saved with
   t_edit. Back on Leste: `kolibri-home status` (blocks unchanged),
   `e2fsck -fn` is not possible on the mounted userdata, but `kolibri-home
   mount /mnt` shows the files; `fsck.fat -n` on the loop device.

### The N950

1. **Read only** (ubiboot's "KolibriOS (test)" line, `docs/n950.md`):
   the debug board (`board`) shows `n950: VEMMC was on` (or switched on),
   `blk: mmc0: eMMC ..., 15160 MiB`, `blk: mmc0: MBR, 3 partitions`
   (four with p4), `mmc0/1: fat`, `mmc0/2: ext4`, `mmc0/4: ext4`, `home:
   no container`. kfar's Alt+F1 lists `mmc0`.
2. **The container** on Leste's p4 (`kolibri-home create --host /
   --size 256M` under Leste) or on MyDocs (`--host /home/user/MyDocs`),
   then read only, then `kolibri.home=rw` in Leste's `/boot/cmdline`
   (ubiboot hands it to KolibriOS).

`docs/n950.md`'s warning holds here too: the turn of the screen and the
input are untried; a hang at the eMMC shows as a black screen with the
debug board's last line; `kolibri.storage=off` leaves the eMMC alone.
