#!/usr/bin/env python3
"""
check_consistency.py — Theia convergence-to-Hyperion consistency gate.

Renders Theia at N, 2N, 4N, ... frames (denoiser/TAA bypassed via --no-postfx
--no-camera-jitter), then verifies two invariants required by the sprint plan's
GI2 gating correctness check:

  1. CONSISTENCY (variance ⊘ 1/N):
        mean_diff(Theia@N, Theia@4N) ≈ ½ · mean_diff(Theia@N/4, Theia@N)
     Equivalent statement: Var(image_N) ∝ 1/N, so the L1 distance between two
     independent accumulation runs halves each time the frame count quadruples.

  2. BIAS FLOOR (consistency of *estimator*, not noise):
        mean_diff(Theia@largest_N, Hyperion@large_spp) ≤ gate
     Whatever residual remains at convergence is bias, not noise — and bias must
     be root-caused, never tuned away.

The script never compares two noisy runs to declare "convergence"; it tests
whether the noise floor actually shrinks at the theoretical 1/√N rate.

Usage:
    python tools/check_consistency.py <theia_exe> <hyperion_exe> <output_dir>
        --scene cornell_classic
        [--frame-scales 64 256 1024]      # default: 64, 256, 1024 (N, 4N, 16N)
        [--hyperion-spp 1024]
        [--bias-threshold 4.0]
        [--width 320] [--height 240]

Exit code: 0 if BOTH invariants hold, 1 otherwise.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

# Reuse the EXR loader + metric core from compare_renders.py.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from compare_renders import load_exr, compute_metrics  # type: ignore  # noqa: E402


def render_theia(exe: Path, scene: str, frames: int, width: int, height: int,
                 out: Path, extra_args: list[str] | None = None) -> None:
    """Headless accumulation render with denoiser/TAA bypassed."""
    cmd = [
        str(exe),
        "--scene", scene,
        "--no-postfx",
        "--no-camera-jitter",
        "--offscreen-frames", str(frames),
        "--width", str(width),
        "--height", str(height),
        "--output", str(out),
        "--no-validation",
    ]
    if extra_args:
        cmd.extend(extra_args)
    print("  >", " ".join(cmd))
    subprocess.run(cmd, check=True)


def render_hyperion(exe: Path, scene: str, spp: int, width: int, height: int,
                    out: Path) -> None:
    cmd = [
        str(exe),
        "--scene", scene,
        "--spp", str(spp),
        "--width", str(width),
        "--height", str(height),
        "--output", str(out),
        "--no-validation",
    ]
    print("  >", " ".join(cmd))
    subprocess.run(cmd, check=True)


def l1_mean_diff_255(a, b) -> float:
    """Mean |a-b| per luminance pixel, scaled to 1/255 units (matches compare_renders)."""
    m = compute_metrics(a, b)
    return m["mean_diff"]


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("theia_exe", type=Path)
    p.add_argument("hyperion_exe", type=Path)
    p.add_argument("output_dir", type=Path)
    p.add_argument("--scene", required=True)
    p.add_argument("--frame-scales", type=int, nargs="+", default=[64, 256, 1024],
                   help="Theia frame counts (default: 64 256 1024 — N, 4N, 16N)")
    p.add_argument("--hyperion-spp", type=int, default=1024,
                   help="Hyperion spp for the bias-floor reference (default 1024)")
    p.add_argument("--bias-threshold", type=float, default=4.0,
                   help="mean_diff gate for Theia@maxN vs Hyperion (default 4.0)")
    p.add_argument("--width", type=int, default=320)
    p.add_argument("--height", type=int, default=240)
    p.add_argument("--extra-theia-args", nargs=argparse.REMAINDER, default=[],
                   help="Extra args passed to Theia (e.g. --no-restir-pt). Must follow '--'.")
    p.add_argument("--skip-hyperion", action="store_true",
                   help="Skip Hyperion render + bias-floor check (consistency-only)")
    p.add_argument("--reuse", action="store_true",
                   help="Reuse existing EXRs in output_dir instead of re-rendering")
    args = p.parse_args()

    args.output_dir.mkdir(parents=True, exist_ok=True)

    scales = sorted(args.frame_scales)
    if len(scales) < 2:
        sys.exit("ERROR: --frame-scales needs at least 2 values to test convergence rate.")

    # ── Step 1: render Theia at each scale (no denoiser, no TAA) ──────────────
    theia_exrs: list[Path] = []
    for n in scales:
        out = args.output_dir / f"theia_{n}f.exr"
        theia_exrs.append(out)
        if args.reuse and out.exists():
            print(f"  reuse {out}")
            continue
        print(f"\n=== Theia {n} frames ===")
        render_theia(args.theia_exe, args.scene, n, args.width, args.height, out,
                     extra_args=args.extra_theia_args)

    # ── Step 2: consistency test — pairwise L1 between consecutive scales ─────
    print("\n=== Consistency (pairwise Theia-vs-Theia mean_diff, 1/255) ===")
    pairwise: list[tuple[int, int, float]] = []
    for i in range(len(scales) - 1):
        n_a, n_b = scales[i], scales[i + 1]
        img_a = load_exr(theia_exrs[i])
        img_b = load_exr(theia_exrs[i + 1])
        d = l1_mean_diff_255(img_a, img_b)
        pairwise.append((n_a, n_b, d))
        print(f"  Theia@{n_a:>5} vs Theia@{n_b:>5}: mean_diff = {d:7.3f}")

    # If we have ≥3 scales, test the 1/√N rate: each 4× frames should halve the
    # L1 distance between consecutive accumulations.
    rate_ok = True
    if len(scales) >= 3:
        print("\n=== Variance rate (each 4x frames should ~halve consecutive-run diff) ===")
        for i in range(len(pairwise) - 1):
            n_a, _, d_a = pairwise[i]
            _, n_b, d_b = pairwise[i + 1]
            ratio = d_b / d_a if d_a > 1e-6 else float("nan")
            # The theoretical rate is 1/√N for L2/MSE; for L1 it's the same 1/√N.
            # So 4× frames → ½ the diff. Allow [0.30, 0.85] as a generous pass band:
            # the lower bound rejects sub-1/√N improvement (correlated samples,
            # i.e. bias masquerading as variance); the upper rejects ~no improvement.
            ok = 0.30 <= ratio <= 0.85
            rate_ok = rate_ok and ok
            verdict = "OK" if ok else "OUT OF BAND"
            print(f"  {n_a:>5}->{n_b:>5}: ratio {ratio:.3f}  (expect ~0.50)  [{verdict}]")

    # ── Step 3: bias floor vs Hyperion ────────────────────────────────────────
    bias_ok = True
    bias_diff = float("nan")
    if not args.skip_hyperion:
        ref_path = args.output_dir / f"hyperion_{args.hyperion_spp}spp.exr"
        if args.reuse and ref_path.exists():
            print(f"  reuse {ref_path}")
        else:
            print(f"\n=== Hyperion {args.hyperion_spp} spp reference ===")
            render_hyperion(args.hyperion_exe, args.scene, args.hyperion_spp,
                            args.width, args.height, ref_path)
        ref = load_exr(ref_path)
        cand = load_exr(theia_exrs[-1])  # largest-N run
        bias_diff = l1_mean_diff_255(cand, ref)
        bias_ok = bias_diff <= args.bias_threshold
        print(f"\n=== Bias floor (Theia@{scales[-1]} vs Hyperion@{args.hyperion_spp}spp) ===")
        print(f"  mean_diff = {bias_diff:7.3f}   gate: <= {args.bias_threshold}  "
              f"[{'PASS' if bias_ok else 'FAIL'}]")
        print("  Note: residual at convergence is BIAS, not noise — root-cause, don't tune.")

    # ── Verdict ───────────────────────────────────────────────────────────────
    print("\n=== SUMMARY ===")
    print(f"  Variance rate O(1/N) : {'PASS' if rate_ok else 'FAIL'}")
    if not args.skip_hyperion:
        print(f"  Bias floor          : {'PASS' if bias_ok else 'FAIL'}  "
              f"(mean_diff {bias_diff:.3f} vs gate {args.bias_threshold})")
    return 0 if (rate_ok and bias_ok) else 1


if __name__ == "__main__":
    raise SystemExit(main())
