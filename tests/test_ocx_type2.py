import os
import re
import subprocess
import sys
from pathlib import Path

import numpy as np

# Conflict-resolution note: this suite is kept synchronized with the main-based harness/simulator APIs.

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from ocx_type2_harness import build_cases, compare
from ocx_type2_wav_sim import PROFILE_PATH, Decoder, Params, ensure_stereo


def _firmware_constants() -> dict[str, float]:
    ino = (ROOT / "ocx_type2_teensy41_decoder.ino").read_text()
    pairs = re.findall(r"k([A-Za-z0-9_]+)\s*=\s*([-0-9.]+)f?;", ino)
    out: dict[str, float] = {}
    for key, value in pairs:
        out[key] = float(value)
    return out


def test_profile_defaults_load():
    params = Params.from_profile(PROFILE_PATH)
    assert params.input_trim_db == -3.0
    assert params.deemph_hz == 1850.0


def test_profile_and_firmware_sync_for_core_defaults():
    params = Params.from_profile(PROFILE_PATH)
    firmware = _firmware_constants()

    assert firmware["LineInLevel"] == 0.0
    assert firmware["LineOutLevel"] == 29.0
    assert firmware["InputTrimDb"] == params.input_trim_db
    assert firmware["OutputTrimDb"] == params.output_trim_db
    assert firmware["Strength"] == params.strength
    assert firmware["ReferenceDb"] == params.reference_db
    assert firmware["AttackMs"] == params.attack_ms
    assert firmware["ReleaseMs"] == params.release_ms
    assert firmware["SidechainHpHz"] == params.sidechain_hp_hz
    assert firmware["SidechainShelfHz"] == params.sidechain_shelf_hz
    assert firmware["SidechainShelfDb"] == params.sidechain_shelf_db
    assert firmware["DeemphHz"] == params.deemph_hz
    assert firmware["DeemphDb"] == params.deemph_db
    assert firmware["SoftClipDrive"] == params.soft_clip_drive
    assert firmware["DcBlockHz"] == params.dc_block_hz
    assert firmware["HeadroomDb"] == params.headroom_db


def test_decoder_finite_on_all_harness_cases_low_rate():
    params = Params.from_profile(PROFILE_PATH)
    fs = 4000
    for name, audio in build_cases(fs).items():
        out = Decoder(fs, params).process(audio)
        assert np.isfinite(out).all(), name


def test_output_bounded_on_all_harness_cases_low_rate():
    params = Params.from_profile(PROFILE_PATH)
    fs = 4000
    for name, audio in build_cases(fs).items():
        out = Decoder(fs, params).process(audio)
        assert np.max(np.abs(out)) <= 1.0 + 1e-9, name


def test_production_rate_path_44100_finite_and_bounded():
    params = Params.from_profile(PROFILE_PATH)
    fs = 44100
    audio = np.sin(2 * np.pi * 1000 * np.arange(fs // 2) / fs)
    out = Decoder(fs, params).process(audio)
    assert np.isfinite(out).all()
    assert np.max(np.abs(out)) <= 1.0 + 1e-9


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


def test_harness_compare_handles_reference_length_mismatch():
    inp = ensure_stereo(np.sin(2 * np.pi * 220 * np.arange(1000) / 4000))
    out = inp * 0.8
    ref = ensure_stereo(np.sin(2 * np.pi * 220 * np.arange(700) / 4000))
    metrics = compare(inp, out, ref)
    assert metrics["mse_vs_reference"] is not None
    assert np.isfinite(metrics["mse_vs_reference"])


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
