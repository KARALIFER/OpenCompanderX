from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import numpy as np

from ocx_type2_harness import build_cases
from ocx_type2_wav_sim import PROFILE_PATH, Decoder, Params


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
    assert np.max(np.abs(out[:, 0] - out[:, 1])) < 1e-9


def test_decoder_process_accepts_mono_1d():
    params = Params.from_profile(PROFILE_PATH)
    dec = Decoder(44100, params)
    mono = np.linspace(-0.25, 0.25, 128)
    out = dec.process(mono)
    assert out.shape == (128, 2)
    assert np.isfinite(out).all()


def test_decoder_process_accepts_mono_2d_col():
    params = Params.from_profile(PROFILE_PATH)
    dec = Decoder(44100, params)
    mono = np.linspace(-0.25, 0.25, 128)[:, None]
    out = dec.process(mono)
    assert out.shape == (128, 2)
    assert np.isfinite(out).all()
