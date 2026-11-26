# Papers

Links to the papers and specifications behind the techniques of this library. PDFs are
not redistributed: download them from the official sources and keep them locally (the
folder ignores PDF files).

## Goals that decide a conflict

- **Hyperion** stands for the best OpenPBR quality and is the ground truth. OpenPBR
  formulas and parameters are mandatory and never diverge from MaterialX.
- **Theia** trades quality for real-time frame time. Sampling and efficiency techniques
  are free to change; a change of the BSDF model must match Hyperion exactly.

A technique that supersedes an older one replaces it; the old code is deleted. Neural
and learned techniques are not used anywhere in the family.

## Adopted

The table records what each technique is used for. Whether a renderer enables it is
described in [README.md](../README.md) and [PLAN.md](../PLAN.md).

| Technique | Paper | Venue | Official link | Used in | Scope |
|-----------|-------|-------|---------------|----------|-------|
| OpenPBR Surface | OpenPBR Surface Specification 1.1.2, with MaterialX as reference implementation | ASWF | [Specification](https://academysoftwarefoundation.github.io/OpenPBR/) · [MaterialX](https://github.com/AcademySoftwareFoundation/MaterialX) | `bsdf_shared.slang`, `bsdf.slang` | The BSDF shared by both renderers |
| Bounded visible-normal sampling for Smith–GGX | Eto & Tokuyoshi — *Bounded VNDF Sampling for Smith–GGX Reflections* | SIGGRAPH Asia 2023 | [doi:10.1145/3610543.3626163](https://doi.org/10.1145/3610543.3626163) · [GPUOpen PDF](https://gpuopen.com/download/Bounded_VNDF_Sampling_for_Smith-GGX_Reflections.pdf) | `math.slang` | Sampling only: same BSDF, faster convergence |
| Anisotropic micrograin distribution | Lucas, Ribardière & Pacanowski — *A Reflectance Model for Anisotropic Micrograin BSDF* | TOG 2024 | [doi:10.1145/3658224](https://doi.org/10.1145/3658224) · [HAL](https://hal.science/hal-04567402) | `math.slang` | The OpenPBR flake lobe |
| Owen-scrambled Sobol' sampler | Burley — *Practical Hash-Based Owen Scrambling* | JCGT 2020 | [JCGT](https://jcgt.org/published/0009/04/01/) | `rng.slang` | Sampling only |
| Specular manifold sampling | Zeltner, Georgiev & Jakob — *Specular Manifold Sampling for Rendering High-Frequency Caustics and Glints* | SIGGRAPH 2020 | [doi:10.1145/3386569.3392408](https://doi.org/10.1145/3386569.3392408) · [EPFL](https://rgl.epfl.ch/publications/Zeltner2020Specular) | `sms.slang`, `sms_geometry.slang`, `sms_solver.slang` | Sampling only |
| Trust regions and priors for specular manifold sampling, with spatiotemporal resampling | Hong et al. — *Sample Space Partitioning and Spatiotemporal Resampling for Specular Manifold Sampling* | SIGGRAPH Asia 2025 | [doi:10.1145/3757377.3763927](https://doi.org/10.1145/3757377.3763927) · [NVIDIA](https://research.nvidia.com/labs/rtr/publication/hong2025partition/) | `sms_partition.slang`, `sms_prior.slang` | Sampling only; the reuse is Theia's |
| ReSTIR path tracing, enhanced (builds on Bitterli et al. 2020 and on the generalized resampled importance sampling of Lin et al. 2022) | Lin, Kettunen & Wyman — *ReSTIR PT Enhanced* | I3D 2026 | [doi:10.1145/3804494](https://doi.org/10.1145/3804494) · [NVIDIA](https://research.nvidia.com/labs/rtr/publication/lin2026restirptenhanced/) | Capture hooks in `path_integrator.slang` | Theia's path reservoir only |
| Converging, algorithm-agnostic denoiser blend | Denisova, Manetti, Bocchi & Iadanza — *Converging Algorithm-Agnostic Denoising for Monte Carlo Rendering* | I3D 2024 | [doi:10.1145/3675384](https://doi.org/10.1145/3675384) | Denoiser pass (`convergingMode`) | Presentation only; no effect in a capture; not enabled by the applications |

## Best-practice sources

| Source | Date | Link | Used for |
|--------|------|------|----------|
| NVIDIA — *Real-Time Path Tracing in Games* (RTXPT performance whitepaper) | June 2026 | [Repository](https://github.com/NVIDIA-RTX/Whitepapers/tree/main/pt_performance_whitepaper) · [PDF](https://github.com/NVIDIA-RTX/Whitepapers/blob/main/pt_performance_whitepaper/pt_performance_whitepaper_en.pdf) | Source of several evaluations ([PLAN.md](../PLAN.md), section 5): material level of detail by ray class and the world-space radiance cache were adopted in Theia; world-space light grids and the NRD denoiser were not |
| NVIDIA Vulkan ray tracing tutorial (nvpro samples) | version 2.0 | [Repository](https://github.com/nvpro-samples/vk_raytracing_tutorial) | Reference for the ray-tracing pipeline versus ray queries, the shader-binding-table layout, and the build and refit of acceleration structures with opacity micromaps |
| *Spatiotemporal reservoir resampling* | online encyclopedia | [Article](https://en.wikipedia.org/wiki/Spatiotemporal_reservoir_resampling) | Introduction to the concepts of the ReSTIR papers: weighted reservoir sampling, confidence weights and shift mappings |

## Evaluated and not adopted

| Technique | Venue | Official link | Reason |
|-----------|-------|---------------|--------|
| ReSTIR subsurface scattering (Werner et al.) | HPG 2024 | [doi:10.1145/3675372](https://doi.org/10.1145/3675372) · [PDF](https://cg.ivd.kit.edu/publications/2024/restir-sss/restir-sss.pdf) | No measured subsurface defect ([PLAN.md](../PLAN.md), section 4) |
| Megakernel versus wavefront (Padilla et al.) | 2026 | [arXiv:2605.27323](https://arxiv.org/abs/2605.27323) | The wavefront split measured about twice the GI cost ([PLAN.md](../PLAN.md), section 5.1) |
| Ray reordering (Meister et al.) | 2025 | [arXiv:2506.11273](https://arxiv.org/abs/2506.11273) | At most about 6 percent gain at interactive ray counts ([PLAN.md](../PLAN.md), section 5.1) |
| Joint denoising and upscaling with a multi-branch, multi-scale feature network (Kazmierczyk et al.) | I3D 2025 | [GPUOpen article](https://gpuopen.com/learn/neural_supersampling_and_denoising_for_real-time_path_tracing) · [Program](https://i3dsymposium.org/2025/papers.html) | Neural; no weights or code released |
| AMD FSR "Redstone" SDK (Ray Regeneration ML denoiser) | 2025–2026 | [FSR SDK](https://gpuopen.com/amd-fsr-sdk) · [GitHub](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) | Neural, limited to RDNA4 hardware and DX12, with binaries and limited source |

## Reference implementations

| Technique | Repository | Framework |
|-----------|-----------|----------|
| Specular manifold sampling | [tizian/specular-manifold-sampling](https://github.com/tizian/specular-manifold-sampling) | Mitsuba 2 |
| Partitioned SMS with resampling | [Utah-Graphics-Lab/PSMS-ReSTIR](https://github.com/Utah-Graphics-Lab/PSMS-ReSTIR) | Falcor 8.0 |
| ReSTIR subsurface scattering | [MircoWerner/ReSTIR-SSS](https://github.com/MircoWerner/ReSTIR-SSS) | Custom Vulkan |
| ReSTIR path tracing | [DQLin/ReSTIR_PT](https://github.com/DQLin/ReSTIR_PT) | Falcor |
