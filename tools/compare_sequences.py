#!/usr/bin/env python3
"""compare_sequences.py - animated-parity gate (ANI6): the PAR0 metric set over a sequence pair.

Owner directive (2026-11-15): **non-AI techniques only in this avenue** - learned/neural
quality metrics are excluded (CGVQM's pretrained Kinetics-400 R3D-18 backbone and MILO's
multiscale CNN are out of policy; see PLAN.md PAR3/PAR2 tombstones). The video metric here
is therefore the sanctioned PAR0 set extended over time:

  * every frame pair is gated with compare_renders' strict-AND still-image metric set
    (mean_diff / Rel-MSE / SSIM / luminance-histogram, same thresholds and defaults),
  * the sequence gate is a strict AND ACROSS FRAMES (one failing frame fails the run),
  * the temporal layer reports per-metric aggregates - mean, worst frame + its index -
    and a first->last drift diagnostic (catches error that accumulates over the timeline).

Inputs are the per-frame scene-referred linear EXRs each renderer writes with
`--sequence-frames` (ANI7): `<stem>_0000.exr`, `<stem>_0001.exr`, ... Frame pairing uses
the trailing `_<index>` number when both directories carry it, else sorted order; a frame
count mismatch is an error (never silently zip-truncated).

Exit code: 0 = sequence gate PASS, 1 = FAIL (same contract as compare_renders.py).
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

import numpy as np

import compare_renders as cr

# Trailing frame index written by ANI7's sequence output: <stem>_0000.exr
_FRAME_INDEX_RE = re.compile(r"_(\d+)\.exr$", re.IGNORECASE)

# The four gated PAR0 metrics and their worst-frame direction (True = larger is worse).
_GATED_MAX = ("mean_diff", "rel_mse")
_GATED_MIN = ("ssim", "lum_hist_corr")


def frame_index(path: Path) -> int | None:
    """Trailing _<n>.exr index, or None when the name does not carry one."""
    m = _FRAME_INDEX_RE.search(path.name)
    return int(m.group(1)) if m else None


def list_frames(directory: Path) -> list[Path]:
    if not directory.is_dir():
        sys.exit(f"ERROR: not a directory: {directory}")
    frames = sorted(p for p in directory.iterdir() if p.suffix.lower() == ".exr")
    if not frames:
        sys.exit(f"ERROR: no .exr frames in {directory}")
    return frames


def pair_frames(ref_frames: list[Path], cand_frames: list[Path]) -> list[tuple[Path, Path]]:
    """Pair reference/candidate frames by trailing index when both sides carry one,
    otherwise by sorted position. Count mismatch is an error (never zip-truncated)."""
    ref_idx = [frame_index(p) for p in ref_frames]
    cand_idx = [frame_index(p) for p in cand_frames]
    if all(i is not None for i in ref_idx) and all(i is not None for i in cand_idx):
        ref_map = dict(zip(ref_idx, ref_frames))
        cand_map = dict(zip(cand_idx, cand_frames))
        missing_cand = sorted(set(ref_map) - set(cand_map))
        missing_ref = sorted(set(cand_map) - set(ref_map))
        if missing_cand or missing_ref:
            sys.exit(
                "ERROR: frame index mismatch - "
                f"missing in candidate: {missing_cand or 'none'}; "
                f"missing in reference: {missing_ref or 'none'}"
            )
        return [(ref_map[i], cand_map[i]) for i in sorted(ref_map)]
    if len(ref_frames) != len(cand_frames):
        sys.exit(
            f"ERROR: frame count mismatch - reference {len(ref_frames)} vs "
            f"candidate {len(cand_frames)} (and no trailing frame indices to pair on)"
        )
    return list(zip(ref_frames, cand_frames))


def make_gate_args(**overrides) -> argparse.Namespace:
    """compare_renders' parser defaults (PLAN §7 thresholds) with optional overrides.
    Kept as a function so the sequence gate and the still gate cannot drift apart:
    change a default in compare_renders' parser and mirror it here."""
    defaults = dict(
        threshold=120.0,
        rel_mse_threshold=250.0,
        ssim_threshold=0.6,
        lum_hist_threshold=0.8,
        psnr_threshold=None,
        relative_threshold=None,
        tm_mean_threshold=None,
        tonemap="aces",
        gate="all",
        channel="luminance",
        rel_mse_eps=cr.REL_MSE_EPS,
        hist_bins=256,
        allow_small_ssim=False,
    )
    unknown = set(overrides) - set(defaults)
    if unknown:
        sys.exit(f"ERROR: unknown gate option(s): {sorted(unknown)}")
    defaults.update(overrides)
    return argparse.Namespace(**defaults)


def aggregate(per_frame: list[dict], metric: str, lower_is_better: bool) -> dict:
    """Temporal aggregate of one metric over the frames: mean, worst value + index.
    Each entry is a per-frame record ({index, metrics, failed, passed})."""
    values = np.array([m["metrics"][metric] for m in per_frame], dtype=np.float64)
    worst_pos = int(np.argmin(values)) if lower_is_better else int(np.argmax(values))
    return {
        "mean": float(np.mean(values)),
        "worst": float(values[worst_pos]),
        "worst_frame": per_frame[worst_pos]["index"],
        "first": float(values[0]),
        "last": float(values[-1]),
    }


def sequence_gate(per_frame: list[dict]) -> tuple[bool, list[str]]:
    """Strict AND across frames: every frame must pass its still-image gate.
    Returns (passed, failed_check_labels)."""
    failures: list[str] = []
    for m in per_frame:
        for label in m["failed"]:
            failures.append(f"frame {m['index']:04d}: {label}")
    return (not failures), failures


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("reference_dir", type=Path, help="Hyperion sequence directory (EXR frames)")
    parser.add_argument("candidate_dir", type=Path, help="Theia sequence directory (EXR frames)")
    # Threshold defaults mirror compare_renders.py's parser (PLAN §7 strict AND).
    parser.add_argument("--threshold", type=float, default=120.0,
                        help="mean-diff threshold in 1/255 units (default: 120)")
    parser.add_argument("--rel-mse-threshold", type=float, default=250.0,
                        help="Rel-MSE threshold (default: 250 - loose guard)")
    parser.add_argument("--ssim-threshold", type=float, default=0.6,
                        help="SSIM threshold (default: 0.6, tone-mapped sRGB)")
    parser.add_argument("--lum-hist-threshold", type=float, default=0.8,
                        help="luminance-histogram correlation threshold (default: 0.8)")
    parser.add_argument("--psnr-threshold", type=float, default=None,
                        help="optional PSNR threshold in dB (ANDed when given)")
    parser.add_argument("--tm-mean-threshold", type=float, default=None,
                        help="optional tone-mapped mean-diff threshold in 1/255 (PAR1; ANDed when given)")
    parser.add_argument("--relative-threshold", type=float, default=None,
                        help="optional relative mean-error threshold in percent (ANDed when given)")
    parser.add_argument("--tonemap", choices=("aces", "agx"), default="aces",
                        help="tone mapper for the tone-mapped metric layer (PAR1; default aces)")
    parser.add_argument("--channel", default="luminance",
                        help="channel to compare: r, g, b, luminance (default: luminance)")
    parser.add_argument("--gate", choices=("all", "mean", "psnr", "relative", "any", "scale-aware"),
                        default="all",
                        help="per-frame gate mode (default: all = strict AND of the declared metrics)")
    parser.add_argument("--hist-bins", type=int, default=256,
                        help="luminance-histogram bin count (default: 256 + 1 black bin)")
    parser.add_argument("--rel-mse-eps", type=float, default=cr.REL_MSE_EPS,
                        help=f"Rel-MSE denominator epsilon (default: {cr.REL_MSE_EPS:g})")
    parser.add_argument("--allow-small-ssim", action="store_true",
                        help="permit the degraded global SSIM for regions smaller than the 11x11 window")
    args = parser.parse_args()

    ref_frames = list_frames(args.reference_dir)
    cand_frames = list_frames(args.candidate_dir)
    pairs = pair_frames(ref_frames, cand_frames)

    gate_args = make_gate_args(
        threshold=args.threshold,
        rel_mse_threshold=args.rel_mse_threshold,
        ssim_threshold=args.ssim_threshold,
        lum_hist_threshold=args.lum_hist_threshold,
        psnr_threshold=args.psnr_threshold,
        relative_threshold=args.relative_threshold,
        tm_mean_threshold=args.tm_mean_threshold,
        tonemap=args.tonemap,
        gate=args.gate,
        channel=args.channel,
        rel_mse_eps=args.rel_mse_eps,
        hist_bins=args.hist_bins,
        allow_small_ssim=args.allow_small_ssim,
    )

    print(f"Reference : {args.reference_dir} ({len(pairs)} frames)")
    print(f"Candidate : {args.candidate_dir}")
    print(f"Channel   : {args.channel}   gate: {args.gate}")
    print()
    print("  frame  mean_diff   rel_mse      SSIM     hist  result")

    per_frame: list[dict] = []
    for ref_path, cand_path in pairs:
        ref = cr.load_exr(ref_path)
        cand = cr.load_exr(cand_path)
        if ref.shape != cand.shape:
            sys.exit(f"ERROR: shape mismatch at {ref_path.name}: {ref.shape} vs {cand.shape}")
        metrics = cr.compute_metrics(
            ref, cand,
            channel=gate_args.channel,
            rel_mse_eps=gate_args.rel_mse_eps,
            hist_bins=gate_args.hist_bins,
            allow_small_ssim=gate_args.allow_small_ssim,
            tonemap=gate_args.tonemap,
        )
        passed, _ = cr.evaluate_gate(metrics, gate_args)
        # failed_gate_labels reports the FAILING subset only (evaluate_gate's label
        # list is the set of active checks, not the failed ones).
        failed = cr.failed_gate_labels(metrics, gate_args)
        index = frame_index(ref_path)
        entry = {
            "index": index if index is not None else len(per_frame),
            "metrics": metrics,
            "failed": failed,
            "passed": passed,
        }
        per_frame.append(entry)
        print(f"  {entry['index']:04d}  {metrics['mean_diff']:9.4f}  {metrics['rel_mse']:8.5f}  "
              f"{metrics['ssim']:8.5f}  {metrics['lum_hist_corr']:7.5f}  "
              f"{'PASS' if passed else 'FAIL'}")

    passed, failures = sequence_gate(per_frame)

    # Temporal aggregates over the four gated PAR0 metrics (+ tm_mean when thresholded).
    print()
    print("aggregates (mean / worst over frames):")
    agg_specs = [
        ("mean_diff", False, f"<= {args.threshold}"),
        ("rel_mse", False, f"<= {args.rel_mse_threshold}"),
        ("ssim", True, f">= {args.ssim_threshold}"),
        ("lum_hist_corr", True, f">= {args.lum_hist_threshold}"),
    ]
    if args.tm_mean_threshold is not None:
        agg_specs.append(("tm_mean_diff", False, f"<= {args.tm_mean_threshold}"))
    for metric, lower_better, threshold_text in agg_specs:
        agg = aggregate(per_frame, metric, lower_better)
        drift = agg["last"] - agg["first"]
        print(f"  {metric:15s}: mean {agg['mean']:9.5f}   worst {agg['worst']:9.5f} @ "
              f"frame {agg['worst_frame']:04d}  (threshold {threshold_text}); "
              f"first->last {drift:+.5f}")

    if failures:
        print()
        print(f"  failed checks ({len(failures)}):")
        for label in failures[:20]:
            print(f"    {label}")
        if len(failures) > 20:
            print(f"    ... and {len(failures) - 20} more")

    print()
    print(f"  RESULT       : {'PASS' if passed else 'FAIL'}")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
