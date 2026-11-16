"""Renders a short MP3 preview of each preset (vita, used only as a tool, never linked into Core).

    python render_previews.py <lib_dir> <out_dir>
"""
import json
import subprocess
import sys
from pathlib import Path

import numpy as np
import soundfile as sf
import vita

SR = 44100


def kick(style):
    p = {"darkpsycore": (280, 47, 0.020, 0.13, 0.5), "hitech": (350, 55, 0.013, 0.095, 0.75),
         "slambient": (130, 37, 0.025, 0.09, 0.03)}[style]
    f0, f1, pd, ad, click = p
    t = np.arange(int(SR * 0.3)) / SR
    f = f1 + (f0 - f1) * np.exp(-t / pd)
    y = np.sin(2 * np.pi * np.cumsum(f) / SR) * np.exp(-t / ad)
    y[:int(SR * 0.004)] += click * np.random.default_rng(1).uniform(-1, 1, int(SR * 0.004)) * np.linspace(1, 0, int(SR * 0.004))
    return np.tanh(1.5 * y) * 0.8


def mix(total, events):
    out = np.zeros((2, int(SR * total)))
    for t0, a in events:
        i = int(t0 * SR)
        n = min(a.shape[-1], out.shape[1] - i)
        if n > 0:
            out[:, i:i + n] += a[..., :n]
    return out


def notes(s, seq, tail):
    """seq: list of (time, note, vel, dur)."""
    return [(t, s.render(n, v, d, d + tail)) for t, n, v, d in seq]


def kicks(style, bpm, bars):
    k = kick(style)
    return [(b * 60 / bpm, np.stack([k, k])) for b in range(bars * 4)]


def rolling(bpm, root, octs, gate, style, bars=4, vel_pat=(0.7, 0.9, 0.8)):
    beat, six = 60 / bpm, 60 / bpm / 4
    seq = []
    for b in range(bars * 4):
        off = octs[(b // 4) % len(octs)]
        for k, v in zip((1, 2, 3), vel_pat):
            seq.append((b * beat + k * six, root + off, v, gate))
    return seq


def preview(p, s):
    st, bpm = p["style"], p["bpm"]
    beat = 60 / bpm
    name = p["name"]
    if name == "DPC KBBB Plonk":
        bars = 4
        ev = notes(s, rolling(bpm, 28, [0, 1, 0, -2], 0.04, st, bars), 0.1) + kicks(st, bpm, bars)
        return mix(bars * 4 * beat + 0.3, ev)
    if name == "HT Snap Bass":
        bars = 4
        ev = notes(s, rolling(bpm, 33, [0, 12, 0, 3], 0.045, st, bars), 0.1) + kicks(st, bpm, bars)
        return mix(bars * 4 * beat + 0.3, ev)
    if name == "SLA Deep Sub":
        bars = 4
        ev = notes(s, rolling(bpm, 28, [0], 0.04, st, bars, vel_pat=(0.7, 0.8)), 0.1)
        ev = [e for e in ev] + kicks(st, bpm, bars)
        return mix(bars * 4 * beat + 0.3, ev)
    if name == "DPC Growl Talk":
        bars, six = 4, beat / 4
        seq = [(b * beat + 3 * six, 40 + [0, 1, 0, -2][(b // 4) % 4], 0.9, 0.06) for b in range(bars * 4)]
        seq += [(b * beat + 2 * six, 40, 0.8, 0.05) for b in range(0, bars * 4, 2)]
        return mix(bars * 4 * beat + 0.3, notes(s, seq, 0.15) + kicks(st, bpm, bars))
    if name == "DPC Acid Squelch":
        six = beat / 4
        pat = [40, 40, 52, 40, 43, 40, 47, 40, 40, 52, 40, 46, 40, 43, 52, 40]
        acc = [1, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0]
        seq = [((r * 16 + i) * six, n, 1.0 if a else 0.6, six * 0.8) for r in range(2) for i, (n, a) in enumerate(zip(pat, acc))]
        return mix(8 * beat + 0.6, notes(s, seq, 0.3) + kicks(st, bpm, 2))
    if p["role"] == "drone":
        root = 40 if st == "darkpsycore" else 35
        return mix(9, notes(s, [(0, root, 0.8, 5.5), (0.0, root + 7, 0.6, 5.5)], 3.4))
    if name == "SLA Ghost Bed":
        return mix(9, notes(s, [(0, 47, 0.8, 5.5), (0, 54, 0.6, 5.5)], 3.4))
    if name == "DPC Metal Hit":
        bars = 4
        seq = [(b * 16 * beat / 4 + k * beat / 4, 50, 0.9, 0.05) for b in range(bars) for k in (6, 13)]
        return mix(bars * 4 * beat + 1, notes(s, seq, 0.8) + kicks(st, bpm, bars))
    if name == "HT Laser Zap":
        bars = 4
        seq = [(b * 4 * beat + k * beat / 4, 60 + (b % 2) * 5, 0.9, 0.06) for b in range(bars) for k in (3, 10)]
        return mix(bars * 4 * beat + 1.5, notes(s, seq, 1.2) + kicks(st, bpm, bars))
    if name == "HT FM Answer":
        seq = [(0.5 * beat, 57, 0.9, 0.1), (1.25 * beat, 60, 0.8, 0.1), (2 * beat, 55, 0.9, 0.15),
               (4.5 * beat, 57, 0.9, 0.1), (5.25 * beat, 64, 0.8, 0.1), (6 * beat, 62, 0.9, 0.15)]
        return mix(8 * beat + 2, notes(s, seq, 2.0) + kicks(st, bpm, 2))
    if name == "HT Glass Bell":
        seq = [(i * beat / 2, n, 0.85, 0.08) for i, n in enumerate([69, 72, 76, 81, 79, 76, 72, 74] * 2)]
        return mix(8 * beat + 1.5, notes(s, seq, 1.0) + kicks(st, bpm, 2))
    if name == "HT Static Riser":
        dur = 8 * 4 * beat
        return mix(dur + 1, notes(s, [(0, 52, 0.9, dur)], 0.6))
    if name == "SLA Low Boom":
        return mix(5, notes(s, [(0.2, 23, 1.0, 0.9)], 3.6))
    raise KeyError(name)


def main(lib, out):
    lib, out = Path(lib), Path(out)
    idx = json.loads((lib / "presets.json").read_text(encoding="utf-8"))
    out.mkdir(parents=True, exist_ok=True)
    for p in idx["presets"]:
        s = vita.Synth()
        s.set_bpm(p["bpm"])
        assert s.load_json((lib / p["file"]).read_text(encoding="utf-8"))
        a = preview(p, s)
        peak = np.abs(a).max()
        assert np.isfinite(a).all() and peak > 1e-3, (p["name"], peak)
        a = a / peak * 0.89
        wav = out / f"{p['name']}.wav"
        sf.write(wav, a.T, SR)
        mp3 = wav.with_suffix(".mp3")
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", str(wav), "-b:a", "192k", str(mp3)], check=True)
        wav.unlink()
        print(f"{p['name']:20s} peak {20*np.log10(peak):6.1f} dBFS  {a.shape[1]/SR:5.1f} s")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
