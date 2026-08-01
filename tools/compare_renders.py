#!/usr/bin/env python3
"""
compare_renders.py — Hyperion / Theia pre-tonemap HDR diff tool.

USAGE
-----
    python tools/compare_renders.py <reference.exr> <candidate.exr> [options]

    reference  : Hyperion output  (--output ref.exr)
    candidate  : Theia output     (--output cand.exr)

OPTIONS
    --threshold FLOAT      Mean-diff threshold for pass/fail (default: 4.0, in 1/255 units)
    --rel-mse-threshold FLOAT
                           Pass if Rel-MSE <= this value (default: 0.01, scene-referred)
    --rel-mse-eps FLOAT    Denominator floor for Rel-MSE (default: 5e-3). sqrt(eps) is the
                           effective black point below which reference pixels are damped.
    --ssim-threshold FLOAT
                           Pass if SSIM >= this value (default: 0.98, tone-mapped sRGB)
    --lum-hist-threshold FLOAT
                           Pass if luminance-histogram Pearson correlation >= this value
                           (default: 0.999, scene-referred EXR)
    --psnr-threshold FLOAT
                           Pass if PSNR >= this value (dB) — opt-in extra metric
    --relative-threshold FLOAT
                           Pass if relative mean error <= this value (percent) — opt-in extra
    --hist-bins INT        Luminance-histogram log-spaced bin count for the positive range,
                           plus one explicit black bin (default: 256)
    --allow-small-ssim     Permit the degraded GLOBAL non-windowed SSIM when the compared
                           region is smaller than the 11x11 window. Without this flag such a
                           region is a hard error rather than a silent metric substitution.
    --gate MODE            Gate mode (default: all):
        all          Strict AND of all declared §10 metrics:
                     mean_diff <= threshold AND rel_mse <= rel-mse-threshold AND
                     SSIM >= ssim-threshold AND lum_hist_corr >= lum-hist-threshold.
                     When --psnr-threshold / --relative-threshold are also given they are
                     ANDed in as extras. This is the default and matches SPRINT-PLAN §10
                     ("all of them, never one alone").
        mean         Pass if mean_diff <= threshold (legacy single-metric diagnostic).
        psnr         Pass if PSNR >= psnr-threshold (requires --psnr-threshold).
        relative     Pass if rel_mean_% <= relative-threshold (requires --relative-threshold).
        any          Pass if ANY of the available metrics passes (loose diagnostic).
        scale-aware  Explicit opt-in for HDR transmissive fixtures (Theia/README.md:253):
                     pass if mean_diff <= threshold OR
                           (rel_mean_% <= relative-threshold AND PSNR >= psnr-threshold),
                     with defaults 2.5% / 20 dB when not provided. The Rel-MSE / SSIM /
                     lum-hist metrics are still REPORTED but not required under this mode,
                     because brightness-dependent false fails on bright HDR transmissions
                     are exactly what scale-aware exists to tolerate.
    --heatmap PATH      Write a false-color difference PNG (default: <candidate>_diff.png)
    --channel CH        Compare single channel: r, g, b, luminance (default: luminance)
    --no-heatmap        Skip heatmap output
    --signed            Also report SIGNED mean (cand - ref) per channel + luminance, and write
                        a diverging heatmap (blue = candidate DARKER, red = candidate BRIGHTER).
    --signed-heatmap PATH
                        Diverging heatmap path (default: <candidate>_signed.png with --signed)
    --mask-box X0,Y0,X1,Y1
                        Restrict ALL metrics to a normalized [0,1] sub-rectangle
                        (e.g. --mask-box 0.7,0.3,1.0,0.7 for the right-hand region).

CONTRACT
--------
Both EXR files must be:
  - Scene-referred linear (produced by --output *.exr, never *.png)
  - Same resolution
  - Same working color space (both renderers must use the same [render] preset)
  - Rendered without SSR / SSAO / bloom (Theia: pass --no-postfx; Hyperion has none)
  - Pre-tonemap: the EXR from harmonia::App::renderOffscreen() is the raw HDR buffer

METRICS
-------
  mean_diff      : mean |ref - cand| per luminance pixel, scaled to [0, 255]   (gate: <= threshold)
  rel_mse        : relative MSE = mean((ref-cand)^2 / (ref^2 + eps))            (gate: <= rel-mse-threshold)
                   on the selected channel of the scene-referred EXR. Standard
                   asymmetric form (Rousselle et al. 2011): the REFERENCE alone is
                   the denominator. Approximately (RMS relative error)^2, so the
                   0.01 default corresponds to roughly 10-15% RMS relative error.
  SSIM           : Wang 2004 mean structural similarity on tone-mapped sRGB     (gate: >= ssim-threshold)
                   (ACES RRT+ODT fit + sRGB OETF, matching Harmonia's PNG capture path).
                   Regions smaller than the 11x11 window are an error, not a
                   silent fallback (see --allow-small-ssim).
  lum_hist_corr  : Pearson correlation of log-spaced luminance histograms       (gate: >= lum-hist-threshold)
                   of the scene-referred EXR, with one explicit leading bin
                   counting black / near-zero pixels so that no pixel is discarded.
  black_frac     : fraction of black / near-zero luminance pixels in each image
                   (diagnostic, NOT gated). Makes a black-out asymmetry between
                   the renderers visible as a raw count, which the scale-invariant
                   Pearson correlation only weakly reflects.
  max_diff       : max  |ref - cand| per luminance pixel, scaled to [0, 255]
  p99_diff       : 99th percentile of |ref-cand| (scaled to [0,255])
  p99.9_diff     : 99.9th percentile of |ref-cand| (scaled to [0,255])
  PSNR           : peak signal-to-noise ratio (dB); inf = identical; < 30 dB = visible
  rel_mean_%     : mean |ref-cand| / max(mean |ref|, eps) * 100
  pass           : selected gate mode with the provided threshold(s)
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np


# ── EXR I/O ──────────────────────────────────────────────────────────────────

def load_exr(path: Path) -> np.ndarray:
    """Load an OpenEXR file and return a float32 HxWx3 array (RGB)."""
    try:
        import OpenEXR
        import Imath
    except ImportError:
        sys.exit("ERROR: OpenEXR Python bindings not found.  Run: pip install openexr imath")

    f = OpenEXR.InputFile(str(path))
    header = f.header()
    dw = header["dataWindow"]
    width  = dw.max.x - dw.min.x + 1
    height = dw.max.y - dw.min.y + 1

    FLOAT = Imath.PixelType(Imath.PixelType.FLOAT)
    channels = {}
    for ch in ("R", "G", "B"):
        raw = f.channel(ch, FLOAT)
        channels[ch] = np.frombuffer(raw, dtype=np.float32).reshape(height, width)

    return np.stack([channels["R"], channels["G"], channels["B"]], axis=-1)


# ── Metrics ───────────────────────────────────────────────────────────────────

def luminance_rec2020(img: np.ndarray) -> np.ndarray:
    """Rec.2020 luminance coefficients (linear)."""
    return 0.2627 * img[..., 0] + 0.6780 * img[..., 1] + 0.0593 * img[..., 2]


def luminance_rec709(img: np.ndarray) -> np.ndarray:
    return 0.2126 * img[..., 0] + 0.7152 * img[..., 1] + 0.0722 * img[..., 2]


def extract_channel(img: np.ndarray, ch: str) -> np.ndarray:
    ch = ch.lower()
    if ch == "r":
        return img[..., 0]
    if ch == "g":
        return img[..., 1]
    if ch == "b":
        return img[..., 2]
    if ch in ("lum", "luminance", "y"):
        return luminance_rec2020(img)
    sys.exit(f"ERROR: unknown channel '{ch}'. Use r, g, b, or luminance.")


# ── Tone mapping (mirrors Harmonia tonemap.slang / ToneMapping.hpp) ───────────
#
# The SDR display / offscreen-PNG capture path (ImageCapture.cpp:115) applies
# acesFittedSDR (Rec.2020 linear -> ACES RRT+ODT fit, Stephen Hill -> Rec.709
# linear) followed by the sRGB OETF. SSIM is a display-referred metric, so the
# EXR pair is run through exactly this transform before the SSIM window. ACES is
# the default tonemapper (eACES = 0); the matrices below are byte-identical to
# the shader rows.

_REC2020_TO_AP1 = np.array([
    [0.6131324, 0.3395255, 0.0474491],
    [0.0701243, 0.9163394, 0.0135363],
    [0.0205076, 0.1096098, 0.8699926],
], dtype=np.float64)

_AP1_TO_REC709 = np.array([
    [ 2.9090, -1.6933, -0.2159],
    [-0.3595,  1.3712, -0.0114],
    [-0.0447, -0.2477,  1.2925],
], dtype=np.float64)


def _aces_rrt_odt_fit(v: np.ndarray) -> np.ndarray:
    a = v * (v + 0.0245786) - 0.000090537
    b = v * (0.983729 * v + 0.4329510) + 0.238081
    return a / b


def _srgb_oetf(x: np.ndarray) -> np.ndarray:
    x = np.maximum(x, 0.0)
    return np.where(x <= 0.0031308, 12.92 * x, 1.055 * np.power(x, 1.0 / 2.4) - 0.055)


def tonemap_aces_srgb(rgb: np.ndarray) -> np.ndarray:
    """Rec.2020 scene-linear HDR → tone-mapped sRGB [0,1] (ACES fit + sRGB OETF).

    Matches Harmonia's offscreen PNG capture path (acesFittedSDR + sRGB OETF),
    so SSIM is evaluated on the same image the display pipeline would show.
    """
    f64 = rgb.astype(np.float64, copy=False)
    ap1 = np.clip(f64 @ _REC2020_TO_AP1.T, 0.0, None)
    ap1 = _aces_rrt_odt_fit(ap1)
    sdr = np.clip(ap1 @ _AP1_TO_REC709.T, 0.0, None)
    return np.clip(_srgb_oetf(sdr), 0.0, 1.0)


# ── Declared §10 metrics ─────────────────────────────────────────────────────

# Rel-MSE denominator floor, in squared scene-referred luminance units. sqrt(eps)
# is the effective "black point": reference pixels darker than that are damped
# rather than allowed to divide by ~0.
#
# 5e-3 => black point 0.071 luminance (~7% of SDR white). Chosen by sweep over
# the 14-scene manifest corpus, scoring each eps by how well the metric
# SEPARATES scenes that are independently clean (mean_diff / SSIM) from scenes
# that are independently diverged:
#
#   eps     max(clean)  min(diverged)  ratio   Spearman vs mean_diff
#   3e-2      0.00301      0.00973     3.24         0.745
#   1e-2      0.00408      0.01344     3.29         0.837   <- DEFAULT (literature)
#   5e-3      0.00504      0.01587     3.15         0.886
#   1e-3      0.01099      0.02022     1.84         0.815
#   1e-4      0.03594      0.02472     0.69 (INVERTED - unusable)
#   1e-6      0.08219      0.02671     0.33 (INVERTED - unusable)
#
# The inversion below ~1e-3 is not a tuning artefact: on cornell_classic at
# eps=1e-4, 51% of the total rel-MSE mass comes from the 1.4% of pixels whose
# reference luminance is under 0.01 (and 72% at eps=1e-6). A "much smaller eps"
# therefore turns Rel-MSE into a report on sub-1%-luminance Monte-Carlo noise.
#
# We use the established value eps = 1e-2 (Rousselle et al., greedy-error-
# minimization adaptive sampling; standard practice in the MC-denoising
# literature). Selecting an eps by scoring candidates against THIS corpus would
# be fitting the metric to the data it is meant to judge - the table above is
# recorded as supporting evidence only, never as the selection criterion.
REL_MSE_EPS = 1.0e-2

# Luminance at or below this counts as "black" for the histogram metric, and is
# tallied in a dedicated bin instead of being silently discarded. Also catches
# negative luminance (possible in EXRs via denoiser ringing or out-of-gamut
# Rec.2020 primaries), which has no logarithm and would otherwise vanish.
BLACK_LUMINANCE_EPS = 1.0e-8


def _mask_bbox(mask2d: np.ndarray) -> tuple[int, int, int, int]:
    rows = np.any(mask2d, axis=1)
    cols = np.any(mask2d, axis=0)
    r0, r1 = np.where(rows)[0][[0, -1]]
    c0, c1 = np.where(cols)[0][[0, -1]]
    return int(r0), int(r1) + 1, int(c0), int(c1) + 1


def compute_rel_mse(ref: np.ndarray, cand: np.ndarray,
                    mask: np.ndarray | None = None, eps: float = REL_MSE_EPS) -> float:
    """Relative mean squared error (Rousselle et al. 2011) on a single channel.

        rel_mse = mean( (ref - cand)^2 / (ref^2 + eps) )

    This is the standard rendering-literature relative MSE: the denominator is
    the REFERENCE only, never a symmetric ref^2 + cand^2. The asymmetric form is
    the one that means "squared error relative to the ground truth"; the
    symmetric variant systematically UNDERSTATES divergence, because a candidate
    that is wrong also inflates its own denominator (measured on
    openpbr_dielectrics: symmetric 0.00711 vs standard 0.01344 — a 1.9x
    understatement).

    `eps` is a squared-luminance floor that keeps the near-black region from
    dominating: it damps pixels whose reference luminance is below sqrt(eps).
    Some floor is mandatory — with eps -> 0 the score is driven entirely by
    sub-1%-luminance pixels, which are both visually irrelevant and the noisiest
    part of a Monte-Carlo reference (see REL_MSE_EPS for the measured data).

    Calibration: in the eps-free limit this equals the mean squared RELATIVE
    error, i.e. rel_mse ~= (RMS relative error)^2. With the default eps a
    uniform 10% error reads 0.005-0.009 and a 15% error reads 0.010-0.021,
    depending on how much of the frame is dark.

    Operates on the selected channel of the scene-referred EXR. Self-comparison
    (ref is cand) returns exactly 0.0.
    """
    if mask is None:
        a = ref
        b = cand
    else:
        a = ref[mask]
        b = cand[mask]
    a = a.astype(np.float64, copy=False)
    b = b.astype(np.float64, copy=False)
    num = (a - b) ** 2
    den = a * a + eps
    return float(np.mean(num / den))


def _gaussian_kernel1d(size: int = 11, sigma: float = 1.5) -> np.ndarray:
    coords = np.arange(size, dtype=np.float64) - (size - 1) / 2.0
    g = np.exp(-(coords ** 2) / (2.0 * sigma * sigma))
    return g / g.sum()


def _conv2d_separable_valid(img: np.ndarray, kernel: np.ndarray) -> np.ndarray:
    win = np.lib.stride_tricks.sliding_window_view(img, len(kernel), axis=1)
    out = np.tensordot(win, kernel, axes=([-1], [0]))
    win2 = np.lib.stride_tricks.sliding_window_view(out, len(kernel), axis=0)
    return np.tensordot(win2, kernel, axes=([-1], [0]))


def compute_ssim(ref_rgb: np.ndarray, cand_rgb: np.ndarray,
                 mask: np.ndarray | None = None, data_range: float = 1.0,
                 allow_small: bool = False) -> float:
    """Mean structural similarity (Wang et al. 2004) on tone-mapped sRGB.

    Per-channel SSIM with an 11x11 Gaussian window (sigma 1.5), averaged across
    channels and the valid window region. Both images are run through
    tonemap_aces_srgb first. When a spatial mask is given, both tone-mapped
    images are cropped to the mask's tight bounding box first, so SSIM is
    measured only on the region of interest.

    If the (possibly cropped) region is smaller than the 11x11 window, windowed
    SSIM is undefined. This raises ValueError rather than silently substituting
    a different metric. Pass allow_small=True to opt into the degraded global,
    NON-WINDOWED scalar SSIM instead; it is a different statistic and its value
    is NOT comparable to the windowed one or to the declared SSIM threshold.
    """
    a = tonemap_aces_srgb(ref_rgb)
    b = tonemap_aces_srgb(cand_rgb)
    if mask is not None:
        r0, r1, c0, c1 = _mask_bbox(mask)
        a = a[r0:r1, c0:c1]
        b = b[r0:r1, c0:c1]

    h, w = a.shape[0], a.shape[1]
    c1 = (0.01 * data_range) ** 2
    c2 = (0.03 * data_range) ** 2

    if h < 11 or w < 11:
        if not allow_small:
            raise ValueError(
                f"SSIM region is {w}x{h}, smaller than the 11x11 Gaussian window, "
                f"so windowed SSIM (Wang 2004) is undefined. Widen --mask-box to "
                f"cover at least 11x11 pixels, or pass --allow-small-ssim to fall "
                f"back to a GLOBAL non-windowed SSIM (a DIFFERENT metric, not "
                f"comparable to the --ssim-threshold contract)."
            )
        print(
            f"  WARNING: SSIM region is {w}x{h} (< 11x11 window). Falling back to "
            f"GLOBAL NON-WINDOWED SSIM — this is a DIFFERENT metric from the "
            f"windowed Wang 2004 SSIM the gate is specified against, it ignores "
            f"all spatial structure, and it is NOT comparable to --ssim-threshold.",
            file=sys.stderr,
        )
        mu_a, mu_b = a.mean(), b.mean()
        s_a2 = ((a - mu_a) ** 2).mean()
        s_b2 = ((b - mu_b) ** 2).mean()
        s_ab = ((a - mu_a) * (b - mu_b)).mean()
        num = (2 * mu_a * mu_b + c1) * (2 * s_ab + c2)
        den = (mu_a ** 2 + mu_b ** 2 + c1) * (s_a2 + s_b2 + c2)
        return float(num / den)

    kernel = _gaussian_kernel1d(11, 1.5)
    ssim_values = []
    for ch in range(a.shape[2]):
        x = a[..., ch]
        y = b[..., ch]
        mu_x = _conv2d_separable_valid(x, kernel)
        mu_y = _conv2d_separable_valid(y, kernel)
        mu_x2 = mu_x * mu_x
        mu_y2 = mu_y * mu_y
        mu_xy = mu_x * mu_y
        sigma_x2 = _conv2d_separable_valid(x * x, kernel) - mu_x2
        sigma_y2 = _conv2d_separable_valid(y * y, kernel) - mu_y2
        sigma_xy = _conv2d_separable_valid(x * y, kernel) - mu_xy
        num = (2 * mu_xy + c1) * (2 * sigma_xy + c2)
        den = (mu_x2 + mu_y2 + c1) * (sigma_x2 + sigma_y2 + c2)
        ssim_values.append((num / den).mean())
    return float(np.mean(ssim_values))


def compute_lum_histogram(lum: np.ndarray, edges: np.ndarray,
                          black_eps: float = BLACK_LUMINANCE_EPS) -> np.ndarray:
    """Histogram of luminance with an explicit leading "black/near-zero" bin.

    Returns an array of length len(edges) (i.e. one black bin + len(edges)-1
    log-spaced bins). NOTHING is discarded: values <= black_eps (including
    negatives) are tallied in bin 0, and positive values are clamped into the
    log-spaced range so nothing can fall off either end.
    """
    black = lum <= black_eps
    pos = lum[~black]
    counts = np.histogram(np.clip(pos, edges[0], edges[-1]), bins=edges)[0]
    return np.concatenate([[int(black.sum())], counts]).astype(np.float64)


def compute_lum_histogram_correlation(ref_rgb: np.ndarray, cand_rgb: np.ndarray,
                                      mask: np.ndarray | None = None,
                                      bins: int = 256,
                                      black_eps: float = BLACK_LUMINANCE_EPS) -> float:
    """Pearson correlation of log-spaced luminance histograms (scene-referred).

    `bins` log10-spaced bins cover the positive luminance range, so HDR dynamic
    range is represented linearly in log space, PLUS one explicit leading bin
    counting black / near-zero pixels (luminance <= black_eps).

    That leading bin is the correctness fix for a real gap: np.histogram with
    explicit edges DISCARDS everything below edges[0], and the previous log floor
    at 1e-8 meant every fully-black pixel was thrown away. On cornell_classic
    that silently dropped 415,155 reference and 430,224 candidate pixels of
    921,600 (~45%) — and because Pearson correlation is scale-invariant, the
    15,069-pixel asymmetry in how many pixels each renderer left black was
    invisible. Counting blacks makes that asymmetry contribute to the score.

    Caveat, measured: because the black bin is enormous relative to the others,
    it dominates Pearson's covariance, so the correlation remains a WEAK detector
    of black-out asymmetry (15k extra blacked-out pixels moves the score from
    1.000000 to 0.999922 — an ~80x improvement in sensitivity over discarding
    them, but still a small absolute shift). The unambiguous signal is the
    black_frac_ref / black_frac_cand diagnostic reported alongside it.

    Returns 1.0 for identical histograms and for the degenerate identical-constant
    case; returns 0.0 when one histogram has zero variance and the other does not.
    """
    lr = luminance_rec2020(ref_rgb)
    lc = luminance_rec2020(cand_rgb)
    if mask is not None:
        lr = lr[mask]
        lc = lc[mask]
    else:
        lr = lr.ravel()
        lc = lc.ravel()
    lr = lr.astype(np.float64, copy=False)
    lc = lc.astype(np.float64, copy=False)

    # Log-spaced edges span the POSITIVE range of the union; blacks get their own
    # bin, so the floor no longer has to double as a discard threshold.
    pos = np.concatenate([lr[lr > black_eps], lc[lc > black_eps]])
    if pos.size == 0:
        # Both frames are entirely black: identical iff the counts match.
        return 1.0 if lr.size == lc.size else 0.0
    lo = float(pos.min())
    hi = float(pos.max())
    if hi <= lo:
        hi = lo * 10.0
    edges = np.logspace(np.log10(lo), np.log10(hi), bins + 1)
    h_ref = compute_lum_histogram(lr, edges, black_eps)
    h_cand = compute_lum_histogram(lc, edges, black_eps)

    sr = h_ref.std()
    sc = h_cand.std()
    if sr < 1.0e-12 and sc < 1.0e-12:
        return 1.0 if np.array_equal(h_ref, h_cand) else 0.0
    if sr < 1.0e-12 or sc < 1.0e-12:
        return 0.0
    if np.array_equal(h_ref, h_cand):
        # Identical histograms are perfectly correlated by definition. Short-circuit
        # so that self-comparison returns EXACTLY 1.0: np.corrcoef's normalisation
        # otherwise leaves 1-2 ULP of residue (observed -2.22e-16), which would make
        # the "comparing an EXR against itself scores 1.0" contract only true to
        # within floating-point slop.
        return 1.0
    return float(np.corrcoef(h_ref, h_cand)[0, 1])


def compute_black_fractions(ref_rgb: np.ndarray, cand_rgb: np.ndarray,
                            mask: np.ndarray | None = None,
                            black_eps: float = BLACK_LUMINANCE_EPS) -> tuple[float, float]:
    """Fraction of black / near-zero luminance pixels in (reference, candidate).

    Reported as a diagnostic so a renderer that blacks out pixels the reference
    lights (or vice versa) is visible as a raw number, independent of how much
    that asymmetry happens to move the scale-invariant Pearson correlation.
    """
    lr = luminance_rec2020(ref_rgb)
    lc = luminance_rec2020(cand_rgb)
    if mask is not None:
        lr = lr[mask]
        lc = lc[mask]
    n = lr.size
    if n == 0:
        return 0.0, 0.0
    return float(np.sum(lr <= black_eps) / n), float(np.sum(lc <= black_eps) / n)



def compute_metrics(ref: np.ndarray, cand: np.ndarray,
                    channel: str = "luminance",
                    mask: np.ndarray | None = None,
                    rel_mse_eps: float = REL_MSE_EPS,
                    hist_bins: int = 256,
                    allow_small_ssim: bool = False) -> dict:
    # ref/cand are full HxWx3 RGB (scene-referred linear). Channel metrics are
    # computed on the requested channel; Rel-MSE shares that channel, while SSIM
    # and the luminance-histogram correlation need the full RGB pair.
    ref_ch = extract_channel(ref, channel)
    cand_ch = extract_channel(cand, channel)

    # Optional spatial mask (boolean HxW). When None, all pixels are used.
    if mask is None:
        mask_ch = np.ones(ref_ch.shape, dtype=bool)
    else:
        mask_ch = mask

    diff = np.abs(ref_ch - cand_ch)
    signed = cand_ch - ref_ch
    sel_ref = ref_ch[mask_ch]
    sel_diff = diff[mask_ch]
    sel_signed = signed[mask_ch]
    # Scale to [0, 255] range via the "255" convention: treat 1.0 HDR = 255 units.
    # This makes thresholds intuitive (4.0 = 4/255 ≈ 1.6% of SDR white).
    diff255 = sel_diff * 255.0
    mean_d = float(np.mean(diff255))
    max_d  = float(np.max(diff255))
    p99_d = float(np.percentile(diff255, 99.0))
    p999_d = float(np.percentile(diff255, 99.9))
    mse    = float(np.mean(sel_diff ** 2))
    mean_ref = float(np.mean(np.abs(sel_ref)))
    rel_mean_pct = float((np.mean(sel_diff) / max(mean_ref, 1.0e-8)) * 100.0)
    # PSNR relative to peak=1.0 (HDR-scene-referred white)
    psnr   = float("inf") if mse == 0.0 else float(10.0 * np.log10(1.0 / mse))

    # Signed stats (cand - ref), scaled to 1/255 units. Positive = candidate brighter.
    signed255 = sel_signed * 255.0
    signed_mean = float(np.mean(signed255))
    signed_p95 = float(np.percentile(signed255, 95.0))
    signed_p05 = float(np.percentile(signed255, 5.0))
    n_pix = int(mask_ch.sum())

    # Declared §10 metrics (Rel-MSE on the selected channel; SSIM + histogram on
    # the full RGB pair, with the spatial mask applied inside each helper).
    rel_mse = compute_rel_mse(ref_ch, cand_ch, mask=mask_ch, eps=rel_mse_eps)
    ssim = compute_ssim(ref, cand, mask=mask if mask is not None else None,
                        allow_small=allow_small_ssim)
    lum_hist_corr = compute_lum_histogram_correlation(ref, cand,
                                                      mask=mask if mask is not None else None,
                                                      bins=hist_bins)
    black_ref, black_cand = compute_black_fractions(ref, cand,
                                                    mask=mask if mask is not None else None)

    return {
        "mean_diff": mean_d,
        "max_diff": max_d,
        "p99_diff": p99_d,
        "p999_diff": p999_d,
        "psnr": psnr,
        "mse": mse,
        "rel_mean_pct": rel_mean_pct,
        "rel_mse": rel_mse,
        "ssim": ssim,
        "lum_hist_corr": lum_hist_corr,
        "black_frac_ref": black_ref,
        "black_frac_cand": black_cand,
        "signed_mean": signed_mean,
        "signed_p95": signed_p95,
        "signed_p05": signed_p05,
        "n_pix": n_pix,
    }


def evaluate_gate(metrics: dict, args: argparse.Namespace) -> tuple[bool, list[str]]:
    mean_ok = metrics["mean_diff"] <= args.threshold
    rel_mse_ok = metrics["rel_mse"] <= args.rel_mse_threshold
    ssim_ok = metrics["ssim"] >= args.ssim_threshold
    hist_ok = metrics["lum_hist_corr"] >= args.lum_hist_threshold
    psnr_ok = (args.psnr_threshold is not None) and (metrics["psnr"] >= args.psnr_threshold)
    relative_ok = (args.relative_threshold is not None) and (metrics["rel_mean_pct"] <= args.relative_threshold)

    if args.gate == "all":
        # Strict AND of all declared §10 metrics (SPRINT-PLAN §10: "all of them,
        # never one alone"). PSNR / rel_mean_% are ANDed in only when explicitly
        # requested via --psnr-threshold / --relative-threshold (opt-in extras).
        checks = [
            mean_ok,
            rel_mse_ok,
            ssim_ok,
            hist_ok,
        ]
        labels = [
            f"mean_diff <= {args.threshold}",
            f"rel_mse <= {args.rel_mse_threshold}",
            f"SSIM >= {args.ssim_threshold}",
            f"lum_hist_corr >= {args.lum_hist_threshold}",
        ]
        if args.psnr_threshold is not None:
            checks.append(psnr_ok)
            labels.append(f"PSNR >= {args.psnr_threshold} dB")
        if args.relative_threshold is not None:
            checks.append(relative_ok)
            labels.append(f"rel_mean_% <= {args.relative_threshold}%")
        return all(checks), labels
    if args.gate == "mean":
        return mean_ok, [f"mean_diff <= {args.threshold}"]
    if args.gate == "psnr":
        if args.psnr_threshold is None:
            sys.exit("ERROR: --gate psnr requires --psnr-threshold")
        return psnr_ok, [f"PSNR >= {args.psnr_threshold} dB"]
    if args.gate == "relative":
        if args.relative_threshold is None:
            sys.exit("ERROR: --gate relative requires --relative-threshold")
        return relative_ok, [f"rel_mean_% <= {args.relative_threshold}%"]
    if args.gate == "any":
        available = [("mean", mean_ok)]
        labels = [f"mean_diff <= {args.threshold}"]
        if args.psnr_threshold is not None:
            available.append(("psnr", psnr_ok))
            labels.append(f"PSNR >= {args.psnr_threshold} dB")
        if args.relative_threshold is not None:
            available.append(("relative", relative_ok))
            labels.append(f"rel_mean_% <= {args.relative_threshold}%")
        return any(v for _, v in available), labels
    if args.gate == "scale-aware":
        # HDR/transmissive scenes can violate a fixed absolute 1/255 threshold even
        # when the relative error is small; accept either strict absolute pass OR
        # relative+PSNR pass. Sanctioned by Theia/README.md:253 for HDR fixtures.
        # Rel-MSE / SSIM / lum-hist are reported but NOT required here.
        psnr_threshold = args.psnr_threshold if args.psnr_threshold is not None else 20.0
        relative_threshold = args.relative_threshold if args.relative_threshold is not None else 2.5
        relative_psnr_pass = (metrics["rel_mean_pct"] <= relative_threshold) and (metrics["psnr"] >= psnr_threshold)
        labels = [
            f"mean_diff <= {args.threshold}",
            f"(rel_mean_% <= {relative_threshold}% and PSNR >= {psnr_threshold} dB)",
        ]
        return mean_ok or relative_psnr_pass, labels

    sys.exit(f"ERROR: unknown gate mode '{args.gate}'")


# ── Heatmap ───────────────────────────────────────────────────────────────────

def save_heatmap(diff_channel: np.ndarray, out_path: Path) -> None:
    """Write a false-color heatmap PNG.  Blue=0, Green=small, Red=large diff."""
    try:
        import imageio.v3 as iio
    except ImportError:
        sys.exit("ERROR: imageio not found.  Run: pip install imageio")

    # Normalise diff to [0, 1] using 95th percentile to avoid outlier saturation.
    p95 = float(np.percentile(diff_channel, 95))
    if p95 < 1e-9:
        p95 = 1.0
    normed = np.clip(diff_channel / p95, 0.0, 1.0)

    # Jet-like colormap: blue → cyan → green → yellow → red
    r = np.clip(1.5 - abs(normed * 4 - 3),   0.0, 1.0)
    g = np.clip(1.5 - abs(normed * 4 - 2),   0.0, 1.0)
    b = np.clip(1.5 - abs(normed * 4 - 1),   0.0, 1.0)
    heatmap = (np.stack([r, g, b], axis=-1) * 255).astype(np.uint8)

    iio.imwrite(str(out_path), heatmap)
    print(f"  heatmap -> {out_path}")


def save_signed_heatmap(signed_channel: np.ndarray, out_path: Path) -> None:
    """Diverging heatmap of SIGNED (cand - ref) diff.

    Blue = candidate DARKER, white ≈ equal, red = candidate BRIGHTER.
    Symmetric around 0, scaled by the 95th percentile of |signed|.
    """
    try:
        import imageio.v3 as iio
    except ImportError:
        sys.exit("ERROR: imageio not found.  Run: pip install imageio")

    p95 = float(np.percentile(np.abs(signed_channel), 95))
    if p95 < 1e-9:
        p95 = 1.0
    n = np.clip(signed_channel / p95, -1.0, 1.0)
    # Blue (negative) -> white (0) -> red (positive).
    pos = np.clip(n, 0.0, 1.0)          # 0..1 red intensity
    neg = np.clip(-n, 0.0, 1.0)         # 0..1 blue intensity
    r = (1.0 - neg) * (1.0 - pos) + pos          # white->red as pos grows
    g = (1.0 - neg) * (1.0 - pos)                # drops to 0 at either extreme
    b = (1.0 - pos) * (1.0 - neg) + neg          # white->blue as neg grows
    heatmap = (np.stack([r, g, b], axis=-1) * 255).astype(np.uint8)
    iio.imwrite(str(out_path), heatmap)
    print(f"  signed heatmap -> {out_path}  (blue=cand darker, red=cand brighter)")


# ── Main ──────────────────────────────────────────────────────────────────────

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("reference",  type=Path, help="Hyperion EXR (ground truth)")
    parser.add_argument("candidate",  type=Path, help="Theia EXR (under test)")
    parser.add_argument("--threshold", type=float, default=4.0,
                        help="Pass/fail mean-diff threshold in 1/255 units (default: 4.0)")
    parser.add_argument("--rel-mse-threshold", type=float, default=0.01,
                        help="Pass/fail Rel-MSE threshold (default: 0.01 ~= 10-15%% RMS relative error)")
    parser.add_argument("--ssim-threshold", type=float, default=0.98,
                        help="Pass/fail SSIM threshold (default: 0.98, tone-mapped sRGB)")
    parser.add_argument("--lum-hist-threshold", type=float, default=0.999,
                        help="Pass/fail luminance-histogram Pearson correlation (default: 0.999)")
    parser.add_argument("--psnr-threshold", type=float, default=None,
                        help="Optional PSNR threshold in dB (ANDed into the 'all' gate when given)")
    parser.add_argument("--relative-threshold", type=float, default=None,
                        help="Optional relative mean-error threshold in percent (ANDed into 'all' when given)")
    parser.add_argument("--rel-mse-eps", type=float, default=REL_MSE_EPS,
                        help=f"Epsilon for the Rel-MSE denominator ref^2+eps "
                             f"(default: {REL_MSE_EPS:g}; sqrt(eps) is the effective black point)")
    parser.add_argument("--hist-bins", type=int, default=256,
                        help="Luminance-histogram log-spaced bin count for the positive range, "
                             "plus one explicit black bin (default: 256)")
    parser.add_argument("--allow-small-ssim", action="store_true",
                        help="Permit the degraded GLOBAL non-windowed SSIM when the compared region "
                             "is smaller than the 11x11 window (otherwise this is a hard error). "
                             "The fallback is a DIFFERENT metric, not comparable to --ssim-threshold.")
    parser.add_argument("--gate", choices=("all", "mean", "psnr", "relative", "any", "scale-aware"), default="all",
                        help="Gate mode for RESULT (default: all = strict AND of the declared §10 metrics; "
                             "scale-aware is the explicit opt-in for HDR transmissive fixtures)")
    parser.add_argument("--heatmap",  type=Path, default=None,
                        help="Output heatmap PNG path (default: <candidate>_diff.png)")
    parser.add_argument("--channel",  default="luminance",
                        help="Channel to compare: r, g, b, luminance (default: luminance)")
    parser.add_argument("--no-heatmap", action="store_true", help="Skip heatmap output")
    parser.add_argument("--signed", action="store_true",
                        help="Report SIGNED (cand-ref) mean per channel + luminance and write a diverging heatmap")
    parser.add_argument("--signed-heatmap", type=Path, default=None,
                        help="Diverging heatmap path (default: <candidate>_signed.png with --signed)")
    parser.add_argument("--mask-box", default=None,
                        help="Restrict metrics to a normalized [0,1] box X0,Y0,X1,Y1 (e.g. 0.7,0.3,1.0,0.7)")
    args = parser.parse_args()

    if not args.reference.exists():
        sys.exit(f"ERROR: reference file not found: {args.reference}")
    if not args.candidate.exists():
        sys.exit(f"ERROR: candidate file not found: {args.candidate}")

    print(f"Reference : {args.reference}")
    print(f"Candidate : {args.candidate}")
    print(f"Channel   : {args.channel}")

    ref  = load_exr(args.reference)
    cand = load_exr(args.candidate)

    if ref.shape != cand.shape:
        sys.exit(f"ERROR: shape mismatch — reference {ref.shape} vs candidate {cand.shape}")

    # Optional normalized spatial mask.
    mask2d = None
    if args.mask_box is not None:
        try:
            x0, y0, x1, y1 = [float(v) for v in args.mask_box.split(",")]
        except ValueError:
            sys.exit("ERROR: --mask-box must be X0,Y0,X1,Y1 in [0,1]")
        H, W = ref.shape[:2]
        ix0 = max(0, min(int(x0 * W), W - 1)); ix1 = max(ix0 + 1, min(int(x1 * W), W))
        iy0 = max(0, min(int(y0 * H), H - 1)); iy1 = max(iy0 + 1, min(int(y1 * H), H))
        mask2d = np.zeros((H, W), dtype=bool)
        mask2d[iy0:iy1, ix0:ix1] = True
        print(f"Mask-box  : [{iy0}:{iy1}, {ix0}:{ix1}]  ({(iy1-iy0)*(ix1-ix0)} px)")

    ref_ch  = extract_channel(ref,  args.channel)
    cand_ch = extract_channel(cand, args.channel)

    # Per-channel signed means (cand - ref), scaled to 1/255. Positive = candidate brighter.
    if args.signed:
        for name, idx in (("R", 0), ("G", 1), ("B", 2)):
            m = mask2d if mask2d is not None else np.ones(ref.shape[:2], dtype=bool)
            signed_mean_ch = float(np.mean(cand[..., idx][m] - ref[..., idx][m]) * 255.0)
            print(f"  signed {name} (cand-ref, 1/255): {signed_mean_ch:+8.4f}")

    metrics = compute_metrics(ref, cand, channel=args.channel, mask=mask2d,
                              rel_mse_eps=args.rel_mse_eps, hist_bins=args.hist_bins,
                              allow_small_ssim=args.allow_small_ssim)

    passed, gate_labels = evaluate_gate(metrics, args)

    print()
    print(f"  mean_diff    : {metrics['mean_diff']:7.3f}  (threshold {args.threshold:.1f})")
    print(f"  max_diff     : {metrics['max_diff']:7.3f}")
    print(f"  p99_diff     : {metrics['p99_diff']:7.3f}")
    print(f"  p99.9_diff   : {metrics['p999_diff']:7.3f}")
    print(f"  PSNR         : {metrics['psnr']:7.2f} dB")
    print(f"  MSE          : {metrics['mse']:.6f}")
    print(f"  rel_mean%    : {metrics['rel_mean_pct']:7.3f} %")
    print(f"  Rel-MSE      : {metrics['rel_mse']:.6f}  (threshold {args.rel_mse_threshold}, "
          f"(ref-cand)^2/(ref^2+{args.rel_mse_eps:g}))")
    print(f"  SSIM         : {metrics['ssim']:.6f}  (threshold {args.ssim_threshold}, ACES+sRGB)")
    print(f"  lum_hist_corr: {metrics['lum_hist_corr']:.6f}  (threshold {args.lum_hist_threshold}, "
          f"{args.hist_bins} log-bins + 1 black bin)")
    print(f"  black_frac   : ref {metrics['black_frac_ref'] * 100:6.2f}%  "
          f"cand {metrics['black_frac_cand'] * 100:6.2f}%  "
          f"(delta {(metrics['black_frac_cand'] - metrics['black_frac_ref']) * 100:+.2f} pp, "
          f"{round((metrics['black_frac_cand'] - metrics['black_frac_ref']) * metrics['n_pix']):+d} px; "
          f"diagnostic, not gated)")
    if args.signed or mask2d is not None:
        print(f"  signed       : mean {metrics['signed_mean']:+7.4f}  "
              f"[p5 {metrics['signed_p05']:+7.3f}, p95 {metrics['signed_p95']:+7.3f}]  (1/255, cand-ref)")
        print(f"  pixels       : {metrics['n_pix']}")
    print(f"  gate         : {args.gate} ({'; '.join(gate_labels)})")
    print()
    print(f"  RESULT       : {'PASS' if passed else 'FAIL'}")

    if not args.no_heatmap:
        heatmap_path = args.heatmap or args.candidate.with_name(
            args.candidate.stem + "_diff.png")
        diff_ch = np.abs(ref_ch - cand_ch)
        if mask2d is not None:
            diff_ch = diff_ch * mask2d
        save_heatmap(diff_ch, heatmap_path)

    if args.signed:
        signed_path = args.signed_heatmap or args.candidate.with_name(
            args.candidate.stem + "_signed.png")
        signed_ch = (cand_ch - ref_ch) * 255.0
        if mask2d is not None:
            signed_ch = signed_ch * mask2d
        save_signed_heatmap(signed_ch, signed_path)

    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
