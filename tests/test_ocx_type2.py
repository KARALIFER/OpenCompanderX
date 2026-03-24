from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import importlib
import subprocess

import numpy as np

from ocx_type2_harness import build_cases
from ocx_type2_wav_sim import PROFILE_PATH, Decoder, Params, ensure_stereo


def test_profile_defaults_load():
    params = Params.from_profile(PROFILE_PATH)
    assert params.input_trim_db == -3.0
    assert params.deemph_hz == 1850.0


def test_decoder_finite_on_all_harness_cases():
    params = Params.from_profile(PROFILE_PATH)
    fs = 44100
    for name, audio in build_cases(fs).items():
        out = Decoder(fs, params).process(audio)
        assert np.isfinite(out).all(), name
        assert np.max(np.abs(out)) <= 1.0 + 1e-9, name


def test_stereo_identity_case_remains_balanced():
    params = Params.from_profile(PROFILE_PATH)
    audio = build_cases(44100)["stereo_identical"]
    out = Decoder(44100, params).process(audio)
    diff = np.max(np.abs(out[:, 0] - out[:, 1]))
    assert diff < 1e-9


def test_ensure_stereo_supports_mono_shapes():
    mono = np.array([0.1, -0.2, 0.3])
    mono_col = mono[:, None]
    stereo = np.column_stack([mono, mono * 0.5])

    out1 = ensure_stereo(mono)
    out2 = ensure_stereo(mono_col)
    out3 = ensure_stereo(stereo)

    assert out1.shape == (3, 2)
    assert out2.shape == (3, 2)
    assert out3.shape == (3, 2)
    assert np.allclose(out1[:, 0], mono) and np.allclose(out1[:, 1], mono)
    assert np.allclose(out2[:, 0], mono) and np.allclose(out2[:, 1], mono)
    assert np.allclose(out3, stereo)


def test_decoder_process_accepts_mono_shapes():
    params = Params.from_profile(PROFILE_PATH)
    dec = Decoder(44100, params)
    mono = np.linspace(-0.25, 0.25, 128)
    out1 = dec.process(mono)
    dec.reset()
    out2 = dec.process(mono[:, None])
    assert out1.shape == (128, 2)
    assert out2.shape == (128, 2)
    assert np.isfinite(out1).all()
    assert np.isfinite(out2).all()


def test_non_plot_path_does_not_require_matplotlib_import():
    sys.modules.pop("matplotlib", None)
    sys.modules.pop("matplotlib.pyplot", None)
    import ocx_type2_wav_sim

    importlib.reload(ocx_type2_wav_sim)
    assert "matplotlib" not in sys.modules
    assert "matplotlib.pyplot" not in sys.modules


def test_simulator_help_without_plot_flag_runs_without_matplotlib():
    cp = subprocess.run([sys.executable, "ocx_type2_wav_sim.py", "--help"], check=True, capture_output=True, text=True)
    assert "--plot" in cp.stdout
