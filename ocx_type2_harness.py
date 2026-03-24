#!/usr/bin/env python3
"""Automated offline validation harness for the OCX Type 2 decoder."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np

from ocx_type2_wav_sim import (
    PROFILE_PATH,
    Decoder,
    Params,
    db_to_lin,
    ensure_stereo,
    read_audio,
    summarize_signal,
    write_audio,
)


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
    n = min(len(a_mono), len(b_mono))
    if n == 0:
        return 0.0
    a_mono = a_mono[:n]
    b_mono = b_mono[:n]
    A = np.fft.rfft(a_mono * np.hanning(n))
    B = np.fft.rfft(b_mono * np.hanning(n))
    mag_a = 20.0 * np.log10(np.maximum(np.abs(A), 1.0e-12))
    mag_b = 20.0 * np.log10(np.maximum(np.abs(B), 1.0e-12))
    return float(np.sqrt(np.mean(np.square(mag_b - mag_a))))


def transient_delta(a: np.ndarray, b: np.ndarray) -> float:
    n = min(len(a), len(b))
    if n == 0:
        return 0.0
    a = a[:n]
    b = b[:n]
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


def align_lengths(*arrays: np.ndarray) -> list[np.ndarray]:
    valid = [a for a in arrays if a is not None]
    n = min(len(a) for a in valid) if valid else 0
    aligned = []
    for a in arrays:
        aligned.append(None if a is None else a[:n])
    return aligned


def compare(inp: np.ndarray, out: np.ndarray, ref: np.ndarray | None = None) -> dict[str, float | None]:
    inp = ensure_stereo(inp)
    out = ensure_stereo(out)
    inp_a, out_a, ref_a = align_lengths(inp, out, ref)

    residual = out_a - inp_a
    metrics: dict[str, float | None] = {
        "residual_rms": float(np.sqrt(np.mean(np.square(residual)))),
        **gain_curve_stats(inp_a, out_a),
        "mse": float(np.mean(np.square(residual))),
        "mae": float(np.mean(np.abs(residual))),
        "max_abs_error": float(np.max(np.abs(residual))),
        "correlation": correlation(inp_a, out_a),
        "freq_response_delta_db": spectral_delta_db(inp_a, out_a),
        "transient_delta": transient_delta(inp_a, out_a),
        "channel_deviation_rms": float(np.sqrt(np.mean(np.square(out_a[:, 0] - out_a[:, 1])))),
    }

    if ref_a is not None:
        diff = out_a - ref_a
        metrics.update(
            {
                "mse_vs_reference": float(np.mean(np.square(diff))),
                "mae_vs_reference": float(np.mean(np.abs(diff))),
                "max_abs_error_vs_reference": float(np.max(np.abs(diff))),
                "correlation_vs_reference": correlation(out_a, ref_a),
                "null_residual_rms_vs_reference": float(np.sqrt(np.mean(np.square(diff)))),
                "freq_response_delta_db_vs_reference": spectral_delta_db(out_a, ref_a),
                "transient_delta_vs_reference": transient_delta(out_a, ref_a),
            }
        )
    else:
        metrics.update(
            {
                "mse_vs_reference": None,
                "mae_vs_reference": None,
                "max_abs_error_vs_reference": None,
                "correlation_vs_reference": None,
                "null_residual_rms_vs_reference": None,
                "freq_response_delta_db_vs_reference": None,
                "transient_delta_vs_reference": None,
            }
        )
    return metrics


def score_candidate(metrics: list[dict[str, float | bool | None]]) -> float:
    score = 1000.0
    for row in metrics:
        clip_penalty = 40.0 * int(bool(row["output_clip_l"])) + 40.0 * int(bool(row["output_clip_r"]))
        stereo_penalty = 50.0 * min(float(row["channel_deviation_rms"]), 1.0)
        spectral_penalty = 1.8 * float(row["freq_response_delta_db"])
        transient_penalty = 8.0 * float(row["transient_delta"])
        gain_std_penalty = 2.5 * float(row["gain_curve_std_db"])
        corr_penalty = 35.0 * max(0.0, 0.95 - float(row["correlation"]))
        score -= clip_penalty + stereo_penalty + spectral_penalty + transient_penalty + gain_std_penalty + corr_penalty
    return float(score)


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


def hf_burst_train(fs: int, seconds: float) -> np.ndarray:
    n = int(fs * seconds)
    out = np.zeros(n)
    carrier = tone(fs, 0.04, 9000.0, -10.0)
    step = int(fs * 0.08)
    for i in range(0, max(n - len(carrier), 1), step):
        out[i : i + len(carrier)] += carrier[: max(0, min(len(carrier), n - i))]
    return np.clip(out, -1.0, 1.0)


def bass_plus_hf(fs: int, seconds: float) -> np.ndarray:
    return np.clip(0.75 * bass_heavy(fs, seconds) + 0.65 * treble_heavy(fs, seconds), -1.0, 1.0)


def transient_train(fs: int, seconds: float) -> np.ndarray:
    n = int(fs * seconds)
    out = np.zeros(n)
    pulse = np.concatenate([np.linspace(0.0, 1.0, 8), np.linspace(1.0, -0.8, 10), np.zeros(12)])
    for i in range(0, n, int(fs * 0.05)):
        take = min(len(pulse), n - i)
        out[i : i + take] = pulse[:take]
    return np.clip(out * 0.8, -1.0, 1.0)


def fast_level_switches(fs: int, seconds: float) -> np.ndarray:
    n = int(fs * seconds)
    out = np.zeros(n)
    seg = int(fs * 0.1)
    levels = [-30.0, -12.0, -24.0, -6.0, -18.0]
    idx = 0
    for i in range(0, n, seg):
        lvl = levels[idx % len(levels)]
        piece = tone(fs, seg / fs, 1200.0, lvl)
        take = min(len(piece), n - i)
        out[i : i + take] = piece[:take]
        idx += 1
    return out


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
        "hf_burst_train": ensure_stereo(hf_burst_train(fs, 3.0)),
        "bass_plus_hf": ensure_stereo(bass_plus_hf(fs, 4.0)),
        "transient_train": ensure_stereo(transient_train(fs, 3.0)),
        "fast_level_switches": ensure_stereo(fast_level_switches(fs, 3.0)),
    }


def parse_override_pairs(values: list[str]) -> dict[str, float]:
    out: dict[str, float] = {}
    for item in values:
        if "=" not in item:
            raise ValueError(f"Override must use key=value, got '{item}'")
        k, v = item.split("=", 1)
        out[k.strip()] = float(v.strip())
    return out


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-dir", type=Path, default=Path("artifacts/harness"))
    ap.add_argument("--profile", type=Path, default=PROFILE_PATH)
    ap.add_argument("--reference-dir", type=Path)
    ap.add_argument("--write-wavs", action="store_true")
    ap.add_argument("--override", action="append", default=[], help="Decoder override key=value (repeatable)")
    ap.add_argument("--tune", action="store_true", help="Run a compact grid search and report the best candidate.")
    ap.add_argument("--tune-fs", type=int, default=4000, help="Sample rate for tuning search workload (default: 4000)")
    args = ap.parse_args()

    profile = json.loads(args.profile.read_text())
    fs = int(profile["sample_rate_hz"])
    cases = build_cases(fs)
    user_overrides = parse_override_pairs(args.override)
    params = Params.from_profile(args.profile, **user_overrides)
    args.out_dir.mkdir(parents=True, exist_ok=True)

    results = []
    for name, inp in cases.items():
        decoder = Decoder(fs, params)
        out = decoder.process(inp)
        ref = None
        if args.reference_dir:
            ref_path = args.reference_dir / f"{name}.wav"
            if ref_path.exists():
                ref_fs, ref_audio = read_audio(ref_path)
                if ref_fs == fs:
                    ref = ref_audio

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
    summary = {
        "score": score_candidate(results),
        "cases": len(results),
        "overrides": user_overrides,
    }
    (args.out_dir / "summary.json").write_text(json.dumps(summary, indent=2))
    try:
        import pandas as pd

        pd.DataFrame(results).to_csv(args.out_dir / "metrics.csv", index=False)
    except ImportError:
        pass

    if args.tune:
        tune_fs = int(max(1000, args.tune_fs))
        tune_cases = build_cases(tune_fs)
        base = json.loads(args.profile.read_text())["decoder"]
        grid = {
            "strength": [base["strength"], min(1.25, base["strength"] + 0.06)],
            "attack_ms": [base["attack_ms"], base["attack_ms"] + 1.0],
            "release_ms": [base["release_ms"], base["release_ms"] + 30.0],
            "deemph_db": [base["deemph_db"], base["deemph_db"] + 1.0],
            "sidechain_shelf_db": [base["sidechain_shelf_db"], base["sidechain_shelf_db"] + 2.0],
            "headroom_db": [base["headroom_db"], min(6.0, base["headroom_db"] + 0.5)],
        }
        best: tuple[float, dict[str, float]] | None = None
        for strength in grid["strength"]:
            for attack_ms in grid["attack_ms"]:
                for release_ms in grid["release_ms"]:
                    for deemph_db in grid["deemph_db"]:
                        for sidechain_shelf_db in grid["sidechain_shelf_db"]:
                            for headroom_db in grid["headroom_db"]:
                                cand = {
                                    "strength": strength,
                                    "attack_ms": attack_ms,
                                    "release_ms": release_ms,
                                    "deemph_db": deemph_db,
                                    "sidechain_shelf_db": sidechain_shelf_db,
                                    "headroom_db": headroom_db,
                                }
                                d = Decoder(tune_fs, Params.from_profile(args.profile, **cand))
                                rows = []
                                for name, inp in tune_cases.items():
                                    out = d.process(inp)
                                    rows.append(
                                        {
                                            "case": name,
                                            **compare(inp, out),
                                            "output_clip_l": bool(d.output_clip[0]),
                                            "output_clip_r": bool(d.output_clip[1]),
                                        }
                                    )
                                s = score_candidate(rows)
                                if best is None or s > best[0]:
                                    best = (s, cand)
        if best:
            (args.out_dir / "tuning_best.json").write_text(json.dumps({"score": best[0], "params": best[1]}, indent=2))


if __name__ == "__main__":
    main()
