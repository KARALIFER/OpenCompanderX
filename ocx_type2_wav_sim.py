#!/usr/bin/env python3
"""Offline WAV simulator for the OCX Type 2 Teensy decoder firmware."""

from __future__ import annotations

import argparse
import json
import math
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import matplotlib.pyplot as plt
import numpy as np
import soundfile as sf

PROFILE_PATH = Path(__file__).with_name("ocx_type2_profile.json")


def clamp(x: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, x))


def db_to_lin(db: float) -> float:
    return 10.0 ** (db / 20.0)


def lin_to_db(x: np.ndarray | float) -> np.ndarray | float:
    return 20.0 * np.log10(np.maximum(np.abs(x), 1.0e-12))


def soft_clip(x: np.ndarray | float, drive: float) -> np.ndarray | float:
    return np.tanh(drive * x) / np.tanh(drive)


@dataclass(frozen=True)
class Params:
    input_trim_db: float
    output_trim_db: float
    strength: float
    reference_db: float
    max_boost_db: float
    max_cut_db: float
    attack_ms: float
    release_ms: float
    sidechain_hp_hz: float
    sidechain_shelf_hz: float
    sidechain_shelf_db: float
    deemph_hz: float
    deemph_db: float
    soft_clip_drive: float
    dc_block_hz: float
    headroom_db: float
    bypass: bool = False

    @classmethod
    def from_profile(cls, profile_path: Path = PROFILE_PATH, **overrides: Any) -> "Params":
        profile = json.loads(profile_path.read_text())
        decoder = profile["decoder"].copy()
        decoder.update(overrides)
        return cls(**decoder)


class Biquad:
    def __init__(self):
        self.b0 = 1.0
        self.b1 = 0.0
        self.b2 = 0.0
        self.a1 = 0.0
        self.a2 = 0.0
        self.z1 = 0.0
        self.z2 = 0.0

    def process(self, x: float) -> float:
        y = self.b0 * x + self.z1
        self.z1 = self.b1 * x - self.a1 * y + self.z2
        self.z2 = self.b2 * x - self.a2 * y
        return y


class OnePoleHP:
    def __init__(self, fs: float, hz: float):
        hz = max(hz, 0.1)
        rc = 1.0 / (2.0 * math.pi * hz)
        dt = 1.0 / fs
        self.alpha = rc / (rc + dt)
        self.prev_x = 0.0
        self.prev_y = 0.0

    def process(self, x: float) -> float:
        y = self.alpha * (self.prev_y + x - self.prev_x)
        self.prev_x = x
        self.prev_y = y
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
    def __init__(self, fs: int, params: Params):
        self.fs = fs
        self.p = params
        self.input_gain = db_to_lin(params.input_trim_db)
        self.output_gain = db_to_lin(params.output_trim_db)
        self.headroom_gain = db_to_lin(-abs(params.headroom_db))
        self.attack_coeff = math.exp(-1.0 / max(fs * params.attack_ms * 0.001, 1.0))
        self.release_coeff = math.exp(-1.0 / max(fs * params.release_ms * 0.001, 1.0))
        self.sc_hp = [design_highpass(fs, params.sidechain_hp_hz) for _ in range(2)]
        self.sc_shelf = [design_high_shelf(fs, params.sidechain_shelf_hz, params.sidechain_shelf_db) for _ in range(2)]
        self.deemph = [design_high_shelf(fs, params.deemph_hz, params.deemph_db) for _ in range(2)]
        self.dc_block = [OnePoleHP(fs, params.dc_block_hz) for _ in range(2)]
        self.env2 = np.full(2, 1.0e-9, dtype=np.float64)
        self.input_clip = np.zeros(2, dtype=np.bool_)
        self.output_clip = np.zeros(2, dtype=np.bool_)

    def reset(self) -> None:
        self.__init__(self.fs, self.p)

    def process_channel(self, x: np.ndarray, ch: int) -> np.ndarray:
        x = np.asarray(x, dtype=np.float64)
        out = np.zeros_like(x)
        for i, sample in enumerate(x):
            x0 = float(sample) * self.input_gain
            x0 = self.dc_block[ch].process(x0)
            if abs(x0) > 0.98:
                self.input_clip[ch] = True

            if self.p.bypass:
                y = soft_clip(x0 * self.output_gain * self.headroom_gain, self.p.soft_clip_drive)
                out[i] = float(np.clip(y, -1.0, 1.0))
                continue

            sc = self.sc_hp[ch].process(x0)
            sc = self.sc_shelf[ch].process(sc)
            power = sc * sc
            coeff = self.attack_coeff if power > self.env2[ch] else self.release_coeff
            self.env2[ch] = coeff * self.env2[ch] + (1.0 - coeff) * power
            env = math.sqrt(self.env2[ch] + 1.0e-12)
            level_db = 20.0 * math.log10(max(env, 1.0e-12))
            gain_db = clamp((level_db - self.p.reference_db) * self.p.strength, -self.p.max_cut_db, self.p.max_boost_db)
            y = x0 * db_to_lin(gain_db)
            y = self.deemph[ch].process(y)
            y *= self.output_gain * self.headroom_gain
            y = soft_clip(y, self.p.soft_clip_drive)
            y = float(np.clip(y, -1.0, 1.0))
            if abs(y) > 0.98:
                self.output_clip[ch] = True
            out[i] = y
        return out

    def process(self, audio: np.ndarray) -> np.ndarray:
        audio = np.asarray(audio, dtype=np.float64)
        if audio.ndim == 1:
            audio = np.column_stack([audio, audio])
        if audio.shape[1] != 2:
            raise ValueError("Expected mono or stereo audio")
        y = np.zeros_like(audio, dtype=np.float64)
        y[:, 0] = self.process_channel(audio[:, 0], 0)
        y[:, 1] = self.process_channel(audio[:, 1], 1)
        return y


def read_audio(path: Path) -> tuple[int, np.ndarray]:
    audio, fs = sf.read(path, always_2d=True)
    return fs, np.asarray(audio, dtype=np.float64)


def write_audio(path: Path, fs: int, audio: np.ndarray) -> None:
    sf.write(path, np.clip(audio, -1.0, 1.0), fs, subtype="PCM_16")


def summarize_signal(name: str, audio: np.ndarray) -> dict[str, float | str]:
    peak = float(np.max(np.abs(audio)))
    rms = float(np.sqrt(np.mean(np.square(audio))))
    crest = float(peak / max(rms, 1.0e-12))
    dc = float(np.mean(audio))
    return {"name": name, "peak": peak, "rms": rms, "crest_factor": crest, "dc": dc}


def build_arg_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser()
    ap.add_argument("input_wav", type=Path)
    ap.add_argument("output_wav", type=Path)
    ap.add_argument("--profile", type=Path, default=PROFILE_PATH)
    ap.add_argument("--plot", action="store_true")
    ap.add_argument("--bypass", action="store_true")
    return ap


def main() -> None:
    args = build_arg_parser().parse_args()
    fs, audio = read_audio(args.input_wav)
    profile = json.loads(args.profile.read_text())
    expected_fs = int(profile["sample_rate_hz"])
    if fs != expected_fs:
        raise ValueError(f"Expected {expected_fs} Hz WAV for direct Teensy comparability, got {fs} Hz")

    params = Params.from_profile(args.profile, bypass=args.bypass)
    decoder = Decoder(fs, params)
    out = decoder.process(audio)
    write_audio(args.output_wav, fs, out)

    print(json.dumps({
        "input": summarize_signal("input", audio),
        "output": summarize_signal("output", out),
        "input_clip": decoder.input_clip.tolist(),
        "output_clip": decoder.output_clip.tolist(),
    }, indent=2))

    if args.plot:
        n = min(len(audio), fs * 2)
        t = np.arange(n) / fs
        fig, ax = plt.subplots(2, 1, figsize=(12, 7), sharex=True)
        ax[0].plot(t, audio[:n, 0], label="input L")
        ax[0].plot(t, out[:n, 0], label="output L", alpha=0.8)
        ax[0].legend()
        ax[0].set_title("OCX Type 2 decoder simulation")
        ax[1].plot(t, audio[:n, 1], label="input R")
        ax[1].plot(t, out[:n, 1], label="output R", alpha=0.8)
        ax[1].legend()
        ax[1].set_xlabel("seconds")
        plt.tight_layout()
        plt.show()


if __name__ == "__main__":
    main()
