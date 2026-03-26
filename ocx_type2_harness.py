#!/usr/bin/env python3
"""Automated offline validation harness for the OCX Type 2 decoder."""

from __future__ import annotations

import argparse
import itertools
import json
from pathlib import Path
import re
import shutil
import urllib.request

import numpy as np

from ocx_type2_wav_sim import (
    PROFILE_PATH,
    Decoder,
    DecoderParams,
    Encoder,
    EncoderParams,
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
    n = min(n, 16384)
    a_mono = a_mono[:n]
    b_mono = b_mono[:n]
    A = np.fft.rfft(a_mono * np.hanning(n))
    B = np.fft.rfft(b_mono * np.hanning(n))
    mag_a = 20.0 * np.log10(np.maximum(np.abs(A), 1.0e-12))
    mag_b = 20.0 * np.log10(np.maximum(np.abs(B), 1.0e-12))
    return float(np.sqrt(np.mean(np.square(mag_b - mag_a))))


def spectral_band_delta_db(a: np.ndarray, b: np.ndarray, fs: int, low_hz: float, high_hz: float) -> float:
    a_mono = np.mean(a, axis=1)
    b_mono = np.mean(b, axis=1)
    n = min(len(a_mono), len(b_mono))
    if n == 0:
        return 0.0
    n = min(n, 16384)
    a_mono = a_mono[:n]
    b_mono = b_mono[:n]
    win = np.hanning(n)
    A = np.fft.rfft(a_mono * win)
    B = np.fft.rfft(b_mono * win)
    freqs = np.fft.rfftfreq(n, 1.0 / fs)
    band = (freqs >= low_hz) & (freqs < high_hz)
    if not np.any(band):
        return 0.0
    mag_a = 20.0 * np.log10(np.maximum(np.abs(A[band]), 1.0e-12))
    mag_b = 20.0 * np.log10(np.maximum(np.abs(B[band]), 1.0e-12))
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


def overshoot_undershoot_delta(a: np.ndarray, b: np.ndarray) -> tuple[float, float]:
    n = min(len(a), len(b))
    if n == 0:
        return 0.0, 0.0
    ref = a[:n]
    out = b[:n]
    err = out - ref
    return float(np.max(err)), float(abs(np.min(err)))


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


def compare(
    inp: np.ndarray,
    out: np.ndarray,
    fs: int,
    ref: np.ndarray | None = None,
    source_target: np.ndarray | None = None,
) -> dict[str, float | int | None]:
    inp = ensure_stereo(inp)
    out = ensure_stereo(out)
    inp_a, out_a, ref_a = align_lengths(inp, out, ref)
    assert inp_a is not None
    assert out_a is not None

    residual = out_a - inp_a
    input_rms = float(np.sqrt(np.mean(np.square(inp_a))))
    output_rms = float(np.sqrt(np.mean(np.square(out_a))))
    residual_rms = float(np.sqrt(np.mean(np.square(residual))))
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
        "freq_delta_low_db": spectral_band_delta_db(inp_a, out_a, fs, 30.0, 250.0),
        "freq_delta_mid_db": spectral_band_delta_db(inp_a, out_a, fs, 250.0, 4000.0),
        "freq_delta_high_db": spectral_band_delta_db(inp_a, out_a, fs, 4000.0, 16000.0),
        "transient_delta": transient_delta(inp_a, out_a),
        "overshoot_delta": overshoot_undershoot_delta(inp_a, out_a)[0],
        "undershoot_delta": overshoot_undershoot_delta(inp_a, out_a)[1],
        "channel_deviation_rms": float(np.sqrt(np.mean(np.square(out_a[:, 0] - out_a[:, 1])))),
        "soft_clip_dependency": softclip_dependency(inp_a, out_a),
        "reference_samples_used": int(len(inp_a)) if ref_a is not None else 0,
    }

    if ref_a is not None:
        assert ref_a is not None
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
    if source_target is not None:
        src_a, out_s = align_lengths(source_target, out_a)
        assert src_a is not None
        assert out_s is not None
        src_diff = out_s - src_a
        metrics.update(
            {
                "mse_vs_source": float(np.mean(np.square(src_diff))),
                "mae_vs_source": float(np.mean(np.abs(src_diff))),
                "correlation_vs_source": correlation(out_s, src_a),
                "freq_response_delta_db_vs_source": spectral_delta_db(out_s, src_a),
            }
        )
    else:
        metrics.update(
            {
                "mse_vs_source": None,
                "mae_vs_source": None,
                "correlation_vs_source": None,
                "freq_response_delta_db_vs_source": None,
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
    transient_penalty = 16.0 * max(0.0, float(row["transient_delta"]) - 0.25)
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

    cassette_primary_rows = [row for row in metrics if str(row.get("case_group", "")).startswith("cassette")]
    for row in metrics:
        plausibility_score -= _case_plausibility_penalty(row)
        if str(row.get("case_group", "")) in {"cassette_tone_matrix", "cassette_reference"}:
            plausibility_score -= 1.5 * max(0.0, float(row["freq_delta_mid_db"]) - 3.5)
            plausibility_score -= 3.0 * max(0.0, float(row["overshoot_delta"]) - 0.15)
            plausibility_score -= 3.0 * max(0.0, float(row["undershoot_delta"]) - 0.15)
        if str(row.get("case_group", "")) == "music_like":
            plausibility_score -= 1.8 * max(0.0, float(row["freq_delta_high_db"]) - 4.0)
            plausibility_score -= 1.6 * max(0.0, float(row["freq_delta_low_db"]) - 4.0)
            plausibility_score -= 12.0 * max(0.0, float(row["gain_curve_diff_std_db"]) - 1.0)
        if row["mse_vs_reference"] is not None:
            reference_cases += 1
            source_type = str(row.get("reference_source_type", "synthetic"))
            trust = str(row.get("reference_trust_level", "approximate"))
            real_weight = 1.4 if source_type == "real" else 1.0
            trust_weight = {"high": 1.2, "medium": 1.0, "approximate": 0.85, "low": 0.7}.get(trust, 1.0)
            w = real_weight * trust_weight
            reference_score += 100.0 * w
            reference_score -= 1000.0 * w * float(row["mse_vs_reference"])
            reference_score -= 1.7 * w * float(row["freq_response_delta_db_vs_reference"])
            reference_score -= 7.0 * w * float(row["transient_delta_vs_reference"])
            reference_score -= 45.0 * w * max(0.0, 0.90 - float(row["correlation_vs_reference"]))

    if means:
        mean_spread = float(np.max(means) - np.min(means))
        plausibility_score -= 5.0 * max(0.0, mean_spread - 16.0)
    if level_spans:
        insufficient_level_coverage = 25.0 * max(0.0, 14.0 - max(level_spans))
        plausibility_score -= insufficient_level_coverage

    total = plausibility_score + reference_score
    matrix_rows = [row for row in metrics if row.get("matrix_frequency_hz") is not None and row.get("matrix_level_db") is not None]
    tone_matrix_coverage = float(len(matrix_rows)) / float(len(CASSETTE_PRIMARY_FREQS_HZ) * len(CASSETTE_PRIMARY_LEVELS_DB))
    if matrix_rows:
        by_freq: dict[float, list[float]] = {}
        for row in matrix_rows:
            by_freq.setdefault(float(row["matrix_frequency_hz"]), []).append(float(row["gain_curve_mean_db"]))
        freq_spreads = [max(vals) - min(vals) for vals in by_freq.values() if vals]
        matrix_gain_spread_db = float(np.mean(freq_spreads)) if freq_spreads else 0.0
    else:
        matrix_gain_spread_db = 0.0
    return {
        "score_total": float(total),
        "score_plausibility": float(plausibility_score),
        "score_reference": float(reference_score),
        "reference_case_count": int(reference_cases),
        "cassette_primary_case_count": int(len(cassette_primary_rows)),
        "tone_matrix_coverage": tone_matrix_coverage,
        "tone_matrix_gain_spread_db": matrix_gain_spread_db,
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


CASSETTE_PRIMARY_FREQS_HZ = (400.0, 1000.0, 3150.0, 10000.0)
CASSETTE_PRIMARY_LEVELS_DB = (-30.0, -24.0, -18.0, -12.0, -6.0)


def _synthetic_case_specs(fs: int) -> dict[str, dict[str, object]]:
    rng = np.random.default_rng(1234)
    silence = np.zeros(int(fs * 3.0))
    base = tone(fs, 3.0, 1000.0, -12.0)
    mismatch = np.column_stack([tone(fs, 3.0, 1000.0, -12.0), tone(fs, 3.0, 1300.0, -18.0)])
    specs: dict[str, dict[str, object]] = {
        "silence": {"input": ensure_stereo(silence), "group": "sanity"},
        "sine_-24db": {"input": ensure_stereo(tone(fs, 3.0, 1000.0, -24.0)), "group": "legacy_tone"},
        "sine_-12db": {"input": ensure_stereo(base), "group": "legacy_tone"},
        "sine_-6db": {"input": ensure_stereo(tone(fs, 3.0, 1000.0, -6.0)), "group": "legacy_tone"},
        "log_sweep": {"input": ensure_stereo(log_sweep(fs, 5.0, 20.0, 20000.0, -18.0)), "group": "broadband"},
        "pink_noise": {"input": ensure_stereo(colored_noise(fs, 4.0, "pink", -18.0, rng)), "group": "broadband"},
        "white_noise": {"input": ensure_stereo(colored_noise(fs, 4.0, "white", -20.0, rng)), "group": "broadband"},
        "bursts": {"input": ensure_stereo(bursts(fs, 3.0, -8.0)), "group": "dynamic"},
        "envelope_steps": {"input": ensure_stereo(envelope_steps(fs, 3.0)), "group": "dynamic"},
        "stereo_identical": {"input": ensure_stereo(tone(fs, 3.0, 500.0, -16.0) + tone(fs, 3.0, 3000.0, -22.0)), "group": "sanity"},
        "stereo_different": {"input": mismatch, "group": "sanity"},
        "bass_heavy": {"input": ensure_stereo(bass_heavy(fs, 4.0)), "group": "music_like"},
        "treble_heavy": {"input": ensure_stereo(treble_heavy(fs, 4.0)), "group": "music_like"},
        "clipped_input": {"input": ensure_stereo(clipped_source(fs, 3.0)), "group": "edge"},
        "too_quiet": {"input": ensure_stereo(tone(fs, 3.0, 1000.0, -42.0)), "group": "edge"},
        "too_hot": {"input": ensure_stereo(np.clip(tone(fs, 3.0, 1000.0, -1.0) * 1.2, -1.0, 1.0)), "group": "edge"},
        "dc_rumble": {"input": ensure_stereo(rumble(fs, 4.0)), "group": "edge"},
        "music_like": {"input": ensure_stereo(music_like(fs, 4.0)), "group": "music_like"},
        "hf_burst_train": {"input": ensure_stereo(hf_burst_train(fs, 3.0)), "group": "dynamic"},
        "bass_plus_hf": {"input": ensure_stereo(bass_plus_hf(fs, 4.0)), "group": "music_like"},
        "transient_train": {"input": ensure_stereo(transient_train(fs, 3.0)), "group": "dynamic"},
        "fast_level_switches": {"input": ensure_stereo(fast_level_switches(fs, 3.0)), "group": "dynamic"},
    }
    for freq in CASSETTE_PRIMARY_FREQS_HZ:
        for level in CASSETTE_PRIMARY_LEVELS_DB:
            name = f"cassette_tone_{int(freq)}hz_{int(level)}db"
            specs[name] = {
                "input": ensure_stereo(tone(fs, 2.5, freq, level)),
                "group": "cassette_tone_matrix",
                "frequency_hz": float(freq),
                "level_db": float(level),
            }
    specs["cassette_two_tone_400_3150"] = {
        "input": ensure_stereo(tone(fs, 3.0, 400.0, -14.0) + tone(fs, 3.0, 3150.0, -16.0)),
        "group": "music_like",
    }
    specs["cassette_multi_tone_bass_hf"] = {
        "input": ensure_stereo(tone(fs, 3.5, 80.0, -13.0) + tone(fs, 3.5, 1000.0, -20.0) + tone(fs, 3.5, 10000.0, -18.0)),
        "group": "music_like",
    }
    return specs




REF_REAL_DIRNAME = "type2_cassette_real"
REF_SYNTH_DIRNAME = "type2_cassette_synth"
KNOWN_MUSIC_CANDIDATES = (
    {
        "filename": "musik_enc.wav",
        "case_name": "musik_enc_candidate",
        "source_type": "provided",
        "category": "cassette_music_candidate",
        "license": "unspecified",
        "origin": "User-provided local file (path unknown in repo state)",
        "notes": (
            "Encoded candidate from user-provided material. No documented encoder chain in repository; "
            "treat as cassette-primary candidate, not hard gold reference."
        ),
        "trust_level": "candidate_only",
    },
    {
        "filename": "musicfox_shopping_street.mp3",
        "case_name": "musicfox_shopping_street_candidate",
        "source_type": "provided",
        "category": "cassette_music_candidate",
        "license": "unspecified",
        "origin": "User-provided local file (path unknown in repo state)",
        "notes": (
            "Lossy MP3 candidate for stress/listening validation only. "
            "Not a primary decoder reference path."
        ),
        "trust_level": "low",
    },
)


def resample_audio_linear(audio: np.ndarray, fs_in: int, fs_out: int) -> np.ndarray:
    arr = ensure_stereo(audio)
    if fs_in == fs_out:
        return arr
    if len(arr) == 0:
        return arr
    duration = len(arr) / float(fs_in)
    out_len = max(1, int(round(duration * fs_out)))
    x_in = np.linspace(0.0, duration, len(arr), endpoint=False)
    x_out = np.linspace(0.0, duration, out_len, endpoint=False)
    out = np.zeros((out_len, 2), dtype=np.float64)
    for ch in range(2):
        out[:, ch] = np.interp(x_out, x_in, arr[:, ch])
    return ensure_stereo(out)


def _approx_type2_encode(source: np.ndarray, fs: int, strength: float = 1.0, reference_db: float = -18.0) -> np.ndarray:
    src = ensure_stereo(source)
    p = EncoderParams.from_profile(
        PROFILE_PATH,
        strength=max(0.0, min(1.25, 0.45 * strength)),
        reference_db=reference_db,
    )
    return Encoder(fs, p).process(src)


def _write_ref_case(
    root: Path,
    case_name: str,
    category: str,
    source: np.ndarray,
    encoded: np.ndarray,
    reference_decode: np.ndarray,
    source_type: str,
    license_name: str,
    origin: str,
    notes: str,
    trust_level: str,
    cassette_priority: bool = True,
    fs: int = 44_100,
) -> dict[str, object]:
    source = ensure_stereo(source)
    encoded = ensure_stereo(encoded)
    reference_decode = ensure_stereo(reference_decode)
    write_audio(root / f"{case_name}_source.wav", fs, source)
    write_audio(root / f"{case_name}_encoded.wav", fs, encoded)
    write_audio(root / f"{case_name}_reference_decode.wav", fs, reference_decode)
    meta = {
        "case_name": case_name,
        "category": category,
        "source_type": source_type,
        "cassette_priority": bool(cassette_priority),
        "license": license_name,
        "origin": origin,
        "notes": notes,
        "trust_level": trust_level,
    }
    (root / f"{case_name}.json").write_text(json.dumps(meta, indent=2))
    return meta


def generate_synthetic_reference_pack(reference_root: Path, profile_path: Path = PROFILE_PATH, fs: int = 44_100) -> dict[str, object]:
    synth_root = reference_root / REF_SYNTH_DIRNAME
    if synth_root.exists():
        shutil.rmtree(synth_root)
    synth_root.mkdir(parents=True, exist_ok=True)
    params = Params.from_profile(profile_path)
    decoder = Decoder(fs, params)
    specs = _synthetic_case_specs(fs)

    created = []
    for name, spec in specs.items():
        src = ensure_stereo(np.asarray(spec["input"]))
        enc = _approx_type2_encode(src, fs=fs, strength=1.0, reference_db=params.reference_db)
        decoder.reset()
        ref_dec = decoder.process(enc)
        category = str(spec.get("group", "synthetic"))
        note = (
            "Synthetic cassette-priority case generated from internal signal model and an "
            "approximate Type-II-like encoder. Not historically verified as original dbx standard behavior."
        )
        created.append(
            _write_ref_case(
                synth_root,
                case_name=name,
                category=category,
                source=src,
                encoded=enc,
                reference_decode=ref_dec,
                source_type="synthetic",
                license_name="CC0-1.0",
                origin="OpenCompanderX synthetic generator",
                notes=note,
                trust_level="approximate",
                cassette_priority=bool(str(category).startswith("cassette") or category in {"music_like", "dynamic", "broadband"}),
                fs=fs,
            )
        )
    index = {
        "generated_utc": "static-local-run",
        "profile": str(profile_path),
        "sample_rate_hz": fs,
        "case_count": len(created),
        "cases": created,
        "encoder_disclaimer": "Synthetic only. Internal approximate encoder is a methodology tool, not proof of historical dbx Type-II conformance.",
    }
    (synth_root / "index.json").write_text(json.dumps(index, indent=2))
    return index


def load_reference_metadata(case_base: str, encoded_path: Path) -> dict[str, object]:
    meta_path = encoded_path.with_name(f"{case_base}.json")
    if not meta_path.exists():
        return {
            "case_name": case_base,
            "category": "unknown",
            "source_type": "unknown",
            "cassette_priority": False,
            "license": "unspecified",
            "origin": "unspecified",
            "notes": "metadata missing",
            "trust_level": "unknown",
        }
    try:
        payload = json.loads(meta_path.read_text())
        if isinstance(payload, dict):
            return payload
    except json.JSONDecodeError:
        pass
    return {
        "case_name": case_base,
        "category": "invalid",
        "source_type": "unknown",
        "cassette_priority": False,
        "license": "unspecified",
        "origin": "invalid-metadata",
        "notes": "metadata parse failed",
        "trust_level": "low",
    }


def index_real_references(reference_root: Path) -> dict[str, object]:
    real_root = reference_root / REF_REAL_DIRNAME
    real_root.mkdir(parents=True, exist_ok=True)
    cases = []
    for encoded in sorted(real_root.glob("*_encoded.wav")):
        base = re.sub(r"_encoded$", "", encoded.stem)
        meta = load_reference_metadata(base, encoded)
        meta["has_source"] = encoded.with_name(f"{base}_source.wav").exists()
        meta["has_reference_decode"] = encoded.with_name(f"{base}_reference_decode.wav").exists()
        cases.append(meta)
    report = {"root": str(real_root), "case_count": len(cases), "cases": cases}
    (real_root / "index.json").write_text(json.dumps(report, indent=2))
    return report


def fetch_real_references_from_manifest(reference_root: Path, manifest_path: Path) -> dict[str, object]:
    real_root = reference_root / REF_REAL_DIRNAME
    real_root.mkdir(parents=True, exist_ok=True)
    manifest = json.loads(manifest_path.read_text())
    entries = manifest.get("files", []) if isinstance(manifest, dict) else []
    downloaded = []
    skipped = []
    for item in entries:
        if not isinstance(item, dict):
            continue
        url = str(item.get("url", "")).strip()
        rel = str(item.get("path", "")).strip()
        if not url or not rel:
            continue
        dest = real_root / rel
        if dest.exists():
            skipped.append(rel)
            continue
        dest.parent.mkdir(parents=True, exist_ok=True)
        urllib.request.urlretrieve(url, dest)
        downloaded.append(rel)
    return {"manifest": str(manifest_path), "downloaded": downloaded, "skipped": skipped, "downloaded_count": len(downloaded)}


def prepare_known_music_candidates(reference_root: Path, fs: int, search_root: Path | None = None) -> dict[str, object]:
    real_root = reference_root / REF_REAL_DIRNAME
    real_root.mkdir(parents=True, exist_ok=True)
    search_root = search_root or Path.cwd()
    prepared: list[dict[str, object]] = []
    missing: list[str] = []
    decode_errors: list[str] = []
    for item in KNOWN_MUSIC_CANDIDATES:
        src_path = search_root / str(item["filename"])
        if not src_path.exists():
            found = sorted(search_root.glob(f"**/{item['filename']}"))
            if found:
                src_path = found[0]
        if not src_path.exists():
            missing.append(str(src_path))
            continue
        try:
            src_fs, src_audio = read_audio(src_path)
        except RuntimeError:
            decode_errors.append(str(src_path))
            continue
        audio_44k1 = resample_audio_linear(src_audio, src_fs, fs)
        case_name = str(item["case_name"])
        encoded_name = f"{case_name}_encoded.wav"
        write_audio(real_root / encoded_name, fs, audio_44k1)
        meta = {
            "case_name": case_name,
            "category": str(item["category"]),
            "source_type": str(item["source_type"]),
            "cassette_priority": True,
            "license": str(item["license"]),
            "origin": str(item["origin"]),
            "notes": str(item["notes"]),
            "trust_level": str(item["trust_level"]),
            "encoded_candidate_only": True,
            "source_filename": str(item["filename"]),
            "source_sample_rate_hz": int(src_fs),
            "prepared_sample_rate_hz": int(fs),
        }
        (real_root / f"{case_name}.json").write_text(json.dumps(meta, indent=2))
        prepared.append(meta)
    report = {
        "sample_rate_hz": fs,
        "prepared_count": len(prepared),
        "prepared": prepared,
        "missing": missing,
        "decode_errors": decode_errors,
    }
    (real_root / "prepared_music_candidates.json").write_text(json.dumps(report, indent=2))
    return report


def _match_source_type(spec: dict[str, object], source_types: set[str]) -> bool:
    if "all" in source_types:
        return True
    st = str(spec.get("source_type", "synthetic"))
    return st in source_types


def _is_cassette_priority(spec: dict[str, object]) -> bool:
    return bool(spec.get("cassette_priority", False))


def discover_reference_case_specs(reference_dir: Path, fs: int) -> dict[str, dict[str, object]]:
    specs: dict[str, dict[str, object]] = {}
    search_roots = [reference_dir / REF_REAL_DIRNAME, reference_dir / REF_SYNTH_DIRNAME, reference_dir / "type2_cassette", reference_dir]
    for root in search_roots:
        if not root.exists():
            continue
        for encoded in sorted(root.glob("*_encoded.wav")):
            base = re.sub(r"_encoded$", "", encoded.stem)
            source = encoded.with_name(f"{base}_source.wav")
            if not source.exists():
                continue
            ref_decode = encoded.with_name(f"{base}_reference_decode.wav")
            enc_fs, enc_audio = read_audio(encoded)
            src_fs, src_audio = read_audio(source)
            enc_audio = resample_audio_linear(enc_audio, enc_fs, fs)
            src_audio = resample_audio_linear(src_audio, src_fs, fs)
            metadata = load_reference_metadata(base, encoded)
            case_name = f"ref_{base}"
            spec: dict[str, object] = {
                "input": enc_audio,
                "source_target": src_audio,
                "group": "cassette_reference",
                "reference_layout": "pair_encoded_source",
                "reference_case_base": base,
                "source_type": str(metadata.get("source_type", "unknown")),
                "cassette_priority": bool(metadata.get("cassette_priority", False)),
                "license": str(metadata.get("license", "unspecified")),
                "origin": str(metadata.get("origin", "unspecified")),
                "notes": str(metadata.get("notes", "")),
                "trust_level": str(metadata.get("trust_level", "unknown")),
                "category": str(metadata.get("category", "cassette_reference")),
                "source_sample_rate_hz": int(src_fs),
                "encoded_sample_rate_hz": int(enc_fs),
                "prepared_sample_rate_hz": int(fs),
            }
            if ref_decode.exists():
                ref_fs, ref_audio = read_audio(ref_decode)
                spec["reference_decode"] = resample_audio_linear(ref_audio, ref_fs, fs)
            specs[case_name] = spec
        for encoded in sorted(root.glob("*_encoded.wav")):
            base = re.sub(r"_encoded$", "", encoded.stem)
            source = encoded.with_name(f"{base}_source.wav")
            if source.exists():
                continue
            enc_fs, enc_audio = read_audio(encoded)
            enc_audio = resample_audio_linear(enc_audio, enc_fs, fs)
            metadata = load_reference_metadata(base, encoded)
            if not bool(metadata.get("encoded_candidate_only", False)):
                continue
            case_name = f"cand_{base}"
            specs[case_name] = {
                "input": enc_audio,
                "group": "cassette_music_candidate",
                "reference_layout": "encoded_only_candidate",
                "reference_case_base": base,
                "source_type": str(metadata.get("source_type", "provided")),
                "cassette_priority": bool(metadata.get("cassette_priority", True)),
                "license": str(metadata.get("license", "unspecified")),
                "origin": str(metadata.get("origin", "unspecified")),
                "notes": str(metadata.get("notes", "")),
                "trust_level": str(metadata.get("trust_level", "candidate_only")),
                "category": str(metadata.get("category", "cassette_music_candidate")),
                "encoded_sample_rate_hz": int(enc_fs),
                "prepared_sample_rate_hz": int(fs),
            }
    return specs


def build_case_specs(
    fs: int,
    reference_dir: Path | None = None,
    source_type_filter: set[str] | None = None,
    cassette_priority_only: bool = False,
) -> dict[str, dict[str, object]]:
    specs = _synthetic_case_specs(fs)
    for spec in specs.values():
        group = str(spec.get("group", "synthetic"))
        spec["source_type"] = "synthetic"
        spec["cassette_priority"] = bool(group.startswith("cassette") or group in {"music_like", "dynamic", "broadband"})
        spec["license"] = "CC0-1.0"
        spec["origin"] = "OpenCompanderX synthetic harness"
        spec["notes"] = "Generated in harness; synthetic approximative signal case."
        spec["trust_level"] = "approximate"
        spec["category"] = group
    if reference_dir is not None:
        specs.update(discover_reference_case_specs(reference_dir, fs))
    if source_type_filter:
        specs = {k: v for k, v in specs.items() if _match_source_type(v, source_type_filter)}
    if cassette_priority_only:
        specs = {k: v for k, v in specs.items() if _is_cassette_priority(v)}
    return specs


def build_cases(fs: int) -> dict[str, np.ndarray]:
    return {k: v["input"] for k, v in build_case_specs(fs).items()}




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


def load_reference(reference_dir: Path | None, case_name: str, fs: int) -> np.ndarray | None:
    if reference_dir is None:
        return None
    ref_path = reference_dir / f"{case_name}.wav"
    if not ref_path.exists():
        return None
    ref_fs, ref_audio = read_audio(ref_path)
    if ref_fs != fs:
        return None
    return ref_audio


def evaluate_candidate(
    profile_path: Path,
    fs: int,
    cases: dict[str, np.ndarray] | dict[str, dict[str, object]],
    mode: str = "decode",
    overrides: dict[str, float] | None = None,
    reference_dir: Path | None = None,
    preset: str = "universal",
) -> list[dict[str, float | bool | int | None | str]]:
    overrides = overrides or {}
    params = Params.from_profile(profile_path, preset=preset, **overrides)
    case_specs: dict[str, dict[str, object]]
    first_value = next(iter(cases.values())) if cases else None
    if isinstance(first_value, dict):
        case_specs = cases  # type: ignore[assignment]
    else:
        case_specs = {name: {"input": audio, "group": "legacy"} for name, audio in cases.items()}  # type: ignore[union-attr]
    rows: list[dict[str, float | bool | int | None | str]] = []
    for name, spec in case_specs.items():
        inp = ensure_stereo(np.asarray(spec["input"]))
        if mode == "decode":
            decoder = Decoder(fs, params)
            out = decoder.process(inp)
            clip_src = decoder
            cmp_in = inp
            embedded_ref = spec.get("reference_decode")
            ref = ensure_stereo(np.asarray(embedded_ref)) if embedded_ref is not None else load_reference(reference_dir, name, fs)
        elif mode == "encode":
            encoder = Encoder(fs, EncoderParams.from_profile(profile_path, preset=preset, **overrides))
            out = encoder.process(inp)
            clip_src = encoder
            cmp_in = inp
            ref = None
        elif mode == "roundtrip":
            encoder = Encoder(fs, EncoderParams.from_profile(profile_path, preset=preset))
            decoder = Decoder(fs, params)
            encoded = encoder.process(inp)
            out = decoder.process(encoded)
            clip_src = decoder
            cmp_in = inp
            ref = None
        else:
            raise ValueError(f"Unsupported mode: {mode}")
        source_target = spec.get("source_target")
        source = ensure_stereo(np.asarray(source_target)) if source_target is not None else None
        row: dict[str, float | bool | int | None | str] = {
            "case": name,
            "mode": mode,
            "case_group": str(spec.get("group", "legacy")),
            **{f"input_{k}": v for k, v in summarize_signal("input", cmp_in).items() if k != "name"},
            **{f"output_{k}": v for k, v in summarize_signal("output", out).items() if k != "name"},
            **compare(cmp_in, out, fs, ref, source),
            "input_clip_l": bool(clip_src.input_clip[0]),
            "input_clip_r": bool(clip_src.input_clip[1]),
            "output_clip_l": bool(clip_src.output_clip[0]),
            "output_clip_r": bool(clip_src.output_clip[1]),
            "reference_available": ref is not None,
            "source_available": source is not None,
            "reference_source_type": str(spec.get("source_type", "synthetic")),
            "reference_trust_level": str(spec.get("trust_level", "approximate")),
            "reference_license": str(spec.get("license", "CC0-1.0")),
            "reference_origin": str(spec.get("origin", "OpenCompanderX synthetic harness")),
            "reference_notes": str(spec.get("notes", "")),
            "cassette_priority": bool(spec.get("cassette_priority", False)),
            "case_category": str(spec.get("category", spec.get("group", "unknown"))),
        }
        if "frequency_hz" in spec:
            row["matrix_frequency_hz"] = float(spec["frequency_hz"])  # type: ignore[assignment]
        if "level_db" in spec:
            row["matrix_level_db"] = float(spec["level_db"])  # type: ignore[assignment]
        rows.append(row)
    return rows


def run_tuning(profile_path: Path, tune_fs: int, final_fs: int, top_k: int, max_candidates: int = 2, mode: str = "decode", preset: str = "universal") -> dict[str, object]:
    base = DecoderParams.from_profile(profile_path, preset=preset).__dict__
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
    coarse_cases = {k: v[: min(len(v), 4096)] for k, v in coarse_cases.items()}
    final_cases = {k: v[: min(len(v), 4096)] for k, v in final_cases.items()}
    if "bass_plus_hf" in final_cases_all:
        final_cases["bass_plus_hf"] = final_cases_all["bass_plus_hf"][:4096]
    if "stereo_different" in final_cases_all:
        final_cases["stereo_different"] = final_cases_all["stereo_different"][:4096]

    coarse_results = []
    keys = list(grid.keys())
    for idx, values in enumerate(itertools.product(*(grid[k] for k in keys))):
        if idx >= max(1, max_candidates):
            break
        cand = {k: float(v) for k, v in zip(keys, values)}
        rows = evaluate_candidate(profile_path, tune_fs, coarse_cases, mode=mode, overrides=cand, preset=preset)
        score = evaluate_scores(rows)
        coarse_results.append({"params": cand, **score})

    coarse_results.sort(key=lambda x: float(x["score_total"]), reverse=True)
    finalists = coarse_results[: max(1, top_k)]

    final_results = []
    for item in finalists:
        cand = item["params"]
        rows = evaluate_candidate(profile_path, final_fs, final_cases, mode=mode, overrides=cand, preset=preset)
        score = evaluate_scores(rows)
        final_results.append({"params": cand, **score, "coarse_score_total": item["score_total"]})

    final_results.sort(key=lambda x: float(x["score_total"]), reverse=True)
    return {
        "method": "two_stage_tuning",
        "mode": mode,
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


def run_detector_study(profile_path: Path, fs: int, mode: str = "decode", preset: str = "universal") -> dict[str, object]:
    cases = {k: v[: min(len(v), 2048)] for k, v in select_tuning_cases(build_cases(fs)).items()}
    energy = evaluate_candidate(profile_path, fs, cases, mode=mode, overrides={"detector_mode": "energy"}, preset=preset)
    rms = evaluate_candidate(profile_path, fs, cases, mode=mode, overrides={"detector_mode": "rms", "detector_rms_ms": 6.0}, preset=preset)
    return {
        "fs": fs,
        "mode": mode,
        "energy": evaluate_scores(energy),
        "rms": evaluate_scores(rms),
        "cpu_note": "rms mode adds one extra detector IIR state/update per sample and channel in simulator.",
        "realtime_note": "Expected to be practical on Teensy 4.1, but firmware-side CPU headroom must still be validated on hardware telemetry.",
    }


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-dir", type=Path, default=Path("artifacts/harness"))
    ap.add_argument("--profile", type=Path, default=PROFILE_PATH)
    ap.add_argument("--mode", choices=["decode", "encode", "roundtrip"], default="decode")
    ap.add_argument("--preset", choices=["universal", "w1200", "auto_cal"], default="universal")
    ap.add_argument("--reference-dir", type=Path, default=Path("refs"))
    ap.add_argument("--reference-source", choices=["all", "real", "synthetic"], default="all")
    ap.add_argument("--cassette-priority-only", action="store_true")
    ap.add_argument("--generate-synth-refs", action="store_true", help="Generate reproducible synthetic Type-II cassette reference pack.")
    ap.add_argument("--index-real-refs", action="store_true", help="Index available real reference files and metadata.")
    ap.add_argument("--fetch-real-refs-manifest", type=Path, help="Optional JSON manifest with legally usable real-reference file URLs.")
    ap.add_argument(
        "--prepare-known-music-candidates",
        action="store_true",
        help="Import known provided music files (musik_enc.wav, musicfox_shopping_street.mp3) into refs/type2_cassette_real at profile sample rate.",
    )
    ap.add_argument("--write-wavs", action="store_true")
    ap.add_argument("--override", action="append", default=[], help="Decoder override key=value (repeatable)")
    ap.add_argument("--tune", action="store_true", help="Run a compact grid search and report the best candidate.")
    ap.add_argument("--tune-fs", type=int, default=4000, help="Coarse sample rate for tuning search workload (default: 4000)")
    ap.add_argument("--tune-final-fs", type=int, default=44100, help="Final rerank sample rate for tuning (default: 44100)")
    ap.add_argument("--tune-top-k", type=int, default=6, help="Number of coarse finalists to rerank at final sample rate.")
    ap.add_argument("--tune-max-candidates", type=int, default=2, help="Limit of coarse candidates to evaluate for compact tuning runs.")
    ap.add_argument("--detector-study", action="store_true", help="Compare energy-like detector and RMS-nearer detector in simulator.")
    args = ap.parse_args()

    profile = json.loads(args.profile.read_text())
    fs = int(profile["sample_rate_hz"])
    args.out_dir.mkdir(parents=True, exist_ok=True)

    # Fast path for long-running study commands:
    # avoid full-case baseline evaluation when caller explicitly requests tune/detector study.
    if args.tune or args.detector_study:
        if args.tune:
            tuning = run_tuning(
                profile_path=args.profile,
                tune_fs=int(max(1000, args.tune_fs)),
                final_fs=int(max(4000, args.tune_final_fs)),
                top_k=int(max(1, args.tune_top_k)),
                max_candidates=int(max(1, args.tune_max_candidates)),
                mode=args.mode,
                preset=args.preset,
            )
            (args.out_dir / "tuning_best.json").write_text(json.dumps(tuning, indent=2))
        if args.detector_study:
            study = run_detector_study(args.profile, fs=fs, mode=args.mode, preset=args.preset)
            (args.out_dir / "detector_study.json").write_text(json.dumps(study, indent=2))
        return

    ref_reports: dict[str, object] = {}
    if args.fetch_real_refs_manifest is not None:
        ref_reports["fetch_real_refs"] = fetch_real_references_from_manifest(args.reference_dir, args.fetch_real_refs_manifest)
    if args.generate_synth_refs:
        ref_reports["generate_synth_refs"] = generate_synthetic_reference_pack(args.reference_dir, profile_path=args.profile, fs=fs)
    if args.index_real_refs:
        ref_reports["index_real_refs"] = index_real_references(args.reference_dir)
    if args.prepare_known_music_candidates:
        ref_reports["prepare_known_music_candidates"] = prepare_known_music_candidates(args.reference_dir, fs=fs, search_root=Path.cwd())

    src_filter = {args.reference_source}
    if args.reference_source == "all":
        src_filter = {"all"}
    case_specs = build_case_specs(fs, args.reference_dir, source_type_filter=src_filter, cassette_priority_only=args.cassette_priority_only)
    user_overrides = parse_override_pairs(args.override)

    results = evaluate_candidate(args.profile, fs, case_specs, mode=args.mode, overrides=user_overrides, reference_dir=args.reference_dir, preset=args.preset)
    if args.write_wavs:
        for name, spec in case_specs.items():
            inp = ensure_stereo(np.asarray(spec["input"]))
            if args.mode == "decode":
                out = Decoder(fs, Params.from_profile(args.profile, preset=args.preset, **user_overrides)).process(inp)
            elif args.mode == "encode":
                out = Encoder(fs, EncoderParams.from_profile(args.profile, preset=args.preset, **user_overrides)).process(inp)
            else:
                encoded = Encoder(fs, EncoderParams.from_profile(args.profile, preset=args.preset, **user_overrides)).process(inp)
                out = Decoder(fs, Params.from_profile(args.profile, preset=args.preset)).process(encoded)
            write_audio(args.out_dir / f"{name}_input.wav", fs, inp)
            write_audio(args.out_dir / f"{name}_output.wav", fs, out)

    (args.out_dir / "metrics.json").write_text(json.dumps(results, indent=2))
    score_summary = evaluate_scores(results)
    summary = {
        **score_summary,
        "cases": len(results),
        "mode": args.mode,
        "preset": args.preset,
        "overrides": user_overrides,
        "reference_cases": int(sum(1 for row in results if bool(row["reference_available"]))),
        "source_cases": int(sum(1 for row in results if bool(row["source_available"]))),
        "cassette_primary_cases": int(sum(1 for row in results if str(row.get("case_group", "")).startswith("cassette"))),
    }
    source_breakdown = {
        "real": int(sum(1 for row in results if str(row.get("reference_source_type", "")) == "real")),
        "synthetic": int(sum(1 for row in results if str(row.get("reference_source_type", "")) == "synthetic")),
        "unknown": int(sum(1 for row in results if str(row.get("reference_source_type", "")) not in {"real", "synthetic"})),
    }
    summary["reference_source_breakdown"] = source_breakdown
    summary["cassette_priority_only"] = bool(args.cassette_priority_only)
    summary["reference_source_filter"] = args.reference_source
    if ref_reports:
        summary["reference_pipeline"] = ref_reports
        (args.out_dir / "reference_pipeline.json").write_text(json.dumps(ref_reports, indent=2))

    synthetic_rows = [r for r in results if str(r.get("reference_source_type", "synthetic")) == "synthetic"]
    real_rows = [r for r in results if str(r.get("reference_source_type", "synthetic")) == "real"]
    split = {
        "all": evaluate_scores(results),
        "synthetic": evaluate_scores(synthetic_rows) if synthetic_rows else None,
        "real": evaluate_scores(real_rows) if real_rows else None,
    }
    (args.out_dir / "split_summary.json").write_text(json.dumps(split, indent=2))
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
            preset=args.preset,
        )
        (args.out_dir / "tuning_best.json").write_text(json.dumps(tuning, indent=2))
    if args.detector_study:
        study = run_detector_study(args.profile, fs=fs, preset=args.preset)
        (args.out_dir / "detector_study.json").write_text(json.dumps(study, indent=2))


if __name__ == "__main__":
    main()
