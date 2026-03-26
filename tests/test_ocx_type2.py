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
from ocx_type2_harness import build_case_specs, compare, evaluate_scores, run_detector_study, run_tuning
from ocx_type2_wav_sim import Decoder, DecoderParams, Encoder, EncoderParams, PROFILE_PATH, ensure_stereo


def test_profile_defaults_load_decoder_and_encoder():
    dec = DecoderParams.from_profile(PROFILE_PATH)
    enc = EncoderParams.from_profile(PROFILE_PATH)
    assert dec.input_trim_db == -3.0
    assert dec.deemph_hz == 1850.0
    assert enc.input_trim_db == 0.0
    assert enc.tilt_db == 5.0


def test_decoder_finite_bounded_on_required_cases():
    fs = 4000
    params = DecoderParams.from_profile(PROFILE_PATH)
    required = ["log_sweep", "pink_noise", "bursts", "envelope_steps", "transient_train", "bass_plus_hf", "music_like"]
    specs = build_case_specs(fs)
    for case in required:
        out = Decoder(fs, params).process(specs[case]["input"])
        assert np.isfinite(out).all(), case
        assert np.max(np.abs(out)) <= 1.0 + 1e-9, case


def test_encoder_finite_bounded_on_required_cases():
    fs = 4000
    params = EncoderParams.from_profile(PROFILE_PATH)
    specs = build_case_specs(fs)
    required = ["log_sweep", "pink_noise", "bursts", "envelope_steps", "transient_train", "bass_plus_hf", "music_like"]
    for case in required:
        out = Encoder(fs, params).process(specs[case]["input"])
        assert np.isfinite(out).all(), case
        assert np.max(np.abs(out)) <= 1.0 + 1e-9, case


def test_roundtrip_finite_bounded_and_no_nan_inf():
    fs = 4000
    enc = Encoder(fs, EncoderParams.from_profile(PROFILE_PATH))
    dec = Decoder(fs, DecoderParams.from_profile(PROFILE_PATH))
    for case, spec in build_case_specs(fs).items():
        source = spec["input"]
        out = dec.process(enc.process(source))
        assert np.isfinite(out).all(), case
        assert np.max(np.abs(out)) <= 1.0 + 1e-9, case


def test_stereo_identical_case_remains_balanced_decode_encode_roundtrip():
    fs = 4000
    audio = build_case_specs(fs)["stereo_identical"]["input"]
    dec_out = Decoder(fs, DecoderParams.from_profile(PROFILE_PATH)).process(audio)
    enc_out = Encoder(fs, EncoderParams.from_profile(PROFILE_PATH)).process(audio)
    rt_out = Decoder(fs, DecoderParams.from_profile(PROFILE_PATH)).process(enc_out)
    assert np.allclose(dec_out[:, 0], dec_out[:, 1], atol=1e-8)
    assert np.allclose(enc_out[:, 0], enc_out[:, 1], atol=1e-8)
    assert np.allclose(rt_out[:, 0], rt_out[:, 1], atol=1e-8)


def test_harness_mode_calls_do_not_crash_and_emit_metrics(tmp_path):
    fs = 4000
    specs = {k: v for k, v in list(build_case_specs(fs).items())[:4]}
    for mode in ["decode", "encode", "roundtrip"]:
        rows = harness_module.evaluate_candidate(PROFILE_PATH, fs, specs, mode=mode)
        assert rows
        assert "mse" in rows[0]
        assert "correlation" in rows[0]


def test_roundtrip_metrics_are_produced():
    rows = harness_module.evaluate_candidate(PROFILE_PATH, 4000, build_case_specs(4000), mode="roundtrip")
    first = rows[0]
    for key in ["mse", "mae", "correlation", "residual_rms", "freq_delta_low_db", "freq_delta_mid_db", "freq_delta_high_db", "transient_delta"]:
        assert key in first


def test_help_works_without_matplotlib_dependency_for_non_plot_path():
    env = os.environ.copy()
    env["PYTHONPATH"] = str(ROOT)
    result = subprocess.run([sys.executable, str(ROOT / "ocx_type2_wav_sim.py"), "--help"], check=False, capture_output=True, text=True, env=env)
    assert result.returncode == 0
    assert "--mode" in result.stdout


def test_compare_handles_reference_length_mismatch_without_crash():
    fs = 4000
    audio = build_case_specs(fs)["music_like"]["input"]
    out = Decoder(fs, DecoderParams.from_profile(PROFILE_PATH)).process(audio)
    short_ref = out[: len(out) // 2]
    metrics = compare(audio, out, fs, short_ref)
    assert metrics["mse_vs_reference"] is not None
    assert np.isfinite(metrics["mse_vs_reference"])
    assert int(metrics["reference_samples_used"]) == len(short_ref)


def test_tuning_rerank_uses_44100_final_stage():
    original = harness_module.build_cases

    def tiny_cases(_: int):
        return {"a": np.zeros((256, 2), dtype=np.float64), "b": np.full((256, 2), 0.03, dtype=np.float64)}

    harness_module.build_cases = tiny_cases
    try:
        run = run_tuning(PROFILE_PATH, tune_fs=4000, final_fs=44_100, top_k=1, max_candidates=2)
    finally:
        harness_module.build_cases = original

    assert run["final_fs"] == 44_100


def test_tuning_honors_requested_mode():
    seen_modes: list[str] = []
    original_eval = harness_module.evaluate_candidate
    original_cases = harness_module.build_cases

    def tiny_cases(_: int):
        return {"a": np.zeros((256, 2), dtype=np.float64), "b": np.full((256, 2), 0.03, dtype=np.float64)}

    def wrapped_eval(*args, **kwargs):
        seen_modes.append(str(kwargs.get("mode", "")))
        return original_eval(*args, **kwargs)

    harness_module.build_cases = tiny_cases
    harness_module.evaluate_candidate = wrapped_eval
    try:
        run_tuning(PROFILE_PATH, tune_fs=4000, final_fs=44_100, top_k=1, max_candidates=1, mode="roundtrip")
    finally:
        harness_module.evaluate_candidate = original_eval
        harness_module.build_cases = original_cases

    assert seen_modes
    assert all(m == "roundtrip" for m in seen_modes)


def test_detector_study_reports_both_modes():
    original = harness_module.build_cases

    def tiny_cases(_: int):
        return {"a": np.zeros((256, 2), dtype=np.float64), "b": np.full((256, 2), 0.03, dtype=np.float64)}

    harness_module.build_cases = tiny_cases
    try:
        report = run_detector_study(PROFILE_PATH, fs=44_100)
    finally:
        harness_module.build_cases = original
    assert "energy" in report
    assert "rms" in report


def test_detector_study_honors_requested_mode():
    seen_modes: list[str] = []
    original_eval = harness_module.evaluate_candidate
    original_cases = harness_module.build_cases

    def tiny_cases(_: int):
        return {"a": np.zeros((256, 2), dtype=np.float64), "b": np.full((256, 2), 0.03, dtype=np.float64)}

    def wrapped_eval(*args, **kwargs):
        seen_modes.append(str(kwargs.get("mode", "")))
        return original_eval(*args, **kwargs)

    harness_module.build_cases = tiny_cases
    harness_module.evaluate_candidate = wrapped_eval
    try:
        run_detector_study(PROFILE_PATH, fs=44_100, mode="roundtrip")
    finally:
        harness_module.evaluate_candidate = original_eval
        harness_module.build_cases = original_cases
    assert seen_modes == ["roundtrip", "roundtrip"]


def test_profile_and_firmware_defaults_are_synced():
    profile = json.loads(PROFILE_PATH.read_text())
    decoder = profile["decoder"]
    encoder = profile["encoder"]
    codec = profile["codec"]
    tone = profile["tone"]
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
        "kEncInputTrimDb": encoder["input_trim_db"],
        "kEncOutputTrimDb": encoder["output_trim_db"],
        "kEncStrength": encoder["strength"],
        "kEncReferenceDb": encoder["reference_db"],
        "kEncMaxBoostDb": encoder["max_boost_db"],
        "kEncMaxCutDb": encoder["max_cut_db"],
        "kEncAttackMs": encoder["attack_ms"],
        "kEncReleaseMs": encoder["release_ms"],
        "kEncSidechainHpHz": encoder["sidechain_hp_hz"],
        "kEncSidechainShelfHz": encoder["sidechain_shelf_hz"],
        "kEncSidechainShelfDb": encoder["sidechain_shelf_db"],
        "kEncTiltHz": encoder["tilt_hz"],
        "kEncTiltDb": encoder["tilt_db"],
        "kEncSoftClipDrive": encoder["soft_clip_drive"],
        "kEncDcBlockHz": encoder["dc_block_hz"],
        "kEncHeadroomDb": encoder["headroom_db"],
    }
    for key, expected in expected_pairs.items():
        match = re.search(rf"{key}\s*=\s*([-0-9.]+)f?;", ino)
        assert match is not None, key
        assert float(match.group(1)) == float(expected), key

    for key, expected in {"kToneHz": tone["frequency_hz"], "kToneDb": tone["level_dbfs"]}.items():
        match = re.search(rf"{key}\s*=\s*([-0-9.]+)f?;", ino)
        assert match is not None, key
        assert float(match.group(1)) == float(expected), key


def test_evaluate_scores_underdecode_penalty_still_blocks_trivial_similarity():
    under = {
        "output_clip_l": False, "output_clip_r": False, "channel_deviation_rms": 0.0,
        "gain_curve_std_db": 1.8, "gain_curve_diff_p95_db": 2.2, "gain_curve_diff_std_db": 1.1,
        "freq_response_delta_db": 5.0, "freq_delta_mid_db": 2.0, "transient_delta": 0.2,
        "overshoot_delta": 0.02, "undershoot_delta": 0.02, "soft_clip_dependency": 0.0,
        "gain_curve_mean_db": -0.05, "input_level_span_db": 18.0, "gain_vs_input_slope": -0.2,
        "gain_vs_input_r2": 0.8, "mse_vs_reference": None, "freq_response_delta_db_vs_reference": None,
        "transient_delta_vs_reference": None, "correlation_vs_reference": None, "case_group": "synthetic",
        "matrix_frequency_hz": None, "matrix_level_db": None, "input_rms": 0.12, "output_rms": 0.118,
        "decode_action_ratio": 0.01, "correlation": 0.999,
    }
    good = dict(under)
    good.update({"decode_action_ratio": 0.12, "gain_curve_mean_db": -1.2, "correlation": 0.94, "output_rms": 0.10})
    assert float(evaluate_scores([good])["score_total"]) > float(evaluate_scores([under])["score_total"])
