# Encoder Feasibility Summary

- Is the encoder path stable offline? **Yes. decode/encode/roundtrip harness runs completed with finite bounded outputs.**
- Which cases are strongest? **Tone-like and transient-structured synthetic cases are strongest in the roundtrip metrics.**
- Which cases are weakest? **Broadband and mixed-material cases remain weakest (notably pink-noise and mixed spectral content).**
- Does broadband / mixed material remain a risk? **Yes. This remains the main caution area.**
- Why does 44.1 kHz remain the default? **Profile, harness, tests, and firmware continue to target 44.1 kHz as the primary baseline.**
- What still blocks production confidence? **Historical dbx Type-II exactness is still not claimed; final confidence remains hardware/material dependent.**

## Snapshot

- Decode score_total: -11640.272222886291
- Encode score_total: -10907.732831217289
- Roundtrip score_total: -11250.051754419952
