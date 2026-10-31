# Copyright 2026 Combust.
# SPDX-License-Identifier: AGPL-3.0-or-later
#
"""Overnight soak for PocketDynamo: random play over serial, logged.

Plays notes, chords, strips, engines, surfaces, dice, echo, tilt routing,
the looper, mic takes and MIDI (notes, CCs, bend, program, clock) at random.
Never touches the SD card (no FN+digits, no SHIFT) or the volume. Every
minute it logs the state line; every 10 minutes it releases everything and
checks the voices drain to 0 (hung notes); every hour it runs the bench.
A reboot or a lost port is logged, and the volume goes back to the floor
before play resumes.

  python3 -u soak.py <hours> <logfile>
"""
import random, re, serial, sys, time

HOURS = float(sys.argv[1]) if len(sys.argv) > 1 else 6.0
LOG = open(sys.argv[2] if len(sys.argv) > 2 else 'soak.log', 'a', buffering=1)
T0 = time.time()
rng = random.Random(20261030)


def log(msg):
    LOG.write('[%7.1f min] %s\n' % ((time.time() - T0) / 60, msg))


class Dev:
    def __init__(self):
        self.s = None
        self.buf = b''
        self.last_up = None
        self.connect()

    def connect(self):
        while True:
            try:
                s = serial.Serial(); s.port = '/dev/ttyACM0'; s.baudrate = 115200
                s.timeout = 0.02; s.write_timeout = 1; s.dtr = True; s.rts = True; s.open()
                self.s = s
                time.sleep(0.5)
                self.mute()
                return
            except Exception:
                time.sleep(2)

    def mute(self):
        for _ in range(40):
            self.raw(b'[')
            time.sleep(0.01)

    def raw(self, b):
        try:
            self.s.write(b)
        except Exception as e:
            log('PORT LOST on write (%s): reconnecting' % type(e).__name__)
            self.reconnect()

    def reconnect(self):
        try: self.s.close()
        except Exception: pass
        time.sleep(3)
        self.connect()
        log('reconnected, muted')

    def read(self):
        try:
            d = self.s.read(65536)
        except Exception as e:
            log('PORT LOST on read (%s): reconnecting' % type(e).__name__)
            self.reconnect()
            return
        if d:
            self.buf = (self.buf + d)[-20000:]
            if b'Guru' in d or b'abort()' in d or b'Backtrace' in d:
                log('CRASH TEXT: %r' % d[-400:])

    def state(self):
        self.read(); self.buf = b''
        self.raw(b'i')
        end = time.time() + 1.0
        while time.time() < end:
            self.read()
            m = re.findall(rb'pd engine=[^\n]*\n', self.buf)
            if m:
                line = m[-1].decode(errors='replace').strip()
                up = re.search(r'up=(\d+)', line)
                if up:
                    up = int(up.group(1))
                    if self.last_up is not None and up < self.last_up:
                        log('REBOOT: uptime went %d -> %d s' % (self.last_up, up))
                        self.mute()
                    self.last_up = up
                return line
        return None

    def key(self, rc, down):
        self.raw(b'K' + rc.encode() + (b'+' if down else b'-'))

    def tap(self, rc, hold=0.04):
        self.key(rc, True); time.sleep(hold); self.key(rc, False)

    def midi(self, *bs):
        self.raw(b'M' + bytes(bs).hex().encode() + b'.')


d = Dev()
held_keys, held_midi = set(), set()
NOTE_KEYS = ['%d%x' % (r, c) for r in (1, 2, 3) for c in range(1, 13) if (r, c) != (2, 1)]   # not SHIFT
STRIP_KEYS = ['0%x' % c for c in range(1, 13)]
TILT_KEYS = ['33', '34', '35', '36', '37', '38', '39', '3a', '24', '25', '23', '26']
FN_SAFE = ['13', '24', '25', '23', '26', '0a', '0b', '00', '01', '33', '34', '35', '36', '37', '38', '39', '3a', '1d', '10']


def fn(rc, hold=0.04):
    d.key('20', True); d.tap(rc, hold); d.key('20', False)


def release_all():
    for k in list(held_keys): d.key(k, False)
    held_keys.clear()
    for n in list(held_midi): d.midi(0x80, n, 0)
    held_midi.clear()
    d.midi(0xB0, 64, 0)
    d.key('3d', False)   # space


def act():
    r = rng.random()
    if r < 0.22:     # a key
        k = rng.choice(NOTE_KEYS)
        if k in held_keys: d.key(k, False); held_keys.discard(k)
        elif len(held_keys) < 6: d.key(k, True); held_keys.add(k)
    elif r < 0.42:   # a MIDI note
        if held_midi and (len(held_midi) > 8 or rng.random() < 0.5):
            n = rng.choice(sorted(held_midi)); d.midi(0x80 | rng.randrange(16), n, 0); held_midi.discard(n)
        else:
            n = rng.randrange(24, 108); d.midi(0x90 | rng.randrange(16), n, rng.randrange(1, 128)); held_midi.add(n)
    elif r < 0.52:   # a control CC sweep step
        d.midi(0xB0, rng.choice([16, 17, 18, 19, 1, 2, 71, 74, 91]), rng.randrange(128))
    elif r < 0.55:   d.midi(0xE0, rng.randrange(128), rng.randrange(128))   # bend
    elif r < 0.57:   d.midi(0xC0, rng.randrange(8))                         # engine by program
    elif r < 0.59:   d.midi(0xB0, 64, rng.choice([0, 127]))                 # MIDI sustain
    elif r < 0.61:   # a burst of MIDI clock
        bpm = rng.uniform(60, 180)
        for _ in range(rng.randrange(24, 72)):
            d.midi(0xF8); time.sleep(60.0 / bpm / 24)
    elif r < 0.66:   fn(rng.choice(FN_SAFE))                                 # FN: echo, tempo, octave, params, surface, engine
    elif r < 0.69:   d.tap('0d')                                            # next strip
    elif r < 0.75:   # a strip key: hold, or a double tap to latch
        k = rng.choice(STRIP_KEYS)
        if rng.random() < 0.3: d.tap(k); time.sleep(0.08); d.tap(k)
        elif k in held_keys: d.key(k, False); held_keys.discard(k)
        else: d.key(k, True); held_keys.add(k)
    elif r < 0.77:   d.tap('1d')                                            # next engine
    elif r < 0.78:   d.tap('10')                                            # next surface
    elif r < 0.80:   d.tap('00', rng.choice([0.04, 0.7]))                   # dice: mutate / roll
    elif r < 0.82:   # tilt: tap, or route an axis
        if rng.random() < 0.4: d.tap('30')
        else: d.key('30', True); d.tap(rng.choice(TILT_KEYS)); d.key('30', False)
    elif r < 0.84:   # sustain pedal
        if '3d' in held_keys: d.key('3d', False); held_keys.discard('3d')
        else: d.key('3d', True); held_keys.add('3d')
    elif r < 0.85:   fn('2a')                                               # looper press
    elif r < 0.855:  fn('29')                                               # looper stop
    elif r < 0.857:  fn('14', rng.uniform(0.3, 1.5))                        # record a take
    elif r < 0.86:   d.tap('2d')                                            # ENTER: all off
    # else: rest


def drain_check():
    release_all()
    fn('29'); fn('29')                     # looper: stop, clear
    time.sleep(6.0)                        # MODAL tails ring ~3 s; strips settle
    line = d.state() or ''
    v = re.search(r'voices=(\d+)', line)
    if not v or v.group(1) != '0':
        log('STUCK? voices=%s after releasing everything: %s' % (v.group(1) if v else '?', line))
        log('   held by the script: keys %s midi %s' % (sorted(held_keys), sorted(held_midi)))
        time.sleep(6); l3 = d.state() or ''
        log('   6 s later, untouched: %s' % (re.search(r'voices=\d+', l3).group(0) if l3 else '?'))
        d.tap('2d'); time.sleep(1)
        line2 = d.state() or ''
        log('   after ENTER: %s' % re.search(r'voices=\d+', line2).group(0) if line2 else '   after ENTER: ?')
    else:
        log('drain ok')


log('soak start: %.1f h' % HOURS)
end = T0 + HOURS * 3600
next_log = time.time()
next_drain = time.time() + (90 if HOURS < 0.1 else 600)
BENCH_EVERY = float(sys.argv[3]) if len(sys.argv) > 3 else 3600
next_bench = time.time() + min(BENCH_EVERY, 1800 if HOURS < 1 else 3600)
peak_max = 0.0; heap_min = 10 ** 9; late_last = None; xrun_total = {}
while time.time() < end:
    act()
    time.sleep(rng.uniform(0.05, 0.3))
    d.read()
    now = time.time()
    if now >= next_log:
        next_log = now + (20 if HOURS < 1 else 60)
        line = d.state()
        if not line:
            log('NO STATE LINE'); continue
        f = dict(re.findall(r'(\w+)=([^\s(]+)', line))
        try:
            peak = float(f.get('peak', '0').rstrip('%')); peak_max = max(peak_max, peak)
            heap = int(f.get('heap', '0/0').split('/')[0]); heap_min = min(heap_min, heap)
            late = int(f.get('late', '0'))
            xrun = int(f.get('xrun', '0'))
        except ValueError:
            peak = heap = late = xrun = -1
        eng = re.search(r'engine=([A-Z ]+?) surface', line).group(1)
        log('up=%s eng=%s voices=%s load=%s peak=%s xrun=%s heap=%s stack=%s faults=%s echo=%s take=%s h=%s t=%s m=%s x=%s' % (
            f.get('up'), eng, f.get('voices'), f.get('load'), f.get('peak'), f.get('xrun'), f.get('heap'), f.get('stack'), f.get('faults'),
            f.get('echo'), f.get('take'), f.get('h'), f.get('t'), f.get('m'), f.get('x')))
        if late_last is not None and xrun > late_last:
            log('XRUNS +%d (%s)' % (xrun - late_last, eng))
            xrun_total[eng] = xrun_total.get(eng, 0) + xrun - late_last
        late_last = xrun
    if now >= next_drain:
        next_drain = now + 600
        drain_check()
    if now >= next_bench:
        next_bench = now + BENCH_EVERY
        release_all(); d.tap('2d'); time.sleep(1); d.buf = b''
        d.raw(b'B'); t = time.time()
        while time.time() < t + 8: d.read()
        for b in re.findall(rb'bench engine=[^\n]*', d.buf):
            log('  ' + b.decode(errors='replace')[:100])
        if BENCH_EVERY < 600: drain_check(); next_drain = time.time() + 600

release_all(); d.tap('2d')
fn('29'); fn('29')
time.sleep(4)
log('xruns by engine at the minute marks: %s' % xrun_total)
log('soak end: peak max %.1f%%, heap min %d, final: %s' % (peak_max, heap_min, d.state()))
