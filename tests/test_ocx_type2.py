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

from ocx_type2_harness import build_cases, compare
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
