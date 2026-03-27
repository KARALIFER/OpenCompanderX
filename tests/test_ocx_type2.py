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
from ocx_type2_auto_cal import (
    BlockTracker,
    SegmentMetaCollector,
    ToneTelemetry,
    decide_candidate,
    evaluate_tone_acceptance,
    has_enough_measurement,
    profile_slot_for_transport,
    resolve_wizard_expected_transport,
    update_block_tracker,
)
from ocx_type2_harness import build_case_specs, compare, evaluate_profile_set, evaluate_scores, run_detector_study, run_tuning
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


def test_profile_set_report_emits_all_slots():
    report = evaluate_profile_set(PROFILE_PATH, fs=4000, mode="decode", reference_dir=ROOT / "refs", profiles=["single_profile", "lw1_profile", "lw2_profile", "common_profile"])
    assert set(report["profiles"]) == {"single_profile", "lw1_profile", "lw2_profile", "common_profile"}
    assert "per_profile" in report
    totals = [report["per_profile"][slot]["summary"]["score_total"] for slot in report["profiles"]]
    assert len(set(totals)) > 1


def test_dual_lw_autocal_keeps_selected_transport_and_slot_mapping():
    assert resolve_wizard_expected_transport("DUAL_LW", "LW2") == "LW2"
    assert profile_slot_for_transport("DUAL_LW", "LW2") == "lw2_profile"
    assert resolve_wizard_expected_transport("SINGLE_LW", "LW2") == "LW1"
    assert profile_slot_for_transport("SINGLE_LW", "LW2") == "single_profile"

    ino = (ROOT / "OpenCompanderX.ino").read_text()
    begin_body = re.search(r"void beginAutoCal\(\)\s*\{(.+?)\n\}", ino, re.S)
    assert begin_body is not None
    body = begin_body.group(1)
    assert "if (deckType == DECK_SINGLE_LW) wizardExpectedTransport = TRANSPORT_LW1;" in body
    assert "wizardExpectedTransport = (deckType == DECK_DUAL_LW) ? TRANSPORT_LW1 : TRANSPORT_LW1;" not in body

    # Start command keeps dual-transport selection from activeTransport.
    assert "wizardExpectedTransport = (activeTransport == TRANSPORT_LW2) ? TRANSPORT_LW2 : TRANSPORT_LW1;" in ino
    # Slot mapping still depends on wizardExpectedTransport, so LW2 runs map to lw2Profile.
    assert "if (wizardExpectedTransport == TRANSPORT_LW1)" in ino
    assert "profileStore.lw2Profile = p;" in ino




def test_arduino_autoprototype_has_calprofile_forward_decl():
    ino = (ROOT / "OpenCompanderX.ino").read_text()
    assert "struct CalProfile;" in ino


def test_dual_lw_common_profile_becomes_default_and_is_persisted():
    ino = (ROOT / "OpenCompanderX.ino").read_text()
    compute = re.search(r"void computeAutoCalResult\(\)\s*\{(.+?)\n\}", ino, re.S)
    assert compute is not None
    body = compute.group(1)
    assert "if (profileStore.lw1Valid && profileStore.lw2Valid)" in body
    assert "profileStore.commonValid = 1;" in body
    assert "selectedProfile = PROFILE_COMMON;" in body
    assert "currentPreset = PRESET_AUTO_CAL;" in body
    assert "applyDecoderPreset(PRESET_AUTO_CAL);" in body
    assert "persistSettings();" in body


def test_segment_meta_collector_keeps_real_per_segment_values():
    c = SegmentMetaCollector()
    c.add_segment(duration_blocks=410, peak_avg=0.41, tone_avg=0.73, peak_spread=0.03)
    c.add_segment(duration_blocks=405, peak_avg=0.40, tone_avg=0.72, peak_spread=0.02)
    c.add_segment(duration_blocks=399, peak_avg=0.39, tone_avg=0.71, peak_spread=0.025)
    assert c.durations_blocks == [410, 405, 399]
    assert c.peak_avg == [0.41, 0.40, 0.39]
    assert c.tone_avg == [0.73, 0.72, 0.71]
    assert c.peak_spread == [0.03, 0.02, 0.025]


def test_firmware_meta_uses_per_segment_arrays_not_live_counter():
    ino = (ROOT / "OpenCompanderX.ino").read_text()
    assert "meta.segmentDurationSec[i] = (uint16_t)((uint32_t)autoSegDurationBlocks[i] * kBlockPeriodMs / 1000UL);" in ino
    assert "meta.segmentPeak[i] = autoSegPeakAvg[i];" in ino
    assert "meta.segmentTone[i] = autoSegToneAvg[i];" in ino
    assert "meta.segmentSpread[i] = autoSegPeakSpread[i];" in ino


def test_profile_and_firmware_defaults_are_synced():
    profile = json.loads(PROFILE_PATH.read_text())
    decoder = profile["decoder"]
    encoder = profile["encoder"]
    codec = profile["codec"]
    tone = profile["tone"]
    ino = (ROOT / "OpenCompanderX.ino").read_text()

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


def test_auto_cal_wait_for_tone_reject_path_low_peak():
    acc = evaluate_tone_acceptance(
        ToneTelemetry(tone_left=0.9, tone_right=0.85, peak_left=0.01, peak_right=0.01),
        prev_peak_left=0.01,
        prev_peak_right=0.01,
    )
    assert acc.accepted is False
    assert acc.reject_reason == "reject_peak_too_low"


def test_auto_cal_wait_for_tone_success_path():
    acc = evaluate_tone_acceptance(
        ToneTelemetry(tone_left=0.82, tone_right=0.75, peak_left=0.24, peak_right=0.21),
        prev_peak_left=0.26,
        prev_peak_right=0.20,
    )
    assert acc.accepted is True
    assert acc.reject_reason == "none"


def test_auto_cal_block_accumulation_and_short_silence_tolerance():
    tracker = BlockTracker()
    for _ in range(140):
        tracker = update_block_tracker(tracker, accepted=True)
    for _ in range(3):
        tracker = update_block_tracker(tracker, accepted=False)
    for _ in range(140):
        tracker = update_block_tracker(tracker, accepted=True)
    assert tracker.tone_segments == 0
    assert tracker.current_segment_valid_blocks >= 280


def test_auto_cal_segment_finalize_after_silence_gap():
    tracker = BlockTracker()
    for _ in range(130):
        tracker = update_block_tracker(tracker, accepted=True)
    for _ in range(4):
        tracker = update_block_tracker(tracker, accepted=False)
    assert tracker.tone_segments == 1
    assert tracker.current_segment_valid_blocks == 0


def test_auto_cal_too_early_lock_prevented():
    tracker = BlockTracker()
    for _ in range(250):
        tracker = update_block_tracker(tracker, accepted=True)
    assert has_enough_measurement(tracker, elapsed_ms=45_000) is False


def test_auto_cal_reject_reasons_cover_no_fresh_data_and_lr_mismatch():
    no_fresh = evaluate_tone_acceptance(
        ToneTelemetry(0.8, 0.8, 0.2, 0.2, fresh_peak_right=False),
        prev_peak_left=0.2,
        prev_peak_right=0.2,
    )
    lr_bad = evaluate_tone_acceptance(
        ToneTelemetry(0.9, 0.2, 0.5, 0.05),
        prev_peak_left=0.52,
        prev_peak_right=0.06,
    )
    assert no_fresh.reject_reason == "reject_no_fresh_data"
    assert lr_bad.reject_reason == "reject_lr_mismatch"


def test_auto_cal_fresh_window_allows_recent_latched_values():
    acc = evaluate_tone_acceptance(
        ToneTelemetry(
            tone_left=0.82,
            tone_right=0.78,
            peak_left=0.24,
            peak_right=0.23,
            fresh_tone_left=False,
            fresh_tone_right=False,
            fresh_peak_left=False,
            fresh_peak_right=False,
            tone_left_age_ms=120,
            tone_right_age_ms=170,
            peak_left_age_ms=100,
            peak_right_age_ms=160,
        ),
        prev_peak_left=0.22,
        prev_peak_right=0.21,
    )
    assert acc.accepted is False
    assert acc.reject_reason == "reject_no_fresh_data"

    # recent latched values with fresh flags should still pass within the freshness window
    acc_recent = evaluate_tone_acceptance(
        ToneTelemetry(
            tone_left=0.82,
            tone_right=0.78,
            peak_left=0.24,
            peak_right=0.23,
            tone_left_age_ms=120,
            tone_right_age_ms=170,
            peak_left_age_ms=100,
            peak_right_age_ms=160,
        ),
        prev_peak_left=0.22,
        prev_peak_right=0.21,
    )
    assert acc_recent.accepted is True
    assert acc_recent.reject_reason == "none"


def test_auto_cal_stale_fresh_window_rejects_even_if_flags_are_true():
    acc = evaluate_tone_acceptance(
        ToneTelemetry(
            tone_left=0.82,
            tone_right=0.80,
            peak_left=0.24,
            peak_right=0.22,
            tone_left_age_ms=480,
            tone_right_age_ms=120,
            peak_left_age_ms=110,
            peak_right_age_ms=140,
        ),
        prev_peak_left=0.23,
        prev_peak_right=0.21,
    )
    assert acc.accepted is False
    assert acc.reject_reason == "reject_no_fresh_data"


def test_auto_cal_low_level_lr_mismatch_does_not_fail_hard():
    low_level = evaluate_tone_acceptance(
        ToneTelemetry(tone_left=0.90, tone_right=0.08, peak_left=0.05, peak_right=0.005),
        prev_peak_left=0.05,
        prev_peak_right=0.006,
    )
    assert low_level.lr_ok is True
    assert low_level.reject_reason != "reject_lr_mismatch"


def test_auto_cal_candidate_selection_is_universal_only():
    chosen_hot, _, _ = decide_candidate(peak_avg=0.78, rms_avg=0.37, lr_mismatch=0.10, stability_penalty=0.05)
    chosen_cool, _, _ = decide_candidate(peak_avg=0.50, rms_avg=0.30, lr_mismatch=0.02, stability_penalty=0.02)
    assert chosen_hot == "universal"
    assert chosen_cool == "universal"


def test_auto_cal_persistence_remains_unchanged_on_failed_attempt():
    ino = (ROOT / "OpenCompanderX.ino").read_text()
    begin_idx = ino.index("void beginAutoCal()")
    begin_body = ino[begin_idx : ino.index("void computeAutoCalResult()", begin_idx)]
    assert "autoCalValid = false" not in begin_body
    assert "currentPreset = PRESET_AUTO_CAL" not in begin_body


def test_auto_cal_invalid_stored_profile_falls_back_to_universal():
    ino = (ROOT / "OpenCompanderX.ino").read_text()
    assert "currentPreset = PRESET_UNIVERSAL;" in ino
    assert "autoCalValid = false;" in ino


def test_auto_cal_stereo_telemetry_command_and_detectors_present():
    ino = (ROOT / "OpenCompanderX.ino").read_text()
    for token in [
        "toneDetectRLo",
        "toneDetectRCenter",
        "toneDetectRHi",
        "peakR",
        "case 'K': printAutoCalRawTelemetry();",
        "kAutoFreshWindowMs",
        "freshWinToneL",
        "gateBlock=",
        "lrCheck=",
    ]:
        assert token in ino


def test_encoder_presets_are_currently_intentionally_identical():
    profile = json.loads(PROFILE_PATH.read_text())
    enc = profile["presets"]["encoder"]
    assert enc["universal"] == enc["auto_cal"]
