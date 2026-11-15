#!/usr/bin/env python3
"""
test_compare_metrics.py - CPU unit receipts for the PAR1/PAR4 metric additions
in compare_renders.py (no GPU, no EXR files - the metric functions take arrays).

PAR1 (tone-map-aware layer): the tone-mapped metric set (tm_mean_diff / tm_PSNR,
SSIM under the selected operator) evaluates the image the display pipeline would
show - ACES (default) or AgX, mirroring Harmonia tonemap.slang byte-faithfully -
and --tm-mean-threshold ANDs the tone-mapped mean into the 'all' gate opt-in.
PAR4 (perceptual uniformity for HDR): mean/max per-pixel distance in BT.2100
ICtCp (PQ-encoded, documented 1.0 scene-linear = 10000 nits convention),
complementing the HDR-FLIP story with a uniform-space distance.

Usage:  python tools/test_compare_metrics.py
Exit:   0 = all checks pass, 1 = any check fails.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))

import compare_renders as cr

_FAILURES: list[str] = []


def check(name: str, ok: bool, detail: str = "") -> None:
    status = "ok  " if ok else "FAIL"
    print(f"  [{status}] {name}" + (f"  ({detail})" if detail else ""))
    if not ok:
        _FAILURES.append(name)


def _test_pair() -> tuple[np.ndarray, np.ndarray]:
    """Deterministic 32x32 HDR pair (>= 11x11 so windowed SSIM is defined)."""
    y, x = np.mgrid[0:32, 0:32].astype(np.float64)
    ref = np.stack([
        0.05 + 0.03 * x / 31.0,
        0.06 + 0.02 * y / 31.0,
        0.4 + 0.5 * (x / 31.0) * (y / 31.0),
    ], axis=-1)
    cand = ref.copy()
    return ref, cand


def main() -> int:
    ref, cand = _test_pair()

    # ── PAR4 + base metrics on an identical pair ──────────────────────────────
    m = cr.compute_metrics(ref, cand)
    check("identical: mean_diff == 0", m["mean_diff"] == 0.0, f"{m['mean_diff']}")
    check("identical: SSIM == 1", abs(m["ssim"] - 1.0) < 1e-12, f"{m['ssim']:.12f}")
    check("identical: tm_mean_diff == 0", m["tm_mean_diff"] == 0.0, f"{m['tm_mean_diff']}")
    check("identical: tm_PSNR == inf", m["tm_psnr"] == float("inf"), f"{m['tm_psnr']}")
    check("identical: ictec_dE == (0, 0)",
          m["ictec_de_mean"] == 0.0 and m["ictec_de_max"] == 0.0,
          f"mean={m['ictec_de_mean']} max={m['ictec_de_max']}")
    check("identical: lum_hist_corr == 1", abs(m["lum_hist_corr"] - 1.0) < 1e-9,
          f"{m['lum_hist_corr']:.9f}")

    # ── Shift monotonicity: a bigger deviation scores bigger on both layers ──
    small = cr.compute_metrics(ref, ref + 0.05)
    big = cr.compute_metrics(ref, ref + 0.10)
    check("tm layer monotone: tm_mean(0.10) > tm_mean(0.05)",
          big["tm_mean_diff"] > small["tm_mean_diff"] > 0.0,
          f"{small['tm_mean_diff']:.4f} < {big['tm_mean_diff']:.4f}")
    check("PAR4 monotone: ictec(0.10) > ictec(0.05) > 0",
          big["ictec_de_mean"] > small["ictec_de_mean"] > 0.0,
          f"{small['ictec_de_mean']:.4f} < {big['ictec_de_mean']:.4f}")

    # ── HDR robustness: values far above display white behave correctly ──────
    # 50→55 fully saturates the ACES shoulder → the TM images are identical
    # (tm_mean 0, PSNR inf — correct); a mid-range HDR pair must be finite.
    hdr_ref = np.full((32, 32, 3), 50.0, dtype=np.float64)
    hdr_cand = np.full((32, 32, 3), 55.0, dtype=np.float64)
    m_hdr = cr.compute_metrics(hdr_ref, hdr_cand)
    check("HDR: ictec finite and > 0 at 50->55",
          np.isfinite(m_hdr["ictec_de_mean"]) and m_hdr["ictec_de_mean"] > 0.0,
          f"mean={m_hdr['ictec_de_mean']:.6f}")
    check("HDR: saturated pair -> tm_mean 0, tm_PSNR inf (no NaN)",
          m_hdr["tm_mean_diff"] == 0.0 and m_hdr["tm_psnr"] == float("inf"),
          f"tm_mean={m_hdr['tm_mean_diff']} tm_psnr={m_hdr['tm_psnr']}")
    mid = cr.compute_metrics(np.full((32, 32, 3), 1.5), np.full((32, 32, 3), 2.0))
    check("HDR: mid-range pair -> finite tm metrics",
          np.isfinite(mid["tm_mean_diff"]) and np.isfinite(mid["tm_psnr"]),
          f"tm_mean={mid['tm_mean_diff']:.3f} tm_psnr={mid['tm_psnr']:.2f}")

    # ── AgX operator (PAR1): range, monotone gray ramp, distinct from ACES ────
    ramp = np.linspace(0.0, 6.0, 64, dtype=np.float64)
    gray = np.stack([ramp, ramp, ramp], axis=-1)[None, :, :]
    agx = cr.tonemap_agx_srgb(gray)
    aces = cr.tonemap_aces_srgb(gray)
    check("agx: output in [0, 1]", float(agx.min()) >= 0.0 and float(agx.max()) <= 1.0,
          f"[{agx.min():.4f}, {agx.max():.4f}]")
    check("agx: gray ramp non-decreasing",
          bool(np.all(np.diff(agx[0, :, 0]) >= -1e-9)),
          f"min diff {float(np.diff(agx[0, :, 0]).min()):.2e}")
    check("agx: differs from ACES (distinct operators)",
          float(np.abs(agx - aces).max()) > 1e-3,
          f"max delta {float(np.abs(agx - aces).max()):.4f}")

    # ── Gate integration (PAR1 opt-in AND, defaults unchanged) ────────────────
    base_args = dict(gate="all", threshold=4.0, rel_mse_threshold=0.01,
                     ssim_threshold=0.98, lum_hist_threshold=0.999,
                     psnr_threshold=None, relative_threshold=None, tonemap="aces")
    clean = dict(m)
    clean["tm_mean_diff"] = 1.0

    ok_pass, labels = cr.evaluate_gate(
        clean, argparse.Namespace(tm_mean_threshold=5.0, **base_args))
    check("gate: clean metrics + tm threshold pass",
          ok_pass and any("tm_mean_diff" in lb for lb in labels),
          "; ".join(labels))

    dirty = dict(clean)
    dirty["tm_mean_diff"] = 10.0
    ok_fail, _ = cr.evaluate_gate(
        dirty, argparse.Namespace(tm_mean_threshold=5.0, **base_args))
    check("gate: tm threshold fails when exceeded", not ok_fail)

    ok_default, labels_default = cr.evaluate_gate(
        clean, argparse.Namespace(tm_mean_threshold=None, **base_args))
    check("gate: defaults unchanged (tm not gated when unset)",
          ok_default and not any("tm_mean" in lb for lb in labels_default))

    ok_other, _ = cr.evaluate_gate(
        dirty, argparse.Namespace(tm_mean_threshold=None, **base_args))
    check("gate: other metrics still drive the default gate (tm ignored)",
          ok_other is True)

    # ── Mask path: metrics stay defined on a sub-rectangle ────────────────────
    mask = np.zeros((32, 32), dtype=bool)
    mask[8:28, 8:28] = True
    m_masked = cr.compute_metrics(ref, ref + 0.1, mask=mask)
    check("mask: tm + ictec defined on sub-rectangle",
          np.isfinite(m_masked["tm_mean_diff"]) and m_masked["ictec_de_mean"] > 0.0,
          f"tm={m_masked['tm_mean_diff']:.3f} ictec={m_masked['ictec_de_mean']:.4f}")

    print()
    if _FAILURES:
        print(f"FAILED: {len(_FAILURES)} check(s): {', '.join(_FAILURES)}")
        return 1
    print("ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
