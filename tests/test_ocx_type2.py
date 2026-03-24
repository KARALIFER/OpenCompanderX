from pathlib import Path

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
    diff = np.max(np.abs(out[:, 0] - out[:, 1]))
    assert diff < 1e-9
