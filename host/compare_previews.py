# Copyright 2026 Combust.
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Render a plaits-lab package's scenarios with pd_render and compare them to
the package's preview WAVs: RMS ratio, best-lag waveform correlation, and
log-spectrum correlation, for main (L) and aux (R).

    python host/compare_previews.py <pd_render binary> <engine id> <package dir> <out dir>

A faithful port at Plaits' rate (-DPD_SAMPLE_RATE=47872.34f) should land near
1.0 on all three; spectrum correlation is the one that matters when phases
drift (free-running oscillators, different control-ramp granularity).
"""
import json
import subprocess
import sys
import wave
from pathlib import Path

import numpy as np


def read_wav(path):
    with wave.open(str(path)) as w:
        x = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float64)
        return x.reshape(-1, w.getnchannels()) / 32768.0


def best_lag_corr(a, b, max_lag=256):
    n = min(len(a), len(b)) - 2 * max_lag
    a0 = a[max_lag:max_lag + n]
    best = -1.0
    for lag in range(-max_lag, max_lag + 1, 1):
        b0 = b[max_lag + lag:max_lag + lag + n]
        d = np.linalg.norm(a0) * np.linalg.norm(b0)
        if d > 0:
            best = max(best, float(np.dot(a0, b0) / d))
    return best


def spectrum_corr(a, b, nfft=4096):
    def avg_spec(x):
        frames = [x[i:i + nfft] * np.hanning(nfft) for i in range(0, len(x) - nfft, nfft // 2)]
        return np.log10(np.mean([np.abs(np.fft.rfft(f)) ** 2 for f in frames], axis=0) + 1e-12)
    sa, sb = avg_spec(a), avg_spec(b)
    return float(np.corrcoef(sa, sb)[0, 1])


def main():
    binary, engine, pkg, out_dir = sys.argv[1], sys.argv[2], Path(sys.argv[3]), Path(sys.argv[4])
    out_dir.mkdir(parents=True, exist_ok=True)
    manifest = json.loads((pkg / "package" / "plaits-engine.json").read_text())
    gain = manifest.get("postProcessing", {}).get("outGain", 0.8)
    scenarios = json.loads((pkg / "package" / "tests" / "scenarios.json").read_text())
    worst = 1.0
    for sc in scenarios:
        c = sc["controls"]
        wav = out_dir / f"{engine}-{sc['id']}.wav"
        args = [binary, engine, str(wav), str(sc["durationSeconds"]), str(sc["note"]),
                str(sc.get("triggerHz", 0)), "12",
                *(str(v) for k in ("harmonics", "timbre", "morph", "macro") for v in c[k]), str(gain)]
        subprocess.run(args, check=True, capture_output=True)
        ours, ref = read_wav(wav), read_wav(pkg / "previews" / f"{sc['id']}.wav")
        row = [sc["id"]]
        for ch, name in ((0, "main"), (1, "aux")):
            a, b = ours[:, ch], ref[:, ch]
            rms_ratio = float(np.sqrt(np.mean(a ** 2)) / max(np.sqrt(np.mean(b ** 2)), 1e-12))
            wc = best_lag_corr(a[:48000], b[:48000])
            spc = spectrum_corr(a, b)
            worst = min(worst, spc)
            row.append(f"{name}: rms_ratio={rms_ratio:.3f} wave_corr={wc:.3f} spec_corr={spc:.3f}")
        print("  ".join(row))
    print(f"worst spectrum correlation: {worst:.3f}")


if __name__ == "__main__":
    main()
