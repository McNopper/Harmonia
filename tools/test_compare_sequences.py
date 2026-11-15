#!/usr/bin/env python3
"""
test_compare_sequences.py - CPU unit receipts for the ANI6 sequence gate
(compare_sequences.py: the PAR0 metric set extended over time; no GPU, no rendering).

Owner directive (2026-11-15): non-AI techniques only in the parity-metric avenue -
the video metric here is the sanctioned still-image set gated per frame plus temporal
aggregates, never a learned/neural score (CGVQM, MILO - see PLAN PAR3/PAR2).

Covered: frame pairing (index-based + sorted fallback + mismatch rejections),
temporal aggregates (worst-frame direction per metric), the strict-AND-across-frames
sequence gate, the compare_renders threshold-default mirror, and the per-frame
compute_metrics -> evaluate_gate wiring both directions.

Usage:  python tools/test_compare_sequences.py
Exit:   0 = all checks pass, 1 = any check fails.
"""

from __future__ import annotations

import sys
import tempfile
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))

import compare_renders as cr
import compare_sequences as cs

_FAILURES: list[str] = []


def check(name: str, ok: bool, detail: str = "") -> None:
    status = "ok  " if ok else "FAIL"
    print(f"  [{status}] {name}" + (f"  ({detail})" if detail else ""))
    if not ok:
        _FAILURES.append(name)


def _expect_exit(fn, *args, **kwargs) -> bool:
    """True when the call rejects its input with a sys.exit error."""
    try:
        fn(*args, **kwargs)
    except SystemExit:
        return True
    return False


def _frame(index: int, metrics: dict, failed: list[str]) -> dict:
    return {"index": index, "metrics": metrics, "failed": failed, "passed": not failed}


def _test_pair() -> tuple[np.ndarray, np.ndarray]:
    """Deterministic 32x32 HDR pair (>= 11x11 so windowed SSIM is defined)."""
    y, x = np.mgrid[0:32, 0:32].astype(np.float64)
    ref = np.stack([
        0.05 + 0.03 * x / 31.0,
        0.06 + 0.02 * y / 31.0,
        0.4 + 0.5 * (x / 31.0) * (y / 31.0),
    ], axis=-1)
    return ref, ref.copy()


def main() -> int:
    # ── frame_index: ANI7 naming contract ─────────────────────────────────────
    check("frame_index: _0012.exr -> 12", cs.frame_index(Path("seq_0012.exr")) == 12)
    check("frame_index: uppercase .EXR still matches",
          cs.frame_index(Path("seq_0003.EXR")) == 3)
    check("frame_index: no trailing index -> None",
          cs.frame_index(Path("reference.exr")) is None)

    # ── pair_frames: index pairing, sorted fallback, mismatch rejections ──────
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        ref_dir = root / "ref"
        cand_dir = root / "cand"
        ref_dir.mkdir()
        cand_dir.mkdir()
        for name in ("seq_0002.exr", "seq_0000.exr", "seq_0001.exr"):
            (ref_dir / name).write_bytes(b"")
        for name in ("theia_0001.exr", "theia_0002.exr", "theia_0000.exr"):
            (cand_dir / name).write_bytes(b"")

        ref_frames = cs.list_frames(ref_dir)
        cand_frames = cs.list_frames(cand_dir)
        pairs = cs.pair_frames(ref_frames, cand_frames)
        check("pairing: by trailing index, ascending, not sorted-name order",
              [cs.frame_index(a) for a, _ in pairs] == [0, 1, 2]
              and [cs.frame_index(b) for _, b in pairs] == [0, 1, 2],
              f"{[a.name for a, _ in pairs]}")

        # Missing candidate frame for an index -> hard error (never zip-truncated).
        (cand_dir / "theia_0001.exr").unlink()
        cand_frames = cs.list_frames(cand_dir)
        check("pairing: missing candidate index is rejected",
              _expect_exit(cs.pair_frames, ref_frames, cand_frames))
        (cand_dir / "theia_0001.exr").write_bytes(b"")

        # No indices at all -> sorted-order fallback requires equal counts.
        plain_ref = ref_dir / "a.exr"
        plain_ref.write_bytes(b"")
        plain_cand = cand_dir / "b.exr"
        plain_cand.write_bytes(b"")
        no_idx_ref = [plain_ref]
        no_idx_cand = [plain_cand, cand_dir / "theia_0000.exr"]
        check("pairing: count mismatch without indices is rejected",
              _expect_exit(cs.pair_frames, no_idx_ref, no_idx_cand))
        check("pairing: equal counts without indices zip in sorted order",
              cs.pair_frames(no_idx_ref, [plain_cand]) == [(plain_ref, plain_cand)])

        check("list_frames: empty directory is rejected",
              _expect_exit(cs.list_frames, root / "does-not-exist"))

    # ── aggregates: worst-frame direction + first/last drift ──────────────────
    frames = [
        _frame(0, {"mean_diff": 1.0, "ssim": 0.99}, []),
        _frame(1, {"mean_diff": 3.0, "ssim": 0.95}, []),
        _frame(2, {"mean_diff": 2.0, "ssim": 0.98}, []),
    ]
    agg_max = cs.aggregate(frames, "mean_diff", lower_is_better=False)
    check("aggregate: mean_diff worst = max @ its frame",
          agg_max["worst"] == 3.0 and agg_max["worst_frame"] == 1,
          f"worst={agg_max['worst']} @ {agg_max['worst_frame']}")
    check("aggregate: mean over frames",
          abs(agg_max["mean"] - 2.0) < 1e-12, f"{agg_max['mean']}")
    agg_min = cs.aggregate(frames, "ssim", lower_is_better=True)
    check("aggregate: SSIM worst = min @ its frame",
          agg_min["worst"] == 0.95 and agg_min["worst_frame"] == 1,
          f"worst={agg_min['worst']} @ {agg_min['worst_frame']}")
    check("aggregate: first->last drift inputs",
          agg_max["first"] == 1.0 and agg_max["last"] == 2.0)

    # ── sequence gate: strict AND across frames ───────────────────────────────
    ok, failures = cs.sequence_gate(frames)
    check("sequence gate: all frames pass -> PASS", ok and not failures)

    failing = [*frames[:2], _frame(3, {"mean_diff": 9.0, "ssim": 0.9},
                                   ["mean_diff <= 4.0"])]
    ok2, failures2 = cs.sequence_gate(failing)
    check("sequence gate: one failing frame fails the run",
          not ok2 and len(failures2) == 1 and "frame 0003" in failures2[0],
          failures2[0] if failures2 else "")

    # ── threshold mirror: compare_renders' documented PLAN §7 defaults ────────
    args = cs.make_gate_args()
    check("gate defaults: mean<=4.0 rel<=0.01 ssim>=0.98 hist>=0.999",
          (args.threshold, args.rel_mse_threshold, args.ssim_threshold,
           args.lum_hist_threshold) == (4.0, 0.01, 0.98, 0.999))
    check("gate defaults: gate=all, tonemap=aces, no opt-in thresholds",
          args.gate == "all" and args.tonemap == "aces"
          and args.psnr_threshold is None and args.tm_mean_threshold is None
          and args.relative_threshold is None)
    check("gate defaults: mirror matches compare_renders' documented values",
          (args.threshold, args.rel_mse_threshold, args.ssim_threshold,
           args.lum_hist_threshold) == (4.0, 0.01, 0.98, 0.999))
    overridden = cs.make_gate_args(threshold=1.0)
    check("make_gate_args: override applies", overridden.threshold == 1.0)
    check("make_gate_args: unknown option rejected",
          _expect_exit(cs.make_gate_args, not_a_option=1))

    # ── per-frame wiring: compute_metrics -> evaluate_gate through make_gate_args
    ref, cand = _test_pair()
    gate_args = cs.make_gate_args()
    m_ident = cr.compute_metrics(ref, cand, channel=gate_args.channel)
    passed_ident, _ = cr.evaluate_gate(m_ident, gate_args)
    check("per-frame wiring: identical pair passes the mirrored gate", passed_ident)

    m_shift = cr.compute_metrics(ref, ref + 0.10, channel=gate_args.channel)
    passed_shift, shift_labels = cr.evaluate_gate(m_shift, gate_args)
    check("per-frame wiring: +0.10 shift fails (mean_diff 25.5 > 4.0)",
          not passed_shift and m_shift["mean_diff"] > 25.0,
          f"mean_diff={m_shift['mean_diff']:.3f}; {', '.join(shift_labels)}")

    # ── failed_gate_labels: the FAILING subset only (not every active label) ──
    # mean/rel_mse/hist pass, SSIM alone fails -> exactly one label.
    partial = dict(m_ident)
    partial["ssim"] = 0.50
    failed_only = cr.failed_gate_labels(partial, gate_args)
    check("failed_gate_labels: partial failure lists ONLY the failing check",
          failed_only == ["SSIM >= 0.98"], f"{failed_only}")
    check("failed_gate_labels: passing metrics -> empty list",
          cr.failed_gate_labels(m_ident, gate_args) == [])
    check("failed_gate_labels: evaluate_gate still returns the full active set",
          len(cr.evaluate_gate(partial, gate_args)[1]) == 4)

    print()
    if _FAILURES:
        print(f"FAILED: {len(_FAILURES)} check(s): {', '.join(_FAILURES)}")
        return 1
    print("ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
