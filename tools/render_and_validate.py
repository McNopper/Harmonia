#!/usr/bin/env python3
"""
render_and_validate.py — render Hyperion/Theia validation pairs and compare them.

Usage:
    python tools/render_and_validate.py <hyperion_exe> <theia_exe> <output_root> [options]

Renders each manifest scene into <output_root>/<scene>/hyperion.exr and
<output_root>/<scene>/theia.exr, then runs validate_renders.py over the batch.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

from validate_renders import DEFAULT_MANIFEST, load_manifest


def _run(cmd: list[str]) -> None:
    print(" ".join(cmd))
    subprocess.run(cmd, check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("hyperion_exe", type=Path, help="Path to hyperion.exe")
    parser.add_argument("theia_exe", type=Path, help="Path to theia.exe")
    parser.add_argument("output_root", type=Path, help="Directory to write per-scene render outputs")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST, help="Validation manifest TOML")
    parser.add_argument("--scene", action="append", dest="scenes", help="Scene name (repeatable)")
    parser.add_argument("--no-validate", action="store_true", help="Render only; skip comparison")
    args = parser.parse_args()

    defaults, manifest_scenes = load_manifest(args.manifest)
    scenes = args.scenes or manifest_scenes
    width = int(defaults.get("width", 320))
    height = int(defaults.get("height", 240))
    hyperion_spp = int(defaults.get("hyperion_spp", 256))
    theia_frames = int(defaults.get("theia_frames", 256))

    args.output_root.mkdir(parents=True, exist_ok=True)

    for scene in scenes:
        scene_dir = args.output_root / scene
        scene_dir.mkdir(parents=True, exist_ok=True)
        hyperion_out = scene_dir / "hyperion.exr"
        theia_out = scene_dir / "theia.exr"

        _run([
            str(args.hyperion_exe),
            "--scene",
            scene,
            "--spp",
            str(hyperion_spp),
            "--output",
            str(hyperion_out),
            "--width",
            str(width),
            "--height",
            str(height),
            "--no-validation",
        ])

        _run([
            str(args.theia_exe),
            "--scene",
            scene,
            "--no-postfx",
            "--no-camera-jitter",
            "--offscreen-frames",
            str(theia_frames),
            "--output",
            str(theia_out),
            "--width",
            str(width),
            "--height",
            str(height),
            "--no-validation",
        ])

    if args.no_validate:
        return 0

    validate_script = Path(__file__).with_name("validate_renders.py")
    validate_args = [sys.executable, str(validate_script), str(args.output_root), str(args.output_root)]
    for scene in scenes:
        validate_args.extend(["--scene", scene])
    validate_args.extend([
        "--reference-template",
        "{scene}\\hyperion.exr",
        "--candidate-template",
        "{scene}\\theia.exr",
    ])
    _run([
        *validate_args,
    ])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
