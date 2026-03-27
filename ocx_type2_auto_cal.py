from __future__ import annotations

from dataclasses import dataclass


def clamp(x: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, x))


@dataclass(frozen=True)
class ToneTelemetry:
    tone_left: float
    tone_right: float
    peak_left: float
    peak_right: float
    fresh_tone_left: bool = True
    fresh_tone_right: bool = True
    fresh_peak_left: bool = True
    fresh_peak_right: bool = True
    tone_left_age_ms: int = 0
    tone_right_age_ms: int = 0
    peak_left_age_ms: int = 0
    peak_right_age_ms: int = 0


@dataclass(frozen=True)
class ToneAcceptance:
    level_ok: bool
    tonal_ok: bool
    stability_ok: bool
    lr_ok: bool
    fresh_ok: bool
    accepted: bool
    reject_reason: str


def evaluate_tone_acceptance(t: ToneTelemetry, prev_peak_left: float, prev_peak_right: float) -> ToneAcceptance:
    peak_mono = max(t.peak_left, t.peak_right)
    tone_metric = max(t.tone_left, t.tone_right)
    fresh_window_ms = 450
    fresh_ok = (
        t.fresh_tone_left
        and t.fresh_tone_right
        and t.fresh_peak_left
        and t.fresh_peak_right
        and t.tone_left_age_ms <= fresh_window_ms
        and t.tone_right_age_ms <= fresh_window_ms
        and t.peak_left_age_ms <= fresh_window_ms
        and t.peak_right_age_ms <= fresh_window_ms
    )
    level_ok = 0.03 < peak_mono < 0.95
    tonal_ok = tone_metric > 0.46
    stability_ok = abs(t.peak_left - prev_peak_left) < 0.12 and abs(t.peak_right - prev_peak_right) < 0.12
    lr_peak_mismatch = abs(t.peak_left - t.peak_right) / max(peak_mono, 1.0e-6)
    lr_tone_mismatch = abs(t.tone_left - t.tone_right) / max(tone_metric, 1.0e-6)
    lr_relevant = peak_mono >= 0.06
    lr_ok = (not lr_relevant) or (lr_peak_mismatch < 0.55 and lr_tone_mismatch < 0.70)

    reason = "none"
    if not fresh_ok:
        reason = "reject_no_fresh_data"
    elif not level_ok and peak_mono <= 0.03:
        reason = "reject_peak_too_low"
    elif not level_ok and peak_mono >= 0.95:
        reason = "reject_peak_too_high"
    elif not tonal_ok:
        reason = "reject_tone_too_weak"
    elif not stability_ok:
        reason = "reject_unstable"
    elif not lr_ok:
        reason = "reject_lr_mismatch"

    accepted = fresh_ok and level_ok and tonal_ok and stability_ok and lr_ok
    return ToneAcceptance(level_ok, tonal_ok, stability_ok, lr_ok, fresh_ok, accepted, reason)


@dataclass
class BlockTracker:
    blocks_seen: int = 0
    blocks_valid: int = 0
    tone_blocks: int = 0
    tone_segments: int = 0
    current_segment_blocks: int = 0
    current_segment_valid_blocks: int = 0
    silence_blocks: int = 0


def update_block_tracker(state: BlockTracker, accepted: bool) -> BlockTracker:
    state.blocks_seen += 1
    if accepted:
        state.blocks_valid += 1
        state.tone_blocks += 1
        state.current_segment_blocks += 1
        state.current_segment_valid_blocks += 1
        state.silence_blocks = 0
    else:
        state.silence_blocks += 1
        if state.current_segment_blocks > 0 and state.silence_blocks >= 4:
            if state.current_segment_valid_blocks >= 120:
                state.tone_segments += 1
            state.current_segment_blocks = 0
            state.current_segment_valid_blocks = 0
    return state


def has_enough_measurement(state: BlockTracker, elapsed_ms: int) -> bool:
    enough_segments = state.tone_segments >= 3 or (state.tone_segments >= 2 and state.current_segment_valid_blocks >= 240)
    return enough_segments and state.tone_blocks >= 700 and state.blocks_seen >= 780 and elapsed_ms >= 180_000


def resolve_wizard_expected_transport(deck_type: str, active_transport: str) -> str:
    if deck_type == "SINGLE_LW":
        return "LW1"
    return "LW2" if active_transport == "LW2" else "LW1"


def profile_slot_for_transport(deck_type: str, expected_transport: str) -> str:
    if deck_type == "SINGLE_LW":
        return "single_profile"
    return "lw2_profile" if expected_transport == "LW2" else "lw1_profile"


@dataclass
class SegmentMetaCollector:
    max_segments: int = 3
    durations_blocks: list[int] | None = None
    peak_avg: list[float] | None = None
    tone_avg: list[float] | None = None
    peak_spread: list[float] | None = None

    def __post_init__(self) -> None:
        if self.durations_blocks is None:
            self.durations_blocks = []
        if self.peak_avg is None:
            self.peak_avg = []
        if self.tone_avg is None:
            self.tone_avg = []
        if self.peak_spread is None:
            self.peak_spread = []

    def add_segment(self, duration_blocks: int, peak_avg: float, tone_avg: float, peak_spread: float) -> None:
        if len(self.durations_blocks or []) >= self.max_segments:
            return
        self.durations_blocks.append(int(duration_blocks))
        self.peak_avg.append(float(peak_avg))
        self.tone_avg.append(float(tone_avg))
        self.peak_spread.append(float(peak_spread))


@dataclass(frozen=True)
class CandidateMetrics:
    output_clip_count: float
    output_peak_headroom: float
    output_rms_plausibility: float
    near_limit_behavior: float
    stability_penalty: float
    channel_balance_penalty: float
    total_score: float


def _candidate_score(
    peak_avg: float,
    rms_avg: float,
    lr_mismatch: float,
    stability_penalty: float,
    output_trim_db: float,
    headroom_db: float,
    target_peak: float,
    target_rms: float,
    hot_penalty_scale: float,
    low_rms_guard: float,
) -> CandidateMetrics:
    out_peak = peak_avg * (10.0 ** (output_trim_db / 20.0)) * (10.0 ** (-headroom_db / 20.0))
    clip_presence = max(0.0, out_peak - 0.98)
    near_limit = max(0.0, peak_avg - (target_peak + 0.34))
    rms_plausibility = abs(rms_avg - target_rms)
    peak_headroom = max(0.0, 0.98 - out_peak)
    score = (
        clip_presence * 8.0
        + near_limit * 3.5
        + max(0.0, peak_avg - 0.62) * hot_penalty_scale
        + max(0.0, rms_avg - 0.32) * (hot_penalty_scale * 0.75)
        + max(0.0, low_rms_guard - rms_avg) * 1.5
        + rms_plausibility * 1.8
        + abs(peak_avg - target_peak) * 1.5
        + lr_mismatch * 1.5
        + stability_penalty * 1.1
    )
    return CandidateMetrics(
        output_clip_count=clip_presence * 120.0,
        output_peak_headroom=peak_headroom,
        output_rms_plausibility=rms_plausibility,
        near_limit_behavior=near_limit,
        stability_penalty=stability_penalty,
        channel_balance_penalty=lr_mismatch,
        total_score=score,
    )


def decide_candidate(peak_avg: float, rms_avg: float, lr_mismatch: float, stability_penalty: float) -> tuple[str, CandidateMetrics, CandidateMetrics]:
    universal = _candidate_score(
        peak_avg, rms_avg, lr_mismatch, stability_penalty, -1.0, 1.0, target_peak=0.52, target_rms=0.30, hot_penalty_scale=4.0, low_rms_guard=0.0
    )
    return ("universal", universal, universal)
