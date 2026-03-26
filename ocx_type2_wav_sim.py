#!/usr/bin/env python3
"""Offline WAV simulator for the OCX Type 2 Teensy codec architecture."""

from __future__ import annotations

import argparse
import json
import math
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import numpy as np
import soundfile as sf

PROFILE_PATH = Path(__file__).with_name("ocx_type2_profile.json")


def clamp(x: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, x))


def sanitize_scalar(x: float, lo: float = -1.0e12, hi: float = 1.0e12) -> float:
    if not math.isfinite(x):
        return 0.0
    return clamp(x, lo, hi)


def sanitize_array(x: np.ndarray, lo: float = -1.0, hi: float = 1.0) -> np.ndarray:
    out = np.nan_to_num(np.asarray(x, dtype=np.float64), nan=0.0, posinf=0.0, neginf=0.0)
    return np.clip(out, lo, hi)


def db_to_lin(db: float) -> float:
    return 10.0 ** (db / 20.0)


def soft_clip(x: float, drive: float) -> float:
    drive = max(1.0e-6, sanitize_scalar(drive, 1.0e-6, 8.0))
    return sanitize_scalar(math.tanh(drive * x) / math.tanh(drive), -1.2, 1.2)


def ensure_stereo(audio: np.ndarray) -> np.ndarray:
    arr = np.asarray(audio, dtype=np.float64)
    if arr.ndim == 1:
        arr = np.column_stack([arr, arr])
    elif arr.ndim == 2 and arr.shape[1] == 1:
        arr = np.column_stack([arr[:, 0], arr[:, 0]])
    elif arr.ndim == 2 and arr.shape[1] == 2:
        pass
    else:
        raise ValueError(f"Expected shape (N,), (N,1), or (N,2). Got {arr.shape}")
    return sanitize_array(arr, -1.0, 1.0)


def read_audio(path: Path) -> tuple[int, np.ndarray]:
    audio, fs = sf.read(path, always_2d=False)
    return int(fs), ensure_stereo(audio)


def write_audio(path: Path, fs: int, audio: np.ndarray) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    sf.write(path, ensure_stereo(audio), fs, subtype="PCM_16")


def summarize_signal(name: str, audio: np.ndarray) -> dict[str, float | str]:
    st = ensure_stereo(audio)
    peak = float(np.max(np.abs(st)))
    rms = float(np.sqrt(np.mean(np.square(st))))
    crest = float(peak / max(rms, 1.0e-12))
    ch_delta = float(np.max(np.abs(st[:, 0] - st[:, 1])))
    return {"name": name, "samples": int(st.shape[0]), "peak": peak, "rms": rms, "crest": crest, "channel_delta_max": ch_delta}


@dataclass(frozen=True)
class DecoderParams:
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
    detector_mode: str = "energy"
    detector_rms_ms: float = 6.0
    bypass: bool = False

    @classmethod
    def from_profile(cls, profile_path: Path = PROFILE_PATH, **overrides: Any) -> "DecoderParams":
        profile = json.loads(profile_path.read_text())
        section = profile["decoder"].copy()
        section.update(overrides)
        return cls(**section)


@dataclass(frozen=True)
class EncoderParams:
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
    tilt_hz: float
    tilt_db: float
    soft_clip_drive: float
    dc_block_hz: float
    headroom_db: float
    detector_mode: str = "energy"
    detector_rms_ms: float = 6.0
    bypass: bool = False

    @classmethod
    def from_profile(cls, profile_path: Path = PROFILE_PATH, **overrides: Any) -> "EncoderParams":
        profile = json.loads(profile_path.read_text())
        section = profile["encoder"].copy()
        section.update(overrides)
        return cls(**section)


class Params(DecoderParams):
    @classmethod
    def from_profile(cls, profile_path: Path = PROFILE_PATH, **overrides: Any) -> "Params":
        base = DecoderParams.from_profile(profile_path, **overrides)
        return cls(**base.__dict__)


class Biquad:
    def __init__(self) -> None:
        self.b0 = 1.0
        self.b1 = 0.0
        self.b2 = 0.0
        self.a1 = 0.0
        self.a2 = 0.0
        self.z1 = 0.0
        self.z2 = 0.0

    def process(self, x: float) -> float:
        x = sanitize_scalar(x)
        y = self.b0 * x + self.z1
        self.z1 = sanitize_scalar(self.b1 * x - self.a1 * y + self.z2)
        self.z2 = sanitize_scalar(self.b2 * x - self.a2 * y)
        return sanitize_scalar(y)

    def reset(self) -> None:
        self.z1 = 0.0
        self.z2 = 0.0


class OnePoleHP:
    def __init__(self, fs: float, hz: float) -> None:
        self.design(fs, hz)

    def design(self, fs: float, hz: float) -> None:
        hz = clamp(float(hz), 0.1, 200.0)
        rc = 1.0 / (2.0 * math.pi * hz)
        dt = 1.0 / float(fs)
        self.alpha = rc / (rc + dt)
        self.reset()

    def process(self, x: float) -> float:
        x = sanitize_scalar(x)
        y = self.alpha * (self.prev_y + x - self.prev_x)
        self.prev_x = x
        self.prev_y = sanitize_scalar(y)
        return self.prev_y

    def reset(self) -> None:
        self.prev_x = 0.0
        self.prev_y = 0.0


def design_highpass(fs: float, hz: float, q: float = 0.7071) -> Biquad:
    f = Biquad()
    hz = clamp(hz, 10.0, fs * 0.45)
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
    hz = clamp(hz, 100.0, fs * 0.45)
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


class _CompanderCore:
    def __init__(self, fs: int, params: Any, mode: str):
        self.fs = int(fs)
        self.p = params
        self.mode = mode
        self.input_gain = db_to_lin(params.input_trim_db)
        self.output_gain = db_to_lin(params.output_trim_db)
        self.headroom_gain = db_to_lin(-abs(params.headroom_db))
        self.attack_coeff = math.exp(-1.0 / max(self.fs * params.attack_ms * 0.001, 1.0))
        self.release_coeff = math.exp(-1.0 / max(self.fs * params.release_ms * 0.001, 1.0))
        self.sc_hp = [design_highpass(self.fs, params.sidechain_hp_hz) for _ in range(2)]
        self.sc_shelf = [design_high_shelf(self.fs, params.sidechain_shelf_hz, params.sidechain_shelf_db) for _ in range(2)]
        tilt_db = float(getattr(params, "deemph_db", getattr(params, "tilt_db", 0.0)))
        tilt_hz = float(getattr(params, "deemph_hz", getattr(params, "tilt_hz", 1850.0)))
        self.tilt = [design_high_shelf(self.fs, tilt_hz, tilt_db) for _ in range(2)]
        self.dc_block = [OnePoleHP(self.fs, params.dc_block_hz) for _ in range(2)]
        self.env2 = np.full(2, 1.0e-9, dtype=np.float64)
        self.detector_env2 = np.full(2, 1.0e-9, dtype=np.float64)
        self.detector_mode = str(params.detector_mode).strip().lower()
        self.rms_coeff = math.exp(-1.0 / max(self.fs * params.detector_rms_ms * 0.001, 1.0))
        self.input_clip = np.zeros(2, dtype=np.bool_)
        self.output_clip = np.zeros(2, dtype=np.bool_)
        self.gain_db_log: list[float] = []

    def reset(self) -> None:
        for bank in (self.sc_hp, self.sc_shelf, self.tilt, self.dc_block):
            for f in bank:
                f.reset()
        self.env2[:] = 1.0e-9
        self.detector_env2[:] = 1.0e-9
        self.input_clip[:] = False
        self.output_clip[:] = False
        self.gain_db_log.clear()

    def _detector_power(self, x0: float, ch: int) -> float:
        sc = self.sc_hp[ch].process(x0)
        sc = self.sc_shelf[ch].process(sc)
        p = sanitize_scalar(sc * sc, 0.0, 1.0e12)
        if self.detector_mode == "rms":
            self.detector_env2[ch] = sanitize_scalar(self.rms_coeff * self.detector_env2[ch] + (1.0 - self.rms_coeff) * p, 0.0, 1.0e12)
            p = self.detector_env2[ch]
        coeff = self.attack_coeff if p > self.env2[ch] else self.release_coeff
        self.env2[ch] = sanitize_scalar(coeff * self.env2[ch] + (1.0 - coeff) * p, 0.0, 1.0e12)
        return math.sqrt(self.env2[ch] + 1.0e-12)

    def _gain_db(self, env: float) -> float:
        raw = (20.0 * math.log10(max(env, 1.0e-12)) - self.p.reference_db) * self.p.strength
        if self.mode == "decode":
            gain_db = clamp(raw, -self.p.max_cut_db, self.p.max_boost_db)
        else:
            gain_db = clamp(-raw, -self.p.max_cut_db, self.p.max_boost_db)
        self.gain_db_log.append(float(gain_db))
        return gain_db

    def process(self, audio: np.ndarray) -> np.ndarray:
        st = ensure_stereo(audio)
        out = np.zeros_like(st)
        for i in range(len(st)):
            for ch in (0, 1):
                x0 = self.dc_block[ch].process(sanitize_scalar(float(st[i, ch]), -2.0, 2.0) * self.input_gain)
                if abs(x0) > 0.98:
                    self.input_clip[ch] = True
                if self.p.bypass:
                    y = soft_clip(x0 * self.output_gain * self.headroom_gain, self.p.soft_clip_drive)
                else:
                    env = self._detector_power(x0, ch)
                    gain_db = self._gain_db(env)
                    y = x0 * db_to_lin(gain_db)
                    y = self.tilt[ch].process(y)
                    y = soft_clip(y * self.output_gain * self.headroom_gain, self.p.soft_clip_drive)
                y = sanitize_scalar(y, -1.0, 1.0)
                if abs(y) > 0.98:
                    self.output_clip[ch] = True
                out[i, ch] = y
        return sanitize_array(out, -1.0, 1.0)


class Decoder(_CompanderCore):
    def __init__(self, fs: int, params: DecoderParams):
        super().__init__(fs, params, mode="decode")


class Encoder(_CompanderCore):
    def __init__(self, fs: int, params: EncoderParams):
        super().__init__(fs, params, mode="encode")


def run_mode(audio: np.ndarray, fs: int, mode: str, profile: Path, overrides: dict[str, Any]) -> tuple[np.ndarray, dict[str, Any]]:
    mode = mode.lower().strip()
    if mode == "decode":
        proc = Decoder(fs, DecoderParams.from_profile(profile, **overrides))
        out = proc.process(audio)
        return out, {"mode": mode, "input_clip": proc.input_clip.tolist(), "output_clip": proc.output_clip.tolist()}
    if mode == "encode":
        proc = Encoder(fs, EncoderParams.from_profile(profile, **overrides))
        out = proc.process(audio)
        gains = np.asarray(proc.gain_db_log) if proc.gain_db_log else np.zeros(1)
        return out, {
            "mode": mode,
            "input_clip": proc.input_clip.tolist(),
            "output_clip": proc.output_clip.tolist(),
            "gain_mean_db": float(np.mean(gains)),
            "gain_std_db": float(np.std(gains)),
        }
    if mode == "roundtrip":
        enc = Encoder(fs, EncoderParams.from_profile(profile, **overrides))
        dec = Decoder(fs, DecoderParams.from_profile(profile))
        encoded = enc.process(audio)
        out = dec.process(encoded)
        return out, {
            "mode": mode,
            "encode": {"input_clip": enc.input_clip.tolist(), "output_clip": enc.output_clip.tolist()},
            "decode": {"input_clip": dec.input_clip.tolist(), "output_clip": dec.output_clip.tolist()},
        }
    raise ValueError(f"Unsupported mode: {mode}")


def build_arg_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description="Offline OCX Type 2 WAV simulator")
    ap.add_argument("input_wav", type=Path)
    ap.add_argument("output_wav", type=Path)
    ap.add_argument("--profile", type=Path, default=PROFILE_PATH)
    ap.add_argument("--mode", choices=["decode", "encode", "roundtrip"], default="decode")
    ap.add_argument("--plot", action="store_true")
    ap.add_argument("--bypass", action="store_true")
    ap.add_argument("--override", action="append", default=[], help="Override as key=value (repeatable)")
    return ap


def parse_overrides(pairs: list[str]) -> dict[str, Any]:
    overrides: dict[str, Any] = {}
    for item in pairs:
        if "=" not in item:
            raise ValueError(f"Override must use key=value, got '{item}'")
        key, value = item.split("=", 1)
        raw = value.strip()
        try:
            overrides[key.strip()] = float(raw)
        except ValueError:
            overrides[key.strip()] = raw.lower() == "true" if raw.lower() in {"true", "false"} else raw
    return overrides


def main() -> None:
    args = build_arg_parser().parse_args()
    fs, audio = read_audio(args.input_wav)
    profile = json.loads(args.profile.read_text())
    expected_fs = int(profile["sample_rate_hz"])
    if fs != expected_fs:
        raise ValueError(f"Expected {expected_fs} Hz WAV for direct Teensy comparability, got {fs} Hz")
    overrides = parse_overrides(args.override)
    if args.bypass:
        overrides["bypass"] = True
    out, meta = run_mode(audio, fs, args.mode, args.profile, overrides)
    write_audio(args.output_wav, fs, out)
    print(json.dumps({"input": summarize_signal("input", audio), "output": summarize_signal("output", out), **meta}, indent=2))


if __name__ == "__main__":
    main()
