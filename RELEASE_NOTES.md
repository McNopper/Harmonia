# Harmonia v0.8.0 (2026-11-16) - minor release

The shared Vulkan pipeline library release for the *animation + deformation* wave and the
section-7 parity-gate resolution. Coordinates with **Aether v0.9.0, Theia v0.9.0, Hyperion
v0.8.0** (each repo carries its own RELEASE_NOTES.md).

## Shipped

### Deformation (ANI4 + ANI5, end-to-end)
- **ANI4 skinning**: `[[skin]]` schema + evaluation in Aether, `SceneBase` pose/refit API,
  BLAS `ALLOW_UPDATE` refit machinery, and the renderer halves - one shared `DeformPass`
  (`deform.comp.slang`) doing **rest -> morph -> skin** per sub-mesh dispatch per tick, in BOTH
  Theia and Hyperion. CPU references are the spec: `deformSkinLbs` and (new)
  `deformSkinDqs` - GPU output matches within 1e-4.
- **ANI4-DQS**: `blend = "lbs" | "dqs"` on `[[skin]]` (default LBS). DQS = dual-quaternion
  blending (Kavan 2008) via the SHARED `skinDualQuaternions` extraction (CPU staging = CPU
  reference = one code path). Contract: rigid - **scale/shear is dropped** (column-normalized
  projection). Sign alignment rescues >180 degree joint blends; candy-wrapper collapse proven
  fixed by test (+-80 degree blend preserves length where LBS collapses to cos80).
- **ANI5 morph targets**: `[[morph]]` schema, `morphs/<mesh>/<name>` weight tracks,
  `evaluateMorphWeights`, `deformMorphs` CPU reference (POSITION-only; normals untouched).

### Section 7 parity gate - RESOLVED (owner decision)
- Thresholds were clamp-era and unreachable under estimator-pure capture (the gate sat at
  0/14 on fresh renders). Recalibrated BETWEEN the measured clean envelope (14 scenes @256/256:
  mean<=98.8, rel<=174.9, SSIM>=0.706, hist>=0.869) and the measured *completely different*
  anchors (cross-scene >=240 mean / <=0.195 SSIM; inverted; 2x-exposure 133.9):
  **mean_diff<=120, rel_mse<=250, SSIM>=0.6, hist>=0.8** - strict-AND unchanged, numbers moved.
- **Budget-specific**: re-measure and recalibrate if spp/frames/resolution change (method is in
  `tools/validation_manifest.toml` + PLAN section 7).
- Known limitation: metrics are luminance-based - a luma-preserving channel swap is invisible.
- Scale-aware routing retired from the manifest (HDR fixtures pass the plain gate); the
  `--gate scale-aware` tool option remains for ad-hoc use.
- The ANI6 sequence gate mirrors the still-gate defaults (now PASS: worst-frame SSIM 0.886).

### Roadmap hygiene (required-or-drop triage)
- Dropped as not-required (tombstones carry the evidence): C11, C4, C13, DN8, SAM1, QMC1,
  VK17. C12 status corrected (shipped v0.7.9). GI-ENH closed (recorded paper-deltas are notes).
- Kept: C15 (external OpenPBR 1.2 spec obligation) + trigger-gated watchlists.

### Removed earlier in the wave (tombstones in PLAN)
GI-PG (engagement flat/negative after the bias fix), PERF4/PERF5/PERF6/BV4, GI-CV, GI-RGIR,
PERF3-extension, PAR2/PAR3 (non-AI directive), DN1/LS1 (non-AI), VK1/VK9.

## Receipts
- `ctest`: **180/180**; `tools` suites ALL PASS; ruff clean; check_dead clean.
- Fresh full manifest gate: **14 passed / 0 failed, exit 0, 0 VUID**; negative controls
  (identical PASS / inverted FAIL / 2x-exposure FAIL).
- Deformation: GPU==CPU at two times (skin, morph, DQS, composition), idempotent re-runs,
  0-VUID e2e sequence renders in both renderers.

## Upgrade notes (for consumers)
- **Pins**: Aether **v0.9.0**, slang-math v0.8.0 (unchanged).
- Downstream gates: adopt the new threshold set (manifest is authoritative; compare_renders
  defaults mirror it; compare_sequences mirrors compare_renders).
- `SkinDeformPass` (formerly SkinDeformPass's scope) now also stages morph weights + DQs;
  scenes with `[[skin]]`/`[[morph]]` attach their source and deform per tick.

## Release mechanics
- Local commits only so far: **tag `v0.8.0` and push the commits + tag together** (the
  consumer pins reference the new tags; local builds are unaffected via
  `FETCHCONTENT_SOURCE_DIR_*` overrides).

## Also in this release (added after the notes assembly)

- **ANI8 animated camera + frame seek** (commit c07a78b): `SceneConfig::
  cameraNodeId`, shared `--frame N` (`AppConfig::animationStartFrame` ->
  `App::animationFrameIndex()` offset: sequence output frame k renders tick
  N + k, interactive starts at N), `Camera::setUp`/`setNearFar`. ANI8 PLAN
  row carries the full receipts (render parity PASS, 0 VUID everywhere).
- **Time-based capture + video tooling (2026-11-16):** `--capture-every-ms` /
  `--start-seconds` (ANI7 timing extension; ONE `App::animationClock` derives
  seconds + change key for every consumer - scene, deform, TLAS, camera) and
  `tools/frames_to_video.py` (sequence -> playable H.264 mp4, numeric frame
  ordering, `--interval-ms` pairs with the capture). Receipts: AppArgs 15/15,
  suite 184/184, tick-grid renders bit-identical pre/post unification,
  interval == tick-grid equivalence 0.000/1.000, 10x100 ms -> 1.000 s video.

