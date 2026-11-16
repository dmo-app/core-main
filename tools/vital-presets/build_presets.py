"""Builds the Core AI Vital starter library (Diogo, 2026-11-16).

14 presets for darkpsycore, hitech and slambient, made from Vital's init patch.
Every preset has the same 4 macros: TOM, SUJO, MOVIMENTO, COMPRIMENTO (bipolar, centre = the designed sound).

    python build_presets.py <out_dir>
"""
import base64
import copy
import json
import math
import sys
from pathlib import Path

import numpy as np
import vita

MACROS = ["TOM", "SUJO", "MOVIMENTO", "COMPRIMENTO"]
MACRO_HELP = {
    "TOM": "filtro / brilho",
    "SUJO": "drive / distorção",
    "MOVIMENTO": "LFO, profundidade do envelope ou velocidade",
    "COMPRIMENTO": "decay / release (duração da nota)",
}
VITAL_VERSION = "1.5.5"


def sec(t):
    """Vital envelope time (quartic): seconds -> raw value."""
    return t ** 0.25


def hz(f):
    """Frequency -> Vital cutoff (MIDI note)."""
    return 69 + 12 * math.log2(f / 440.0)


def dsec(t):
    """Delay time in seconds -> delay_frequency raw value (exponential, 2^-v)."""
    return -math.log2(t)


def wave(kind, n=2048):
    x = np.arange(n) / n
    if kind == "sine":
        y = np.sin(2 * np.pi * x)
    elif kind == "square":
        y = np.where(x < 0.5, 1.0, -1.0)
    elif kind == "triangle":
        y = 1 - 4 * np.abs(x - 0.5)
        y = np.roll(y, n // 4)
    elif kind == "saw":
        y = 2 * x - 1
        y = -y
    else:
        raise ValueError(kind)
    return y.astype("<f4")


def wavetable(name, frames):
    """frames: list of 2048-sample arrays, spread over Vital's 256 positions."""
    keys = []
    for i, f in enumerate(frames):
        pos = 0 if len(frames) == 1 else round(i * 256 / (len(frames) - 1))
        keys.append({"position": pos, "wave_data": base64.b64encode(np.asarray(f, "<f4").tobytes()).decode()})
    return {"author": "Core AI", "full_normalize": True, "name": name, "remove_all_dc": True,
            "version": VITAL_VERSION,
            "groups": [{"components": [{"interpolation": 1, "interpolation_style": 1, "keyframes": keys,
                                        "type": "Wave Source"}]}]}


def growl_table():
    """Saw -> vowel-like frames: harmonic bumps moving up (gives the growl when the frame moves)."""
    n = 2048
    frames = []
    for k in range(5):
        spec = np.zeros(n // 2, complex)
        f1, f2 = 3 + k * 2.5, 9 + k * 3.5          # two formant peaks, in harmonics
        for h in range(1, 128):
            amp = 1.0 / h
            amp *= 0.25 + 1.6 * math.exp(-((h - f1) / 2.0) ** 2) + 1.1 * math.exp(-((h - f2) / 3.0) ** 2)
            spec[h] = -1j * amp
        y = np.fft.irfft(np.concatenate([spec, [0]]), n)
        frames.append(y / np.abs(y).max())
    return frames


INIT = None


def init_json():
    global INIT
    if INIT is None:
        INIT = json.loads(vita.Synth().to_json())
    return copy.deepcopy(INIT)


class Preset:
    def __init__(self, name, style, role, desc, vital_style, bpm, tags):
        self.name, self.style, self.role, self.desc, self.bpm, self.tags = name, style, role, desc, bpm, tags
        self.j = init_json()
        self.s = self.j["settings"]
        self.j.update({"author": "Core AI", "preset_name": name, "preset_style": vital_style,
                       "synth_version": VITAL_VERSION,
                       "comments": f"{desc} | estilo: {style} | papel: {role} | macros: "
                                   + ", ".join(f"{m} = {MACRO_HELP[m]}" for m in MACROS)})
        for i, m in enumerate(MACROS, 1):
            self.j[f"macro{i}"] = m
            self.s[f"macro_control_{i}"] = 0.5
        self.s["beats_per_minute"] = bpm / 60.0
        self.mods = []
        self.macro_map = {m: [] for m in MACROS}

    def set(self, **kw):
        for k, v in kw.items():
            if k not in self.s:
                raise KeyError(k)
            self.s[k] = float(v)
        return self

    def table(self, osc, name, frames):
        self.s["wavetables"][osc - 1] = wavetable(name, frames)
        return self

    def mod(self, src, dst, amount, bipolar=False, power=0.0):
        if dst not in vita.get_modulation_destinations() and not dst.startswith("modulation_"):
            raise KeyError(dst)
        self.mods.append((src, dst, amount, bipolar, power))
        return len(self.mods)  # slot number

    def macro(self, name, dst, amount, what):
        i = MACROS.index(name) + 1
        self.mod(f"macro_control_{i}", dst, amount, bipolar=True)
        self.macro_map[name].append(what)

    def finish(self):
        self.s["modulations"] = [{"source": s, "destination": d} for s, d, *_ in self.mods] + \
            [{"source": "", "destination": ""} for _ in range(64 - len(self.mods))]
        for i, (_, _, amt, bip, pw) in enumerate(self.mods, 1):
            self.s[f"modulation_{i}_amount"] = float(amt)
            self.s[f"modulation_{i}_bipolar"] = 1.0 if bip else 0.0
            self.s[f"modulation_{i}_power"] = float(pw)
        for m in MACROS:
            assert self.macro_map[m], f"{self.name}: macro {m} without target"
        return self


def amp(p, a, d, s, r):
    p.set(env_1_attack=sec(a), env_1_decay=sec(d), env_1_sustain=s, env_1_release=sec(r))


def base(p, poly=1):
    p.set(polyphony=poly, filter_1_on=1, osc_1_destination=0, osc_2_destination=0, osc_3_destination=0,
          osc_1_unison_voices=1, osc_1_random_phase=0, osc_1_phase=0.25)


def length_macro(p, env_slots=(1,)):
    for e in env_slots:
        p.macro("COMPRIMENTO", f"env_{e}_decay", 0.12, f"decay do env {e}")
        p.macro("COMPRIMENTO", f"env_{e}_release", 0.08, f"release do env {e}")


# ---------------------------------------------------------------- presets
def rolling_bass(name, style, bpm, desc, cutoff_hz, decay, click_semis, click_t, waves, drive, tags):
    p = Preset(name, style, "bass", desc, "Bass", bpm, tags)
    base(p)
    p.table(1, "Core Saw", [wave(waves[0])])
    p.table(2, "Core Sine", [wave("sine")])
    p.set(osc_1_level=0.75, osc_2_on=1, osc_2_level=0.35, osc_2_transpose=-12,
          filter_1_model=0, filter_1_style=1, filter_1_cutoff=hz(cutoff_hz), filter_1_resonance=0.35,
          filter_1_keytrack=0.5,
          distortion_on=1, distortion_type=0, distortion_drive=drive, distortion_mix=1,
          eq_on=1, eq_low_mode=1, eq_low_cutoff=hz(40), eq_low_gain=0)
    if waves[1]:
        p.set(osc_3_on=1, osc_3_level=0.3)
        p.table(3, "Core Square", [wave(waves[1])])
    amp(p, 0.0006, decay, 0.0, 0.006)
    # filter envelope (env 2) and pitch "plonk" (env 3)
    p.set(env_2_attack=sec(0.0005), env_2_decay=sec(decay * 0.8), env_2_sustain=0, env_2_release=sec(0.005),
          env_3_attack=0, env_3_decay=sec(click_t), env_3_sustain=0, env_3_release=0)
    fenv = p.mod("env_2", "filter_1_cutoff", 0.32)
    p.mod("env_3", "osc_1_transpose", click_semis / 96.0)
    p.mod("velocity", "filter_1_cutoff", 0.06)
    p.macro("TOM", "filter_1_cutoff", 0.12, "corte do filtro")
    p.macro("SUJO", "distortion_drive", 0.25, "drive da distorção")
    p.macro("MOVIMENTO", f"modulation_{fenv}_amount", 0.25, "profundidade do envelope do filtro")
    length_macro(p, (1, 2))
    return p


def growl(bpm):
    p = Preset("DPC Growl Talk", "darkpsycore", "growl",
               "Camada média com vogais que falam, para a 3.ª nota do KBBB; sem sub (HP 150 Hz).",
               "Bass", bpm, ["growl", "formante", "kbbb", "camada"])
    base(p)
    p.table(1, "Core Growl", growl_table())
    p.set(osc_1_level=0.8, osc_1_unison_voices=3, osc_1_unison_detune=1.2, osc_1_stereo_spread=0.3,
          filter_1_model=5, filter_1_formant_x=0.3, filter_1_formant_y=0.6, filter_1_formant_resonance=0.75,
          filter_1_mix=0.85,
          filter_2_on=1, filter_2_model=0, filter_2_style=0, filter_2_blend=2, filter_2_cutoff=hz(150),
          filter_2_filter_input=1, osc_1_destination=2,
          distortion_on=1, distortion_type=0, distortion_drive=10, distortion_mix=0.9)
    amp(p, 0.001, 0.09, 0.0, 0.01)
    p.set(lfo_1_sync=1, lfo_1_tempo=10, lfo_1_sync_type=0,
          lfo_2_sync=1, lfo_2_tempo=9, lfo_2_sync_type=0)
    wav = p.mod("lfo_1", "osc_1_wave_frame", 0.7)
    fx = p.mod("lfo_2", "filter_1_formant_x", 0.5, bipolar=True)
    p.macro("TOM", "filter_1_formant_transpose", 0.3, "transposição dos formantes")
    p.macro("SUJO", "distortion_drive", 0.25, "drive da distorção")
    p.macro("MOVIMENTO", f"modulation_{wav}_amount", 0.3, "quanto as vogais se mexem")
    p.macro("MOVIMENTO", f"modulation_{fx}_amount", 0.3, "quanto as vogais se mexem")
    length_macro(p)
    return p.finish()


def acid(bpm):
    p = Preset("DPC Acid Squelch", "darkpsycore", "acid",
               "Acid ressonante para pergunta e resposta; slide em notas ligadas e acento na velocity.",
               "Lead", bpm, ["acid", "303", "pergunta-resposta", "slide"])
    base(p)
    p.set(osc_1_level=0.8, legato=1, portamento_time=-6.5, portamento_slope=0,
          filter_1_model=4, filter_1_style=1, filter_1_cutoff=hz(380), filter_1_resonance=0.82,
          filter_1_drive=6, filter_1_keytrack=0.3,
          filter_2_on=1, filter_2_model=0, filter_2_style=0, filter_2_blend=2, filter_2_cutoff=hz(200),
          filter_2_filter_input=1, osc_1_destination=2,
          distortion_on=1, distortion_type=0, distortion_drive=8, distortion_mix=1,
          delay_on=1, delay_sync=0, delay_frequency=dsec(60 / bpm * 0.75), delay_feedback=0.35,
          delay_dry_wet=0.18, delay_style=2, delay_filter_cutoff=hz(1500))
    amp(p, 0.001, 0.14, 0.15, 0.02)
    p.set(env_2_attack=0, env_2_decay=sec(0.09), env_2_sustain=0, env_2_release=sec(0.02))
    fenv = p.mod("env_2", "filter_1_cutoff", 0.38)
    p.mod("velocity", "filter_1_cutoff", 0.12)
    p.macro("TOM", "filter_1_cutoff", 0.25, "corte do filtro")
    p.macro("TOM", "filter_1_resonance", 0.15, "ressonância")
    p.macro("SUJO", "distortion_drive", 0.25, "drive da distorção")
    p.macro("MOVIMENTO", f"modulation_{fenv}_amount", 0.3, "profundidade do squelch")
    length_macro(p, (1, 2))
    return p.finish()


def drone(name, style, bpm, desc, transpose, cutoff_hz, rev_mix, predelay, tags):
    p = Preset(name, style, "drone", desc, "Pad", bpm, tags)
    base(p, poly=4)
    p.table(2, "Core Square", [wave("square")])
    p.set(osc_1_level=0.6, osc_1_unison_voices=6, osc_1_unison_detune=2.2, osc_1_stereo_spread=0.8,
          osc_1_transpose=transpose, osc_1_random_phase=1,
          osc_2_on=1, osc_2_level=0.4, osc_2_transpose=transpose - 12, osc_2_tune=0.07,
          filter_1_model=0, filter_1_style=1, filter_1_cutoff=hz(cutoff_hz), filter_1_resonance=0.25,
          chorus_on=1, chorus_dry_wet=0.35,
          reverb_on=1, reverb_dry_wet=rev_mix, reverb_size=0.9, reverb_decay_time=2.5, reverb_delay=predelay,
          reverb_high_shelf_gain=-4,
          eq_on=1, eq_low_mode=1, eq_low_cutoff=hz(60))
    amp(p, 1.2, 1.0, 1.0, 2.0)
    p.set(lfo_1_sync=0, lfo_1_frequency=-3.3, lfo_1_sync_type=0,
          lfo_2_sync=0, lfo_2_frequency=-4.5, lfo_2_sync_type=0)
    lf = p.mod("lfo_1", "filter_1_cutoff", 0.08, bipolar=True)
    p.mod("lfo_2", "osc_1_wave_frame", 0.0)
    p.mod("lfo_2", "osc_2_tune", 0.05, bipolar=True)
    p.mod("random_1", "osc_1_pan", 0.3, bipolar=True)
    p.set(distortion_on=1, distortion_type=0, distortion_drive=0, distortion_mix=1)
    p.macro("TOM", "filter_1_cutoff", 0.15, "corte do filtro")
    p.macro("SUJO", "distortion_drive", 0.3, "drive da distorção")
    p.macro("MOVIMENTO", f"modulation_{lf}_amount", 0.15, "quanto o filtro respira")
    p.macro("MOVIMENTO", "lfo_1_frequency", 0.1, "velocidade do LFO")
    p.macro("COMPRIMENTO", "env_1_release", 0.15, "release")
    p.macro("COMPRIMENTO", "reverb_decay_time", 0.15, "cauda do reverb")
    return p.finish()


def fm_voice(name, style, bpm, role, desc, ratio_semis, decay, fm_amt, transpose, fx, tags, vital_style="Keys",
             comb=False):
    p = Preset(name, style, role, desc, vital_style, bpm, tags)
    base(p, poly=4)
    p.table(1, "Core Sine", [wave("sine")])
    p.table(2, "Core Sine", [wave("sine")])
    p.set(osc_1_level=0.8, osc_1_transpose=transpose, osc_1_distortion_type=7, osc_1_distortion_amount=fm_amt,
          osc_2_on=1, osc_2_level=0.0, osc_2_transpose=transpose + ratio_semis, osc_2_destination=4,
          filter_1_model=0, filter_1_style=0, filter_1_cutoff=hz(9000), filter_1_resonance=0.1)
    if comb:
        p.set(filter_1_model=6, filter_1_cutoff=hz(700), filter_1_resonance=0.75, filter_1_mix=0.6)
    amp(p, 0.0005, decay, 0.0, decay * 0.5)
    p.set(env_2_attack=0, env_2_decay=sec(decay * 0.4), env_2_sustain=0, env_2_release=sec(0.02))
    fm = p.mod("env_2", "osc_1_distortion_amount", 0.35)
    p.set(lfo_1_sync=1, lfo_1_tempo=10, lfo_1_sync_type=0)
    vib = p.mod("lfo_1", "osc_2_tune", 0.0, bipolar=True)
    p.set(distortion_on=1, distortion_type=0, distortion_drive=0, distortion_mix=1)
    p.set(**fx)
    p.macro("TOM", "filter_1_cutoff", 0.15, "corte do filtro")
    p.macro("TOM", "osc_1_distortion_amount", 0.2, "quantidade de FM (brilho)")
    p.macro("SUJO", "distortion_drive", 0.3, "drive da distorção")
    p.macro("MOVIMENTO", f"modulation_{fm}_amount", 0.3, "quanto o FM se mexe no ataque")
    p.macro("MOVIMENTO", f"modulation_{vib}_amount", 0.2, "LFO 1/16 no rácio FM")
    length_macro(p, (1, 2))
    return p.finish()


def zap(bpm):
    p = Preset("HT Laser Zap", "hitech", "fx", "Zap com queda rápida de pitch, metálico; delay ping-pong curto e pouco reverb.",
               "SFX", bpm, ["zap", "laser", "fx", "acento"])
    base(p, poly=4)
    p.table(1, "Core Square", [wave("square")])
    p.set(osc_1_level=0.7, osc_1_transpose=24,
          filter_1_model=0, filter_1_style=0, filter_1_blend=1, filter_1_cutoff=hz(2500), filter_1_resonance=0.7,
          distortion_on=1, distortion_type=0, distortion_drive=6, distortion_mix=1,
          delay_on=1, delay_sync=0, delay_frequency=dsec(60 / bpm * 0.375), delay_feedback=0.35,
          delay_dry_wet=0.22, delay_style=2,
          reverb_on=1, reverb_dry_wet=0.1, reverb_size=0.3, reverb_decay_time=-1)
    amp(p, 0.0005, 0.12, 0.0, 0.03)
    p.set(env_2_attack=0, env_2_decay=sec(0.08), env_2_sustain=0, env_2_release=0, env_2_decay_power=-6)
    pz = p.mod("env_2", "osc_1_transpose", 36 / 96)
    p.mod("env_2", "filter_1_cutoff", 0.25)
    p.macro("TOM", "filter_1_cutoff", 0.15, "corte do filtro")
    p.macro("SUJO", "distortion_drive", 0.3, "drive da distorção")
    p.macro("MOVIMENTO", f"modulation_{pz}_amount", 0.25, "tamanho da queda de pitch")
    p.macro("MOVIMENTO", "env_2_decay", 0.12, "velocidade da queda")
    length_macro(p)
    return p.finish()


def riser(bpm):
    bars8 = 8 * 4 * 60 / bpm
    p = Preset("HT Static Riser", "hitech", "riser",
               f"Riser de ruído e serra que abre em 8 compassos a {bpm} BPM; segurar a nota até ao drop.",
               "SFX", bpm, ["riser", "build", "ruido", "transicao"])
    base(p, poly=2)
    p.set(osc_1_level=0.35, osc_1_unison_voices=8, osc_1_unison_detune=4, osc_1_stereo_spread=1,
          sample_on=1, sample_level=0.7, sample_destination=0,
          filter_1_model=0, filter_1_style=0, filter_1_blend=1, filter_1_cutoff=hz(300), filter_1_resonance=0.45,
          reverb_on=1, reverb_dry_wet=0.3, reverb_size=0.6, reverb_decay_time=0.5,
          eq_on=1, eq_low_mode=1, eq_low_cutoff=hz(200))
    amp(p, bars8 * 0.6, 1.0, 1.0, 0.4)
    p.set(env_2_attack=sec(bars8), env_2_decay=sec(1), env_2_sustain=1, env_2_release=sec(0.3), env_2_attack_power=2)
    fr = p.mod("env_2", "filter_1_cutoff", 0.55)
    p.mod("env_2", "osc_1_transpose", 12 / 96)
    p.set(lfo_1_sync=1, lfo_1_tempo=10)
    tr = p.mod("lfo_1", "voice_amplitude", 0.0)
    p.set(distortion_on=1, distortion_type=0, distortion_drive=0, distortion_mix=1)
    p.macro("TOM", "filter_1_cutoff", 0.12, "corte do filtro")
    p.macro("SUJO", "distortion_drive", 0.3, "drive da distorção")
    p.macro("MOVIMENTO", f"modulation_{tr}_amount", 0.6, "trémulo em 1/16 (sobe com o macro)")
    p.macro("MOVIMENTO", f"modulation_{fr}_amount", 0.15, "quanto o filtro abre")
    p.macro("COMPRIMENTO", "env_2_attack", 0.15, "tempo de subida")
    p.macro("COMPRIMENTO", "env_1_attack", 0.15, "tempo de subida do volume")
    return p.finish()


def deep_sub(bpm):
    p = Preset("SLA Deep Sub", "slambient", "bass",
               "Baixo macio com sub forte uma oitava abaixo, sem clique; nota 25 a 45 ms, raiz B0 a F#1.",
               "Bass", bpm, ["sub", "baixo", "macio", "sem-clique"])
    base(p)
    p.table(1, "Core Triangle", [wave("triangle")])
    p.table(2, "Core Sine", [wave("sine")])
    p.set(osc_1_level=0.3, osc_1_transpose=12, osc_2_on=1, osc_2_level=0.85, osc_2_transpose=0,
          filter_1_model=0, filter_1_style=1, filter_1_cutoff=hz(260), filter_1_resonance=0.1,
          distortion_on=1, distortion_type=0, distortion_drive=3, distortion_mix=1,
          eq_on=1, eq_band_mode=0, eq_band_cutoff=hz(400), eq_band_gain=-3, eq_band_resonance=0.3,
          eq_low_mode=1, eq_low_cutoff=hz(20))
    amp(p, 0.003, 0.04, 0.0, 0.008)
    p.set(env_2_attack=0.0, env_2_decay=sec(0.03), env_2_sustain=0, env_2_release=0)
    fenv = p.mod("env_2", "filter_1_cutoff", 0.12)
    p.macro("TOM", "filter_1_cutoff", 0.12, "corte do filtro")
    p.macro("SUJO", "distortion_drive", 0.25, "drive da distorção")
    p.macro("MOVIMENTO", f"modulation_{fenv}_amount", 0.15, "profundidade do envelope do filtro")
    length_macro(p, (1,))
    return p.finish()


def ghost_bed(bpm):
    p = Preset("SLA Ghost Bed", "slambient", "texture",
               "Cama granulada e larga uma oitava acima, para as camadas; delay 3/16 e reverb grande.",
               "Pad", bpm, ["textura", "granular", "camada", "atmosfera"])
    base(p, poly=6)
    p.set(osc_1_level=0.5, osc_1_transpose=12, osc_1_unison_voices=5, osc_1_unison_detune=3, osc_1_stereo_spread=1,
          osc_1_spectral_morph_type=5, osc_1_spectral_morph_amount=0.6, osc_1_random_phase=1,
          sample_on=1, sample_level=0.25, sample_destination=0,
          filter_1_model=0, filter_1_style=0, filter_1_blend=1, filter_1_cutoff=hz(1400), filter_1_resonance=0.3,
          delay_on=1, delay_sync=0, delay_frequency=dsec(60 / bpm * 0.75), delay_feedback=0.7, delay_dry_wet=0.35,
          delay_style=2, delay_filter_cutoff=hz(2500),
          reverb_on=1, reverb_dry_wet=0.6, reverb_size=1.0, reverb_decay_time=3, reverb_delay=0.05,
          eq_on=1, eq_low_mode=1, eq_low_cutoff=hz(150))
    amp(p, 0.6, 1.0, 1.0, 1.5)
    p.set(random_1_sync=1, random_1_tempo=10, random_1_style=1,
          lfo_1_sync=0, lfo_1_frequency=-2.5)
    gr = p.mod("random_1", "voice_amplitude", 0.6)
    p.mod("random_1", "osc_1_spectral_morph_amount", 0.4, bipolar=True)
    sw = p.mod("lfo_1", "filter_1_cutoff", 0.1, bipolar=True)
    p.set(distortion_on=1, distortion_type=0, distortion_drive=4, distortion_mix=0.7)
    p.macro("TOM", "filter_1_cutoff", 0.15, "corte do filtro")
    p.macro("SUJO", "distortion_drive", 0.25, "drive da distorção")
    p.macro("MOVIMENTO", f"modulation_{gr}_amount", 0.4, "quanto o grão corta")
    p.macro("MOVIMENTO", f"modulation_{sw}_amount", 0.1, "quanto o filtro respira")
    p.macro("COMPRIMENTO", "env_1_release", 0.15, "release")
    p.macro("COMPRIMENTO", "reverb_decay_time", 0.15, "cauda do reverb")
    return p.finish()


def low_boom(bpm):
    p = Preset("SLA Low Boom", "slambient", "impact",
               "Boom grave 90 para 30 Hz com 900 ms e reverb grande; tocar na raiz (B0 a F#1) a cada 4 compassos.",
               "SFX", bpm, ["boom", "impacto", "sub", "transicao"])
    base(p, poly=2)
    p.table(1, "Core Sine", [wave("sine")])
    p.table(2, "Core Triangle", [wave("triangle")])
    p.set(osc_1_level=0.9, osc_2_on=1, osc_2_level=0.35, osc_2_transpose=12,
          filter_1_model=0, filter_1_style=1, filter_1_cutoff=hz(220), filter_1_resonance=0.1,
          reverb_on=1, reverb_dry_wet=0.45, reverb_size=0.9, reverb_decay_time=2.5, reverb_high_shelf_gain=-6,
          reverb_pre_low_cutoff=hz(80),
          distortion_on=1, distortion_type=0, distortion_drive=4, distortion_mix=1)
    amp(p, 0.005, 0.9, 0.0, 0.6)
    p.set(env_2_attack=0, env_2_decay=sec(0.35), env_2_sustain=0, env_2_release=0, env_2_decay_power=-4)
    pd = p.mod("env_2", "osc_1_transpose", 19 / 96)
    p.mod("env_2", "osc_2_transpose", 19 / 96)
    p.macro("TOM", "filter_1_cutoff", 0.2, "corte do filtro (corpo acima do sub)")
    p.macro("SUJO", "distortion_drive", 0.25, "drive da distorção")
    p.macro("MOVIMENTO", f"modulation_{pd}_amount", 0.12, "tamanho da queda de pitch")
    p.macro("MOVIMENTO", "env_2_decay", 0.12, "velocidade da queda")
    length_macro(p, (1,))
    return p.finish()


def build_all():
    out = []
    out.append(rolling_bass("DPC KBBB Plonk", "darkpsycore", 225,
                            "Baixo rolante seco para KBBB, nota até 40 ms, plonk de pitch no ataque, sub estreito.",
                            cutoff_hz=220, decay=0.042, click_semis=12, click_t=0.012, waves=("saw", None), drive=9,
                            tags=["kbbb", "rolling", "baixo", "seco"]).finish())
    out.append(growl(225))
    out.append(acid(225))
    out.append(drone("DPC Abyss Drone", "darkpsycore", 225,
                     "Drone escuro e largo para os breaks; ataque 1,2 s, release 2 s, reverb grande.",
                     transpose=0, cutoff_hz=520, rev_mix=0.6, predelay=0.0, tags=["drone", "break", "escuro"]))
    out.append(fm_voice("DPC Metal Hit", "darkpsycore", 225, "metal",
                        "Acerto metálico curto e escuro para os acentos; afinar à raiz do baixo.",
                        ratio_semis=7.3, decay=0.22, fm_amt=0.55, transpose=0, comb=True,
                        fx=dict(reverb_on=1, reverb_dry_wet=0.15, reverb_size=0.25, reverb_decay_time=-1.5),
                        tags=["metal", "acento", "percussao"], vital_style="Percussion"))
    out.append(rolling_bass("HT Snap Bass", "hitech", 190,
                            "Baixo com clique forte e oitavas, nota 40 a 50 ms; brilhante e seco.",
                            cutoff_hz=420, decay=0.048, click_semis=24, click_t=0.008, waves=("saw", "square"),
                            drive=11, tags=["kbbb", "clique", "baixo", "oitavas"]).finish())
    out.append(zap(190))
    out.append(fm_voice("HT FM Answer", "hitech", 190, "lead",
                        "Resposta FM uma oitava acima, curta, feita para levar delay (0,75 de batida).",
                        ratio_semis=12, decay=0.16, fm_amt=0.45, transpose=12,
                        fx=dict(delay_on=1, delay_sync=0, delay_frequency=dsec(60 / 190 * 0.75), delay_feedback=0.5,
                                delay_dry_wet=0.3, delay_style=2,
                                reverb_on=1, reverb_dry_wet=0.3, reverb_size=0.6, reverb_decay_time=0.7),
                        tags=["fm", "resposta", "lead", "delay"], vital_style="Lead"))
    out.append(fm_voice("HT Glass Bell", "hitech", 190, "bell",
                        "Sino FM de vidro e metal, curto, com pouco reverb.",
                        ratio_semis=21.69, decay=0.45, fm_amt=0.5, transpose=12,
                        fx=dict(reverb_on=1, reverb_dry_wet=0.12, reverb_size=0.35, reverb_decay_time=-0.5),
                        tags=["fm", "sino", "metal", "pluck"]))
    out.append(riser(190))
    out.append(deep_sub(300))
    out.append(drone("SLA Abyss Drone", "slambient", 300,
                     "Drone uma oitava acima da raiz, filtro baixo (1200 Hz), reverb 0,7 com pré-delay 60 ms.",
                     transpose=12, cutoff_hz=1200, rev_mix=0.7, predelay=0.06, tags=["drone", "camada", "escuro"]))
    out.append(ghost_bed(300))
    out.append(low_boom(300))
    return out


def level_of(p):
    s = vita.Synth()
    s.set_bpm(p.bpm)
    assert s.load_json(json.dumps(p.j)), p.name
    note = 30 if p.role in ("bass", "impact") else 52
    dur = {"bass": 0.04, "drone": 4.0, "texture": 4.0, "riser": 12.0}.get(p.role, 0.3)
    a = s.render(note, 0.8, dur, dur + 1.0)
    return 20 * np.log10(np.abs(a).max() + 1e-12)


TARGET_DB = -6.0


def calibrate(p):
    """Same loudness for every preset: peak of a reference note at -6 dBFS, via Vital's master volume."""
    for _ in range(4):
        err = TARGET_DB - level_of(p)
        if abs(err) < 0.5:
            break
        p.s["volume"] = volume_raw(volume_db(p.s["volume"]) + err)


_VS = None


def volume_db(raw):
    global _VS
    _VS = _VS or vita.Synth()
    _VS.get_controls()["volume"].set(raw)
    return float(_VS.get_control_text("volume").replace("dB", ""))


def volume_raw(db):
    lo, hi = 0.0, 7399.4404296875
    for _ in range(40):
        mid = (lo + hi) / 2
        lo, hi = (mid, hi) if volume_db(mid) < db else (lo, mid)
    return lo


def slug(name):
    return name.lower().replace(" ", "-")


def main(out_dir):
    out = Path(out_dir)
    (out / "presets").mkdir(parents=True, exist_ok=True)
    index = {"format": "core-ai-vital-presets", "version": 1, "synth": "Vital", "macros": MACROS,
             "macro_help": MACRO_HELP,
             "macro_note": "Os 4 macros começam a 0,5 (o som desenhado). 0 = menos, 1 = mais.",
             "presets": []}
    synth = vita.Synth()
    for p in build_all():
        calibrate(p)
        text = json.dumps(p.j)
        assert synth.load_json(text), p.name
        fname = f"presets/{p.name}.vital"
        (out / fname).write_text(text, encoding="utf-8")
        index["presets"].append({"id": slug(p.name), "name": p.name, "file": fname, "style": p.style,
                                 "role": p.role, "bpm": p.bpm, "tags": p.tags, "description": p.desc,
                                 "macros": {m: p.macro_map[m] for m in MACROS}, "origin": "core"})
    (out / "presets.json").write_text(json.dumps(index, ensure_ascii=False, indent=2), encoding="utf-8")
    print(len(index["presets"]), "presets")


if __name__ == "__main__":
    main(sys.argv[1])
