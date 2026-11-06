"""VK22 next-gen shader model verification: does slangc 2026.17 emit the quartet?

Probes the Slang compiler surface for the four §5.8 VK22 capabilities
(shader_untyped_pointers, shader_replicated_composites, shader_64bit_indexing,
shader_long_vector). Each probe compiles a minimal compute kernel through
``slangc`` to SPIR-V and inspects the result with ``spirv-val`` (and, on
success, ``spirv-dis`` for the SPIR-V capability). The matrix is recorded in
Harmonia ``PLAN.md`` §5.8 (VK22 row); adoption still waits for a consumer.

Usage: python tools/check_slang_ngsm.py [--sdk C:/VulkanSDK/<version>]
Exit code 0 when the probe ran to completion (regardless of per-probe results).
"""

from __future__ import annotations

import argparse
import os
import subprocess
import tempfile
from pathlib import Path

# (probe id, slang source, note)
PROBES: list[tuple[str, str, str]] = [
    (
        "long-vector",
        """
RWStructuredBuffer<uint> g_out;
[shader("compute")]
[numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    vector<uint, 64> v;
    for (int i = 0; i < 64; i++) { v[i] = tid.x + uint(i); }
    g_out[tid.x] = v[0] + v[63];
}
""",
        "vector<uint, 64> - Slang long-vector user surface",
    ),
    (
        "untyped-pointer-ptr",
        """
[shader("compute")]
[numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    Ptr<uint> p;
    g_dummy(tid.x, p);
}
""",
        "Ptr<T> generic shader pointer - Slang pointer surface",
    ),
    (
        "untyped-pointer-cstyle",
        """
RWStructuredBuffer<uint> g_out;
[shader("compute")]
[numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    uint value = tid.x;
    uint* p = &value;
    g_out[tid.x] = *p;
}
""",
        "C-style uint* local pointer - Slang pointer surface (CUDA-style)",
    ),
    (
        "replicated-composites",
        """
RWStructuredBuffer<uint4> g_out;
struct Payload { uint4 a; uint4 b; };
[shader("compute")]
[numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    Payload p;
    p.a = uint4(tid.x, 1, 2, 3);
    p.b = p.a; // composite copy - replicated composites would collapse storage
    g_out[tid.x] = p.a + p.b;
}
""",
        "composite copy - replicated composites (compiler-internal construct)",
    ),
    (
        "64bit-indexing",
        """
RWStructuredBuffer<uint> g_out;
[shader("compute")]
[numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    uint64_t index = uint64_t(tid.x) * 2u;
    g_out[tid.x] = uint(index);
}
""",
        "uint64_t index arithmetic feeding a structured-buffer store",
    ),
]


def run_tool(tool: Path, args: list[str], cwd: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(tool), *args],
        cwd=cwd,
        capture_output=True,
        text=True,
        shell=False,
        timeout=120,
        check=False,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", default=os.environ.get("VULKAN_SDK", ""), help="Vulkan SDK root")
    args = parser.parse_args()
    sdk = Path(args.sdk)
    slangc = sdk / "Bin" / "slangc.exe"
    spirv_val = sdk / "Bin" / "spirv-val.exe"
    spirv_dis = sdk / "Bin" / "spirv-dis.exe"
    for tool in (slangc, spirv_val):
        if not tool.exists():
            raise SystemExit(f"tool not found: {tool}")
    workdir = Path(tempfile.mkdtemp(prefix="slang_ngsm_"))

    print(f"slangc: {slangc}")
    results: list[tuple[str, str, str]] = []
    for probe_id, source, note in PROBES:
        src = workdir / f"{probe_id}.slang"
        spv = workdir / f"{probe_id}.spv"
        src.write_text(source.strip() + "\n", encoding="utf-8")
        compile_result = run_tool(slangc, [str(src), "-stage", "compute", "-target", "spirv", "-o", str(spv)], workdir)
        if compile_result.returncode != 0:
            first_error = next(
                (line.strip() for line in compile_result.stderr.splitlines() if "error" in line.lower()),
                compile_result.stderr.strip().splitlines()[0] if compile_result.stderr else "no output",
            )
            results.append((probe_id, "REJECTED by slangc", first_error[:160]))
            print(f"[{probe_id}] REJECTED ({note})\n    {first_error.strip()[:160]}")
            continue
        val = run_tool(spirv_val, [str(spv)], workdir)
        if val.returncode != 0:
            results.append((probe_id, "spirv-val FAILED", val.stderr.strip()[:160]))
            print(f"[{probe_id}] compiled but spirv-val FAILED\n    {val.stderr.strip()[:160]}")
            continue
        capabilities = ""
        if spirv_dis.exists():
            dis = run_tool(spirv_dis, [str(spv)], workdir)
            for line in dis.stdout.splitlines():
                if "OpCapability" in line:
                    capabilities += line.split("OpCapability")[1].strip() + " "
        results.append((probe_id, "compiled+valid", capabilities.strip()))
        print(f"[{probe_id}] compiled+valid ({note}) capabilities: {capabilities.strip()}")

    print("\n=== matrix (record in PLAN.md §5.8 VK22) ===")
    for probe_id, status, detail in results:
        print(f"{probe_id:28s} {status:22s} {detail}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
