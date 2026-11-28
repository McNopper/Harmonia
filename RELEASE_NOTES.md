# Release notes

Notes exist for v1.0.0, v0.12.0 and v0.9.0. The complete list of releases is in
[PLAN.md](PLAN.md), section 9.

## Harmonia v1.0.0-pre-alpha (2026-11-28): prerelease

Pre-alpha development snapshot and GitHub backup. Adds ACEScg working spaces, full ACES 1.0 SDR
Video, chromaticity-aware EXR comparison, selected-view CPU PNG capture, and scene-wide exposure
compensation. OpenPBR corrections cover exact dielectric/coat Fresnel, consistent smooth/rough/
medium/SMS radiance transport, rough-TIR sampling, and bulk-SSS albedo/reflection ownership.

Verification: **327 tests passed, one optional data-dependent DN3-M4 receipt skipped, zero failures**;
Aether, Hyperion, and Theia family gates also pass. The release remains a prerelease: detailed
reference-camera, emission-unit, and remaining material-validation work continues after the snapshot.

## Harmonia v1.0.0 (2026-11-26): major stable release

Stable public baseline of the shared Vulkan library. It is released together with Aether
v1.0.0, slang-math v1.0.0, Theia v1.0.0 and Hyperion v1.0.0.

### Review

- Code and documentation were reviewed across the family.
- Deterministic analyzer findings were removed (signed and unsigned mixing, loops,
  lambdas, containers and `min`).
- Aether is declared as an external include boundary where it is consumed.
- Strict documentation-reference checks were adopted. Historical records are excluded
  from the path assertions of a clean checkout.
- The architecture contracts and the dead-code checks are clean.

### Verification

| Check | Result |
|-------|--------|
| Test suites | slang-math 95/95, Aether 168/168, Harmonia 193/193, Theia 67/67, Hyperion 79/79 |
| `verify-full` | passes in all five repositories |
| `check_arch` | clean in all five repositories |
| `check_dead` | clean in all five repositories (family mode) |
| Documentation reference check, strict mode | clean in all five repositories |

### Compatibility

- The behavior of the public headers and sources is unchanged.
- The dependency pins resolve to the coordinated v1.0.0 family tags.

### Release state

- Release-preparation commits are local. Push and the v1.0.0 tag wait for explicit
  authorization.

## Harmonia v0.12.0 (2026-11-24): minor release

Release of the shared scene-camera assembly. It is released together with Aether v0.12.0,
slang-math v0.12.0, Theia v0.12.0 and Hyperion v0.12.0.

### Changes

- `harmonia::renderer::CameraAssembly` owns the interpretation of the scene camera read
  by the loader and the placement of a camera bound to an animated node. Position,
  target, up, vertical field of view, EV100 and the scene-scale near and far planes now
  have one definition that both renderers use.
- Theia and Hyperion keep their own camera controllers, jitter, the rules that reset
  accumulation, and presentation.
- Documentation and roadmap are aligned with the camera as released and the release
  state.

### Verification

- Shared camera assembly: 7/7 tests.
- Harmonia 191/191, Theia 67/67, Hyperion 79/79.
- Architecture and format checks are clean in all three consuming repositories.
- Direct capture identity on `cornell_classic`: `mean_diff` 0, `rel_mse` 0, SSIM 1.0,
  PSNR infinite.
- Parity: 14/14 pass. Vulkan validation over the complete galleries: 60/60 clean.

### Gallery

- Hyperion: 30/30 images byte-identical to the tracked README gallery.
- Theia: 23/30 byte-identical; seven images differ at speckle level and were approved
  after a visual review.

## Harmonia v0.9.0 (2026-11-16): minor release

Release of the animation and deformation work and of the recalibrated parity limits. It
is released together with Aether v0.9.0, Theia v0.9.0 and Hyperion v0.9.0.

### Deformation

- **Skinning.** The `[[skin]]` schema and its evaluation are in Aether. Harmonia adds
  the pose and refit API of `SceneBase`, the BLAS refit machinery and the shared
  `DeformPass` (`deform.comp.slang`), which applies rest pose, morph targets and
  skinning in one dispatch per sub-mesh and per animation tick, in both renderers. The
  CPU references `deformSkinLbs` and `deformSkinDqs` are the specification; the GPU
  output matches them within 1e-4.
- **Dual quaternion skinning.** `blend = "lbs"` or `"dqs"` on `[[skin]]` (default LBS).
  DQS blends dual quaternions (Kavan 2008) through the shared `skinDualQuaternions`
  extraction, which the CPU staging and the CPU reference share. The contract is rigid:
  scale and shear are dropped. Sign alignment rescues blends of more than 180 degrees; a
  test covers the candy-wrapper collapse (a blend of ±80 degrees keeps the length where
  LBS collapses to cos 80°).
- **Morph targets.** The `[[morph]]` schema, `morphs/<mesh>/<name>` weight tracks,
  `evaluateMorphWeights` and the `deformMorphs` CPU reference (position only; normals
  are not re-derived).

### Parity limits recalibrated

- The old limits were measured with firefly clamps and could not be reached by pure
  captures (the comparison failed 0/14 on fresh renders). The new limits lie between the clean
  envelope of the 14 scenes at 256 samples against 256 frames (mean ≤ 98.8,
  Rel-MSE ≤ 174.9, SSIM ≥ 0.706, histogram ≥ 0.869) and the completely different anchors
  (different scenes: mean ≥ 240, SSIM ≤ 0.195; inverted; twice the exposure: 133.9):
  `mean_diff` ≤ 120, `rel_mse` ≤ 250, SSIM ≥ 0.6, histogram ≥ 0.8. All limits must still
  hold together.
- The limits belong to this budget; the method is in `tools/validation_manifest.toml` and
  in `PLAN.md`, section 7.
- Known limitation: the metrics are based on luminance.
- The manifest no longer routes scenes through the relaxed scale-aware mode (the HDR
  scenes pass the plain comparison); `--gate scale-aware` remains for ad-hoc use.
- The sequence comparison uses the same limits as the still comparison (worst frame
  SSIM 0.886).

### Roadmap

- Dropped as not required, with the reasons in `PLAN.md`: a reservoir-based subsurface
  layer, a glint estimator, NRD, MIS sample-allocation optimization, an alternative
  low-discrepancy sequence, shader module identifier caching.
- The anisotropic micrograin model was already released (v0.7.9).
- Removed earlier in the same work, with the reasons in `PLAN.md`: path guiding, software
  ray reordering, the wavefront split, multi-node ray forwarding, control variates,
  world-space light grids, the importance-driven acceleration-structure extension,
  learned metrics and neural denoising.
- Kept: the OpenPBR 1.2 switch-over and the watchlists with revisit triggers.

### Verification

- `ctest` 180/180; the tool suites pass; ruff and the dead-code check are clean.
- Fresh comparison over the manifest: 14 passed, 0 failed, no validation message. Negative controls:
  an identical image passes; an inverted image and an image with twice the exposure
  fail.
- Deformation: GPU equals CPU at two times (skin, morph, DQS, composition); repeated
  runs are idempotent; sequence renders in both renderers produce no validation
  message.

### Upgrade notes

- Pins: Aether v0.9.0, slang-math v0.8.0.
- Downstream comparisons use the new limits. The manifest is authoritative;
  `compare_renders.py` mirrors it in its defaults and `compare_sequences.py` mirrors
  `compare_renders.py`.
- `DeformPass` (then named `SkinDeformPass`) also stages morph weights and dual
  quaternions. Scenes with `[[skin]]` or `[[morph]]` attach their source and deform per
  tick.

### Added in the same release

- **Camera bound to a node and frame seek** (commit c07a78b). `SceneConfig::cameraNodeId`;
  the shared `--frame N` (`AppConfig::animationStartFrame`, added as an offset by
  `App::animationFrameIndex()`); `Camera::setUp` and `Camera::setNearFar`.
- **Time-based capture and video tooling.** `--capture-every-ms` and `--start-seconds`;
  one animation clock (`App::animationClock`) derives the seconds and the change key
  for every consumer (scene, deformation, TLAS, camera); `tools/frames_to_video.py`.
  Checks: argument parsing 15/15, suite 184/184, tick-grid renders identical before and
  after the clock was unified, interval capture equal to the tick grid (0.000 / 1.000),
  10 frames every 100 ms give a 1.000 s video.
