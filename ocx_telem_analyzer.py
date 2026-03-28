#!/usr/bin/env python3
"""Parse OpenCompanderX DIAG/TLM serial logs and summarize clip/guard behavior."""

from __future__ import annotations

import argparse
import json
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Any

NUM_RE = re.compile(r"^-?\d+(?:\.\d+)?$")
KEYVAL_RE = re.compile(r"([A-Za-z][A-Za-z0-9_]*?)=([^\s]+)")
TS_BRACKET_RE = re.compile(r"\[(\d+(?:\.\d+)?)\]")


@dataclass
class ClipJump:
    line_no: int
    time_hint_s: float | None
    prev_total: int
    new_total: int
    delta: int
    out_clip_new: int | None
    guard_state: str | None
    near_limit_pct_10s: float | None
    clamp_boost_pct_10s: float | None


def _coerce(token: str) -> float | int | str:
    if NUM_RE.match(token):
        if "." in token:
            return float(token)
        return int(token)
    return token


def parse_line(line: str) -> dict[str, Any]:
    parsed: dict[str, Any] = {}
    for k, v in KEYVAL_RE.findall(line):
        parsed[k] = _coerce(v.rstrip(",;"))
    ts_match = TS_BRACKET_RE.search(line)
    if ts_match:
        parsed.setdefault("time_hint_s", float(ts_match.group(1)))
    return parsed


def summarize_log(lines: list[str]) -> dict[str, Any]:
    clip_jumps: list[ClipJump] = []
    guard_states: dict[str, int] = {}
    total_samples = 0
    prev_total: int | None = None

    for idx, line in enumerate(lines, start=1):
        parsed = parse_line(line)
        if "samples" in parsed and isinstance(parsed["samples"], int):
            total_samples = max(total_samples, int(parsed["samples"]))

        state = parsed.get("guardState") or parsed.get("guard")
        if isinstance(state, str):
            guard_states[state] = guard_states.get(state, 0) + 1

        out_total = parsed.get("clipOutTotal")
        if isinstance(out_total, int):
            if prev_total is not None and out_total > prev_total:
                clip_jumps.append(
                    ClipJump(
                        line_no=idx,
                        time_hint_s=float(parsed["time_hint_s"]) if isinstance(parsed.get("time_hint_s"), (float, int)) else None,
                        prev_total=prev_total,
                        new_total=out_total,
                        delta=out_total - prev_total,
                        out_clip_new=int(parsed["clipOutNew"]) if isinstance(parsed.get("clipOutNew"), int) else None,
                        guard_state=state if isinstance(state, str) else None,
                        near_limit_pct_10s=float(parsed["nearLimitPct10s"]) if isinstance(parsed.get("nearLimitPct10s"), (float, int)) else None,
                        clamp_boost_pct_10s=float(parsed["clampBoostPct10s"]) if isinstance(parsed.get("clampBoostPct10s"), (float, int)) else None,
                    )
                )
            prev_total = out_total

    total_clip_delta = int(sum(j.delta for j in clip_jumps))
    first_jump = clip_jumps[0].__dict__ if clip_jumps else None
    likely_startup_only = bool(clip_jumps) and all((j.line_no <= 25) for j in clip_jumps)

    return {
        "lines": len(lines),
        "max_samples_seen": total_samples,
        "clip_out_total_final": prev_total,
        "clip_jump_count": len(clip_jumps),
        "clip_out_delta_sum": total_clip_delta,
        "likely_startup_only": likely_startup_only,
        "guard_states_seen": guard_states,
        "first_clip_jump": first_jump,
        "clip_jumps": [j.__dict__ for j in clip_jumps],
    }


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", type=Path, required=True, help="Path to raw serial capture with [TLM]/[DIAG] lines.")
    ap.add_argument("--out", type=Path, help="Optional JSON output file.")
    args = ap.parse_args()

    lines = args.log.read_text(encoding="utf-8", errors="ignore").splitlines()
    summary = summarize_log(lines)
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(summary, indent=2))
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
