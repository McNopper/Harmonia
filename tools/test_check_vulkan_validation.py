#!/usr/bin/env python3
"""
test_check_vulkan_validation.py - CPU unit receipts for the scene set of the glare
runs of check_vulkan_validation.py (no GPU, no renderer, no render).

The glare runs used to rest on one hard-coded scene (GLARE_SCENES = cornell_classic).
They now default to the scenes of the validation manifest, or to those named with
--glare-scenes: covered here are the default, the argument, the order and the
de-duplication, --glare 0, and that at least two manifest scenes exercise the glare.

Usage:  python tools/test_check_vulkan_validation.py
Exit:   0 = all checks pass, 1 = any check fails.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_vulkan_validation as cv
from validate_renders import DEFAULT_MANIFEST, load_manifest

_FAILURES: list[str] = []


def check(name: str, ok: bool, detail: str = "") -> None:
    status = "ok  " if ok else "FAIL"
    print(f"  [{status}] {name}" + (f"  ({detail})" if detail else ""))
    if not ok:
        _FAILURES.append(name)


def main() -> int:
    _FAILURES.clear()
    _, manifest = load_manifest(DEFAULT_MANIFEST)

    # ── the default glare set is the manifest ──────────────────────────────────
    default = cv.resolve_glare_scenes(None, manifest)
    check(
        "default glare scenes are the manifest scenes, in its order",
        default == tuple(manifest),
        f"{len(default)} scenes",
    )
    check(
        "at least two manifest scenes exercise the glare",
        len(cv.glare_runs(manifest, default, cv.DEFAULT_GLARE)) >= 2,
        f"{len(cv.glare_runs(manifest, default, cv.DEFAULT_GLARE))} scenes",
    )
    check(
        "every default glare scene is a manifest scene", set(default) <= set(manifest)
    )
    check("the hard-coded single-scene set is gone", not hasattr(cv, "GLARE_SCENES"))

    # ── --glare-scenes ─────────────────────────────────────────────────────────
    check(
        "requested scenes replace the default",
        cv.resolve_glare_scenes(["a", "b"], manifest) == ("a", "b"),
    )
    check(
        "a scene named twice runs once, in the order named",
        cv.resolve_glare_scenes(["b", "a", "b"], manifest) == ("b", "a"),
    )
    check(
        "an empty request is the default",
        cv.resolve_glare_scenes([], manifest) == tuple(manifest),
    )
    check(
        "a requested scene outside the manifest is kept",
        cv.resolve_glare_scenes(["adhoc"], manifest) == ("adhoc",),
    )

    # ── which of the rendered scenes get the glare capture ────────────────────
    check(
        "--glare 0 switches the glare runs off",
        cv.glare_runs(manifest, default, 0.0) == (),
    )
    check(
        "the glare runs follow the rendered scenes: an ad-hoc scene outside the set gets none",
        cv.glare_runs(["adhoc"], default, cv.DEFAULT_GLARE) == (),
    )
    check(
        "the rendered order is kept",
        cv.glare_runs(list(reversed(manifest)), default, cv.DEFAULT_GLARE)
        == tuple(reversed(manifest)),
    )
    adhoc = cv.resolve_glare_scenes(["adhoc"], manifest)
    check(
        "an ad-hoc scene named with --glare-scenes gets its glare run",
        cv.glare_runs(["adhoc", "other"], adhoc, cv.DEFAULT_GLARE) == ("adhoc",),
    )

    # ── the argument ───────────────────────────────────────────────────────────
    parser = cv.build_parser(Path("theia.exe"), Path("hyperion.exe"))
    check(
        "--glare-scenes defaults to none (the manifest)",
        parser.parse_args([]).glare_scenes is None,
    )
    repeated = parser.parse_args(
        ["--glare-scenes", "cornell_classic", "--glare-scenes", "haze"]
    )
    check(
        "--glare-scenes is repeatable",
        repeated.glare_scenes == ["cornell_classic", "haze"],
    )
    check(
        "the gate derives its glare set from the parsed arguments",
        cv.resolve_glare_scenes(repeated.glare_scenes, manifest)
        == ("cornell_classic", "haze"),
    )

    print(
        f"\n{'ALL CHECKS PASSED' if not _FAILURES else f'{len(_FAILURES)} FAILED: ' + ', '.join(_FAILURES)}"
    )
    return 1 if _FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
