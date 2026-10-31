# Copyright 2026 Combust.
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Runs the live serial B bench N times (default 3) on patch 2's take, at the
# volume floor, and prints each engine's averages and peaks side by side.
#   python3 tools/bench.py [runs]
import serial, time, re, sys, collections
runs = int(sys.argv[1]) if len(sys.argv) > 1 else 3
s = serial.Serial(); s.port='/dev/ttyACM0'; s.timeout=0.1; s.write_timeout=1; s.dtr=True; s.rts=True; s.open(); time.sleep(0.5)
for _ in range(40): s.write(b'['); time.sleep(0.01)
s.write(b'K20+'); time.sleep(0.03); s.write(b'K02+'); time.sleep(0.05); s.write(b'K02-'); time.sleep(0.03); s.write(b'K20-'); time.sleep(3)
res = collections.defaultdict(list)
for r in range(runs):
    s.read(65536); s.write(b'B'); t = time.time(); buf = b''
    while time.time() - t < 14: buf += s.read(65536)
    for e, a, p, x in re.findall(rb'bench engine=([A-Z ]+) voices=\d+ avg=([\d.]+)% peak=([\d.]+)% xrun=\+(\d+)', buf):
        res[e.decode()].append((float(a), float(p), int(x)))
for e, v in res.items():
    print('%-10s avg %s  peak %s  xrun %s' % (e, ' '.join('%5.1f' % a for a, _, _ in v), ' '.join('%5.1f' % p for _, p, _ in v), sum(x for *_, x in v)))
s.write(b'i'); time.sleep(0.5); m = re.findall(rb'heap=\S+', s.read(65536)); print('heap', m[-1].decode() if m else '?')
