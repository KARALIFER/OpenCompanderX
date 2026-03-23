#!/usr/bin/env python3
"""
Offline WAV simulator for the OCX Type 2 Teensy decoder firmware.

Purpose:
- Tests the same approximate decode algorithm on stereo WAV files.
- Mirrors the same default coefficients and signal flow as the Teensy firmware.
- Helps explore likely edge cases before touching hardware.

Usage examples:
  python ocx_type2_wav_sim.py input.wav output.wav
  python ocx_type2_wav_sim.py input.wav output.wav --plot
"""

from __future__ import annotations

import argparse
import math
from dataclasses import dataclass
from pathlib import Path

import numpy as np
from scipy.io import wavfile

try:
    import matplotlib.pyplot as plt
except Exception:
    plt = None


def clamp(x: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, x))


def db_to_lin(db: float) -> float:
    return 10.0 ** (db / 20.0)


def lin_to_db(x):
    return 20.0 * np.log10(np.maximum(np.abs(x), 1.0e-12))


def soft_clip(x):
    drive = 1.10
    return np.tanh(drive * x) / np.tanh(drive)


@dataclass
class Params:
    input_trim_db: float = 0.0
    output_trim_db: float = 0.0
    strength: float = 0.78
    reference_db: float = -18.0
    max_boost_db: float = 10.0
    max_cut_db: float = 24.0
    attack_ms: float = 4.0
    release_ms: float = 120.0
    sidechain_hp_hz: float = 65.0
    sidechain_shelf_hz: float = 2500.0
    sidechain_shelf_db: float = 20.0
    deemph_hz: float = 750.0
    deemph_db: float = -12.0
    bypass: bool = False


class Biquad:
    def __init__(self):
        self.b0 = 1.0
        self.b1 = 0.0
        self.b2 = 0.0
        self.a1 = 0.0
        self.a2 = 0.0
        self.z1 = 0.0
        self.z2 = 0.0

    def process(self, x):
        y = self.b0 * x + self.z1
        self.z1 = self.b1 * x - self.a1 * y + self.z2
        self.z2 = self.b2 * x - self.a2 * y
        return y


def design_highpass(fs: float, hz: float, q: float = 0.7071) -> Biquad:
    f = Biquad()
    w0 = 2.0 * math.pi * hz / fs
    cosw0 = math.cos(w0)
    sinw0 = math.sin(w0)
    alpha = sinw0 / (2.0 * q)

    b0 = (1.0 + cosw0) * 0.5
    b1 = -(1.0 + cosw0)
    b2 = (1.0 + cosw0) * 0.5
    a0 = 1.0 + alpha
    a1 = -2.0 * cosw0
    a2 = 1.0 - alpha

    f.b0 = b0 / a0
    f.b1 = b1 / a0
    f.b2 = b2 / a0
    f.a1 = a1 / a0
    f.a2 = a2 / a0
    return f


def design_high_shelf(fs: float, hz: float, gain_db: float, slope: float = 0.8) -> Biquad:
    f = Biquad()
    A = 10.0 ** (gain_db / 40.0)
    w0 = 2.0 * math.pi * hz / fs
    cosw0 = math.cos(w0)
    sinw0 = math.sin(w0)
    alpha = sinw0 / 2.0 * math.sqrt((A + 1.0 / A) * (1.0 / slope - 1.0) + 2.0)
    beta = 2.0 * math.sqrt(A) * alpha

    b0 = A * ((A + 1.0) + (A - 1.0) * cosw0 + beta)
    b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cosw0)
    b2 = A * ((A + 1.0) + (A - 1.0) * cosw0 - beta)
    a0 = (A + 1.0) - (A - 1.0) * cosw0 + beta
    a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cosw0)
    a2 = (A + 1.0) - (A - 1.0) * cosw0 - beta

    f.b0 = b0 / a0
    f.b1 = b1 / a0
    f.b2 = b2 / a0
    f.a1 = a1 / a0
    f.a2 = a2 / a0
    return f


class Decoder:
    def __init__(self, fs: int, p: Params):
        self.fs = fs
        self.p = p
        self.input_gain = db_to_lin(p.input_trim_db)
        self.output_gain = db_to_lin(p.output_trim_db)

        self.attack_coeff = math.exp(-1.0 / (fs * p.attack_ms * 0.001))
        self.release_coeff = math.exp(-1.0 / (fs * p.release_ms * 0.001))

        self.sc_hp = [design_highpass(fs, p.sidechain_hp_hz) for _ in range(2)]
        self.sc_shelf = [design_high_shelf(fs, p.sidechain_shelf_hz, p.sidechain_shelf_db) for _ in range(2)]
        self.deemph = [design_high_shelf(fs, p.deemph_hz, p.deemph_db) for _ in range(2)]
        self.env2 = np.full(2, 1.0e-9, dtype=np.float64)

    def process_channel(self, x: np.ndarray, ch: int) -> np.ndarray:
        x = x.astype(np.float64).copy()
        x *= self.input_gain

        if self.p.bypass:
            y = soft_clip(x * self.output_gain)
            return np.clip(y, -1.0, 1.0)

        out = np.zeros_like(x)
        hp = self.sc_hp[ch]
        shelf = self.sc_shelf[ch]
        deemph = self.deemph[ch]

        for i, sample in enumerate(x):
            sc = hp.process(sample)
            sc = shelf.process(sc)

            power = sc * sc
            coeff = self.attack_coeff if power > self.env2[ch] else self.release_coeff
            self.env2[ch] = coeff * self.env2[ch] + (1.0 - coeff) * power
            env = math.sqrt(self.env2[ch] + 1.0e-12)

            level_db = 20.0 * math.log10(max(env, 1.0e-12))
            gain_db = (level_db - self.p.reference_db) * self.p.strength
            gain_db = clamp(gain_db, -self.p.max_cut_db, self.p.max_boost_db)
            gain_lin = db_to_lin(gain_db)

            y = sample * gain_lin
            y = deemph.process(y)
            y *= self.output_gain
            y = soft_clip(y)
            out[i] = max(-1.0, min(1.0, y))
        return out

    def process(self, audio: np.ndarray) -> np.ndarray:
        if audio.ndim == 1:
            audio = np.column_stack([audio, audio])
        y = np.zeros_like(audio, dtype=np.float64)
        y[:, 0] = self.process_channel(audio[:, 0], 0)
        y[:, 1] = self.process_channel(audio[:, 1], 1)
        return y


def read_wav(path: Path):
    fs, data = wavfile.read(path)
    if data.dtype == np.int16:
        audio = data.astype(np.float64) / 32768.0
    elif data.dtype == np.int32:
        audio = data.astype(np.float64) / 2147483648.0
    elif np.issubdtype(data.dtype, np.floating):
        audio = data.astype(np.float64)
    else:
        raise ValueError(f"Unsupported WAV dtype: {data.dtype}")
    return fs, audio


def write_wav(path: Path, fs: int, audio: np.ndarray):
    out = np.clip(audio, -1.0, 1.0)
    out_i16 = (out * 32767.0).astype(np.int16)
    wavfile.write(path, fs, out_i16)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input_wav")
    ap.add_argument("output_wav")
    ap.add_argument("--plot", action="store_true")
    args = ap.parse_args()

    fs, audio = read_wav(Path(args.input_wav))
    if fs != 44100:
        raise ValueError("Expected 44.1 kHz WAV for best comparability with the Teensy firmware")

    params = Params()
    dec = Decoder(fs, params)
    out = dec.process(audio)
    write_wav(Path(args.output_wav), fs, out)

    in_peak = float(np.max(np.abs(audio)))
    out_peak = float(np.max(np.abs(out)))
    print(f"input peak:  {in_peak:.6f}")
    print(f"output peak: {out_peak:.6f}")
    if out_peak >= 0.999:
        print("warning: output reached limiter / full scale")

    if args.plot:
        if plt is None:
            raise RuntimeError("matplotlib is not available")
        plot_audio = audio if audio.ndim > 1 else np.column_stack([audio, audio])
        n = min(len(audio), fs // 2)
        t = np.arange(n) / fs
        fig, ax = plt.subplots(2, 1, figsize=(12, 7), sharex=True)
        ax[0].plot(t, plot_audio[:n, 0], label="input L")
        ax[0].plot(t, out[:n, 0], label="output L", alpha=0.8)
        ax[0].legend()
        ax[0].set_title("OCX Type 2 decoder simulation")
        ax[1].plot(t, plot_audio[:n, 1], label="input R")
        ax[1].plot(t, out[:n, 1], label="output R", alpha=0.8)
        ax[1].legend()
        ax[1].set_xlabel("seconds")
        plt.tight_layout()
        plt.show()


if __name__ == "__main__":
    main()
