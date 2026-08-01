#!/usr/bin/env python3
"""
validate_renders.py — batch validation wrapper for Hyperion/Theia EXR pairs.

Usage:
    python tools/validate_renders.py <reference_dir> <candidate_dir> [options]

The harness compares a set of scene-named EXR pairs using
tools/compare_renders.py's metrics and gates, then prints a summary table and
exits non-zero if any scene fails.
"""

from __future__ import annotations

import argparse
import tomllib
import sys
from dataclasses import dataclass
from pathlib import Path

from compare_renders import (REL_MSE_EPS, compute_metrics, evaluate_gate, extract_channel,
                             load_exr, save_heatmap)


# Scenes that always use the scale-aware gate regardless of the manifest. Empty
# by design: the real scale-aware fixtures (shaderball_transmission,
# openpbr_dielectrics) are declared in validation_manifest.toml's
# defaults.scale_aware, which is where they belong. This previously hard-coded
# "furnace_coverage", a scene that exists nowhere in the tree — no asset, no EXR,
# no reference in any repo — so it was a phantom that could never match.
DEFAULT_SCALE_AWARE: set[str] = set()
DEFAULT_MANIFEST = Path(__file__).with_name("validation_manifest.toml")


@dataclass(frozen=True)
class SceneResult:
    scene: str
    reference: Path
    candidate: Path
    passed: bool
    gate: str
    metrics: dict
    labels: list[str]


def _resolve(template: str, scene: str, root: Path) -> Path:
    return root / template.format(scene=scene)


def load_manifest(path: Path) -> tuple[dict, list[str]]:
    if not path.exists():
        raise FileNotFoundError(f"manifest file not found: {path}")

    data = tomllib.loads(path.read_text(encoding="utf-8"))
    defaults = dict(data.get("defaults", {}))
    scenes = data.get("scene", [])
    if isinstance(scenes, dict):
        scenes = [scenes]
    names = [entry["name"] for entry in scenes]
    if not names:
        raise ValueError(f"manifest contains no scenes: {path}")
    return defaults, names


def _compare_scene(
    scene: str,
    reference: Path,
    candidate: Path,
    channel: str,
    gate: str,
    threshold: float,
    rel_mse_threshold: float,
    ssim_threshold: float,
    lum_hist_threshold: float,
    psnr_threshold: float | None,
    relative_threshold: float | None,
    rel_mse_eps: float,
    hist_bins: int,
    heatmap_dir: Path | None,
    emit_heatmap: bool,
) -> SceneResult:
    if not reference.exists():
        raise FileNotFoundError(f"reference file not found: {reference}")
    if not candidate.exists():
        raise FileNotFoundError(f"candidate file not found: {candidate}")

    ref = load_exr(reference)
    cand = load_exr(candidate)
    if ref.shape != cand.shape:
        raise ValueError(f"shape mismatch for '{scene}' — reference {ref.shape} vs candidate {cand.shape}")

    metrics = compute_metrics(ref, cand, channel=channel,
                              rel_mse_eps=rel_mse_eps, hist_bins=hist_bins)

    args = argparse.Namespace(
        threshold=threshold,
        rel_mse_threshold=rel_mse_threshold,
        ssim_threshold=ssim_threshold,
        lum_hist_threshold=lum_hist_threshold,
        psnr_threshold=psnr_threshold,
        relative_threshold=relative_threshold,
        gate=gate,
    )
    passed, labels = evaluate_gate(metrics, args)

    if emit_heatmap:
        if heatmap_dir is None:
            heatmap_path = candidate.with_name(candidate.stem + "_diff.png")
        else:
            heatmap_dir.mkdir(parents=True, exist_ok=True)
            heatmap_path = heatmap_dir / f"{scene}_diff.png"
        ref_ch = extract_channel(ref, channel)
        cand_ch = extract_channel(cand, channel)
        save_heatmap(abs(ref_ch - cand_ch), heatmap_path)

    return SceneResult(
        scene=scene,
        reference=reference,
        candidate=candidate,
        passed=passed,
        gate=gate,
        metrics=metrics,
        labels=labels,
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("reference_dir", type=Path, help="Directory containing Hyperion reference EXRs")
    parser.add_argument("candidate_dir", type=Path, help="Directory containing Theia candidate EXRs")
    parser.add_argument(
        "--manifest",
        type=Path,
        default=DEFAULT_MANIFEST,
        help="Validation manifest TOML (default: tools/validation_manifest.toml)",
    )
    parser.add_argument("--scene", action="append", dest="scenes", help="Scene name (repeatable)")
    parser.add_argument(
        "--scale-aware",
        action="append",
        dest="scale_aware",
        default=[],
        help="Scene name that should use the scale-aware gate (repeatable)",
    )
    parser.add_argument("--reference-template", default=None, help="Reference path template")
    parser.add_argument("--candidate-template", default=None, help="Candidate path template")
    parser.add_argument("--threshold", type=float, default=None, help="Mean-diff threshold (1/255 units)")
    parser.add_argument("--rel-mse-threshold", type=float, default=None,
                        help="Rel-MSE threshold (default: 0.01, scene-referred)")
    parser.add_argument("--ssim-threshold", type=float, default=None,
                        help="SSIM threshold (default: 0.98, tone-mapped sRGB)")
    parser.add_argument("--lum-hist-threshold", type=float, default=None,
                        help="Luminance-histogram Pearson correlation (default: 0.999)")
    parser.add_argument("--psnr-threshold", type=float, default=None, help="PSNR threshold for scale-aware gate")
    parser.add_argument("--relative-threshold", type=float, default=None, help="Relative mean-error threshold (percent)")
    parser.add_argument("--rel-mse-eps", type=float, default=None,
                        help=f"Rel-MSE denominator floor ref^2+eps (default: {REL_MSE_EPS:g})")
    parser.add_argument("--hist-bins", type=int, default=None,
                        help="Luminance-histogram log-spaced bin count for the positive range (default: 256)")
    parser.add_argument(
        "--channel",
        default=None,
        help="Channel to compare (r, g, b, luminance)",
    )
    parser.add_argument(
        "--heatmap-dir",
        type=Path,
        default=None,
        help="Write heatmaps to this directory (default: alongside candidate EXRs)",
    )
    parser.add_argument("--no-heatmap", action="store_true", help="Skip heatmap output")
    parser.add_argument("--fail-fast", action="store_true", help="Stop after the first failed scene")
    args = parser.parse_args()

    manifest_defaults, manifest_scenes = load_manifest(args.manifest)
    scenes = args.scenes or manifest_scenes
    scale_aware = set(args.scale_aware) | set(manifest_defaults.get("scale_aware", [])) | DEFAULT_SCALE_AWARE
    reference_template = args.reference_template or manifest_defaults.get("reference_template", "{scene}.exr")
    candidate_template = args.candidate_template or manifest_defaults.get("candidate_template", "{scene}.exr")
    threshold = args.threshold if args.threshold is not None else float(manifest_defaults.get("threshold", 4.0))
    channel = args.channel or str(manifest_defaults.get("channel", "luminance"))
    rel_mse_threshold = args.rel_mse_threshold if args.rel_mse_threshold is not None else float(manifest_defaults.get("rel_mse_threshold", 0.01))
    ssim_threshold = args.ssim_threshold if args.ssim_threshold is not None else float(manifest_defaults.get("ssim_threshold", 0.98))
    lum_hist_threshold = args.lum_hist_threshold if args.lum_hist_threshold is not None else float(manifest_defaults.get("lum_hist_threshold", 0.999))
    rel_mse_eps = args.rel_mse_eps if args.rel_mse_eps is not None else float(manifest_defaults.get("rel_mse_eps", REL_MSE_EPS))
    hist_bins = args.hist_bins if args.hist_bins is not None else int(manifest_defaults.get("hist_bins", 256))
    psnr_threshold = args.psnr_threshold if args.psnr_threshold is not None else manifest_defaults.get("psnr_threshold")
    if psnr_threshold is not None:
        psnr_threshold = float(psnr_threshold)
    relative_threshold = args.relative_threshold if args.relative_threshold is not None else manifest_defaults.get("relative_threshold")
    if relative_threshold is not None:
        relative_threshold = float(relative_threshold)

    results: list[SceneResult] = []
    for scene in scenes:
        gate = "scale-aware" if scene in scale_aware else "all"
        ref = _resolve(reference_template, scene, args.reference_dir)
        cand = _resolve(candidate_template, scene, args.candidate_dir)
        try:
            result = _compare_scene(
                scene=scene,
                reference=ref,
                candidate=cand,
                channel=channel,
                gate=gate,
                threshold=threshold,
                rel_mse_threshold=rel_mse_threshold,
                ssim_threshold=ssim_threshold,
                lum_hist_threshold=lum_hist_threshold,
                psnr_threshold=psnr_threshold,
                relative_threshold=relative_threshold,
                rel_mse_eps=rel_mse_eps,
                hist_bins=hist_bins,
                heatmap_dir=args.heatmap_dir,
                emit_heatmap=not args.no_heatmap,
            )
        except Exception as exc:  # noqa: BLE001 - surface the failure, don't hide it
            print(f"{scene:24} FAIL  {exc}")
            if args.fail_fast:
                return 1
            results.append(
                SceneResult(
                    scene=scene,
                    reference=ref,
                    candidate=cand,
                    passed=False,
                    gate=gate,
                    metrics={"mean_diff": float("inf"), "psnr": float("-inf"), "rel_mean_pct": float("inf"),
                             "rel_mse": float("inf"), "ssim": float("-inf"), "lum_hist_corr": float("-inf"),
                             "black_frac_ref": float("nan"), "black_frac_cand": float("nan")},
                    labels=[str(exc)],
                )
            )
            continue

        results.append(result)
        print(
            f"{scene:24} {'PASS' if result.passed else 'FAIL'}  "
            f"mean={result.metrics['mean_diff']:.3f}  "
            f"relmse={result.metrics['rel_mse']:.4f}  "
            f"ssim={result.metrics['ssim']:.4f}  "
            f"hist={result.metrics['lum_hist_corr']:.4f}  "
            f"blackd={(result.metrics['black_frac_cand'] - result.metrics['black_frac_ref']) * 100:+.2f}pp  "
            f"gate={result.gate}"
        )

    passed = sum(1 for result in results if result.passed)
    failed = len(results) - passed
    print()
    print(f"summary: {passed} passed / {failed} failed")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
