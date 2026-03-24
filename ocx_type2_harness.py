#!/usr/bin/env python3
"""Automated offline validation harness for the OCX Type 2 decoder.

Merged and kept conflict-free against the current main-based repository state.
"""

from __future__ import annotations

import argparse
import json
import itertools
from pathlib import Path

import numpy as np

from ocx_type2_wav_sim import (
    DECODER_OVERRIDE_KEYS,
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


def score_run(metrics: dict[str, float | bool | None]) -> float:
    score = 0.0
    output_peak = float(metrics["output_peak"])
    gain_std = float(metrics["gain_curve_std_db"])
    channel_delta = float(metrics["output_channel_delta_max"])
    freq_delta = float(metrics["freq_response_delta_db"])
    transient = float(metrics["transient_delta"])

    score += 8.0 if metrics["output_clip_l"] else 0.0
    score += 8.0 if metrics["output_clip_r"] else 0.0
    score += 4.0 * max(0.0, output_peak - 0.98)
    score += 0.8 * gain_std
    score += 50.0 * channel_delta
    score += 0.05 * freq_delta
    score += 2.0 * transient

    case = str(metrics["case"])
    if case in {"bursts", "envelope_steps", "hf_bursts", "transient_train", "rapid_level_swings"}:
        score += 0.6 * gain_std + 1.5 * transient
    if case in {"too_hot", "clipped_input", "bass_plus_hf"}:
        score += 2.0 * max(0.0, output_peak - 0.95)
    return score


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


def hf_bursts(fs: int, seconds: float) -> np.ndarray:
    n = int(fs * seconds)
    out = np.zeros(n)
    burst = tone(fs, 0.03, 8500.0, -8.0)
    gap = int(fs * 0.07)
    idx = 0
    while idx + len(burst) < n:
        out[idx : idx + len(burst)] = burst
        idx += len(burst) + gap
    return out


def bass_plus_hf(fs: int, seconds: float) -> np.ndarray:
    return 0.7 * bass_heavy(fs, seconds) + 0.6 * treble_heavy(fs, seconds)


def transient_train(fs: int, seconds: float) -> np.ndarray:
    n = int(fs * seconds)
    out = np.zeros(n)
    click_len = max(1, int(0.001 * fs))
    step = int(fs * 0.05)
    for idx in range(0, n - click_len, step):
        out[idx : idx + click_len] = 0.9
    return out


def rapid_level_swings(fs: int, seconds: float) -> np.ndarray:
    t = np.arange(int(fs * seconds)) / fs
    carrier = np.sin(2 * np.pi * 1200.0 * t)
    env = 0.15 + 0.75 * (np.sin(2 * np.pi * 3.8 * t) > 0).astype(np.float64)
    return carrier * env


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
        "hf_bursts": ensure_stereo(hf_bursts(fs, 3.0)),
        "bass_plus_hf": ensure_stereo(bass_plus_hf(fs, 4.0)),
        "transient_train": ensure_stereo(transient_train(fs, 3.0)),
        "rapid_level_swings": ensure_stereo(rapid_level_swings(fs, 4.0)),
    }


def evaluate_candidate(
    fs: int,
    cases: dict[str, np.ndarray],
    params: Params,
    reference_dir: Path | None = None,
) -> tuple[list[dict[str, float | bool | None]], float]:
    results: list[dict[str, float | bool | None]] = []
    total_score = 0.0
    for name, inp in cases.items():
        decoder = Decoder(fs, params)
        out = decoder.process(inp)
        ref = None
        if reference_dir:
            ref_path = reference_dir / f"{name}.wav"
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
        total_score += score_run(metrics)
        results.append(metrics)
    return results, total_score


def sweep_space() -> dict[str, list[float]]:
    return {
        "strength": [0.72, 0.76, 0.80],
        "reference_db": [-20.0, -18.0, -16.0],
        "attack_ms": [2.5, 3.5, 5.0],
        "release_ms": [120.0, 140.0, 180.0],
        "sidechain_hp_hz": [70.0, 90.0, 120.0],
        "sidechain_shelf_hz": [2400.0, 2800.0, 3400.0],
        "sidechain_shelf_db": [14.0, 16.0, 18.0],
        "deemph_hz": [1600.0, 1850.0, 2200.0],
        "deemph_db": [-7.0, -6.0, -5.0],
        "soft_clip_drive": [1.04, 1.08, 1.12],
        "headroom_db": [0.8, 1.0, 1.4],
        "input_trim_db": [-3.5, -3.0, -2.5],
        "output_trim_db": [-1.5, -1.0, -0.5],
    }


def candidate_overrides_for_mode(mode: str) -> list[dict[str, float]]:
    space = sweep_space()
    keys = list(space.keys())
    if mode == "coarse":
        values = [space[k] for k in keys]
        candidates = []
        for combo in itertools.product(*values):
            overrides = {k: float(v) for k, v in zip(keys, combo)}
            candidates.append(overrides)
        return candidates

    if mode == "refine":
        center = {k: space[k][1] for k in keys}
        candidates = [center]
        for key in keys:
            low = dict(center)
            high = dict(center)
            low[key] = space[key][0]
            high[key] = space[key][2]
            candidates.append(low)
            candidates.append(high)
        return candidates

    raise ValueError("Unknown tuning mode. Use 'coarse' or 'refine'.")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-dir", type=Path, default=Path("artifacts/harness"))
    ap.add_argument("--profile", type=Path, default=PROFILE_PATH)
    ap.add_argument("--reference-dir", type=Path)
    ap.add_argument("--write-wavs", action="store_true")
    ap.add_argument("--analysis-fs", type=int, help="Override sample rate for offline sweeps.")
    ap.add_argument("--tune", action="store_true", help="Run reproducible parameter sweep and score candidates.")
    ap.add_argument("--tune-mode", choices=["coarse", "refine"], default="refine")
    ap.add_argument("--top-k", type=int, default=10)
    ap.add_argument("--max-candidates", type=int, help="Optional cap for candidate count to keep offline runtime bounded.")
    args = ap.parse_args()

    profile = json.loads(args.profile.read_text())
    fs = int(args.analysis_fs) if args.analysis_fs else int(profile["sample_rate_hz"])
    cases = build_cases(fs)
    params = Params.from_profile(args.profile)
    args.out_dir.mkdir(parents=True, exist_ok=True)

    results, baseline_score = evaluate_candidate(fs, cases, params, args.reference_dir)
    for name, inp in cases.items():
        if args.write_wavs:
            out = Decoder(fs, params).process(inp)
            write_audio(args.out_dir / f"{name}_input.wav", fs, inp)
            write_audio(args.out_dir / f"{name}_output.wav", fs, out)

    (args.out_dir / "metrics.json").write_text(json.dumps(results, indent=2))
    try:
        import pandas as pd

        pd.DataFrame(results).to_csv(args.out_dir / "metrics.csv", index=False)
    except ImportError:
        pass

    summary: dict[str, object] = {
        "baseline_score": baseline_score,
        "profile": str(args.profile),
        "tune_mode": args.tune_mode if args.tune else None,
    }

    if args.tune:
        candidates = candidate_overrides_for_mode(args.tune_mode)
        if args.max_candidates is not None:
            candidates = candidates[: max(1, args.max_candidates)]
        ranking = []
        for idx, overrides in enumerate(candidates):
            clean = {k: v for k, v in overrides.items() if k in DECODER_OVERRIDE_KEYS}
            candidate = Params.from_profile(args.profile, **clean)
            _, score = evaluate_candidate(fs, cases, candidate, args.reference_dir)
            ranking.append({"candidate_index": idx, "score": score, "overrides": clean})

        ranking = sorted(ranking, key=lambda r: float(r["score"]))
        top = ranking[: max(1, args.top_k)]
        summary["tuning_results"] = {
            "candidate_count": len(ranking),
            "top": top,
            "best": top[0],
        }
        (args.out_dir / "tuning_results.json").write_text(json.dumps(summary["tuning_results"], indent=2))
    (args.out_dir / "summary.json").write_text(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
