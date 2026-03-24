import os
import re
import subprocess
import sys
from pathlib import Path

import json
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

import ocx_type2_harness as harness_module
from ocx_type2_harness import build_cases, compare, evaluate_scores, run_detector_study, run_tuning
from ocx_type2_wav_sim import PROFILE_PATH, Decoder, Params, ensure_stereo


def test_profile_defaults_load():
    params = Params.from_profile(PROFILE_PATH)
    assert params.input_trim_db == -3.0
    assert params.deemph_hz == 1850.0


def test_decoder_finite_on_all_harness_cases():
    params = Params.from_profile(PROFILE_PATH)
    fs = 4000
    for name, audio in build_cases(fs).items():
        out = Decoder(fs, params).process(audio)
        assert np.isfinite(out).all(), name


def test_output_bounded_on_all_harness_cases():
    params = Params.from_profile(PROFILE_PATH)
    fs = 4000
    for name, audio in build_cases(fs).items():
        out = Decoder(fs, params).process(audio)
        assert np.max(np.abs(out)) <= 1.0 + 1e-9, name


def test_stereo_identical_case_remains_balanced():
    params = Params.from_profile(PROFILE_PATH)
    audio = build_cases(4000)["stereo_identical"]
    out = Decoder(4000, params).process(audio)
    assert np.allclose(out[:, 0], out[:, 1], atol=1e-9)


def test_ensure_stereo_accepts_supported_shapes():
    mono = np.array([0.1, -0.2, 0.3], dtype=np.float64)
    mono_2d = mono[:, None]
    stereo = np.column_stack([mono, -mono])

    out_mono = ensure_stereo(mono)
    out_mono_2d = ensure_stereo(mono_2d)
    out_stereo = ensure_stereo(stereo)

    assert out_mono.shape == (3, 2)
    assert out_mono_2d.shape == (3, 2)
    assert out_stereo.shape == (3, 2)


def test_decoder_process_accepts_mono_shapes():
    params = Params.from_profile(PROFILE_PATH)
    decoder = Decoder(4000, params)
    mono = np.sin(2 * np.pi * 440 * np.arange(1000) / 4000)
    out_a = decoder.process(mono)
    decoder.reset()
    out_b = decoder.process(mono[:, None])

    assert out_a.shape == (1000, 2)
    assert out_b.shape == (1000, 2)


def test_help_works_without_matplotlib_dependency_for_non_plot_path():
    env = os.environ.copy()
    env["PYTHONPATH"] = str(ROOT)
    result = subprocess.run(
        [sys.executable, str(ROOT / "ocx_type2_wav_sim.py"), "--help"],
        check=False,
        capture_output=True,
        text=True,
        env=env,
    )
    assert result.returncode == 0
    assert "--plot" in result.stdout


def test_decoder_cases_at_44100_finite_and_bounded():
    params = Params.from_profile(PROFILE_PATH)
    fs = 44_100
    selected = ["silence", "sine_-12db", "log_sweep", "pink_noise", "fast_level_switches"]
    for key in selected:
        audio = build_cases(fs)[key]
        out = Decoder(fs, params).process(audio)
        assert np.isfinite(out).all(), key
        assert np.max(np.abs(out)) <= 1.0 + 1e-9, key


def test_compare_handles_reference_length_mismatch_without_crash():
    fs = 4000
    params = Params.from_profile(PROFILE_PATH)
    audio = build_cases(fs)["music_like"]
    out = Decoder(fs, params).process(audio)
    short_ref = out[: len(out) // 2]
    metrics = compare(audio, out, short_ref)
    assert metrics["mse_vs_reference"] is not None
    assert np.isfinite(metrics["mse_vs_reference"])
    assert int(metrics["reference_samples_used"]) == len(short_ref)


def test_no_reference_score_penalizes_under_decoding_even_if_input_similarity_is_high():
    base = {
        "case": "synthetic",
        "output_clip_l": False,
        "output_clip_r": False,
        "channel_deviation_rms": 0.0,
        "gain_curve_std_db": 1.8,
        "gain_curve_diff_p95_db": 2.2,
        "gain_curve_diff_std_db": 1.1,
        "freq_response_delta_db": 5.0,
        "transient_delta": 0.2,
        "soft_clip_dependency": 0.0,
        "gain_curve_mean_db": -1.0,
        "input_level_span_db": 18.0,
        "mse_vs_reference": None,
        "freq_response_delta_db_vs_reference": None,
        "transient_delta_vs_reference": None,
        "correlation_vs_reference": None,
        "reference_available": False,
        "mse": 0.0,
        "mae": 0.0,
        "max_abs_error": 0.0,
        "correlation": 1.0,
        "residual_rms": 0.0,
    }
    under_decoded = dict(base)
    under_decoded["input_rms"] = 0.12
    under_decoded["output_rms"] = 0.118
    under_decoded["decode_action_ratio"] = 0.01
    under_decoded["gain_curve_mean_db"] = -0.05
    under_decoded["correlation"] = 0.999
    under_decoded["residual_rms"] = 0.0012

    plausibly_decoded = dict(base)
    plausibly_decoded["input_rms"] = 0.12
    plausibly_decoded["output_rms"] = 0.1
    plausibly_decoded["decode_action_ratio"] = 0.12
    plausibly_decoded["gain_curve_mean_db"] = -1.2
    plausibly_decoded["correlation"] = 0.94
    plausibly_decoded["residual_rms"] = 0.015

    score_under = float(evaluate_scores([under_decoded])["score_total"])
    score_decoded = float(evaluate_scores([plausibly_decoded])["score_total"])
    assert score_decoded > score_under


def test_tuning_rerank_uses_44100_final_stage():
    original_build_cases = harness_module.build_cases

    def tiny_cases(fs: int):
        _ = fs
        return {
            "tiny_a": np.zeros((256, 2), dtype=np.float64),
            "tiny_b": np.full((256, 2), 0.03, dtype=np.float64),
        }

    harness_module.build_cases = tiny_cases
    try:
        run = run_tuning(PROFILE_PATH, tune_fs=4000, final_fs=44_100, top_k=1, max_candidates=2)
    finally:
        harness_module.build_cases = original_build_cases

    assert run["final_fs"] == 44_100
    assert len(run["coarse_ranking"]) > 0
    assert len(run["final_ranking"]) == 1
    assert run["best"] == run["final_ranking"][0]


def test_tuning_mode_is_reproducible_for_identical_inputs():
    original_build_cases = harness_module.build_cases

    def tiny_cases(fs: int):
        _ = fs
        return {
            "tiny_a": np.zeros((256, 2), dtype=np.float64),
            "tiny_b": np.full((256, 2), 0.03, dtype=np.float64),
        }

    harness_module.build_cases = tiny_cases
    try:
        run_a = run_tuning(PROFILE_PATH, tune_fs=4000, final_fs=44_100, top_k=1, max_candidates=3)
        run_b = run_tuning(PROFILE_PATH, tune_fs=4000, final_fs=44_100, top_k=1, max_candidates=3)
    finally:
        harness_module.build_cases = original_build_cases

    assert run_a["best"] == run_b["best"]
    assert run_a["final_ranking"] == run_b["final_ranking"]


def test_detector_study_reports_both_modes():
    original_build_cases = harness_module.build_cases

    def tiny_cases(fs: int):
        _ = fs
        return {
            "tiny_a": np.zeros((256, 2), dtype=np.float64),
            "tiny_b": np.full((256, 2), 0.03, dtype=np.float64),
        }

    harness_module.build_cases = tiny_cases
    try:
        report = run_detector_study(PROFILE_PATH, fs=44_100)
    finally:
        harness_module.build_cases = original_build_cases
    assert "energy" in report
    assert "rms" in report


def test_profile_and_firmware_defaults_are_synced():
    profile = json.loads(PROFILE_PATH.read_text())
    decoder = profile["decoder"]
    codec = profile["codec"]
    ino = (ROOT / "ocx_type2_teensy41_decoder.ino").read_text()

    expected_pairs = {
        "kLineInLevel": codec["line_in_level"],
        "kLineOutLevel": codec["line_out_level"],
        "kInputTrimDb": decoder["input_trim_db"],
        "kOutputTrimDb": decoder["output_trim_db"],
        "kStrength": decoder["strength"],
        "kReferenceDb": decoder["reference_db"],
        "kMaxBoostDb": decoder["max_boost_db"],
        "kMaxCutDb": decoder["max_cut_db"],
        "kAttackMs": decoder["attack_ms"],
        "kReleaseMs": decoder["release_ms"],
        "kSidechainHpHz": decoder["sidechain_hp_hz"],
        "kSidechainShelfHz": decoder["sidechain_shelf_hz"],
        "kSidechainShelfDb": decoder["sidechain_shelf_db"],
        "kDeemphHz": decoder["deemph_hz"],
        "kDeemphDb": decoder["deemph_db"],
        "kSoftClipDrive": decoder["soft_clip_drive"],
        "kDcBlockHz": decoder["dc_block_hz"],
        "kHeadroomDb": decoder["headroom_db"],
    }

    for key, expected in expected_pairs.items():
        match = re.search(rf"{key}\s*=\s*([-0-9.]+)f?;", ino)
        assert match is not None, key
        actual = float(match.group(1))
        assert actual == float(expected), key
