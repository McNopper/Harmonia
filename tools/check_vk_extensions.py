"""§5.8 capability-audit receipt: the driver's KHR/EXT inventory vs the docs.

Runs ``vulkaninfoSDK`` (discovered via ``--sdk``, ``%VULKAN_SDK%``, or the
newest install under ``C:/VulkanSDK``), parses the instance and device
KHR/EXT extension lists, and diffs them against every ``VK_KHR_*`` /
``VK_EXT_*`` name written in PLAN.md + AGENTS.md (the §5.8 doc-named set).
A doc token ending in ``_`` (e.g. the ``VK_KHR_video_*`` family wildcard)
counts as satisfied when any driver extension starts with it, and claims
that family for the docs. The PLAN §5.8 grounding quotes this tool's counts
- re-run it after every driver or SDK update to re-stamp the audit (vendor
extensions are ignored by construction: only KHR/EXT names are ever matched,
per the §8 guardrail).

Exit codes:
    0 - every doc-named extension is available (device or instance)
    1 - a doc-named extension is absent from the driver entirely
    2 - environment problem (vulkaninfo not found, or it failed)

Usage: python tools/check_vk_extensions.py [--sdk C:/VulkanSDK/<version>]
           [--dump <saved-vulkaninfo.txt>] [--list-unadjudicated]
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DOCS = (REPO / "PLAN.md", REPO / "AGENTS.md")

# extension lines: "\tVK_KHR_xyz : extension revision N" (TAB-prefixed in the
# device section, column-aligned in the instance section)
EXT_LINE = re.compile(r"^\s*(VK_(?:KHR|EXT)_[A-Za-z0-9_]+)\s+:\s+extension revision")
DOC_NAME = re.compile(r"VK_(?:KHR|EXT)_[A-Za-z0-9_]+")


def parse(text: str) -> tuple[set[str], set[str], int | None]:
    """Split vulkaninfo output into (instance KHR/EXT, device KHR/EXT, total)."""
    instance: set[str] = set()
    device: set[str] = set()
    dev_total: int | None = None
    mode: str | None = None  # "instance" | "device"
    for line in text.splitlines():
        # anchored: must NOT fire on the layers' "Layer-Device Extensions: count"
        dev = re.match(r"\s*Device Extensions: count = (\d+)", line)
        if dev:
            mode = "device"
            dev_total = int(dev.group(1))
            continue
        if "Instance Extensions: count" in line:
            mode = "instance"
            continue
        if mode == "instance" and "count =" in line:
            mode = None  # next section header ends the instance list
        ext = EXT_LINE.match(line)
        if ext:
            if mode == "instance":
                instance.add(ext.group(1))
            elif mode == "device":
                device.add(ext.group(1))
    return instance, device, dev_total


def find_vulkaninfo(explicit: str | None) -> Path | None:
    if explicit:
        p = Path(explicit)
        if p.is_dir():
            p = p / "Bin" / "vulkaninfoSDK.exe"
        return p if p.exists() else None
    env = os.environ.get("VULKAN_SDK")
    if env:
        for name in ("vulkaninfoSDK.exe", "vulkaninfo"):
            p = Path(env) / "Bin" / name
            if p.exists():
                return p
    root = Path("C:/VulkanSDK")
    if root.is_dir():
        for sdk in sorted(root.iterdir(), key=lambda d: d.name, reverse=True):
            for name in ("vulkaninfoSDK.exe", "vulkaninfo"):
                p = sdk / "Bin" / name
                if p.exists():
                    return p
    return None


def run(exe: Path) -> str:
    env = dict(os.environ)
    env["PATH"] = str(exe.parent) + os.pathsep + env.get("PATH", "")
    try:
        out = subprocess.run(
            [str(exe)],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            env=env,
            timeout=180,
            check=False,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        print(f"ERROR: cannot run {exe}: {exc}", file=sys.stderr)
        sys.exit(2)
    if out.returncode != 0:
        print(
            f"ERROR: {exe} exited {out.returncode}: {out.stderr.strip()[:400]}",
            file=sys.stderr,
        )
        sys.exit(2)
    return out.stdout


def main() -> int:
    ap = argparse.ArgumentParser(description="KHR/EXT inventory vs the roadmap docs")
    ap.add_argument("--sdk", help="SDK root or Bin dir (default: VULKAN_SDK, then C:/VulkanSDK)")
    ap.add_argument("--dump", type=Path, help="parse a saved vulkaninfo output instead of running it")
    ap.add_argument("--list-unadjudicated", action="store_true", help="print the device KHR/EXT names never mentioned in the docs")
    args = ap.parse_args()

    if args.dump:
        text = args.dump.read_text(encoding="utf-8", errors="replace")
        source = str(args.dump)
    else:
        exe = find_vulkaninfo(args.sdk)
        if exe is None:
            print("ERROR: vulkaninfoSDK not found (use --sdk or set VULKAN_SDK)", file=sys.stderr)
            return 2
        text = run(exe)
        source = str(exe)

    instance, device, dev_total = parse(text)
    if not device:
        print("ERROR: no device KHR/EXT extensions parsed (malformed dump?)", file=sys.stderr)
        return 2

    doc_tokens: set[str] = set()
    for doc in DOCS:
        doc_tokens |= set(DOC_NAME.findall(doc.read_text(encoding="utf-8", errors="replace")))
    wildcards = {t for t in doc_tokens if t.endswith("_")}  # e.g. VK_KHR_video_
    doc_names = doc_tokens - wildcards

    available = instance | device
    missing = sorted(n for n in doc_names if n not in available)
    missing += sorted(w for w in wildcards if not any(e.startswith(w) for e in available))
    instance_only = sorted((doc_names & instance) - device)
    claimed = {e for e in device if any(e.startswith(w) for w in wildcards)}
    unadjudicated = sorted(device - doc_names - claimed)

    print(f"source                 : {source}")
    print(f"device extensions      : {dev_total} total, {len(device)} KHR/EXT")
    print(f"instance KHR/EXT       : {len(instance)}")
    print(
        f"doc-named (PLAN+AGENTS): {len(doc_names)} names + {len(wildcards)} wildcard"
        f" - on device {len(doc_names & device)}"
    )
    if instance_only:
        print(f"doc-named, instance-level only: {', '.join(instance_only)}")
    print(
        f"unadjudicated (device KHR/EXT, never named in docs): {len(unadjudicated)}"
        + (f" (+{len(claimed)} claimed by wildcard)" if claimed else "")
    )
    if args.list_unadjudicated:
        for name in unadjudicated:
            print(f"  {name}")

    if missing:
        print("MISSING from the driver entirely (docs vs driver mismatch):")
        for name in missing:
            print(f"  {name}")
        return 1
    print("OK - every doc-named KHR/EXT extension is available (device or instance)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
