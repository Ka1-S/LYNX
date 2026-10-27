#!/usr/bin/env python3
"""Round 163c: renders sample pitching in LYNX (Windows, LYNX.exe --render) and measures the pitch and length.

usage: pitchcheck.py make <dir> <winDir> <realSample.wav>   -> writes the samples and .lynx projects into <dir>
       pitchcheck.py measure <dir>                            -> prints a table from the rendered <case>.out.wav files
<winDir> is <dir> as Windows sees it (D:\\lynx-work\\...), used for the sample paths inside the projects.
"""
import json, math, os, shutil, sys, wave

import numpy as np

RATE = 44100
CASES = [
    # name, kind (clip / sampler), semitones (pitch knob), fine cents, keep length, key (sampler)
    ("clip_0", "clip", 0.0, 0, False, 60),
    ("clip_p7", "clip", 7.0, 0, False, 60),
    ("clip_m12", "clip", -12.0, 0, False, 60),
    ("clip_p50c", "clip", 0.5, 0, False, 60),
    ("clip_p7_keep", "clip", 7.0, 0, True, 60),
    ("clip_m12_keep", "clip", -12.0, 0, True, 60),
    ("clip_p50c_keep", "clip", 0.5, 0, True, 60),
    ("clip_fine50", "clip", 0.0, 50, False, 60),
    ("smp_0", "sampler", 0.0, 0, False, 60),
    ("smp_key67", "sampler", 0.0, 0, False, 67),
    ("smp_key48", "sampler", 0.0, 0, False, 48),
    ("smp_p50c", "sampler", 0.5, 0, False, 60),
    ("smp_key67_keep", "sampler", 0.0, 0, True, 67),
    ("smp_key48_keep", "sampler", 0.0, 0, True, 48),
    ("smp_p50c_keep", "sampler", 0.5, 0, True, 60),
]
PPQ = 960


def write_wav(path, x, rate=RATE):
    data = (np.clip(x, -1, 1) * 32767).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(data.tobytes())


def read_wav(path):
    with wave.open(path, "rb") as w:
        n, ch, sw, rate = w.getnframes(), w.getnchannels(), w.getsampwidth(), w.getframerate()
        raw = w.readframes(n)
    if sw == 2:
        x = np.frombuffer(raw, "<i2").astype(np.float64) / 32768.0
    elif sw == 3:
        b = np.frombuffer(raw, np.uint8).reshape(-1, 3)
        v = (b[:, 0].astype(np.int32) | (b[:, 1].astype(np.int32) << 8) | (b[:, 2].astype(np.int32) << 16))
        v = np.where(v >= 1 << 23, v - (1 << 24), v)
        x = v.astype(np.float64) / (1 << 23)
    else:
        x = np.frombuffer(raw, "<f4").astype(np.float64) if sw == 4 else None
    return x.reshape(-1, ch)[:, 0], rate


def project(kind, sample, semis, fine, keep, key):
    ch = {"name": "S", "instrument": "audioClip" if kind == "clip" else "sampler", "volume": 0.78, "pan": 0.0,
          "muted": False, "mixer": 0, "sample": sample,
          "samplerSettings": {"pitch": semis, "attack": 0.0, "decay": 0.0, "sustain": 1.0, "release": 0.0,
                              "stretchBeats": 0.0, "keepPitch": keep, "region": [0.0, 1.0], "reverse": False}}
    if fine:
        ch["misc"] = {"fineTune": fine}
    p = {"format": "lynx-project", "version": 1, "bpm": 120.0, "beatsPerBar": 4, "currentPattern": 0, "channels": [ch],
         "patterns": [{"name": "P", "length": PPQ * 16, "notes": [] if kind == "clip" else [[0, PPQ * 16, 0, key, 0.8]]}],
         "playlist": [[0, 0, 0, PPQ * 16, 0, 1, 0]] if kind == "clip" else [],
         "mode": "song" if kind == "clip" else "pattern"}
    return p


def make(d, wd, real):
    os.makedirs(d, exist_ok=True)
    t = np.arange(RATE) / RATE
    write_wav(os.path.join(d, "sine440.wav"), 0.5 * np.sin(2 * math.pi * 440 * t))
    shutil.copy(real, os.path.join(d, "real.wav"))
    for src in ("sine440", "real"):
        for name, kind, semis, fine, keep, key in CASES:
            pj = project(kind, wd + "\\" + src + ".wav", semis, fine, keep, key)
            with open(os.path.join(d, f"{src}_{name}.lynx"), "w") as f:
                json.dump(pj, f, indent=1)


def f0(x, rate):
    # steady part (30..70 % of the sounding part): FFT peak with parabolic interpolation on a Hann window, refined by
    # the harmonic-sum candidate (fundamental of the strongest harmonic series between 40 Hz and 2 kHz)
    n = len(x)
    seg = x[int(n * 0.3):int(n * 0.7)]
    if len(seg) < 4096:
        return 0.0
    w = np.hanning(len(seg))
    size = 1 << int(math.ceil(math.log2(len(seg) * 4)))
    spec = np.abs(np.fft.rfft(seg * w, size))
    hz = np.fft.rfftfreq(size, 1 / rate)
    best, bestScore = 0.0, -1
    for k in range(int(40 * size / rate), int(2000 * size / rate)):
        score = sum(spec[min(len(spec) - 1, k * h)] for h in range(1, 6)) if spec[k] > spec.max() * 0.05 else -1
        if score > bestScore:
            best, bestScore = k, score
    k = best
    a, b, c = spec[k - 1], spec[k], spec[k + 1]
    off = 0.5 * (a - c) / (a - 2 * b + c) if (a - 2 * b + c) != 0 else 0.0
    return (k + off) * rate / size


def sounding(x):
    idx = np.nonzero(np.abs(x) > 0.001)[0]
    return (idx[-1] + 1) if len(idx) else 0


def measure(d):
    print("| sample | case | expected shift (cents) | measured shift (cents) | error (cents) | length (s) | expected length (s) |")
    print("|---|---|---|---|---|---|---|")
    for src in ("sine440", "real"):
        base = {}
        for kind in ("clip", "smp"):
            p = os.path.join(d, f"{src}_{kind}_0.out.wav")
            if os.path.exists(p):
                x, r = read_wav(p)
                base[kind] = (f0(x[:sounding(x)], r), sounding(x) / r)
        for name, kind, semis, fine, keep, key in CASES:
            p = os.path.join(d, f"{src}_{name}.out.wav")
            if not os.path.exists(p):
                continue
            x, r = read_wav(p)
            n = sounding(x)
            k = "clip" if kind == "clip" else "smp"
            f, l0 = base.get(k, (0, 0))
            hz = f0(x[:n], r)
            shift = semis * 100 + fine + (key - 60) * 100
            got = 1200 * math.log2(hz / f) if hz > 0 and f > 0 else float("nan")
            exp_len = l0 if keep else l0 / (2 ** (shift / 1200))
            print(f"| {src} | {name} | {shift:+.0f} | {got:+.1f} | {got - shift:+.1f} | {n / r:.3f} | {exp_len:.3f} |")


if __name__ == "__main__":
    if sys.argv[1] == "make":
        make(sys.argv[2], sys.argv[3], sys.argv[4])
    else:
        measure(sys.argv[2])
