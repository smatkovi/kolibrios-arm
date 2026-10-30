#!/usr/bin/env python3
"""The random number generator's reference (docs/rng.md), against the
kernel's code built for the PC (tests/rng/test dump):

  - ChaCha20 here, checked against OpenSSL's (openssl enc -chacha20) for
    random keys, nonces and counters
  - the pool (SHA-256), the reseed (HMAC-SHA256), the stir before the
    seed, the requests with fast key erasure: the same sequence as
    test.c's dump, byte for byte
  - FIPS 140-2's statistics of three blocks, SP 800-90B's most common
    value estimate (in floating point: within 2/256 bit), log2

    model.py ./test
"""
import hashlib
import hmac
import math
import os
import struct
import subprocess
import sys

fails = 0


def check(ok, what):
    global fails
    print(('ok: ' if ok else 'FAIL: ') + what)
    if not ok:
        fails += 1


def rotl(v, n):
    return ((v << n) & 0xffffffff) | (v >> (32 - n))


def chacha20_block(key, counter, nonce):
    s = [0x61707865, 0x3320646e, 0x79622d32, 0x6b206574] + list(struct.unpack('<8I', key)) + \
        [counter] + list(struct.unpack('<3I', nonce))
    x = s[:]

    def qr(a, b, c, d):
        x[a] = (x[a] + x[b]) & 0xffffffff; x[d] = rotl(x[d] ^ x[a], 16)
        x[c] = (x[c] + x[d]) & 0xffffffff; x[b] = rotl(x[b] ^ x[c], 12)
        x[a] = (x[a] + x[b]) & 0xffffffff; x[d] = rotl(x[d] ^ x[a], 8)
        x[c] = (x[c] + x[d]) & 0xffffffff; x[b] = rotl(x[b] ^ x[c], 7)
    for _ in range(10):
        qr(0, 4, 8, 12); qr(1, 5, 9, 13); qr(2, 6, 10, 14); qr(3, 7, 11, 15)
        qr(0, 5, 10, 15); qr(1, 6, 11, 12); qr(2, 7, 8, 13); qr(3, 4, 9, 14)
    return struct.pack('<16I', *[(x[i] + s[i]) & 0xffffffff for i in range(16)])


def openssl_keystream(key, counter, nonce, n):
    iv = struct.pack('<I', counter) + nonce
    r = subprocess.run(['openssl', 'enc', '-chacha20', '-K', key.hex(), '-iv', iv.hex()],
                       input=bytes(n), capture_output=True, check=True)
    return r.stdout


class Rng:
    RESEED = b'KolibriOS for ARM rng reseed'
    STIR = b'KolibriOS for ARM rng before the seed'

    def __init__(self):
        self.pool = hashlib.sha256()
        self.bits = 0
        self.key = bytes(32)
        self.reseeds = 0

    def mix(self, data, bits):
        self.pool.update(data)
        self.bits += bits

    def stir(self):
        h = self.pool.copy().digest()
        self.key = hmac.new(self.key, self.STIR + h, hashlib.sha256).digest()

    def reseed(self):
        if self.bits < 256:
            return False
        h = self.pool.digest()
        self.key = hmac.new(self.key, self.RESEED + h + struct.pack('<Q', self.reseeds), hashlib.sha256).digest()
        self.pool = hashlib.sha256(h)
        self.bits = 0
        self.reseeds += 1
        return True

    def generate(self, n):
        b = chacha20_block(self.key, 0, bytes(12))
        self.key, k = b[:32], b[32:]
        out = b''
        ctr = 0
        while len(out) < n:
            out += chacha20_block(k, ctr, bytes(12))
            ctr += 1
        return out[:n]


def fips(block):
    bits = ''.join(format(v, '08b') for v in block)
    ones = bits.count('1')
    f = [0] * 16
    for v in block:
        f[v >> 4] += 1
        f[v & 15] += 1
    px = 16 * sum(c * c for c in f) - 25000000
    runs = [[0] * 6, [0] * 6]
    longest = 0
    i = 0
    while i < len(bits):
        j = i
        while j < len(bits) and bits[j] == bits[i]:
            j += 1
        n = j - i
        runs[int(bits[i])][min(n, 6) - 1] += 1
        longest = max(longest, n)
        i = j
    lo = [2315, 1114, 527, 240, 103, 103]
    hi = [2685, 1386, 723, 384, 209, 209]
    fail = 0
    if not 9725 < ones < 10275:
        fail |= 1
    if not 10800 < px < 230850:
        fail |= 2
    if any(not lo[l] <= runs[b][l] <= hi[l] for b in range(2) for l in range(6)):
        fail |= 4
    if longest >= 26:
        fail |= 8
    return [fail, ones, max(px, 0), longest] + runs[0] + runs[1]


def mcv(samples):
    n = len(samples)
    p = max(samples.count(v) for v in set(samples)) / n
    pu = min(1.0, p + 2.576 * math.sqrt(p * (1 - p) / (n - 1)))
    return -math.log2(pu) if pu < 1 else 0.0


def main():
    test = sys.argv[1] if len(sys.argv) > 1 else './test'
    # ChaCha20 against OpenSSL
    bad = 0
    for i in range(20):
        key, nonce, ctr = os.urandom(32), os.urandom(12), struct.unpack('<I', os.urandom(4))[0] & 0xfffffff
        if chacha20_block(key, ctr, nonce) != openssl_keystream(key, ctr, nonce, 64):
            bad += 1
    check(bad == 0, 'ChaCha20 (this model) as OpenSSL\'s for 20 random keys, nonces, counters')

    out = subprocess.run([test, 'dump'], capture_output=True, text=True, check=True).stdout.split('\n')
    lines = [l.split(' ', 1) for l in out if l]
    got = {}
    for tag, val in lines:
        got.setdefault(tag, []).append(val)

    r = Rng()
    r.mix(b"the boot loader's seed", 128)
    r.mix(b'virtio-rng', 128)
    r.stir()
    check(got['stir'][0] == r.generate(40).hex(), 'before the seed: the stirred key\'s output')
    r.reseed()
    check(got['key1'][0] == r.key.hex(), 'the first seed\'s key (HMAC-SHA256 over the pool)')
    check(got['gen1'][0] == r.generate(1).hex(), 'a request of 1 byte')
    check(got['gen64'][0] == r.generate(64).hex(), 'a request of 64 bytes')
    check(got['gen130'][0] == r.generate(130).hex(), 'a request of 130 bytes (three blocks)')
    r.mix(b'a minute later', 300)
    r.reseed()
    check(got['key2'][0] == r.key.hex(), 'the second reseed\'s key (the pool chained, the counter)')
    check(got['gen33'][0] == r.generate(33).hex(), 'a request after it')
    for i in range(3):
        block = r.generate(2500)
        check(got['fipsblock'][i] == block.hex(), 'FIPS block %d: the same bytes' % i)
        want = fips(block)
        have = [int(v) for v in got['fips'][i].split()]
        check(have == want, 'FIPS 140-2 statistics of block %d as here: %s' % (i, have[:4]))
    for i in range(4):
        m = bytes.fromhex(got['mcvsamples'][i])
        have = int(got['mcv'][i]) / 256
        want = mcv(list(m))
        check(abs(have - want) <= 2 / 256, 'MCV estimate %d: %.3f bits, here %.3f' % (i, have, want))
    for v in got['log2']:
        x, q = (int(t) for t in v.split())
        check(q == math.floor(math.log2(x) * 256) or q == math.floor(math.log2(x) * 256) - 1,
              'log2(%d) * 256: %d (here %.2f)' % (x, q, math.log2(x) * 256))
    print('model: %s' % ('all as here' if not fails else '%d differ' % fails))
    return 1 if fails else 0


sys.exit(main())
