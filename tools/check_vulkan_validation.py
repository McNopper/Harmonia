#!/usr/bin/env python3
"""
check_vulkan_validation.py — Vulkan validation / zero-warning gate.

Runs each renderer across a scene set with Vulkan validation layers ON (the
opposite of render_and_validate.py, which passes --no-validation for speed) and
FAILS on any validation message or app-level WARN/ERROR line.

This is the runtime half of the "zero warnings / errors" sprint bar; the build
half is enforced by /WX -Werror in Platform.cmake. A change is not done until
this script is green alongside `ctest`.

Usage:
    python tools/check_vulkan_validation.py [options]

Defaults resolve the renderer exes relative to this file
(<root>/Theia/build, <root>/Hyperion/build); override with --theia/--hyperion.
The scene set defaults to tools/validation_manifest.toml (single source of
truth shared with the parity sweep); override ad-hoc with repeated --scene.

Examples:
    # canonical gate (manifest scenes, offscreen, tiny sampling)
    python tools/check_vulkan_validation.py

    # stress a specific path (transparency/dielectrics/subsurface/...)
    python tools/check_vulkan_validation.py --scene shaderball_transmission \
        --scene openpbr_dielectrics --scene shaderball_subsurface

    # also exercise the interactive window path (swapchain/present/TAA/resize)
    python tools/check_vulkan_validation.py --window-seconds 6
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

from validate_renders import DEFAULT_MANIFEST, load_manifest

# Detection: a line is a failure if it matches ANY of these.
#   VUID-        canonical Khronos validation error id (e.g. "VUID-vkCmdDraw-...")
#   UNASSIGNED   unassigned validation messages
#   Vulkan warning|error   the DebugUtils callback format
#                        ("Vulkan warning validation: ...", "Vulkan error performance: ...")
#   ][WARN|ERROR]         app-level Logger level ([THEIA][WARN], [HYPERION][ERROR])
# INFO lines never match.
FAILURE_RE = re.compile(
    r"VUID-|UNASSIGNED|Vulkan\s+(?:warning|error)|\]\[(?:WARN|ERROR)\]",
    re.IGNORECASE,
)

# Terse scene-set each renderer runs. Validation surfaces (init, barriers,
# descriptor pools, layout transitions, queue ownership) are independent of
# sample count, so tiny sampling keeps the gate fast enough to run often.
DEFAULT_FRAMES = 4
OFFSCREEN_TIMEOUT_S = 120


@dataclass
class RunResult:
    renderer: str
    scene: str
    mode: str  # "offscreen" | "window"
    passed: bool
    hits: list[str] = field(default_factory=list)
    elapsed: float = 0.0
    note: str = ""


def _exe_defaults() -> tuple[Path, Path]:
    root = Path(__file__).resolve().parents[2]
    return root / "Theia" / "build" / "theia.exe", root / "Hyperion" / "build" / "hyperion.exe"


def _base_args(exe: Path, scene: str, width: int, height: int) -> list[str]:
    return [str(exe), "--scene", scene, "--width", str(width), "--height", str(height), "--validation"]


def _offscreen_args(exe: Path, renderer: str, scene: str, width: int, height: int, frames: int) -> list[str]:
    args = _base_args(exe, scene, width, height)
    out = Path(sys.argv[0]).resolve().parent / "_vk_logs" / f"{renderer}_{scene}_offscreen.exr"
    out.parent.mkdir(parents=True, exist_ok=True)
    if renderer == "theia":
        args += ["--offscreen-frames", str(frames)]
    else:  # hyperion
        args += ["--spp", str(frames)]
    args += ["--output", str(out)]
    return args


def _run_offscreen(label: str, cmd: list[str]) -> RunResult:
    t0 = time.perf_counter()
    try:
        proc = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=OFFSCREEN_TIMEOUT_S,
        )
    except subprocess.TimeoutExpired as e:
        return RunResult(*label, passed=False, note=f"TIMEOUT after {OFFSCREEN_TIMEOUT_S}s")
    elapsed = time.perf_counter() - t0
    combined = (proc.stdout or "") + (proc.stderr or "")
    hits = [ln for ln in combined.splitlines() if FAILURE_RE.search(ln)]
    return RunResult(*label, passed=(not hits and proc.returncode == 0), hits=hits, elapsed=elapsed,
                     note="" if proc.returncode == 0 else f"exit={proc.returncode}")


def _run_window(label: str, cmd: list[str], seconds: int) -> RunResult:
    cmd = cmd + []  # window mode = same args minus --output (added by caller)
    t0 = time.perf_counter()
    try:
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    except FileNotFoundError as e:
        return RunResult(*label, passed=False, note=f"exe not found: {e}")
    time.sleep(seconds)
    hits: list[str] = []
    note = ""
    if proc.poll() is None:
        proc.terminate()
        try:
            out, _ = proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            out, _ = proc.communicate()
        note = "killed"
    else:
        out, _ = proc.communicate()
        note = f"exited rc={proc.returncode}"
    elapsed = time.perf_counter() - t0
    if out:
        hits = [ln for ln in out.splitlines() if FAILURE_RE.search(ln)]
    return RunResult(*label, passed=(not hits), hits=hits, elapsed=elapsed, note=note)


def _print_table(results: list[RunResult]) -> None:
    print(f"\n{'renderer':<10} {'scene':<28} {'mode':<9} {'result':<6} {'hits':<4} {'time':>6}  note")
    print("-" * 84)
    for r in results:
        status = "PASS" if r.passed else "FAIL"
        print(f"{r.renderer:<10} {r.scene:<28} {r.mode:<9} {status:<6} {len(r.hits):<4} {r.elapsed:6.1f}s  {r.note}")
    fails = [r for r in results if not r.passed]
    if fails:
        print("\n=== failures (first hit per run) ===")
        for r in fails:
            first = r.hits[0].strip()[:160] if r.hits else "(no matching line)"
            print(f"[{r.renderer}/{r.scene}/{r.mode}] {first}")


def main() -> int:
    default_theia, default_hyperion = _exe_defaults()
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--theia", type=Path, default=default_theia, help=f"Theia exe (default: {default_theia})")
    p.add_argument("--hyperion", type=Path, default=default_hyperion, help=f"Hyperion exe (default: {default_hyperion})")
    p.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST, help="Scene manifest TOML")
    p.add_argument("--scene", action="append", dest="scenes", help="Scene name (repeatable; overrides manifest)")
    p.add_argument("--width", type=int, help="Render width (default: manifest)")
    p.add_argument("--height", type=int, help="Render height (default: manifest)")
    p.add_argument("--frames", type=int, default=DEFAULT_FRAMES, help=f"offscreen-frames / spp (default: {DEFAULT_FRAMES})")
    p.add_argument("--window-seconds", type=int, default=0, help="Also run a timed window capture (0=offscreen only)")
    p.add_argument("--renderers", default="theia,hyperion", help="Comma list: theia,hyperion (default: both)")
    args = p.parse_args()

    defaults, manifest_scenes = load_manifest(args.manifest)
    scenes = args.scenes or manifest_scenes
    width = args.width or int(defaults.get("width", 320))
    height = args.height or int(defaults.get("height", 240))
    active = [r.strip() for r in args.renderers.split(",") if r.strip()]

    renderers: dict[str, Path] = {}
    if "theia" in active:
        renderers["theia"] = args.theia
    if "hyperion" in active:
        renderers["hyperion"] = args.hyperion

    for name, exe in renderers.items():
        if not exe.exists():
            print(f"error: {name} exe not found: {exe}", file=sys.stderr)
            return 2

    print(f"Vulkan validation gate: {len(scenes)} scene(s) x {list(renderers)} "
          f"@ {width}x{height}, frames/spp={args.frames}, window={'off' if not args.window_seconds else str(args.window_seconds)+'s'}")

    results: list[RunResult] = []
    for scene in scenes:
        for name, exe in renderers.items():
            results.append(_run_offscreen((name, scene, "offscreen"),
                                          _offscreen_args(exe, name, scene, width, height, args.frames)))
            if args.window_seconds:
                # window: same base args (validation on), no --output
                results.append(_run_window((name, scene, "window"),
                                           _base_args(exe, scene, width, height), args.window_seconds))

    _print_table(results)
    n_fail = sum(1 for r in results if not r.passed)
    print(f"\n{len(results) - n_fail}/{len(results)} runs clean. ", end="")
    if n_fail:
        print(f"{n_fail} FAILED with validation/warning hits.")
        return 1
    print("Zero Vulkan validation warnings/errors.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
