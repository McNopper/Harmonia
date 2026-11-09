# Papers

Official links for adopted and tracked techniques. **PDFs are not redistributed** —
download from the official sources below and keep locally (gitignored).

## Goals (drive conflict analysis)

- **Hyperion**: best OpenPBR quality (ground truth). **OpenPBR formulas and
  parameters are mandatory** — never diverge from MaterialX.
- **Theia**: real-time vs. quality tradeoff. Sampling/efficiency techniques
  are safe; BSDF model changes must match Hyperion exactly.

## Doctrine

Same as Vulkan modernization: **newer/better replaces old — drop, don't keep
alongside**. If a paper supersedes a prior technique, delete the old code.

## Adopted

The table records algorithm/model compatibility, not implementation completion.
**GI-SMS is shipped** (2026-11-06 coordinated wave — Harmonia v0.7.20 / Theia v0.8.2 /
Hyperion v0.7.19): SMS sampling default-on in both renderers, spatiotemporal reuse
qualified on the C1/C2/C3 track (supersedes the 2026-11-02 "not integrated" source
audit). See [How to continue](../PLAN.md#3-how-to-continue).

| Technique | Paper | Venue | Official link | Conflict check |
|-----------|-------|-------|---------------|----------------|
| C9 | Eto & Tokuyoshi — *Bounded VNDF Sampling for Smith–GGX Reflections* | SIGA 2023 TC | [doi:10.1145/3610543.3626163](https://doi.org/10.1145/3610543.3626163) · [GPUOpen PDF](https://gpuopen.com/download/Bounded_VNDF_Sampling_for_Smith-GGX_Reflections.pdf) | ✅ Sampling only |
| DN3 | Denisova & Bocchi — *Converging Algorithm-Agnostic Denoising* | I3D 2024 | [doi:10.1145/3675384](https://doi.org/10.1145/3675384) | ✅ Denoiser only |
| GI-ENH | Lin, Kettunen, Wyman — *ReSTIR PT Enhanced* | I3D 2026 (Best Paper) | [doi:10.1145/3804494](https://doi.org/10.1145/3804494) · [NVIDIA](https://research.nvidia.com/labs/rtr/publication/lin2026restirptenhanced/) | ✅ Theia reservoir only |
| SMS (base) | Zeltner, Georgiev, Jakob — *Specular Manifold Sampling* | SIGGRAPH 2020 | [doi:10.1145/3386569.3392408](https://doi.org/10.1145/3386569.3392408) · [RGL](https://rgl.epfl.ch/publications/Zeltner2020Specular) · [code](https://github.com/tizian/specular-manifold-sampling) | ✅ Sampling only |
| GI-SMS | Hong et al. — *Partitioned SMS + ReSTIR* | SIGA 2025 | [doi:10.1145/3757377.3763927](https://doi.org/10.1145/3757377.3763927) · [NVIDIA](https://research.nvidia.com/labs/rtr/publication/hong2025partition/) · [code](https://github.com/Utah-Graphics-Lab/PSMS-ReSTIR) | ✅ Sampling only |
| C11 | Werner et al. — *ReSTIR SSS* | HPG 2024 | [doi:10.1145/3675372](https://doi.org/10.1145/3675372) · [KIT PDF](https://cg.ivd.kit.edu/publications/2024/restir-sss/restir-sss.pdf) · [code](https://github.com/MircoWerner/ReSTIR-SSS) | ⚠️ SSS — verify shared BSSRDF |
| C12 | Lucas et al. — *Anisotropic Micrograin BSDF* | TOG 2024 | [doi:10.1145/3658224](https://doi.org/10.1145/3658224) · [HAL](https://hal.science/hal-04567402) | ⚠️ **OpenPBR flake lobe** — must conform |
| PERF5 | Padilla et al. — *Megakernel vs Wavefront* | 2026 | [arXiv:2605.27323](https://arxiv.org/abs/2605.27323) | ✅ Scheduling only |
| PERF4 | Meister et al. — *Ray Reordering* | 2025 | [arXiv:2506.11273](https://arxiv.org/abs/2506.11273) | ✅ Scheduling only |

## Whitepapers / best-practice sources

| Source | Venue | Official link | Feeds |
|--------|-------|---------------|-------|
| NVIDIA — *Real-Time Path Tracing in Games* (RTXPT performance whitepaper) | June 2026 | [NVIDIA-RTX/Whitepapers](https://github.com/NVIDIA-RTX/Whitepapers/tree/main/pt_performance_whitepaper) · [en PDF](https://github.com/NVIDIA-RTX/Whitepapers/blob/main/pt_performance_whitepaper/pt_performance_whitepaper_en.pdf) | PERF4/PERF5 external corroboration; PERF8 / GI-RGIR / GI-NRC / DN8 triage; VK5 probe + cache receipts (Harmonia `PLAN.md` §5.6, §5.8) |
| Kazmierczyk et al. — *Joint Denoising and Upscaling via Multi-branch and Multi-scale Feature Network* | I3D 2025 | [GPUOpen article](https://gpuopen.com/learn/neural_supersampling_and_denoising_for_real-time_path_tracing) · [I3D program](https://i3dsymposium.org/2025/papers.html) | DN1 slot landscape (2026-11-09): best input-contract fit (flat 1-spp + G-buffer guides) — **no weights/code released**, tracked paper-tier |
| AMD — FSR "Redstone" SDK (Ray Regeneration ML denoiser) | Dec 2025 – 2026 | [FSR SDK](https://gpuopen.com/amd-fsr-sdk) · [GitHub known issues](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) | DN1 slot landscape (2026-11-09): **adjudicated out** — RDNA4-only hardware, DX12-only for the denoiser (no Vulkan), binaries + limited source (vendor-proprietary tier, §8) |

## Tracked (backlog/watchlist)

| Technique | Venue | Official link | Notes |
|-----------|-------|---------------|-------|
| C11 ReSTIR SSS | HPG 2024 | [doi:10.1145/3675372](https://doi.org/10.1145/3675372) | SSS accelerator over shared BSSRDF |

## Source Code References

| Technique | Repository | Framework |
|-----------|-----------|-----------|
| SMS (base) | [github.com/tizian/specular-manifold-sampling](https://github.com/tizian/specular-manifold-sampling) | Mitsuba 2 |
| GI-SMS | [github.com/Utah-Graphics-Lab/PSMS-ReSTIR](https://github.com/Utah-Graphics-Lab/PSMS-ReSTIR) | Falcor 8.0 |
| ReSTIR SSS | [github.com/MircoWerner/ReSTIR-SSS](https://github.com/MircoWerner/ReSTIR-SSS) | Custom Vulkan |
| ReSTIR PT | [github.com/DQLin/ReSTIR_PT](https://github.com/DQLin/ReSTIR_PT) | Falcor |

## Conflict Matrix

| | Hyperion (OpenPBR quality) | Theia (real-time) |
|---|---|---|
| **C9** Bounded VNDF | ✅ Same BSDF, better sampling | ✅ Faster convergence |
| **DN3** Converging denoiser | N/A (identity in capture) | ✅ Converges to identity |
| **GI-ENH** ReSTIR PT Enhanced | N/A (path tracer) | ✅ 2-3× cost/quality |
| **GI-SMS** Specular manifold (**shipped** — Theia v0.8.2 + Hyperion v0.7.19) | ✅ Shipped caustic sampling | ✅ Shipped interactive caustics + spatiotemporal reuse |
| **C11** ReSTIR SSS | ⚠️ Shared BSSRDF — must match | ✅ Real-time SSS |
| **C12** Micrograin BSDF | ⚠️ **OpenPBR flake — must conform** | ⚠️ Same BSDF |
| **PERF5/PERF4** | ✅ Scheduling only | ✅ Scheduling only |
