# Reference data layout (dbx Type II cassette primary)

This project separates reference material into:

- `refs/type2_cassette_real/` for legally usable real recordings (with explicit origin/license metadata).
- `refs/type2_cassette_synth/` for generated synthetic/approximative references.

Per case files:

- `<case>_source.wav`
- `<case>_encoded.wav`
- `<case>_reference_decode.wav` (optional but recommended)
- `<case>.json` metadata (`case_name`, `category`, `source_type`, `cassette_priority`, `license`, `origin`, `notes`, `trust_level`)

Use `python3 ocx_type2_harness.py --generate-synth-refs --reference-dir refs` to generate a reproducible synthetic pack.

Real references are intentionally empty by default unless legal files with clear rights are available.
