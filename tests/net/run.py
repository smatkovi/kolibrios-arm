#!/usr/bin/env python3
"""lwIP's timers on demand (kernel/net/timers.c) against lwIP's own
(timeouts.c): every scenario of net.c run with both builds, net-kos and
net-lwip, then compared (make -C tests/net).

What must hold:
  - the same frames in the same order, each at the same time give or take
    the timers' grids: TCP's 250 ms (lwIP's TCP timer starts when the
    first connection is made, the kernel's runs on a grid from lwIP's
    start), DHCP's 500 ms fine timer (the kernel's starts its points when
    a request is sent); tcp-bulk, whose ACKs and retransmissions may
    interleave differently at another phase of the grid: the same data,
    the same outcome, as many frames give or take a few;
  - the same events (connected, received, the errors, the connections'
    lists, the address) at the same times, as close;
  - the scenario's own expectations (keepalive probes, persist probes,
    FIN_WAIT_2's and TIME_WAIT's ends, SYN retries, delayed ACKs, the
    lease's renewals, ARP's repeats), checked on both;
  - the kernel's wake-ups by a timer in the idle phases at most a few,
    lwIP's as many as its cyclic timers make."""
import re
import subprocess
import sys

HERE = sys.path[0]


def run(build, scen):
    out = subprocess.run([f'{HERE}/net-{build}', scen], capture_output=True, text=True, check=True).stdout
    frames, events, phases = [], [], {}
    for line in out.splitlines():
        k, t, rest = line.split(' ', 2)
        if k == 'F':
            frames.append((int(t), rest))
        elif k == 'E':
            if rest.startswith(('done', 'timers')):
                continue
            events.append((int(t), rest))
        elif k == 'W':
            name, timer, every = rest.split()
            phases[name] = (int(timer), int(every))
    return frames, events, phases


fails = []


def check(cond, what):
    if not cond:
        fails.append(what)
    return cond


def same_timeline(scen, what, a, b, tol):
    """two lists of (time, text): the same texts in order, times within tol"""
    if not check([x for _, x in a] == [x for _, x in b], f'{scen}: {what} differ'):
        for i, (x, y) in enumerate(zip(a, b)):
            if x[1] != y[1]:
                print(f'  first difference at {i}: kos {x}, lwip {y}')
                break
        return
    worst = max((abs(x[0] - y[0]) for x, y in zip(a, b)), default=0)
    check(worst <= tol, f'{scen}: {what} {worst} ms apart (at most {tol})')


def same_items(scen, what, a, b, tol):
    """the same items, each at the same time within tol, in any order
    among those close together"""
    left = list(b)
    for t, x in a:
        m = next((i for i, (u, y) in enumerate(left) if y == x and abs(u - t) <= tol), None)
        if not check(m is not None, f'{scen}: {what}: kos {t} {x} not in lwIP\'s'):
            return
        left.pop(m)
    check(not left, f'{scen}: {what}: lwIP\'s {left[:3]} not in the kernel\'s')


def times(frames, pat):
    return [t for t, f in frames if re.search(pat, f)]


SCENARIOS = {
    # name: (tolerance of frames' and events' times in ms, phases: the most timer wake-ups of the kernel's)
    'dhcp': (1000, {'dhcp': 60}),
    'dhcp-expire': (1000, {'dhcp': 80}),
    'arp': (10, {'arp': 30}),
    'tcp-syn': (260, {'syn': 40}),
    'tcp-bulk': (260, {'closed': 6}),
    'tcp-delack': (260, {}),
    'tcp-zerowin': (260, {}),
    'tcp-keepalive': (260, {'answered': 15, 'cut': 8}),
    'tcp-idle': (260, {'idle': 4}),
    'tcp-finwait2': (260, {'finwait2': 6}),
    'tcp-timewait': (260, {'timewait': 4}),
}

only = sys.argv[1:]
for scen, (tol, limits) in SCENARIOS.items():
    if only and scen not in only:
        continue
    kf, ke, kw = run('kos', scen)
    lf, le, lw = run('lwip', scen)
    for name, (f, e) in (('kos', (kf, ke)), ('lwip', (lf, le))):
        # each build's own expectations
        tag = f'{scen} ({name})'
        if scen == 'dhcp':
            req = times(f, r'^dhcp-request$')
            check(len(req) == 8 and all(abs((t - 100000) % 300000) <= 1000 or abs((t - 100000) % 300000 - 300000) <= 1000
                                        for t in req), f'{tag}: renewals every 300 s: {req}')
        elif scen == 'dhcp-expire':
            check(times(f, 'dhcp-request broadcast$')[-1:] and times(f, 'dhcp-release'), f'{tag}: rebinding, the lease ended')
            check(any(x == 'address 0.0.0.0' for _, x in e), f'{tag}: the address gone')
        elif scen == 'arp':
            nine = times(f, r'arp-req 10\.0\.0\.9')
            check(len(nine) == 5 and nine[-1] - nine[0] == 4000, f'{tag}: 10.0.0.9 asked 5 times a second apart: {nine}')
        elif scen == 'tcp-syn':
            syn = times(f, r'>81 S ')
            # the first after 2.5 to 3 s: the slow timer's first point after the SYN
            check(len(syn) == 7 and 2500 <= syn[1] - syn[0] <= 3000 and all(b - a == 3000 for a, b in zip(syn[1:], syn[2:])),
                  f'{tag}: SYN every 3 s: {syn}')
            check(any(x == 'client-error -13' for _, x in e), f'{tag}: given up')
        elif scen == 'tcp-bulk':
            check(any(x == 'sent 300000 got 300000' for _, x in e), f'{tag}: all data')
            check(not any(x.startswith('corrupt') for _, x in e), f'{tag}: the data intact')
        elif scen == 'tcp-delack':
            data = times(f, r'^tcp \d+>80 P\.')
            acks = times(f, r'^tcp 80>\d+ \. ')
            check(len(data) == 10 and len(acks) >= 10 and all(any(0 < a - d <= 260 for a in acks) for d in data),
                  f'{tag}: every segment acknowledged within 250 ms: {data} {acks}')
        elif scen == 'tcp-zerowin':
            probes = [t for t, x in f if re.search(r'>80 \. s=\d+ a=1 l=1 ', x)]
            check(len(probes) >= 4, f'{tag}: window probes: {probes}')
            check(any(x.startswith('got 200000') for _, x in e), f'{tag}: all data after the window opened')
        elif scen == 'tcp-keepalive':
            ka = times(f, r'^tcp \d+>80 \. s=0 a=1 l=0')
            cut = next(t for t, x in e if x == 'cut')
            before = [t for t in ka if t < cut]
            after = [t for t in ka if t > cut]
            check(len(before) == 5 and all(20400 <= b - a <= 20600 for a, b in zip(before, before[1:])),
                  f'{tag}: a probe 20 s after the last answer: {before}')
            check(len(after) == 3 and all(4900 <= b - a <= 5100 for a, b in zip(after, after[1:])),
                  f'{tag}: three probes 5 s apart after the cut: {after}')
            end = [t for t, x in e if x == 'client-error -13']
            check(end and 4900 <= end[0] - after[-1] <= 5600, f'{tag}: ended 5 s after the last probe: {end}')
        elif scen == 'tcp-idle':
            check(any(x == 'got 5000' for _, x in e), f'{tag}: data after the idle half hour')
        elif scen == 'tcp-finwait2':
            ack = max(t for t, x in f if x.startswith('tcp 80>') and '.' in x.split()[2])
            end = [t for t, x in e if x == 'client-error -13']
            # 20 s after the connection's last activity (pcb->tmr)
            check(end and 19000 <= end[0] - ack <= 20600, f'{tag}: FIN_WAIT_2 ended 20 s after its ACK: {end} {ack}')
        elif scen == 'tcp-timewait':
            tw = [t for t, x in e if x == 'pcbs active 0 time-wait 1']
            gone = [t for t, x in e if x == 'pcbs active 0 time-wait 0']
            # 2 MSL from the connection's last activity (pcb->tmr), a little before TIME_WAIT
            check(tw and gone and 117000 <= gone[-1] - tw[0] <= 120600, f'{tag}: TIME_WAIT for 2 MSL: {tw} {gone}')
    # the two builds against each other
    if scen == 'tcp-bulk':
        for pat in (r'>80 P\.', r'^tcp 80>\d+ \. ', r'F'):
            a, b = len(times(kf, pat)), len(times(lf, pat))
            check(abs(a - b) <= max(4, a // 20), f'{scen}: frames {pat}: {a} against {b}')
        same_timeline(scen, 'events', [x for x in ke if 'pcbs' not in x[1]], [x for x in le if 'pcbs' not in x[1]], tol)
        same_timeline(scen, 'connections', [x for x in ke if 'pcbs' in x[1]], [x for x in le if 'pcbs' in x[1]], 600)
    elif scen.startswith('dhcp'):
        # timers due in the same millisecond: lwIP's coarse DHCP timer
        # may run before its ARP timer ages the server's entry out, the
        # kernel's counts ARP's second first (net_enter), and then the
        # renewal's or the release's frame waits for ARP's answer, 1 ms:
        # the DHCP frames compared in any order among close ones, ARP's
        # requests as many give or take one a renewal
        same_items(scen, 'DHCP frames', [x for x in kf if x[1].startswith('dhcp')],
                   [x for x in lf if x[1].startswith('dhcp')], tol)
        a, b = len(times(kf, 'arp-req')), len(times(lf, 'arp-req'))
        check(abs(a - b) <= 10, f'{scen}: ARP requests {a} against {b}')
        same_items(scen, 'events', ke, le, tol)
    else:
        same_timeline(scen, 'frames', kf, lf, tol)
        same_timeline(scen, 'events', ke, le, 600 if scen != 'arp' else tol)
    for ph, most in limits.items():
        k, l = kw.get(ph, (None, None)), lw.get(ph, (None, None))
        check(k[0] is not None and k[0] <= most, f'{scen}: {ph}: {k[0]} wake-ups by a timer (at most {most})')
    summary = ', '.join(f'{ph} {kw[ph][0]} against {lw[ph][0]}' for ph in kw)
    print(f'{scen}: {len(kf)} frames{"; wake-ups by a timer " + summary if summary else ""}')

if fails:
    print('FAILED:\n  ' + '\n  '.join(fails))
    sys.exit(1)
print(f'tests/net: all {len(only) or len(SCENARIOS)} scenarios as lwIP\'s own timers, with fewer wake-ups')
