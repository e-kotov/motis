#!/usr/bin/env python3
"""Regression tests for `motis-accessibility stream-decode` (stream version 2) and connector bounds.
Usage: stream_test.py TMPDIR MOTIS_ACCESSIBILITY [--data DATA_DIR]
Standard library only. The version-2 streams are written here by an independent encoder (README layout)."""
import os
import random
import struct
import subprocess
import sys


def header(version, n_dests, n_dep, flags, target=0):
    return b'VRTTMATX' + struct.pack('<6IHBB5I', version, 50, n_dests, n_dep, 0, 50, 120, flags, target, 0, 0, 0, 0, 0)


def streams(rng, n_dests, n_dep, flags, n_rec, fill):
    """Returns (v1 bytes, v2 bytes) for the same records; v2 picks the smaller payload per record."""
    tsz = 2 if flags & 4 else 1
    un = 0xFFFF if tsz == 2 else 0xFF
    cells = n_dests * n_dep
    deps = b''.join(struct.pack('<q', 29927880 + i) for i in range(n_dep))
    v1, v2 = [header(1, n_dests, n_dep, flags) + deps], [header(2, n_dests, n_dep, flags) + deps]
    for o in rng.sample(range(50), n_rec):
        tt = [rng.randrange(0, 300 if tsz == 2 else 121) if rng.random() < fill else un for _ in range(cells)]
        tr = [255 if t == un else rng.randrange(0, 15) for t in tt]
        via = [0xFFFFFFFF if t == un else rng.randrange(0, 1 << 31) for t in tt]
        pack_tt = lambda xs: b''.join(struct.pack('<H' if tsz == 2 else '<B', x) for x in xs)
        dense = pack_tt(tt) + (bytes(tr) if flags & 1 else b'') + (b''.join(struct.pack('<I', x) for x in via) if flags & 2 else b'')
        idx = [i for i, t in enumerate(tt) if t != un]
        bits = bytearray((cells + 7) // 8)
        for i in idx:
            bits[i // 8] |= 1 << (i % 8)
        sp = struct.pack('<I', len(idx)) + bytes(bits) + pack_tt([tt[i] for i in idx]) + \
            (bytes(tr[i] for i in idx) if flags & 1 else b'') + (b''.join(struct.pack('<I', via[i]) for i in idx) if flags & 2 else b'')
        v1.append(struct.pack('<I', o) + dense)
        v2.append(struct.pack('<IB', o, 1) + sp if len(sp) < len(dense) else struct.pack('<IB', o, 0) + dense)
    return b''.join(v1), b''.join(v2)


def run(tool, *args):
    return subprocess.run([tool] + list(args), capture_output=True, text=True)


def main():
    tmp, tool = sys.argv[1], sys.argv[2]
    data = sys.argv[sys.argv.index('--data') + 1] if '--data' in sys.argv else None
    os.makedirs(tmp, exist_ok=True)
    rng = random.Random(20261125)
    fails = 0

    def check(name, ok, detail=''):
        nonlocal fails
        print('%s %s %s' % ('PASS' if ok else 'FAIL', name, detail))
        fails += 0 if ok else 1

    p2, p1, out = os.path.join(tmp, 's2.bin'), os.path.join(tmp, 's1.bin'), os.path.join(tmp, 'd.bin')
    for flags in range(8):
        for fill in (0.0, 0.1, 0.6, 1.0):
            v1, v2 = streams(rng, 37, 3, flags, 6, fill)
            open(p2, 'wb').write(v2)
            r = run(tool, 'stream-decode', '--in', p2, '--out', out)
            check('decode flags=%d fill=%.1f' % (flags, fill), r.returncode == 0 and open(out, 'rb').read() == v1, r.stderr[-200:])
    v1, v2 = streams(rng, 37, 3, 3, 6, 0.1)
    open(p2, 'wb').write(v2)
    r = run(tool, 'stream-decode', '--in', p2, '--out', p2)
    check('refuses --out == --in', r.returncode == 2 and open(p2, 'rb').read() == v2, r.stderr[-200:])
    os.link(p2, os.path.join(tmp, 'hard.bin'))
    r = run(tool, 'stream-decode', '--in', p2, '--out', os.path.join(tmp, 'hard.bin'))
    check('refuses a hard link to --in', r.returncode == 2 and open(p2, 'rb').read() == v2, r.stderr[-200:])
    os.remove(os.path.join(tmp, 'hard.bin'))
    sentinel = b'unrelated file'
    open(out + '.partial', 'wb').write(sentinel)
    leftovers = lambda: [f for f in os.listdir(tmp) if f.startswith('d.bin.partial-')]
    for name, bad in (('truncated', v2[:-1]), ('trailing byte', v2 + b'\0'), ('unknown tag', v2[:56 + 24 + 4] + b'\7' + v2[56 + 24 + 5:]),
                      ('huge destination count', header(2, 1 << 31, 1, 0) + struct.pack('<q', 0) + struct.pack('<IB', 1, 0) + b'\0' * 16),
                      ('huge bitmap', header(2, 1 << 31, 1, 0) + struct.pack('<q', 0) + struct.pack('<IBI', 1, 1, 0) + b'\0' * 16)):
        if os.path.exists(out):
            os.remove(out)
        open(p2, 'wb').write(bad)
        r = run(tool, 'stream-decode', '--in', p2, '--out', out)
        check('rejects %s, writes no output' % name, r.returncode == 2 and not os.path.exists(out) and not leftovers(), r.stderr[-200:])
    readp = lambda: open(out + '.partial', 'rb').read() if os.path.exists(out + '.partial') else None
    check('leaves an existing OUT.partial untouched', readp() == sentinel)
    open(p2, 'wb').write(v2)
    r = run(tool, 'stream-decode', '--in', p2, '--out', out)
    check('decodes next to an existing OUT.partial', r.returncode == 0 and open(out, 'rb').read() == v1 and readp() == sentinel and not leftovers(), r.stderr[-200:])
    os.remove(out)
    open(p2, 'wb').write(header(2, 0, 1 << 29, 0))
    r = run(tool, 'stream-decode', '--in', p2, '--out', out)
    check('rejects a departure count larger than the file', r.returncode == 2 and not os.path.exists(out), r.stderr[-200:])
    open(p1, 'wb').write(v1)
    r = run(tool, 'r5r-reduce', '--stream', p2, '--r5r-out', os.path.join(tmp, 'r5r'))
    check('r5r-reduce rejects version 2', r.returncode == 2 and 'stream-decode' in r.stderr, r.stderr[-200:])
    if data:
        c = os.path.join(tmp, 'access.csv')
        open(c, 'w').write('point,stop,minutes\n0,0,1441\n')
        r = run(tool, 'route', '--data', data, '--n-origins', '1', '--n-dests', '1', '--access', c, '--egress', c,
                '--dep-start-unix-minutes', '29927850', '--max-travel-minutes', '120', '--discard', '--out', os.path.join(tmp, 'r.bin'))
        check('route rejects connector minutes > 1440', r.returncode == 2 and 'connector minutes' in r.stderr, r.stderr[-300:])
    print('FAILURES %d' % fails)
    sys.exit(1 if fails else 0)


if __name__ == '__main__':
    main()
