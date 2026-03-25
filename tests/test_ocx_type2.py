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
from ocx_type2_harness import (
    CASSETTE_PRIMARY_FREQS_HZ,
    CASSETTE_PRIMARY_LEVELS_DB,
    REF_SYNTH_DIRNAME,
    build_case_specs,
    build_cases,
    compare,
    generate_synthetic_reference_pack,
    evaluate_scores,
    prepare_known_music_candidates,
    resample_audio_linear,
    run_detector_study,
    run_tuning,
)
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
    metrics = compare(audio, out, fs, short_ref)
    assert metrics["mse_vs_reference"] is not None
    assert np.isfinite(metrics["mse_vs_reference"])
    assert int(metrics["reference_samples_used"]) == len(short_ref)


def test_cassette_tone_matrix_contains_required_frequencies_and_levels():
    specs = build_case_specs(4000)
    matrix_rows = [v for v in specs.values() if v.get("group") == "cassette_tone_matrix"]
    freqs = sorted({float(v["frequency_hz"]) for v in matrix_rows})
    levels = sorted({float(v["level_db"]) for v in matrix_rows})
    assert freqs == sorted(CASSETTE_PRIMARY_FREQS_HZ)
    assert levels == sorted(CASSETTE_PRIMARY_LEVELS_DB)


def test_reference_case_discovery_from_type2_cassette_layout(tmp_path):
    import soundfile as sf

    fs = 4000
    ref_root = tmp_path / "refs" / "type2_cassette"
    ref_root.mkdir(parents=True)
    n = 256
    t = np.arange(n) / fs
    src = np.sin(2 * np.pi * 400.0 * t)
    enc = 0.7 * src
    dec = 0.95 * src
    sf.write(ref_root / "case_a_source.wav", np.column_stack([src, src]), fs)
    sf.write(ref_root / "case_a_encoded.wav", np.column_stack([enc, enc]), fs)
    sf.write(ref_root / "case_a_reference_decode.wav", np.column_stack([dec, dec]), fs)
    sf.write(ref_root / "case_b_encoded.wav", np.column_stack([enc, enc]), fs)
    specs = build_case_specs(fs, tmp_path / "refs")
    assert "ref_case_a" in specs
    assert specs["ref_case_a"]["group"] == "cassette_reference"
    assert "source_target" in specs["ref_case_a"]
    assert "reference_decode" in specs["ref_case_a"]
    assert "ref_case_b" not in specs


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
        "freq_delta_mid_db": 2.0,
        "transient_delta": 0.2,
        "overshoot_delta": 0.02,
        "undershoot_delta": 0.02,
        "soft_clip_dependency": 0.0,
        "gain_curve_mean_db": -1.0,
        "input_level_span_db": 18.0,
        "gain_vs_input_slope": -0.2,
        "gain_vs_input_r2": 0.8,
        "mse_vs_reference": None,
        "freq_response_delta_db_vs_reference": None,
        "transient_delta_vs_reference": None,
        "correlation_vs_reference": None,
        "reference_available": False,
        "case_group": "synthetic",
        "matrix_frequency_hz": None,
        "matrix_level_db": None,
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
    assert run["selection_basis"] == "final_stage_only"
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
    }

    for key, expected in expected_pairs.items():
        match = re.search(rf"{key}\s*=\s*([-0-9.]+)f?;", ino)
        assert match is not None, key
        actual = float(match.group(1))
        assert actual == float(expected), key

    tone_pairs = {
        "kToneHz": tone["frequency_hz"],
        "kToneDb": tone["level_dbfs"],
    }
    for key, expected in tone_pairs.items():
        match = re.search(rf"{key}\s*=\s*([-0-9.]+)f?;", ino)
        assert match is not None, key
        actual = float(match.group(1))
        assert actual == float(expected), key


def test_calibration_tone_level_and_decoder_reference_remain_separate():
    profile = json.loads(PROFILE_PATH.read_text())
    tone_dbfs = float(profile["tone"]["level_dbfs"])
    reference_db = float(profile["decoder"]["reference_db"])
    assert tone_dbfs != reference_db


def test_help_text_mentions_400hz_calibration_tone():
    ino = (ROOT / "ocx_type2_teensy41_decoder.ino").read_text()
    assert "toggle 400 Hz calibration tone" in ino


def test_help_text_mentions_signal_diag_and_tone_channel_mode_commands():
    ino = (ROOT / "ocx_type2_teensy41_decoder.ino").read_text()
    assert "print signal diagnostics snapshot" in ino
    assert "reset signal diagnostics counters" in ino
    assert "print NEW clip counts since last v/m/p call" in ino
    assert "cycle tone channel mode BOTH -> LEFT -> RIGHT" in ino
    assert "Snapshot includes clamp-hit/near-limit stats" in ino


def test_firmware_documents_post_decoder_calibration_tone_routing():
    ino = (ROOT / "ocx_type2_teensy41_decoder.ino").read_text()
    assert "calibration tone is mixed post-decoder into the output path" in ino
    assert "post-decoder output injection" in ino


def test_firmware_compact_and_snapshot_output_include_bypass_and_signal_metrics():
    ino = (ROOT / "ocx_type2_teensy41_decoder.ino").read_text()
    assert "bypass=" in ino
    assert "inClipNew=" in ino
    assert "OCX SIGNAL DIAGNOSTICS" in ino
    assert "Gain dB last/min/max/avg" in ino
    assert "Snapshot bypass now" in ino
    assert "L/R correlation in/out" in ino
    assert "Sidechain spectral proxy (high-vs-low)" in ino
    assert "Gain clamp hits cut/boost" in ino
    assert "Clamp interpretation:" in ino
    assert "Cassette quick hints" in ino


def test_tone_channel_label_helper_is_ino_preprocessor_safe():
    ino = (ROOT / "ocx_type2_teensy41_decoder.ino").read_text()
    assert "enum ToneChannelMode : uint8_t" in ino
    assert "toneChannelModeLabel(uint8_t mode)" in ino


def test_firmware_status_mentions_tone_channel_mode():
    ino = (ROOT / "ocx_type2_teensy41_decoder.ino").read_text()
    assert "Tone channel mode" in ino
    assert "tone channel mode is an output routing test" in ino


def test_partial_reference_coverage_does_not_break_evaluation(tmp_path):
    import soundfile as sf

    fs = 4000
    case_specs = build_case_specs(fs)
    name = "sine_-12db"
    inp = case_specs[name]["input"]
    ref_dir = tmp_path / "refs"
    ref_dir.mkdir()
    sf.write(ref_dir / f"{name}.wav", inp, fs)
    rows = harness_module.evaluate_candidate(PROFILE_PATH, fs, case_specs, reference_dir=ref_dir)
    with_ref = [r for r in rows if r["case"] == name][0]
    no_ref = [r for r in rows if r["case"] == "pink_noise"][0]
    assert with_ref["reference_available"] is True
    assert no_ref["reference_available"] is False


def test_generate_synthetic_reference_pack_writes_metadata_and_audio(tmp_path):
    report = generate_synthetic_reference_pack(tmp_path, profile_path=PROFILE_PATH, fs=4000)
    synth_root = tmp_path / REF_SYNTH_DIRNAME
    assert report["case_count"] > 0
    assert (synth_root / "index.json").exists()
    first = report["cases"][0]["case_name"]
    assert (synth_root / f"{first}_source.wav").exists()
    assert (synth_root / f"{first}_encoded.wav").exists()
    assert (synth_root / f"{first}_reference_decode.wav").exists()
    meta = json.loads((synth_root / f"{first}.json").read_text())
    assert meta["source_type"] == "synthetic"
    assert "approximate" in meta["trust_level"]


def test_build_case_specs_can_filter_cassette_priority_and_source_type(tmp_path):
    generate_synthetic_reference_pack(tmp_path, profile_path=PROFILE_PATH, fs=4000)
    specs = build_case_specs(4000, reference_dir=tmp_path, source_type_filter={"synthetic"}, cassette_priority_only=True)
    assert specs
    assert all(bool(v.get("cassette_priority")) for v in specs.values())
    assert all(str(v.get("source_type")) == "synthetic" for v in specs.values())


def test_resample_audio_linear_changes_sample_count_and_stays_stereo():
    fs_in = 48_000
    fs_out = 44_100
    t = np.arange(int(fs_in * 0.25)) / fs_in
    src = np.column_stack([np.sin(2 * np.pi * 1000.0 * t), np.sin(2 * np.pi * 1200.0 * t)])
    out = resample_audio_linear(src, fs_in=fs_in, fs_out=fs_out)
    assert out.ndim == 2 and out.shape[1] == 2
    assert abs(len(out) - int(round(len(src) * fs_out / fs_in))) <= 1


def test_prepare_known_music_candidates_imports_musik_enc_and_exposes_candidate_case(tmp_path):
    import soundfile as sf

    fs = 48_000
    t = np.arange(fs) / fs
    audio = np.column_stack([0.3 * np.sin(2 * np.pi * 400.0 * t), 0.25 * np.sin(2 * np.pi * 1000.0 * t)])
    sf.write(tmp_path / "musik_enc.wav", audio, fs)
    refs = tmp_path / "refs"
    report = prepare_known_music_candidates(refs, fs=44_100, search_root=tmp_path)
    assert report["prepared_count"] == 1
    assert not report["decode_errors"]
    assert (refs / "type2_cassette_real" / "musik_enc_candidate_encoded.wav").exists()
    specs = build_case_specs(44_100, reference_dir=refs)
    assert "cand_musik_enc_candidate" in specs
    assert specs["cand_musik_enc_candidate"]["group"] == "cassette_music_candidate"


def test_prepare_known_music_candidates_finds_file_recursively(tmp_path):
    import soundfile as sf

    fs = 44_100
    t = np.arange(fs // 4) / fs
    audio = np.column_stack([0.2 * np.sin(2 * np.pi * 400.0 * t), 0.2 * np.sin(2 * np.pi * 800.0 * t)])
    nested = tmp_path / "nested" / "deeper"
    nested.mkdir(parents=True)
    sf.write(nested / "musik_enc.wav", audio, fs)
    refs = tmp_path / "refs"
    report = prepare_known_music_candidates(refs, fs=44_100, search_root=tmp_path)
    assert report["prepared_count"] == 1
    assert (refs / "type2_cassette_real" / "musik_enc_candidate_encoded.wav").exists()


def test_reference_case_discovery_resamples_to_requested_sample_rate(tmp_path):
    import soundfile as sf

    fs_ref = 48_000
    fs_target = 44_100
    ref_root = tmp_path / "refs" / "type2_cassette_real"
    ref_root.mkdir(parents=True)
    t = np.arange(fs_ref // 5) / fs_ref
    src = np.sin(2 * np.pi * 1000.0 * t)
    enc = 0.8 * src
    dec = 0.9 * src
    sf.write(ref_root / "resample_case_source.wav", np.column_stack([src, src]), fs_ref)
    sf.write(ref_root / "resample_case_encoded.wav", np.column_stack([enc, enc]), fs_ref)
    sf.write(ref_root / "resample_case_reference_decode.wav", np.column_stack([dec, dec]), fs_ref)
    (ref_root / "resample_case.json").write_text(
        json.dumps(
            {
                "case_name": "resample_case",
                "category": "cassette_reference",
                "source_type": "real",
                "cassette_priority": True,
                "license": "CC-BY 4.0",
                "origin": "unit-test",
                "notes": "resample test",
                "trust_level": "medium",
            }
        )
    )
    specs = build_case_specs(fs_target, reference_dir=tmp_path / "refs", source_type_filter={"real"})
    case = specs["ref_resample_case"]
    assert int(case["prepared_sample_rate_hz"]) == fs_target
    assert int(case["encoded_sample_rate_hz"]) == fs_ref
    assert np.asarray(case["input"]).shape[0] > 0


def test_firmware_ino_has_linked_stereo_detector_and_central_output_finalize():
    ino = (ROOT / "ocx_type2_teensy41_decoder.ino").read_text(encoding="utf-8")
    assert "processStereo(" in ino
    assert "finalizeOutput(" in ino
    assert "linkedEnv2" in ino
    assert "fmaxf(scL * scL, scR * scR)" in ino
