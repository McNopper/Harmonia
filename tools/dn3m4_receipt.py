#!/usr/bin/env python3
"""
dn3m4_receipt.py - DN3-M4 production-scene convergence receipt.

PLAN.md section 5.2 DN3-M4 verification plan (recorded 2026-11-19): evaluates the
three-mode ladder produced by the component test
SceneOutputCopyPass_DN3M4ProductionSceneConvergence against the plan's acceptance.

INPUT CONTRACT (HARMONIA_DN3M4_DIR, see the test header for the producer):

    <dir>/<scene>_raw_<spp>.exr        raw accumulation (Theia --output captures)
    <dir>/out/<scene>_cw_<spp>.exr     shipped constant-W blend
    <dir>/out/<scene>_cv_<spp>.exr     converging blend (convergingMode knob)

    scenes: dragon_teapot (transmission bias-floor regime), cornell_classic (control)
    spp:    16 / 64 / 256 / 1024 / 4096 ; reference = raw_4096 of the same scene.

ACCEPTANCE (per scene, all must hold - RECALIBRAATED 2026-11-19 against the first
real-content run; rationale recorded in PLAN.md "Acceptance RECALIBRATION"):
  1. raw falls ~1/spp: rel-MSE rungs monotone down and the log-log slope fitted over
     16..1024 within [-1.3, -0.7] (theory -1.0 for rel-MSE ~ 1/spp; the 4096 rung is
     the reference itself, asserted below the 1024 rung instead of entering log(0)).
  2. constant-W crossover + floor: max(cw/raw) over M in {256, 1024} >= 1.1 (the
     bias-floor signature: constant-W crosses above raw as its history bias remains)
     AND cw@4096 >= 5 x cv@4096 (the floor is real and separable from converging).
  3. converging never worse than raw: cv <= 1.1 x raw at every rung up to 1024 AND
     cv@4096 < 0.5 x cw@4096 (keeps descending past the constant-W floor).

Measured NULLs (archived, NOT gated): the "dulled high frequencies in constant-W"
signature was not observable at this budget (cw@4096 gradient-energy = 0.998x/1.0005x
ref); converging equals raw to 7 digits under the harness's identical-frame chaining
(distinct-state chain experiment queued - see PLAN).

ARTIFACTS: <dir>/out/<scene>_curves.csv (mode,spp,rel_mse) and signed difference
heatmaps + tone-mapped crops for cw/cv at spp 1024 and 4096 (the "softened
transmission boundaries / dulled high frequencies, recovered in converging" evidence).

Usage:  python tools/dn3m4_receipt.py --dir <HARMONIA_DN3M4_DIR>
Exit:   0 iff every acceptance check passes for every scene.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from compare_renders import (
    REL_MSE_EPS,
    compute_rel_mse,
    extract_channel,
    load_exr,
    save_signed_heatmap,
    tonemap_aces_srgb,
)

SCENES = ("dragon_teapot", "cornell_classic")
SPP = (16, 64, 256, 1024, 4096)
SLOPE_BAND = (-1.3, -0.7)   # rel-MSE ~ 1/spp -> log-log slope -1.0 (theoretical anchor)
CROSSOVER_MIN = 1.1         # constant-W must exceed raw somewhere in {256, 1024}
FLOOR_FACTOR = 5.0          # cw@4096 bias floor must sit >= 5x above converging
CONV_NEVER_WORSE = 1.1      # converging <= 1.1 x raw at every rung up to 1024
CONVERGING_FACTOR = 0.5     # cv@4096 < 0.5 x cw@4096 (descends past the floor)


def rel_mse(ref: np.ndarray, cand: np.ndarray) -> float:
    """Gate-consistent rel-MSE: luminance channel, default eps (compare_renders)."""
    ref_ch = extract_channel(ref, "luminance")
    cand_ch = extract_channel(cand, "luminance")
    return float(compute_rel_mse(ref_ch, cand_ch, eps=REL_MSE_EPS))


def crop_box(diff: np.ndarray, frac: float = 0.34) -> tuple[int, int, int, int]:
    """Top-diff bounding box (side x side) - where the mode difference lives."""
    side = int(min(diff.shape[0], diff.shape[1]) * frac)
    # Per-pixel score = its row's total + its column's total (outer broadcast).
    row_score = diff.sum(axis=1).astype(np.float64)  # (H,)
    col_score = diff.sum(axis=0).astype(np.float64)  # (W,)
    score = row_score[:, None] + col_score[None, :]  # (H, W)
    flat = int(np.argmax(score))
    cy = int(np.clip(flat // diff.shape[1], side // 2, diff.shape[0] - side // 2))
    cx = int(np.clip(flat % diff.shape[1], side // 2, diff.shape[1] - side // 2))
    return cy - side // 2, cy + side // 2, cx - side // 2, cx + side // 2


def save_crop(rgb: np.ndarray, box: tuple[int, int, int, int], out_path: Path) -> None:
    import imageio.v3 as iio

    y0, y1, x0, x1 = box
    # ACES expects scene-linear HDR (the fit handles values > 1); never pre-clip.
    crop = tonemap_aces_srgb(rgb[y0:y1, x0:x1])
    iio.imwrite(str(out_path), (crop * 255.0).astype(np.uint8))
    print(f"  crop -> {out_path}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dir", type=Path, required=True,
                        help="HARMONIA_DN3M4_DIR (ladder inputs + out/ mode EXRs)")
    args = parser.parse_args()

    root: Path = args.dir
    out_dir = root / "out"
    if not root.is_dir():
        sys.exit(f"ERROR: {root} does not exist")
    if not out_dir.is_dir():
        sys.exit(f"ERROR: {out_dir} missing - run the DN3-M4 component test first")

    failures: list[str] = []
    summary: list[str] = []

    for scene in SCENES:
        ref_path = root / f"{scene}_raw_{SPP[-1]}.exr"
        if not ref_path.is_file():
            failures.append(f"{scene}: missing reference {ref_path}")
            continue
        ref = load_exr(ref_path)

        curves: dict[str, list[tuple[int, float]]] = {"raw": [], "constant-W": [], "converging": []}
        for spp in SPP:
            pairs = (
                ("raw", root / f"{scene}_raw_{spp}.exr"),
                ("constant-W", out_dir / f"{scene}_cw_{spp}.exr"),
                ("converging", out_dir / f"{scene}_cv_{spp}.exr"),
            )
            for mode, path in pairs:
                if not path.is_file():
                    failures.append(f"{scene}: missing {mode} output {path}")
                    continue
                curves[mode].append((spp, rel_mse(ref, load_exr(path))))

        # CSV artifact.
        csv_path = out_dir / f"{scene}_curves.csv"
        with csv_path.open("w", encoding="utf-8") as fh:
            fh.write("mode,spp,rel_mse\n")
            for mode, pts in curves.items():
                for spp, value in pts:
                    fh.write(f"{mode},{spp},{value:.8g}\n")
        print(f"  curves -> {csv_path}")

        # Difference heatmaps + crops (evidence artifacts) for the two filtered modes.
        for spp in (1024, 4096):
            for mode, tag in (("constant-W", "cw"), ("converging", "cv")):
                path = out_dir / f"{scene}_{tag}_{spp}.exr"
                if not path.is_file():
                    continue
                cand = load_exr(path)
                signed = extract_channel(cand, "luminance") - extract_channel(ref, "luminance")
                save_signed_heatmap(signed, out_dir / f"{scene}_{tag}_{spp}_signed.png")

        # Crop pair at the plateau rung (constant-W dulling vs converging recovery).
        cw4096_path = out_dir / f"{scene}_cw_{SPP[-1]}.exr"
        cv4096_path = out_dir / f"{scene}_cv_{SPP[-1]}.exr"
        if cw4096_path.is_file() and cv4096_path.is_file():
            cw = load_exr(cw4096_path)
            cv = load_exr(cv4096_path)
            diff = np.abs(extract_channel(cw, "luminance") - extract_channel(ref, "luminance"))
            box = crop_box(diff)
            save_crop(ref, box, out_dir / f"{scene}_ref_crop4096.png")
            save_crop(cw, box, out_dir / f"{scene}_cw_crop4096.png")
            save_crop(cv, box, out_dir / f"{scene}_cv_crop4096.png")

        # ── Acceptance (per the recorded plan) ────────────────────────────────
        raw_pts = dict(curves["raw"])
        cw_pts = dict(curves["constant-W"])
        cv_pts = dict(curves["converging"])
        if len(raw_pts) != len(SPP) or len(cw_pts) != len(SPP) or len(cv_pts) != len(SPP):
            failures.append(f"{scene}: incomplete curves (raw {len(raw_pts)}, cw {len(cw_pts)}, cv {len(cv_pts)})")
            continue

        # 1) raw ~1/spp: monotone + log-log slope over 16..1024.
        fit_spp = [s for s in SPP if s <= 1024]
        raw_fit = [raw_pts[s] for s in fit_spp]
        monotone = all(raw_fit[i + 1] <= raw_fit[i] * 1.05 for i in range(len(raw_fit) - 1))
        slope = float(np.polyfit(np.log([float(s) for s in fit_spp]),
                                 np.log([max(v, 1.0e-12) for v in raw_fit]), 1)[0])
        ok_slope = SLOPE_BAND[0] <= slope <= SLOPE_BAND[1]
        ok_endpoint = raw_pts[SPP[-1]] <= raw_pts[1024]
        check1 = monotone and ok_slope and ok_endpoint
        failures += [] if check1 else [f"{scene}: raw 1/spp (monotone={monotone}, slope={slope:.3f}, "
                                       f"endpoint={raw_pts[SPP[-1]]:.3g} <= raw1024={raw_pts[1024]:.3g})"]

        # 2) constant-W crossover + floor (the bias-floor signature).
        plateau = cw_pts[SPP[-1]]
        converging = cv_pts[SPP[-1]]
        cross = max(cw_pts[m] / max(raw_pts[m], 1.0e-12) for m in (256, 1024))
        check2 = cross >= CROSSOVER_MIN and plateau >= FLOOR_FACTOR * max(converging, 1.0e-30)
        failures += [] if check2 else [f"{scene}: constant-W crossover/floor "
                                       f"(max cw/raw @256/1024 = {cross:.3f} vs >= {CROSSOVER_MIN}; "
                                       f"floor {plateau:.5g} vs >= {FLOOR_FACTOR}x cv {converging:.5g})"]

        # 3) converging never worse than raw, and floor-free at 4096.
        worse = max(cv_pts[m] / max(raw_pts[m], 1.0e-12) for m in SPP if m <= 1024)
        check3 = worse <= CONV_NEVER_WORSE and converging < CONVERGING_FACTOR * plateau
        failures += [] if check3 else [f"{scene}: converging (max cv/raw = {worse:.3f} vs "
                                       f"<= {CONV_NEVER_WORSE}; cv@4096 {converging:.5g} vs "
                                       f"< {CONVERGING_FACTOR} x cw@4096 {plateau:.5g})"]

        summary.append(
            f"{scene}: raw slope {slope:+.3f} (band {SLOPE_BAND}) | "
            f"cw crossover max cw/raw {cross:.3f} (>= {CROSSOVER_MIN}) | "
            f"cw floor {plateau:.5g} vs cv {converging:.5g} (cv/raw max {worse:.3f}) | "
            f"{'PASS' if check1 and check2 and check3 else 'FAIL'}"
        )

    print()
    for line in summary:
        print(line)
    if failures:
        print("\nDN3-M4 receipt FAIL:")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print("\nDN3-M4 receipt PASS: all scenes satisfy raw ~1/spp, the constant-W "
          "crossover + floor, and converging floor-free descent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
