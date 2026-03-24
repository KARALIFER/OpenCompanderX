#!/usr/bin/env python3
"""Automated offline validation harness for the OCX Type 2 decoder."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np



def frame_rms(x: np.ndarray, frame: int = 1024, hop: int = 512) -> np.ndarray:
    values = []
    for start in range(0, max(len(x) - frame + 1, 1), hop):
        chunk = x[start : start + frame]
        values.append(np.sqrt(np.mean(np.square(chunk), axis=0)))
    return np.asarray(values) if values else np.zeros((0, x.shape[1]))


def correlation(a: np.ndarray, b: np.ndarray) -> float:
    a1 = a.reshape(-1)
    b1 = b.reshape(-1)
    if np.allclose(a1, a1[0]) or np.allclose(b1, b1[0]):
        return 1.0 if np.allclose(a1, b1) else 0.0
    return float(np.corrcoef(a1, b1)[0, 1])


def spectral_delta_db(a: np.ndarray, b: np.ndarray) -> float:
    a_mono = np.mean(a, axis=1)
    b_mono = np.mean(b, axis=1)
    A = np.fft.rfft(a_mono * np.hanning(len(a_mono)))
    B = np.fft.rfft(b_mono * np.hanning(len(b_mono)))
    mag_a = 20.0 * np.log10(np.maximum(np.abs(A), 1.0e-12))
    mag_b = 20.0 * np.log10(np.maximum(np.abs(B), 1.0e-12))
    return float(np.sqrt(np.mean(np.square(mag_b - mag_a))))


def transient_delta(a: np.ndarray, b: np.ndarray) -> float:
    da = np.diff(a, axis=0, prepend=a[:1])
    db = np.diff(b, axis=0, prepend=b[:1])
    return float(np.max(np.abs(db - da)))


def gain_curve_stats(inp: np.ndarray, out: np.ndarray) -> dict[str, float]:
    in_rms = frame_rms(inp)
    out_rms = frame_rms(out)
    n = min(len(in_rms), len(out_rms))
    if n == 0:
        return {"gain_curve_mean_db": 0.0, "gain_curve_std_db": 0.0}
    gains = 20.0 * np.log10(np.maximum(out_rms[:n], 1.0e-12) / np.maximum(in_rms[:n], 1.0e-12))
    gains = np.nan_to_num(gains, nan=0.0, posinf=0.0, neginf=0.0)
    return {
        "gain_curve_mean_db": float(np.mean(gains)),
        "gain_curve_std_db": float(np.std(gains)),
    }



        })
    else:
        metrics.update({
            "mse_vs_reference": None,
            "mae_vs_reference": None,
            "max_abs_error_vs_reference": None,
            "correlation_vs_reference": None,
            "null_residual_rms_vs_reference": None,
            "freq_response_delta_db_vs_reference": None,
            "transient_delta_vs_reference": None,
        })
    return metrics


def tone(fs: int, seconds: float, freq: float, level_db: float, phase: float = 0.0) -> np.ndarray:
    t = np.arange(int(fs * seconds)) / fs
    return db_to_lin(level_db) * np.sin(2.0 * np.pi * freq * t + phase)


def log_sweep(fs: int, seconds: float, start_hz: float, end_hz: float, level_db: float) -> np.ndarray:
    t = np.linspace(0.0, seconds, int(fs * seconds), endpoint=False)
    k = np.log(end_hz / start_hz) / seconds
    phase = 2.0 * np.pi * start_hz * (np.exp(k * t) - 1.0) / k
    return db_to_lin(level_db) * np.sin(phase)


def colored_noise(fs: int, seconds: float, color: str, level_db: float, rng: np.random.Generator) -> np.ndarray:
    n = int(fs * seconds)
    white = rng.standard_normal(n)
    spectrum = np.fft.rfft(white)
    freqs = np.fft.rfftfreq(n, 1.0 / fs)
    shaping = np.ones_like(freqs)
    nz = freqs > 0
    if color == "pink":
        shaping[nz] = 1.0 / np.sqrt(freqs[nz])
    shaped = np.fft.irfft(spectrum * shaping, n=n)
    shaped /= max(np.max(np.abs(shaped)), 1.0e-12)
    return shaped * db_to_lin(level_db)


def bursts(fs: int, seconds: float, level_db: float) -> np.ndarray:
    n = int(fs * seconds)
    out = np.zeros(n)
    burst = tone(fs, 0.1, 1000.0, level_db)
    gap = int(fs * 0.2)
    idx = 0
    while idx + len(burst) < n:
        out[idx : idx + len(burst)] = burst
        idx += len(burst) + gap
    return out


def envelope_steps(fs: int, seconds: float) -> np.ndarray:
    t = np.arange(int(fs * seconds)) / fs
    carrier = np.sin(2.0 * np.pi * 1000.0 * t)
    env = np.piecewise(t, [t < 1.0, (t >= 1.0) & (t < 2.0), t >= 2.0], [0.08, 0.3, 0.7])
    return carrier * env


def clipped_source(fs: int, seconds: float) -> np.ndarray:
    x = tone(fs, seconds, 1000.0, -1.0) + tone(fs, seconds, 3500.0, -6.0, phase=0.2)
    return np.clip(x * 1.4, -1.0, 1.0)


def rumble(fs: int, seconds: float) -> np.ndarray:
    t = np.arange(int(fs * seconds)) / fs
    low = 0.15 * np.sin(2.0 * np.pi * 8.0 * t)
    dc = np.full_like(t, 0.08)
    audio = 0.12 * np.sin(2.0 * np.pi * 400.0 * t)
    return np.clip(low + dc + audio, -1.0, 1.0)


def bass_heavy(fs: int, seconds: float) -> np.ndarray:
    return tone(fs, seconds, 60.0, -6.0) + tone(fs, seconds, 120.0, -10.0) + tone(fs, seconds, 800.0, -20.0)


def treble_heavy(fs: int, seconds: float) -> np.ndarray:
    return tone(fs, seconds, 4000.0, -8.0) + tone(fs, seconds, 9000.0, -12.0)


def music_like(fs: int, seconds: float) -> np.ndarray:
    t = np.arange(int(fs * seconds)) / fs
    return (
        0.22 * np.sin(2.0 * np.pi * 110.0 * t)
        + 0.14 * np.sin(2.0 * np.pi * 220.0 * t)
        + 0.12 * np.sin(2.0 * np.pi * 440.0 * t)
        + 0.08 * np.sin(2.0 * np.pi * 1760.0 * t)
    ) * (0.6 + 0.4 * np.sin(2.0 * np.pi * 0.7 * t) ** 2)


def build_cases(fs: int) -> dict[str, np.ndarray]:
    rng = np.random.default_rng(1234)
    silence = np.zeros(int(fs * 3.0))
    base = tone(fs, 3.0, 1000.0, -12.0)
    mismatch = np.column_stack([tone(fs, 3.0, 1000.0, -12.0), tone(fs, 3.0, 1300.0, -18.0)])
    return {
        "silence": ensure_stereo(silence),
        "sine_-24db": ensure_stereo(tone(fs, 3.0, 1000.0, -24.0)),
        "sine_-12db": ensure_stereo(base),
        "sine_-6db": ensure_stereo(tone(fs, 3.0, 1000.0, -6.0)),
        "log_sweep": ensure_stereo(log_sweep(fs, 5.0, 20.0, 20000.0, -18.0)),
        "pink_noise": ensure_stereo(colored_noise(fs, 4.0, "pink", -18.0, rng)),
        "white_noise": ensure_stereo(colored_noise(fs, 4.0, "white", -20.0, rng)),
        "bursts": ensure_stereo(bursts(fs, 3.0, -8.0)),
        "envelope_steps": ensure_stereo(envelope_steps(fs, 3.0)),
        "stereo_identical": ensure_stereo(tone(fs, 3.0, 500.0, -16.0) + tone(fs, 3.0, 3000.0, -22.0)),
        "stereo_different": mismatch,
        "bass_heavy": ensure_stereo(bass_heavy(fs, 4.0)),
        "treble_heavy": ensure_stereo(treble_heavy(fs, 4.0)),
        "clipped_input": ensure_stereo(clipped_source(fs, 3.0)),
        "too_quiet": ensure_stereo(tone(fs, 3.0, 1000.0, -42.0)),
        "too_hot": ensure_stereo(np.clip(tone(fs, 3.0, 1000.0, -1.0) * 1.2, -1.0, 1.0)),
        "dc_rumble": ensure_stereo(rumble(fs, 4.0)),
        "music_like": ensure_stereo(music_like(fs, 4.0)),
    }


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-dir", type=Path, default=Path("artifacts/harness"))
    ap.add_argument("--profile", type=Path, default=PROFILE_PATH)
    ap.add_argument("--reference-dir", type=Path)
    ap.add_argument("--write-wavs", action="store_true")
    args = ap.parse_args()

    profile = json.loads(args.profile.read_text())
    fs = int(profile["sample_rate_hz"])
    cases = build_cases(fs)
    params = Params.from_profile(args.profile)
    args.out_dir.mkdir(parents=True, exist_ok=True)

    results = []
    for name, inp in cases.items():
        decoder = Decoder(fs, params)
        out = decoder.process(inp)
        ref = None
        ref_path = args.reference_dir / f"{name}.wav" if args.reference_dir else None
        if ref_path and ref_path.exists():
            ref_fs, ref_audio = read_audio(ref_path)
            if ref_fs == fs:

        metrics = {
            "case": name,
            **{f"input_{k}": v for k, v in summarize_signal("input", inp).items() if k != "name"},
            **{f"output_{k}": v for k, v in summarize_signal("output", out).items() if k != "name"},
            **compare(inp, out, ref),
            "input_clip_l": bool(decoder.input_clip[0]),
            "input_clip_r": bool(decoder.input_clip[1]),
            "output_clip_l": bool(decoder.output_clip[0]),
            "output_clip_r": bool(decoder.output_clip[1]),
            "reference_available": ref is not None,
        }
        results.append(metrics)
        if args.write_wavs:
            write_audio(args.out_dir / f"{name}_input.wav", fs, inp)
            write_audio(args.out_dir / f"{name}_output.wav", fs, out)

    (args.out_dir / "metrics.json").write_text(json.dumps(results, indent=2))
    try:
        import pandas as pd

        pd.DataFrame(results).to_csv(args.out_dir / "metrics.csv", index=False)
    except Exception:
        pass




if __name__ == "__main__":
    main()
