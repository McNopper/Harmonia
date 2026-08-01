#!/usr/bin/env python3
"""
test_denoiser_contract.py — DEN2 regression gate for the two-tier denoiser output contract.

The sprint plan (§5.2 / §12) holds the parity invariant via a contract, not via the filter:
the A-SVGF stage is an interactive presentation filter only and is forced off for offscreen
capture (`--output`) in BOTH renderers (`App.cpp`). A capture must therefore be the raw
scene-referred estimator result regardless of any `--denoiser-*` flags or `--no-postfx`.

This script asserts that contract is bit-identical: for each renderer it renders the same
scene twice — once plain, once with the denoiser explicitly requested — and requires the two
EXRs to be pixel-identical. For Theia it also checks `--no-postfx` against the plain capture.
A drift here means a post stage has silently escaped onto the capture path.

Usage:
    python tools/test_denoiser_contract.py <hyperion_exe> <theia_exe> <output_dir>
        --scene cornell_classic
        [--width 320] [--height 240]
        [--hyperion-spp 8] [--theia-frames 8]

Exit code: 0 if every capture pair is pixel-identical, 1 otherwise.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from compare_renders import load_exr  # type: ignore  # noqa: E402


def _run(cmd: list[str]) -> None:
    print("  >", " ".join(cmd))
    subprocess.run(cmd, check=True)


def _identical(a: Path, b: Path) -> bool:
    """Pixel-identical (true bit-identical image, robust to EXR header timestamps)."""
    import numpy as np
    img_a = load_exr(a)
    img_b = load_exr(b)
    if img_a.shape != img_b.shape:
        return False
    return bool(np.array_equal(img_a, img_b))


def check_pair(label: str, a: Path, b: Path) -> bool:
    ok = a.exists() and b.exists() and _identical(a, b)
    verdict = "IDENTICAL" if ok else "DIFFERS"
    print(f"  [{verdict}] {label}")
    return ok


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("hyperion_exe", type=Path)
    p.add_argument("theia_exe", type=Path)
    p.add_argument("output_dir", type=Path)
    p.add_argument("--scene", default="cornell_classic")
    p.add_argument("--width", type=int, default=320)
    p.add_argument("--height", type=int, default=240)
    p.add_argument("--hyperion-spp", type=int, default=8,
                   help="Low spp is fine — this tests determinism, not convergence (default 8)")
    p.add_argument("--theia-frames", type=int, default=8,
                   help="Low frame count is fine — tests determinism, not convergence (default 8)")
    p.add_argument("--skip-renderer", choices=["hyperion", "theia"], action="append", default=[],
                   help="Skip a renderer (repeatable)")
    args = p.parse_args()

    args.output_dir.mkdir(parents=True, exist_ok=True)
    w, h = str(args.width), str(args.height)
    denoiser_force = ["--denoiser-strength", "0.9",
                      "--denoiser-iterations", "4",
                      "--denoiser-history"]
    results: list[bool] = []

    # ── Hyperion: plain capture vs denoiser-forced-on capture ────────────────
    if "hyperion" not in args.skip_renderer:
        print("\n=== Hyperion ===")
        base = args.output_dir / "hy_base.exr"
        forced = args.output_dir / "hy_forced.exr"
        _run([str(args.hyperion_exe), "--scene", args.scene,
              "--spp", str(args.hyperion_spp),
              "--width", w, "--height", h,
              "--output", str(base), "--no-validation"])
        _run([str(args.hyperion_exe), "--scene", args.scene,
              "--spp", str(args.hyperion_spp),
              *denoiser_force,
              "--width", w, "--height", h,
              "--output", str(forced), "--no-validation"])
        results.append(check_pair("Hyperion plain == denoiser-forced", base, forced))

    # ── Theia: plain vs denoiser-forced vs --no-postfx ───────────────────────
    if "theia" not in args.skip_renderer:
        print("\n=== Theia ===")
        base = args.output_dir / "th_base.exr"
        forced = args.output_dir / "th_forced.exr"
        postfx = args.output_dir / "th_nopostfx.exr"
        common = ["--scene", args.scene,
                  "--offscreen-frames", str(args.theia_frames),
                  "--no-camera-jitter",
                  "--width", w, "--height", h, "--no-validation"]
        _run([str(args.theia_exe), *common, "--output", str(base)])
        _run([str(args.theia_exe), *common, *denoiser_force, "--output", str(forced)])
        _run([str(args.theia_exe), *common, "--no-postfx", "--output", str(postfx)])
        results.append(check_pair("Theia plain == denoiser-forced", base, forced))
        results.append(check_pair("Theia plain == --no-postfx", base, postfx))

    print("\n=== SUMMARY ===")
    print(f"  {'PASS' if all(results) and results else 'FAIL'} — "
          f"{sum(results)}/{len(results)} capture pairs pixel-identical")
    if not results:
        print("  (no checks ran — did you skip both renderers?)")
        return 1
    return 0 if all(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
