#!/usr/bin/env python3
"""Automated offline validation harness for the OCX Type 2 decoder."""

from __future__ import annotations

import argparse
import itertools
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


def spectral_band_delta_db(
    a: np.ndarray,
    b: np.ndarray,
    fs: int,
    bands: tuple[tuple[float, float], ...] = ((20.0, 200.0), (200.0, 4000.0), (4000.0, 16000.0)),
) -> dict[str, float]:
    a_mono = np.mean(a, axis=1)
    b_mono = np.mean(b, axis=1)
    n = min(len(a_mono), len(b_mono))
    if n == 0:
        return {"low": 0.0, "mid": 0.0, "high": 0.0}
    a_mono = a_mono[:n]
    b_mono = b_mono[:n]
    window = np.hanning(n)
    freqs = np.fft.rfftfreq(n, 1.0 / fs)
    A = 20.0 * np.log10(np.maximum(np.abs(np.fft.rfft(a_mono * window)), 1.0e-12))
    B = 20.0 * np.log10(np.maximum(np.abs(np.fft.rfft(b_mono * window)), 1.0e-12))
    keys = ("low", "mid", "high")
    out: dict[str, float] = {}
    for key, (lo, hi) in zip(keys, bands):
        mask = (freqs >= lo) & (freqs < hi)
        if not np.any(mask):
            out[key] = 0.0
        else:
            out[key] = float(np.sqrt(np.mean(np.square(B[mask] - A[mask]))))
    return out


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
        return {
            "gain_curve_mean_db": 0.0,
            "gain_curve_std_db": 0.0,
            "gain_curve_diff_std_db": 0.0,
            "gain_curve_diff_p95_db": 0.0,
            "input_level_span_db": 0.0,
            "gain_vs_input_slope": 0.0,
            "gain_vs_input_r2": 0.0,
        }
    gains = 20.0 * np.log10(np.maximum(out_rms[:n], 1.0e-12) / np.maximum(in_rms[:n], 1.0e-12))
    gains = np.nan_to_num(gains, nan=0.0, posinf=0.0, neginf=0.0)
    in_db = 20.0 * np.log10(np.maximum(in_rms[:n], 1.0e-12))
    in_db = np.nan_to_num(in_db, nan=-120.0, posinf=0.0, neginf=-120.0)
    gain_diff = np.diff(gains, axis=0, prepend=gains[:1])
    gain_diff_abs = np.abs(gain_diff)
    in_db_mono = np.mean(in_db, axis=1)
    gains_mono = np.mean(gains, axis=1)
    centered_x = in_db_mono - np.mean(in_db_mono)
    centered_y = gains_mono - np.mean(gains_mono)
    var_x = float(np.dot(centered_x, centered_x))
    if var_x > 1.0e-12:
        slope = float(np.dot(centered_x, centered_y) / var_x)
        y_fit = np.mean(gains_mono) + slope * centered_x
        sst = float(np.dot(centered_y, centered_y))
        sse = float(np.dot(gains_mono - y_fit, gains_mono - y_fit))
        r2 = 1.0 - (sse / max(sst, 1.0e-12))
    else:
        slope = 0.0
        r2 = 0.0
    return {
        "gain_curve_mean_db": float(np.mean(gains)),
        "gain_curve_std_db": float(np.std(gains)),
        "gain_curve_diff_std_db": float(np.std(gain_diff)),
        "gain_curve_diff_p95_db": float(np.percentile(gain_diff_abs, 95.0)),
        "input_level_span_db": float(np.max(in_db) - np.min(in_db)),
        "gain_vs_input_slope": slope,
        "gain_vs_input_r2": float(clamp(r2, -1.0, 1.0)),
    }


def clamp(x: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, x))


def softclip_dependency(inp: np.ndarray, out: np.ndarray) -> float:
    in_peak = float(np.max(np.abs(inp)))
    out_peak = float(np.max(np.abs(out)))
    in_rms = float(np.sqrt(np.mean(np.square(inp))))
    out_rms = float(np.sqrt(np.mean(np.square(out))))
    out_crest = out_peak / max(out_rms, 1.0e-12)
    driven = max(0.0, (out_peak - 0.94) * 20.0)
    compressed_crest = max(0.0, 2.0 - out_crest)
    input_push = max(0.0, in_peak - 0.90)
    return float(driven * (1.0 + compressed_crest) * (1.0 + input_push))


def align_lengths(*arrays: np.ndarray | None) -> list[np.ndarray | None]:
    valid = [a for a in arrays if a is not None]
    n = min(len(a) for a in valid) if valid else 0
    aligned: list[np.ndarray | None] = []
    for a in arrays:
        aligned.append(None if a is None else a[:n])
    return aligned


def compare(inp: np.ndarray, out: np.ndarray, fs: int, ref: np.ndarray | None = None) -> dict[str, float | int | None]:
    inp = ensure_stereo(inp)
    out = ensure_stereo(out)
    inp_a, out_a, ref_a = align_lengths(inp, out, ref)
    assert inp_a is not None
    assert out_a is not None

    residual = out_a - inp_a
    input_rms = float(np.sqrt(np.mean(np.square(inp_a))))
    output_rms = float(np.sqrt(np.mean(np.square(out_a))))
    residual_rms = float(np.sqrt(np.mean(np.square(residual))))
    band_delta = spectral_band_delta_db(inp_a, out_a, fs)
    metrics: dict[str, float | int | None] = {
        "residual_rms": residual_rms,
        "input_rms": input_rms,
        "output_rms": output_rms,
        "decode_action_ratio": float(residual_rms / max(input_rms, 1.0e-12)),
        **gain_curve_stats(inp_a, out_a),
        "mse": float(np.mean(np.square(residual))),
        "mae": float(np.mean(np.abs(residual))),
        "max_abs_error": float(np.max(np.abs(residual))),
        "correlation": correlation(inp_a, out_a),
        "freq_response_delta_db": spectral_delta_db(inp_a, out_a),
        "freq_response_delta_low_db": band_delta["low"],
        "freq_response_delta_mid_db": band_delta["mid"],
        "freq_response_delta_high_db": band_delta["high"],
        "transient_delta": transient_delta(inp_a, out_a),
        "overshoot_peak_delta": float(np.max(out_a) - np.max(inp_a)),
        "undershoot_peak_delta": float(np.min(out_a) - np.min(inp_a)),
        "channel_deviation_rms": float(np.sqrt(np.mean(np.square(out_a[:, 0] - out_a[:, 1])))),
        "soft_clip_dependency": softclip_dependency(inp_a, out_a),
        "reference_samples_used": int(len(inp_a)) if ref_a is not None else 0,
    }

    if ref_a is not None:
        assert ref_a is not None
        diff = out_a - ref_a
        ref_band_delta = spectral_band_delta_db(out_a, ref_a, fs)
        metrics.update(
            {
                "mse_vs_reference": float(np.mean(np.square(diff))),
                "mae_vs_reference": float(np.mean(np.abs(diff))),
                "max_abs_error_vs_reference": float(np.max(np.abs(diff))),
                "correlation_vs_reference": correlation(out_a, ref_a),
                "null_residual_rms_vs_reference": float(np.sqrt(np.mean(np.square(diff)))),
                "freq_response_delta_db_vs_reference": spectral_delta_db(out_a, ref_a),
                "freq_response_delta_low_db_vs_reference": ref_band_delta["low"],
                "freq_response_delta_mid_db_vs_reference": ref_band_delta["mid"],
                "freq_response_delta_high_db_vs_reference": ref_band_delta["high"],
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
                "freq_response_delta_low_db_vs_reference": None,
                "freq_response_delta_mid_db_vs_reference": None,
                "freq_response_delta_high_db_vs_reference": None,
                "transient_delta_vs_reference": None,
            }
        )
    return metrics


def _case_plausibility_penalty(row: dict[str, float | bool | int | None]) -> float:
    clip_penalty = 70.0 * int(bool(row["output_clip_l"])) + 70.0 * int(bool(row["output_clip_r"]))
    stereo_penalty = 80.0 * min(float(row["channel_deviation_rms"]), 1.0)
    gain_std_penalty = 3.0 * max(0.0, float(row["gain_curve_std_db"]) - 1.5)
    gain_jump_penalty = 8.0 * max(0.0, float(row["gain_curve_diff_p95_db"]) - 2.5)
    breathing_penalty = 4.0 * max(0.0, float(row["gain_curve_diff_std_db"]) - 1.2)
    spectral_penalty = 2.0 * max(0.0, float(row["freq_response_delta_db"]) - 5.5)
    spectral_penalty += 1.2 * max(0.0, float(row["freq_response_delta_low_db"]) - 6.0)
    spectral_penalty += 1.2 * max(0.0, float(row["freq_response_delta_mid_db"]) - 6.0)
    spectral_penalty += 1.2 * max(0.0, float(row["freq_response_delta_high_db"]) - 7.0)
    transient_penalty = 16.0 * max(0.0, float(row["transient_delta"]) - 0.25)
    overshoot_penalty = 20.0 * max(0.0, abs(float(row["overshoot_peak_delta"])) - 0.18)
    undershoot_penalty = 20.0 * max(0.0, abs(float(row["undershoot_peak_delta"])) - 0.18)
    softclip_penalty = 10.0 * float(row["soft_clip_dependency"])
    under_decode_penalty = 0.0
    if float(row["input_rms"]) > 0.015:
        under_decode_penalty += 220.0 * max(0.0, 0.06 - float(row["decode_action_ratio"]))
        under_decode_penalty += 36.0 * max(0.0, 0.35 - abs(float(row["gain_curve_mean_db"])))
        if float(row["correlation"]) > 0.995 and float(row["decode_action_ratio"]) < 0.08:
            under_decode_penalty += 35.0
    level_span_penalty = 5.0 * max(0.0, 12.0 - float(row["input_level_span_db"]))
    tracking_slope_penalty = 0.0
    if float(row["input_level_span_db"]) > 10.0 and float(row["input_rms"]) > 0.02:
        tracking_slope_penalty += 35.0 * max(0.0, float(row["gain_vs_input_slope"]) + 0.02)
        tracking_slope_penalty += 30.0 * max(0.0, -0.60 - float(row["gain_vs_input_slope"]))
        tracking_slope_penalty += 12.0 * max(0.0, 0.25 - float(row["gain_vs_input_r2"]))
    return (
        clip_penalty
        + stereo_penalty
        + gain_std_penalty
        + gain_jump_penalty
        + breathing_penalty
        + spectral_penalty
        + transient_penalty
        + overshoot_penalty
        + undershoot_penalty
        + softclip_penalty
        + under_decode_penalty
        + level_span_penalty
        + tracking_slope_penalty
    )


def evaluate_scores(metrics: list[dict[str, float | bool | int | None]]) -> dict[str, float | int]:
    plausibility_score = 1000.0
    reference_score = 0.0
    reference_cases = 0

    means = [float(row["gain_curve_mean_db"]) for row in metrics]
    level_spans = [float(row["input_level_span_db"]) for row in metrics]

    for row in metrics:
        plausibility_score -= _case_plausibility_penalty(row)
        if row["mse_vs_reference"] is not None:
            reference_cases += 1
            reference_score += 100.0
            reference_score -= 1000.0 * float(row["mse_vs_reference"])
            reference_score -= 1.7 * float(row["freq_response_delta_db_vs_reference"])
            reference_score -= 0.8 * float(row["freq_response_delta_low_db_vs_reference"])
            reference_score -= 0.8 * float(row["freq_response_delta_mid_db_vs_reference"])
            reference_score -= 1.0 * float(row["freq_response_delta_high_db_vs_reference"])
            reference_score -= 7.0 * float(row["transient_delta_vs_reference"])
            reference_score -= 45.0 * max(0.0, 0.90 - float(row["correlation_vs_reference"]))

    if means:
        mean_spread = float(np.max(means) - np.min(means))
        plausibility_score -= 5.0 * max(0.0, mean_spread - 16.0)
    if level_spans:
        insufficient_level_coverage = 25.0 * max(0.0, 14.0 - max(level_spans))
        plausibility_score -= insufficient_level_coverage

    total = plausibility_score + reference_score
    return {
        "score_total": float(total),
        "score_plausibility": float(plausibility_score),
        "score_reference": float(reference_score),
        "reference_case_count": int(reference_cases),
    }


def score_candidate(metrics: list[dict[str, float | bool | int | None]]) -> float:
    return float(evaluate_scores(metrics)["score_total"])


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


def music_like_dense(fs: int, seconds: float) -> np.ndarray:
    t = np.arange(int(fs * seconds)) / fs
    harmonics = (
        0.22 * np.sin(2.0 * np.pi * 82.41 * t)
        + 0.16 * np.sin(2.0 * np.pi * 164.81 * t)
        + 0.12 * np.sin(2.0 * np.pi * 329.63 * t)
        + 0.09 * np.sin(2.0 * np.pi * 659.25 * t)
        + 0.05 * np.sin(2.0 * np.pi * 2637.02 * t)
        + 0.04 * np.sin(2.0 * np.pi * 5274.04 * t)
    )
    envelope = 0.45 + 0.35 * np.sin(2.0 * np.pi * 0.45 * t) ** 2 + 0.20 * np.sin(2.0 * np.pi * 2.3 * t) ** 2
    return np.clip(harmonics * envelope, -1.0, 1.0)


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


def tone_level_matrix_cases(
    fs: int,
    seconds: float = 2.0,
    freqs_hz: tuple[float, ...] = (400.0, 1000.0, 3150.0, 10000.0),
    levels_db: tuple[float, ...] = (-30.0, -20.0, -12.0, -6.0),
) -> dict[str, np.ndarray]:
    cases: dict[str, np.ndarray] = {}
    for f in freqs_hz:
        for lv in levels_db:
            name = f"tone_{int(round(f))}hz_{str(int(round(lv))).replace('-', 'm')}db"
            cases[name] = ensure_stereo(tone(fs, seconds, f, lv))
    return cases


def build_cases(fs: int) -> dict[str, np.ndarray]:
    rng = np.random.default_rng(1234)
    silence = np.zeros(int(fs * 3.0))
    base = tone(fs, 3.0, 1000.0, -12.0)
    mismatch = np.column_stack([tone(fs, 3.0, 1000.0, -12.0), tone(fs, 3.0, 1300.0, -18.0)])
    out = {
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
        "music_like_dense": ensure_stereo(music_like_dense(fs, 4.0)),
        "hf_burst_train": ensure_stereo(hf_burst_train(fs, 3.0)),
        "bass_plus_hf": ensure_stereo(bass_plus_hf(fs, 4.0)),
        "transient_train": ensure_stereo(transient_train(fs, 3.0)),
        "fast_level_switches": ensure_stereo(fast_level_switches(fs, 3.0)),
    }
    out.update(tone_level_matrix_cases(fs))
    return out


def case_group(case_name: str) -> str:
    if case_name.startswith("tone_") and "hz_" in case_name and case_name.endswith("db"):
        return "tone_level_matrix"
    if case_name in {"music_like", "music_like_dense", "bass_plus_hf", "hf_burst_train", "transient_train", "fast_level_switches", "bursts", "envelope_steps"}:
        return "music_dynamic"
    if case_name in {"pink_noise", "white_noise", "log_sweep"}:
        return "broadband"
    if case_name in {"bass_heavy", "treble_heavy", "dc_rumble"}:
        return "spectral_extremes"
    return "general"




def select_tuning_cases(cases: dict[str, np.ndarray]) -> dict[str, np.ndarray]:
    preferred = [
        "sine_-24db",
        "sine_-6db",
        "log_sweep",
        "pink_noise",
        "bursts",
        "envelope_steps",
        "music_like",
        "hf_burst_train",
        "transient_train",
        "fast_level_switches",
        "tone_400hz_m12db",
        "tone_3150hz_m12db",
        "tone_10000hz_m12db",
    ]
    return {k: cases[k] for k in preferred if k in cases}

def parse_override_pairs(values: list[str]) -> dict[str, float]:
    out: dict[str, float] = {}
    for item in values:
        if "=" not in item:
            raise ValueError(f"Override must use key=value, got '{item}'")
        k, v = item.split("=", 1)
        out[k.strip()] = float(v.strip())
    return out


def _load_wav_if_exists(path: Path, fs: int) -> np.ndarray | None:
    if not path.exists():
        return None
    ref_fs, ref_audio = read_audio(path)
    if ref_fs != fs:
        return None
    return ref_audio


def load_reference_bundle(reference_dir: Path | None, case_name: str, fs: int) -> dict[str, np.ndarray | None]:
    if reference_dir is None:
        return {"source": None, "encoded": None, "reference_decode": None}

    roots = [reference_dir, reference_dir / "type2_cassette"]
    for root in roots:
        source = _load_wav_if_exists(root / f"{case_name}_source.wav", fs)
        encoded = _load_wav_if_exists(root / f"{case_name}_encoded.wav", fs)
        reference_decode = _load_wav_if_exists(root / f"{case_name}_reference_decode.wav", fs)
        if source is not None or encoded is not None or reference_decode is not None:
            return {"source": source, "encoded": encoded, "reference_decode": reference_decode}

    legacy = _load_wav_if_exists(reference_dir / f"{case_name}.wav", fs)
    return {"source": None, "encoded": None, "reference_decode": legacy}


def evaluate_candidate(
    profile_path: Path,
    fs: int,
    cases: dict[str, np.ndarray],
    overrides: dict[str, float] | None = None,
    reference_dir: Path | None = None,
) -> list[dict[str, float | bool | int | None | str]]:
    overrides = overrides or {}
    params = Params.from_profile(profile_path, **overrides)
    rows: list[dict[str, float | bool | int | None | str]] = []
    for name, inp in cases.items():
        ref_bundle = load_reference_bundle(reference_dir, name, fs)
        encoded = ref_bundle["encoded"]
        source = ref_bundle["source"]
        reference_decode = ref_bundle["reference_decode"]
        inp_eval = encoded if encoded is not None else inp
        ref_target = reference_decode if reference_decode is not None else source
        decoder = Decoder(fs, params)
        out = decoder.process(inp_eval)
        row: dict[str, float | bool | int | None | str] = {
            "case": name,
            "case_group": case_group(name),
            "reference_source_available": source is not None,
            "reference_encoded_available": encoded is not None,
            "reference_decode_available": reference_decode is not None,
            **{f"input_{k}": v for k, v in summarize_signal("input", inp_eval).items() if k != "name"},
            **{f"output_{k}": v for k, v in summarize_signal("output", out).items() if k != "name"},
            **compare(inp_eval, out, fs, ref_target),
            "input_clip_l": bool(decoder.input_clip[0]),
            "input_clip_r": bool(decoder.input_clip[1]),
            "output_clip_l": bool(decoder.output_clip[0]),
            "output_clip_r": bool(decoder.output_clip[1]),
            "reference_available": ref_target is not None,
            "reference_mode": (
                "encoded_to_reference_decode"
                if encoded is not None and reference_decode is not None
                else "encoded_to_source"
                if encoded is not None and source is not None
                else "output_to_reference_decode"
                if reference_decode is not None
                else "output_to_source"
                if source is not None
                else "none"
            ),
        }
        if source is not None:
            source_cmp = compare(out, source, fs, None)
            row["mse_vs_source"] = source_cmp["mse"]
            row["correlation_vs_source"] = source_cmp["correlation"]
        else:
            row["mse_vs_source"] = None
            row["correlation_vs_source"] = None
        rows.append(row)
    return rows


def run_tuning(profile_path: Path, tune_fs: int, final_fs: int, top_k: int, max_candidates: int = 24) -> dict[str, object]:
    base = json.loads(profile_path.read_text())["decoder"]
    grid = {
        "strength": [base["strength"], min(1.25, base["strength"] + 0.06)],
        "attack_ms": [base["attack_ms"], base["attack_ms"] + 1.0],
        "release_ms": [base["release_ms"], base["release_ms"] + 30.0],
        "deemph_db": [base["deemph_db"], base["deemph_db"] + 1.0],
        "sidechain_shelf_db": [base["sidechain_shelf_db"], base["sidechain_shelf_db"] + 2.0],
        "headroom_db": [base["headroom_db"], min(6.0, base["headroom_db"] + 0.5)],
    }

    coarse_cases_all = build_cases(tune_fs)
    final_cases_all = build_cases(final_fs)
    coarse_cases = select_tuning_cases(coarse_cases_all)
    final_cases = select_tuning_cases(final_cases_all)
    if "bass_plus_hf" in final_cases_all:
        final_cases["bass_plus_hf"] = final_cases_all["bass_plus_hf"]
    if "stereo_different" in final_cases_all:
        final_cases["stereo_different"] = final_cases_all["stereo_different"]

    coarse_results = []
    keys = list(grid.keys())
    for idx, values in enumerate(itertools.product(*(grid[k] for k in keys))):
        if idx >= max(1, max_candidates):
            break
        cand = {k: float(v) for k, v in zip(keys, values)}
        rows = evaluate_candidate(profile_path, tune_fs, coarse_cases, cand)
        score = evaluate_scores(rows)
        coarse_results.append({"params": cand, **score})

    coarse_results.sort(key=lambda x: float(x["score_total"]), reverse=True)
    finalists = coarse_results[: max(1, top_k)]

    final_results = []
    for item in finalists:
        cand = item["params"]
        rows = evaluate_candidate(profile_path, final_fs, final_cases, cand)
        score = evaluate_scores(rows)
        final_results.append({"params": cand, **score, "coarse_score_total": item["score_total"]})

    final_results.sort(key=lambda x: float(x["score_total"]), reverse=True)
    return {
        "method": "two_stage_tuning",
        "selection_basis": "final_stage_only",
        "coarse_stage_role": "prescan_only_not_final_decision",
        "coarse_fs": tune_fs,
        "final_fs": final_fs,
        "candidate_limit": max(1, max_candidates),
        "candidate_count": len(coarse_results),
        "top_k_finalists": len(final_results),
        "coarse_ranking": coarse_results,
        "final_ranking": final_results,
        "best": final_results[0] if final_results else None,
    }


def run_detector_study(profile_path: Path, fs: int) -> dict[str, object]:
    cases = select_tuning_cases(build_cases(fs))
    energy = evaluate_candidate(profile_path, fs, cases, {"detector_mode": "energy"})
    rms = evaluate_candidate(profile_path, fs, cases, {"detector_mode": "rms", "detector_rms_ms": 6.0})
    return {
        "fs": fs,
        "energy": evaluate_scores(energy),
        "rms": evaluate_scores(rms),
        "cpu_note": "rms mode adds one extra detector IIR state/update per sample and channel in simulator.",
        "realtime_note": "Expected to be practical on Teensy 4.1, but firmware-side CPU headroom must still be validated on hardware telemetry.",
    }


def summarize_by_group(rows: list[dict[str, float | bool | int | None | str]]) -> dict[str, dict[str, float | int]]:
    grouped: dict[str, list[dict[str, float | bool | int | None]]] = {}
    for row in rows:
        group = str(row.get("case_group", "general"))
        grouped.setdefault(group, []).append(dict(row))

    summary: dict[str, dict[str, float | int]] = {}
    for group, items in grouped.items():
        score = evaluate_scores(items)
        summary[group] = {
            "cases": len(items),
            "score_total": float(score["score_total"]),
            "score_plausibility": float(score["score_plausibility"]),
            "score_reference": float(score["score_reference"]),
            "reference_cases": int(sum(1 for x in items if bool(x.get("reference_available", False)))),
        }
    return summary


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-dir", type=Path, default=Path("artifacts/harness"))
    ap.add_argument("--profile", type=Path, default=PROFILE_PATH)
    ap.add_argument("--reference-dir", type=Path)
    ap.add_argument("--write-wavs", action="store_true")
    ap.add_argument("--override", action="append", default=[], help="Decoder override key=value (repeatable)")
    ap.add_argument("--tune", action="store_true", help="Run a compact grid search and report the best candidate.")
    ap.add_argument("--tune-fs", type=int, default=4000, help="Coarse sample rate for tuning search workload (default: 4000)")
    ap.add_argument("--tune-final-fs", type=int, default=44100, help="Final rerank sample rate for tuning (default: 44100)")
    ap.add_argument("--tune-top-k", type=int, default=6, help="Number of coarse finalists to rerank at final sample rate.")
    ap.add_argument("--tune-max-candidates", type=int, default=24, help="Limit of coarse candidates to evaluate for compact tuning runs.")
    ap.add_argument("--detector-study", action="store_true", help="Compare energy-like detector and RMS-nearer detector in simulator.")
    args = ap.parse_args()

    profile = json.loads(args.profile.read_text())
    fs = int(profile["sample_rate_hz"])
    cases = build_cases(fs)
    user_overrides = parse_override_pairs(args.override)
    args.out_dir.mkdir(parents=True, exist_ok=True)

    results = evaluate_candidate(args.profile, fs, cases, user_overrides, args.reference_dir)
    if args.write_wavs:
        params = Params.from_profile(args.profile, **user_overrides)
        for name, inp in cases.items():
            decoder = Decoder(fs, params)
            out = decoder.process(inp)
            write_audio(args.out_dir / f"{name}_input.wav", fs, inp)
            write_audio(args.out_dir / f"{name}_output.wav", fs, out)

    (args.out_dir / "metrics.json").write_text(json.dumps(results, indent=2))
    score_summary = evaluate_scores(results)
    summary = {
        **score_summary,
        "cases": len(results),
        "overrides": user_overrides,
        "reference_cases": int(sum(1 for row in results if bool(row["reference_available"]))),
        "case_groups": summarize_by_group(results),
        "cassette_primary_note": "tone_level_matrix + music_dynamic + broadband are the primary offline cassette-oriented groups.",
    }
    (args.out_dir / "summary.json").write_text(json.dumps(summary, indent=2))
    try:
        import pandas as pd

        pd.DataFrame(results).to_csv(args.out_dir / "metrics.csv", index=False)
    except ImportError:
        pass

    if args.tune:
        tuning = run_tuning(
            profile_path=args.profile,
            tune_fs=int(max(1000, args.tune_fs)),
            final_fs=int(max(4000, args.tune_final_fs)),
            top_k=int(max(1, args.tune_top_k)),
            max_candidates=int(max(1, args.tune_max_candidates)),
        )
        (args.out_dir / "tuning_best.json").write_text(json.dumps(tuning, indent=2))
    if args.detector_study:
        study = run_detector_study(args.profile, fs=fs)
        (args.out_dir / "detector_study.json").write_text(json.dumps(study, indent=2))


if __name__ == "__main__":
    main()
