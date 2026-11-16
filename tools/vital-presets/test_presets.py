"""Checks every preset: loads, modulations survive, 4 macros named and each one changes the sound,
bass notes fit the style gate, no clipping/NaN. python test_presets.py <lib_dir>"""
import json
import sys
from pathlib import Path

import numpy as np
import vita

SR = 44100
GATE_MS = {"darkpsycore": 45, "hitech": 50, "slambient": 45}
fails = []


def check(cond, msg):
    if not cond:
        fails.append(msg)
    return cond


def feats(a):
    m = a.mean(0)
    env = np.abs(m)
    peak = env.max()
    win = int(SR * 0.002)
    sm = np.convolve(env, np.ones(win) / win, "same")
    above = np.where(sm > peak * 0.01)[0]          # -40 dB
    length = (above[-1] - above[0]) / SR if len(above) else 0
    spec = np.abs(np.fft.rfft(m * np.hanning(len(m))))
    f = np.fft.rfftfreq(len(m), 1 / SR)
    cen = (spec * f).sum() / max(spec.sum(), 1e-12)
    return dict(peak=peak, length=length, centroid=cen, rms=np.sqrt((m ** 2).mean()))


def render(s, p, note):
    dur, tail = (0.04, 0.3) if p["role"] == "bass" else (0.5, 1.5)
    if p["role"] == "riser":
        dur, tail = 3.0, 0.5
    return s.render(note, 0.9, dur, dur + tail)


def main(lib):
    lib = Path(lib)
    idx = json.loads((lib / "presets.json").read_text(encoding="utf-8"))
    check(len(idx["presets"]) == 14, "14 presets")
    for p in idx["presets"]:
        n = p["name"]
        text = (lib / p["file"]).read_text(encoding="utf-8")
        j = json.loads(text)
        check([j[f"macro{i}"] for i in range(1, 5)] == idx["macros"], f"{n}: macro names")
        check(j["synth_version"] == "1.5.5", f"{n}: version")
        s = vita.Synth()
        s.set_bpm(p["bpm"])
        if not check(s.load_json(text), f"{n}: load"):
            continue
        back = json.loads(s.to_json())
        want = {(m["source"], m["destination"]) for m in j["settings"]["modulations"] if m["source"]}
        got = {(m["source"], m["destination"]) for m in back["settings"]["modulations"] if m["source"]}
        check(want == got, f"{n}: modulations lost {want - got}")
        note = 30 if p["role"] in ("bass", "impact") else 52
        base = feats(render(s, p, note))
        check(np.isfinite(base["peak"]) and 1e-3 < base["peak"] < 4, f"{n}: level {base['peak']:.3f}")
        line = f"{n:18s} pk {20*np.log10(base['peak']):6.1f} dB  len {base['length']*1000:6.0f} ms  cen {base['centroid']:6.0f} Hz"
        if p["role"] == "bass":
            check(base["length"] * 1000 <= GATE_MS[p["style"]] + 15,
                  f"{n}: bass rings {base['length']*1000:.0f} ms (gate 40 ms)")
        c = s.get_controls()
        for i, m in enumerate(idx["macros"], 1):
            diffs = []
            for v in (0.0, 1.0):
                c[f"macro_control_{i}"].set(v)
                f = feats(render(s, p, note))
                diffs.append(f)
            c[f"macro_control_{i}"].set(0.5)
            d = max(abs(np.log(max(diffs[1][k], 1e-9) / max(diffs[0][k], 1e-9))) for k in ("centroid", "length", "rms"))
            line += f"  {m[:4]} {d:4.2f}"
            check(d > 0.05, f"{n}: macro {m} does not change the sound ({d:.3f})")
        print(line)
    print("\nFALHAS:" if fails else "\nTUDO OK", *fails, sep="\n  ")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
