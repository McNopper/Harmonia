#!/usr/bin/env python3
"""frames_to_video.py - convert a numbered image sequence into a playable video.

Input: a directory of stills written by `--sequence-frames` captures
(`<stem>_0000.png`, `<stem>_0001.png`, ...) or any similarly named frames.
Frames are ordered NUMERICALLY by the trailing `_ <index>` (zero-padding
independent - lexicographic order would break past frame 9999), hard-linked
into a temporary `frame_%06d.png` sequence beside the output (same volume, no
data copy) and encoded through ffmpeg's image2 demuxer - exact ordering by
construction.

Frame rate pairs with the capture: `--fps N` (default 30) for tick-grid
captures, or `--interval-ms X` for time-based captures (`--capture-every-ms
X`), where fps = 1000 / X - so the playback duration equals the captured
time range (10 frames every 100 ms -> a 1.0 s video at 10 fps).

Usage:
    python tools/frames_to_video.py <input_dir> <output.mp4>
        [--fps N | --interval-ms X] [--pattern "*.png"] [--crf N]

Requires ffmpeg on PATH (ffprobe is used for the printed summary when
present). Output is H.264 / yuv420p (universally playable; odd dimensions
are padded to even).
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

_FRAME_INDEX = re.compile(r"_(\d+)\.[A-Za-z0-9]+$")


def discover_frames(input_dir: Path, pattern: str) -> list[Path]:
    """Frames ordered by their trailing _<n> index (numeric, not lexical)."""
    frames = [p for p in input_dir.glob(pattern) if p.is_file()]
    if not frames:
        sys.exit(f"error: no frames matching {pattern!r} in {input_dir}")
    indexed: list[tuple[int, str]] = []
    plain: list[Path] = []
    for frame in frames:
        match = _FRAME_INDEX.search(frame.name)
        if match is not None:
            indexed.append((int(match.group(1)), frame.name))
        else:
            plain.append(frame)
    if indexed and plain:
        sys.exit(f"error: mixed naming - no trailing _<index> on: {', '.join(p.name for p in plain)}")
    if indexed:
        indexed.sort()
        return [input_dir / name for _, name in indexed]
    return sorted(plain)


def stage_sequence(frames: list[Path], staging_dir: Path) -> Path:
    """Hard-link (fallback: copy) the frames into frame_%06d.<ext> order."""
    staging_dir.mkdir(parents=True, exist_ok=True)
    suffix = frames[0].suffix
    for index, frame in enumerate(frames):
        link = staging_dir / f"frame_{index:06d}{suffix}"
        if link.exists():
            link.unlink()
        try:
            os.link(frame, link)
        except OSError:
            shutil.copy2(frame, link)
    return staging_dir / f"frame_%06d{suffix}"


def probe(pattern_path: Path) -> str:
    try:
        result = subprocess.run(
            [
                "ffprobe",
                "-v",
                "error",
                "-select_streams",
                "v:0",
                "-show_entries",
                "stream=width,height,nb_read_frames",
                "-count_frames",
                "-of",
                "csv=p=0",
                str(pattern_path),
            ],
            capture_output=True,
            text=True,
            check=True,
        )
    except (OSError, subprocess.CalledProcessError):
        return ""
    return f" ({result.stdout.strip()} WxH, frames counted)"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("input_dir", type=Path, help="directory containing the frame images")
    parser.add_argument("output", type=Path, help="output video path (.mp4)")
    parser.add_argument("--pattern", default="*.png", help="glob matching the frames (default *.png)")
    rate = parser.add_mutually_exclusive_group()
    rate.add_argument("--fps", type=float, help="playback frames per second (default 30)")
    rate.add_argument(
        "--interval-ms",
        type=float,
        help="capture spacing in ms (--capture-every-ms): fps = 1000 / interval",
    )
    parser.add_argument("--crf", type=int, default=18, help="x264 quality (lower = better, default 18)")
    args = parser.parse_args()

    if not args.input_dir.is_dir():
        sys.exit(f"error: not a directory: {args.input_dir}")
    if args.interval_ms is not None:
        if args.interval_ms <= 0.0:
            sys.exit("error: --interval-ms must be > 0")
        fps = 1000.0 / args.interval_ms
    else:
        fps = args.fps if args.fps is not None else 30.0
    if fps <= 0.0:
        sys.exit("error: fps must be > 0")

    frames = discover_frames(args.input_dir, args.pattern)
    print(f"frames: {len(frames)} ({frames[0].name} .. {frames[-1].name}) at {fps:g} fps")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    staging = args.output.parent / f"{args.output.stem}_frames"
    try:
        pattern_path = stage_sequence(frames, staging)
        command = [
            "ffmpeg",
            "-y",
            "-framerate",
            f"{fps:g}",
            "-i",
            str(pattern_path),
            "-c:v",
            "libx264",
            "-preset",
            "medium",
            "-crf",
            str(args.crf),
            # yuv420p needs even dimensions - pad rather than fail.
            "-vf",
            "pad=ceil(iw/2)*2:ceil(ih/2)*2",
            "-pix_fmt",
            "yuv420p",
            str(args.output),
        ]
        try:
            result = subprocess.run(command, capture_output=True, text=True, check=True)
        except FileNotFoundError:
            sys.exit("error: ffmpeg not found on PATH (install ffmpeg to encode videos)")
        except subprocess.CalledProcessError as exc:
            sys.stderr.write(exc.stderr[-2000:] if exc.stderr else "")
            sys.exit(f"error: ffmpeg failed with exit code {exc.returncode}")
        for line in (result.stderr or "").splitlines():
            if "frame=" in line or "error" in line.lower():
                print("ffmpeg:", line.strip())
    finally:
        shutil.rmtree(staging, ignore_errors=True)

    size = args.output.stat().st_size
    summary = probe(args.output)
    print(f"wrote {args.output} ({size} bytes){summary}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
