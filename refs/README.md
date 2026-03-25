# Reference data layout (dbx Type II cassette primary)

This project separates reference material into:

- `refs/type2_cassette_real/` for legally usable real recordings (with explicit origin/license metadata).
- `refs/type2_cassette_synth/` for generated synthetic/approximative references.

Per case files:

- `<case>_source.wav`
- `<case>_encoded.wav`
- `<case>_reference_decode.wav` (optional but recommended)
- `<case>.json` metadata (`case_name`, `category`, `source_type`, `cassette_priority`, `license`, `origin`, `notes`, `trust_level`)

Optional encoded-only candidate layout (not hard reference, only stress/practice material):

- `<case>_encoded.wav`
- `<case>.json` with `"encoded_candidate_only": true`

Use `python3 ocx_type2_harness.py --generate-synth-refs --reference-dir refs` to generate a reproducible synthetic pack.
Use `python3 ocx_type2_harness.py --prepare-known-music-candidates --reference-dir refs` to import local `musik_enc.wav` / `musicfox_shopping_street.mp3` if present.

Real references are intentionally empty by default unless legal files with clear rights are available.
