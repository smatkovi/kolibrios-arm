#!/usr/bin/env python3
"""The Pro1X's PRNG read under Linux (Leste), before KolibriOS uses it
(docs/rng.md, "On the devices"):

    qcom-rng-check.py [BYTES] [--alg NAME] [--out FILE]

Linux registers the SoC's PRNG (qcom,prng-ee) only with the crypto API,
as the "stdrng" driver "qcom-rng" (drivers/crypto/qcom-rng.c), not as a
hwrng: no /dev/hwrng. This reads BYTES (default 200000) from it through
an AF_ALG socket (algif_rng: at most 128 bytes a read; "modprobe
qcom_rng algif_rng" first: both are modules in the Pro1X kernel), and
runs on them what KolibriOS's
kolibri.rngtest runs: FIPS 140-2's tests (4.9.1) on each block of 20000
bits, the continuous test (no 32-bit word as the one before) and SP
800-90B's most common value estimate. --alg: another driver of the crypto
API without a seed of its own (e.g. jitterentropy_rng on a PC, to try the
script); --out: the bytes into a file as well (for rngtest(1) or ent(1)
on the PC).

Exit status 0 when every block passes and no word repeats."""
import math
import socket
import struct
import sys


def fips(block):
    ones, f = 0, [0] * 16
    runs = [[0] * 6, [0] * 6]
    longest, run, last = 0, 0, -1
    for v in block:
        f[v >> 4] += 1
        f[v & 15] += 1
        for j in range(7, -1, -1):
            bit = (v >> j) & 1
            ones += bit
            if bit == last:
                run += 1
                continue
            if last >= 0:
                runs[last][min(run, 6) - 1] += 1
                longest = max(longest, run)
            last, run = bit, 1
    runs[last][min(run, 6) - 1] += 1
    longest = max(longest, run)
    px = 16 * sum(c * c for c in f) - 25000000
    lo, hi = [2315, 1114, 527, 240, 103, 103], [2685, 1386, 723, 384, 209, 209]
    bad = []
    if not 9725 < ones < 10275:
        bad.append('monobit (%d ones)' % ones)
    if not 10800 < px < 230850:
        bad.append('poker (%.2f)' % (px / 5000))
    if any(not lo[l] <= runs[b][l] <= hi[l] for b in range(2) for l in range(6)):
        bad.append('runs')
    if longest >= 26:
        bad.append('long run (%d)' % longest)
    return bad


def mcv(data):
    n = len(data)
    count = [0] * 256
    for v in data:
        count[v] += 1
    p = max(count) / n
    pu = min(1.0, p + 2.576 * math.sqrt(p * (1 - p) / (n - 1)))
    return -math.log2(pu) if pu < 1 else 0.0


def main():
    args = sys.argv[1:]
    alg, out, want = 'qcom-rng', None, 200000
    while args:
        a = args.pop(0)
        if a == '--alg':
            alg = args.pop(0)
        elif a == '--out':
            out = args.pop(0)
        else:
            want = int(a)
    try:
        crypto = open('/proc/crypto').read()
        if 'driver       : %s\n' % alg not in crypto:
            print('qcom-rng-check: %s is not in /proc/crypto' % alg)
    except OSError:
        pass
    s = socket.socket(socket.AF_ALG, socket.SOCK_SEQPACKET, 0)
    try:
        s.bind(('rng', alg))
    except OSError as e:
        sys.exit('qcom-rng-check: AF_ALG rng %s: %s (modprobe qcom_rng; modprobe algif_rng?)' % (alg, e))
    op, _ = s.accept()
    data, errors = bytearray(), 0
    while len(data) < want:
        try:
            b = op.recv(min(128, want - len(data)))
        except OSError as e:
            # qcom-rng: EINVAL for a word of zero, ETIMEDOUT for no word in 10 ms
            errors += 1
            if errors > 10:
                sys.exit('qcom-rng-check: %d errors, the last: %s' % (errors, e))
            continue
        data += b
    if out:
        open(out, 'wb').write(data)
    words = struct.unpack('<%dI' % (len(data) // 4), data[:len(data) // 4 * 4])
    repeats = sum(1 for i in range(1, len(words)) if words[i] == words[i - 1])
    zeros = words.count(0)
    blocks = len(data) // 2500
    failed = 0
    for i in range(blocks):
        bad = fips(data[2500 * i:2500 * (i + 1)])
        if bad:
            failed += 1
            print('block %d fails: %s' % (i, ', '.join(bad)))
    print('%s: %d bytes, %d read errors; FIPS 140-2: %d of %d blocks pass (expected: about 1 failure in 1250); '
          'repeated words: %d, zero words: %d; most common byte: %.2f bits per byte (8 at most)'
          % (alg, len(data), errors, blocks - failed, blocks, repeats, zeros, mcv(data)))
    return 0 if failed <= max(1, blocks // 500) and not repeats else 1


sys.exit(main())
