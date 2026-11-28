# Tools

Scripts in this directory compare and validate renders, check the source tree, and
produce reference values for two test areas. The table says which one to reach for.

| Task | Tool | Runs a renderer |
|---|---|---|
| Compare two EXR images and get a pass/fail result | `compare_renders.py` | no |
| Compare two directories of EXR pairs over the manifest scenes | `validate_renders.py` | no |
| Render the manifest scenes with both renderers, then compare | `render_and_validate.py` | yes |
| Compare two animated sequences frame by frame | `compare_sequences.py` | no |
| Check that a renderer run produces no validation message and no application error | `check_vulkan_validation.py` | yes |
| Tell noise from bias at increasing sample counts | `check_consistency.py` | yes |
| Check that the denoiser never reaches a capture | `test_denoiser_contract.py` | yes |
| Encode a captured sequence as a video | `frames_to_video.py` | no (needs ffmpeg) |
| Check the include graph against `ARCHITECTURE.md` | `check_arch.py` | no |
| Find dead code | `check_dead.py` | no |
| Run clang-tidy over the compile database | `check_tidy.py` | no |
| Measure test coverage | `coverage.py` | runs the test suite |
| List the driver's KHR/EXT extensions and compare them with the docs | `check_vk_extensions.py` | no |
| Probe which shader-model extensions the Slang compiler emits | `check_slang_ngsm.py` | no |
| Regenerate the curved-glass reference for the SMS tests | `sms_curved_oracle.py` | no |
| Evaluate the denoiser convergence test against its acceptance limits | `dn3m4_receipt.py` | no |

`ARCHITECTURE.md` is at the repository root, the other files are in this directory.
The comparison tools are written for the pair Hyperion (reference) and Theia
(candidate) and assume the contract described under [Comparing renders](#comparing-renders).

## Requirements

- Python 3.11 or newer (the batch tools read the manifest with `tomllib`).
- `numpy` for every comparison tool; the `OpenEXR` and `Imath` packages to read EXR
  files; `imageio` when a heatmap or a crop is written.
- Built `hyperion.exe` and `theia.exe` for every tool in the "Runs a renderer" column.
- `ffmpeg` on `PATH` for `frames_to_video.py`.
- `ctest`, `cmake` and a configured build directory for `coverage.py`, `check_tidy.py`
  and the never-compiled check of `check_dead.py`.

The tools keep Vulkan validation on. A GPU is used by one job at a time: run one
render, test suite or comparison, wait for it, then start the next.

## Comparing renders

All comparison tools assume the same inputs:

- The reference is a Hyperion EXR and the candidate is a Theia EXR, both written by
  `--output` (scene-referred linear values, not tone mapped, denoiser disabled). A PNG
  is never compared.
- Both images have the same resolution and were rendered in the same working color
  space (same `[render]` preset).
- A reference rendered with few samples per pixel is noisy and inflates every
  difference. At the manifest budget the Hyperion reference uses 256 samples per pixel.

### validation_manifest.toml

The manifest is the single source of the compared scene set and its limits. It is
read by `validate_renders.py`, `render_and_validate.py` and `check_vulkan_validation.py`.

| Key | Value | Meaning |
|---|---|---|
| `reference_template`, `candidate_template` | `{scene}.exr` | File name pattern per scene |
| `threshold` | 120.0 | Largest accepted `mean_diff` (in 1/255 units) |
| `rel_mse_threshold` | 250.0 | Largest accepted Rel-MSE |
| `rel_mse_eps` | 5e-3 | Denominator floor of Rel-MSE (squared luminance) |
| `ssim_threshold` | 0.6 | Smallest accepted SSIM |
| `lum_hist_threshold` | 0.8 | Smallest accepted luminance-histogram correlation |
| `channel` | luminance | Channel compared by the scalar metrics |
| `width`, `height` | 320, 240 | Render resolution |
| `hyperion_spp` | 256 | Hyperion samples per pixel |
| `theia_frames` | 256 | Theia accumulated frames |
| `scale_aware` | not set | Scenes routed through the relaxed `scale-aware` mode |
| `[[scene]] name` | 14 entries | The compared scenes |

The 14 compared scenes are `cornell_classic`, `cornell_spheres`, `cornell_suzanne`,
`cornell_textured_cube`, `dragon_teapot`, `shaderball_metal`, `openpbr_metals`,
`openpbr_specular`, `openpbr_coat`, `shaderball_transmission`, `openpbr_dielectrics`,
`shaderball_subsurface`, `shaderball_emission` and `bunny_shaderball`.

The thresholds belong to this budget. When the resolution, the samples per pixel or
the frame count change, measure the clean envelope and the "completely different"
anchors again and recalibrate; the method and the measured values are recorded in
`PLAN.md`, section 7.

### compare_renders.py

Compares one pair of EXR images, prints every metric and decides whether the limits hold.

```text
python tools/compare_renders.py <reference.exr> <candidate.exr> [options]
```

| Option | Default | Meaning |
|---|---|---|
| `--threshold` | 120.0 | `mean_diff` limit, in 1/255 units |
| `--rel-mse-threshold` | 250.0 | Rel-MSE limit |
| `--ssim-threshold` | 0.6 | SSIM limit |
| `--lum-hist-threshold` | 0.8 | Luminance-histogram correlation limit |
| `--psnr-threshold`, `--relative-threshold` | off | Extra checks, included in the `all` mode when given |
| `--tm-mean-threshold` | off | Tone-mapped mean-diff limit, included in the `all` mode when given |
| `--tonemap {aces,agx}` | aces | Tone mapper for the tone-mapped metrics |
| `--rel-mse-eps` | 1e-2 | Rel-MSE denominator floor |
| `--hist-bins` | 256 | Luminance-histogram bins (plus one bin for black) |
| `--allow-small-ssim` | off | Permit a region smaller than the 11x11 SSIM window (a different, non-windowed statistic) |
| `--gate {all,mean,psnr,relative,any,scale-aware}` | all | Mode of the pass/fail rule |
| `--channel {r,g,b,luminance}` | luminance | Channel of the scalar metrics |
| `--heatmap PATH`, `--no-heatmap` | written next to the candidate | False-color difference image |
| `--signed`, `--signed-heatmap PATH` | off | Signed statistics and a diverging difference image |
| `--mask-box X0,Y0,X1,Y1` | whole image | Restrict all metrics to a normalized sub-rectangle |

Modes:

- `all`: every limit must hold (`mean_diff`, Rel-MSE, SSIM, histogram correlation, plus
  the extra checks that were given). This is the mode the manifest uses.
- `mean`, `psnr`, `relative`: single-metric diagnostics.
- `any`: passes when at least one of the available metrics passes (a loose diagnostic).
- `scale-aware`: passes when `mean_diff` holds, or when the relative mean error is at
  most 2.5 percent and PSNR is at least 20 dB (defaults). Use it for ad-hoc comparisons
  of very bright, transmissive content; the manifest does not route any scene through it.

Metrics:

| Metric | Computed on | Meaning |
|---|---|---|
| `mean_diff` | selected channel, linear | Mean absolute difference, scaled so that 1.0 is 255 |
| `max_diff`, `p99_diff`, `p99.9_diff` | selected channel, linear | Maximum and percentiles of the same difference |
| `MSE`, `PSNR` | selected channel, linear | Squared error; PSNR relative to a peak of 1.0 |
| `rel_mean_%` | selected channel, linear | Mean difference divided by the mean reference value |
| `rel_mse` | selected channel, linear | Mean of `(ref - cand)^2 / (ref^2 + eps)`. The reference alone is the denominator |
| `SSIM` | tone-mapped sRGB (ACES or AgX) | Structural similarity, 11x11 Gaussian window |
| `tm_mean_diff`, `tm_PSNR` | tone-mapped sRGB | Difference and PSNR in the display domain |
| `ictec_dE` | BT.2100 ICtCp | Per-pixel color distance in a perceptually uniform HDR space (1.0 scene-linear is taken as 10000 nits) |
| `lum_hist_corr` | RGB luminance, linear | Pearson correlation of the log-spaced luminance histograms, with one extra bin for black pixels |
| `black_frac` | RGB luminance, linear | Fraction of black pixels in each image (diagnostic) |
| `signed` | selected channel, linear | Mean, 5th and 95th percentile of `cand - ref`; positive means the candidate is brighter |

The `luminance` channel always uses Rec.2020 coefficients. The tone-mapped metrics
mirror the tone mappers in `tonemap.slang`, so SSIM judges the image the display
would show. Identical images score a `mean_diff` of 0, an SSIM of 1 and an infinite PSNR.

Output: one line per metric with its limit, the result of the limit checks and a final
`RESULT` line with `PASS` or `FAIL`. The heatmap is blue for no difference and red for
a large one, scaled by the 95th percentile. The signed heatmap is blue where the
candidate is darker and red where it is brighter.

Exit code: 0 when the limits hold, 1 when they do not or an input is missing or unreadable.

### compare_sequences.py

Runs the `all` limit checks on every frame pair of two sequences and aggregates the
results over time. It is the animated counterpart of the still comparison; no learned
or neural video metric is used.

```text
python tools/compare_sequences.py <reference_dir> <candidate_dir> [options]
```

Every `.exr` file in each directory is a frame. Frames are paired by the trailing
`_<index>` of their file name (the pattern written by `--sequence-frames`) when both
directories carry one, otherwise by sorted order. A missing or extra frame index is an
error; the directories are never truncated to the shorter length.

Options: the four threshold options (same defaults as `compare_renders.py`),
`--psnr-threshold`, `--relative-threshold`, `--tm-mean-threshold`, `--tonemap`,
`--channel`, `--gate`, `--hist-bins`, `--rel-mse-eps`, `--allow-small-ssim`.

Output: a table with one row per frame (`mean_diff`, Rel-MSE, SSIM, histogram
correlation, `PASS`/`FAIL`); an aggregate per metric with its mean, its worst value and
the frame it occurs on, and the change from the first to the last frame (a drift that
builds up over time shows here); the list of failed checks.

Exit code: 0 when every frame passes, 1 otherwise. One failing frame fails the sequence.

### validate_renders.py

Compares a set of scene-named EXR pairs with the manifest's limits.

```text
python tools/validate_renders.py <reference_dir> <candidate_dir> [options]
```

Options: `--manifest`, `--scene NAME` (repeatable; replaces the manifest's scene list),
`--scale-aware NAME` (repeatable), `--reference-template`, `--candidate-template`,
the threshold options of `compare_renders.py` as overrides, `--rel-mse-eps`,
`--hist-bins`, `--channel`, `--heatmap-dir`, `--no-heatmap`, `--fail-fast`.

Output: one line per scene with `PASS` or `FAIL` and its `mean`, `relmse`, `ssim`,
`hist` and `blackd` values, then `summary: N passed / M failed`. A missing EXR or a
resolution mismatch counts as a failure and prints the reason.

Exit code: 0 when every scene passes, 1 otherwise.

### render_and_validate.py

Renders every manifest scene with both renderers and compares the batch. This runs the
complete parity comparison.

```text
python tools/render_and_validate.py <hyperion_exe> <theia_exe> <output_root> [--manifest PATH] [--scene NAME] [--no-validate]
```

For each scene it runs, with validation on:

```text
hyperion.exe --scene <scene> --spp <hyperion_spp> --output <output_root>/<scene>/hyperion.exr --width <width> --height <height>
theia.exe    --scene <scene> --no-postfx --offscreen-frames <theia_frames> --output <output_root>/<scene>/theia.exr --width <width> --height <height>
```

`--no-validate` only renders. The values come from the manifest. A renderer that
exits with an error stops the run. The comparison step passes file patterns that
use a backslash as separator, so it works on Windows.

Exit code: 0 when the renders and the comparison succeed.

### check_vulkan_validation.py

Runs both renderers over a scene set with validation on and fails a run on any validation
message in its output and on any warning or error line logged by the application.

```text
python tools/check_vulkan_validation.py [--theia PATH] [--hyperion PATH] [--manifest PATH]
    [--scene NAME] [--glare-scenes NAME] [--width N] [--height N] [--frames N] [--glare F]
    [--window-seconds N] [--renderers theia,hyperion]
```

Defaults: the executables are expected next to this repository in `Theia/build` and
`Hyperion/build`; the scene set is the manifest; the resolution is the manifest's;
`--frames` is 4 (accumulation frames for Theia, samples per pixel for Hyperion);
`--glare` is 0.1; the glare scenes are the scenes of the manifest; `--window-seconds` is 0.

The lens glare is a stage of the host that both renderers share, but it scatters the
light of the scene. Every scene of the manifest (or every scene named with
`--glare-scenes`) is captured a second time with `--glare F`, once per renderer, with the
mode `glare`; a scene that is not rendered gets no glare run. `--glare 0` skips these
runs. With `--window-seconds N` the window of these scenes is opened with the glare as
well (mode `win+glare`). `tools/test_check_vulkan_validation.py` checks the scene set
of the glare runs without rendering.

A line in a renderer's output fails the run when it contains one of these patterns:
`VUID-`, `UNASSIGNED`, `Vulkan warning`, `Vulkan error`, `][WARNING]` or `][ERROR]` (the
logger writes lines as `[TAG][LEVEL] message`; an application warning or error fails the
run, an information line does not). An offscreen run also fails when the renderer exits
with an error or does not finish within 120 seconds. With `--window-seconds N` the
interactive window is also opened for N seconds per scene and its output is checked the
same way.

Output: a table (renderer, scene, mode, result, number of offending lines, time, note),
the first offending line per failed run, then `N/M runs clean` and
`Zero Vulkan validation warnings/errors.` The captures are written to a `_vk_logs`
directory beside this file.

Exit code: 0 when every run is clean, 1 when a run fails, 2 when an executable is missing.

### check_consistency.py

Decides whether Theia's remaining difference to Hyperion is noise or bias. It renders
Theia at increasing frame counts with `--no-postfx` and renders a Hyperion reference.

```text
python tools/check_consistency.py <theia_exe> <hyperion_exe> <output_dir> --scene NAME
    [--frame-scales 64 256 1024] [--hyperion-spp 1024] [--bias-threshold 4.0]
    [--width 320] [--height 240] [--skip-hyperion] [--reuse] [-- --extra theia args]
```

Two checks:

1. Variance rate. With at least three frame counts, the `mean_diff` between
   consecutive accumulations is compared from one step to the next. Quadrupling the
   frame count should halve it; the ratio of the two differences must lie in
   [0.30, 0.85]. A lower ratio means the samples are correlated (bias appearing as
   variance); a higher one means no improvement.
2. Bias floor. `mean_diff` between the largest Theia accumulation and the Hyperion
   reference must not exceed `--bias-threshold` (4.0, in 1/255 units).

Output: the pairwise differences, the ratios with their result, the bias floor with
its result and a summary. Files are written as `theia_<frames>f.exr` and
`hyperion_<spp>spp.exr` in the output directory; `--reuse` uses files already there.

Exit code: 0 when both checks hold, 1 otherwise. On a scene lit by a sun or an
environment map the reference needs many more samples; a reference that is itself
noisy fails the bias floor without indicating a defect.

### test_denoiser_contract.py

Checks the two-tier output contract: a capture must not depend on the denoiser.

```text
python tools/test_denoiser_contract.py <hyperion_exe> <theia_exe> <output_dir>
    [--scene NAME] [--width 320] [--height 240] [--hyperion-spp 8] [--theia-frames 8]
    [--skip-renderer hyperion|theia]
```

For each renderer it renders the same scene twice, once plainly and once with the
denoiser explicitly requested (`--denoiser-strength 0.9 --denoiser-iterations 4
--denoiser-history`), and requires the two EXR files to be pixel-identical. For Theia
it also checks a capture made with `--no-postfx`. Low sample counts are enough: the
test is about determinism, not convergence.

Exit code: 0 when every pair is identical, 1 otherwise.

### frames_to_video.py

Encodes a numbered image sequence into an H.264 video.

```text
python tools/frames_to_video.py <input_dir> <output.mp4> [--fps N | --interval-ms X] [--pattern "*.png"] [--crf N]
```

Frames are ordered by the trailing `_<index>` of their file names, not alphabetically.
`--fps` (default 30) suits captures on the tick grid. `--interval-ms` takes the value
of `--capture-every-ms`, so the playback duration equals the captured time range (10
frames every 100 ms give a 1.0 s video at 10 fps). `--crf` (default 18) sets the
quality. Odd dimensions are padded to even ones.

## Checking the source

### check_arch.py

Checks the include graph of the sources against the contract in `ARCHITECTURE.md`:
only declared dependencies, the layer order, every file in a declared module, no
include cycles, headers opening their namespace, a size limit per file, and no drift
of the generated diagrams. The rules and their meaning are described in that document.

```text
python tools/check_arch.py [--repo DIR] [--doc FILE] [--report] [--json]
python tools/check_arch.py [--repo DIR] [--doc FILE] --diagram      # regenerate the diagrams
python tools/check_arch.py --derive [--scan DIRS] [--out FILE]     # draft a contract from the sources
python tools/check_arch.py --selftest
```

Output: the violations, one per line. `--report` adds the layer stack and the measured
module graph; `--json` prints the summary as JSON. The code-smell indicators
(hub, pass-through, orphan modules) are reported only; they never fail a run.

Exit codes: 0 clean, 1 violations, 2 configuration or usage error, 77 skipped.
The same check is registered as the ctest `test_arch` (label `analysis`) on machines
where the check-arch skill is installed.

### check_dead.py

Finds code that nothing uses.

```text
python tools/check_dead.py [--repo DIR] [--family DIR] [--report]
```

| Check | Finding |
|---|---|
| Orphan headers | A header that no file includes |
| Never compiled | A source file that is neither in the newest compile database nor named in any CMake file |
| FetchContent | A declared dependency that is never made available |

`--family DIR` gives an orphaned header a second chance: when a sibling repository
under DIR includes it, the header is listed as a cross-repo consumer instead of a
finding. Every finding needs a decision before anything is deleted. A justified
finding goes into `check_dead.exceptions` in this directory, one line per entry:

```text
<check> <path-or-name> <reason>
```

Exit codes: 0 clean, 1 findings, 77 nothing scanned. The same check is registered as
the ctest `test_dead` (label `analysis`) on machines where the check-dead skill is
installed.

### check_tidy.py

Runs clang-tidy in parallel over every project translation unit in the compile
database and sorts the findings as the `.clang-tidy` file demands: `clang-diagnostic`,
`clang-analyzer` and `bugprone` findings fail the run; `modernize`, `performance` and
`portability` findings are summarized. clang-tidy is looked up in the newest Visual
Studio installation. The complete output is written to `build/clang_tidy.log`.

```text
python tools/check_tidy.py [--db PATH] [--jobs N]
```

Exit codes: 0 clean, 1 failing findings, 77 when clang-tidy or the database is missing.
Also registered as the ctest `test_tidy` (label `analysis`).

### coverage.py

Builds an instrumented tree, runs the test suite, merges the profiles and prints a
line coverage report for the sources.

```text
cmake -S . -B build/coverage -G Ninja -DCMAKE_BUILD_TYPE=Release <compiler and toolchain flags> -DHARMONIA_COVERAGE=ON
python tools/coverage.py [--build DIR] [--skip-build] [--show]
```

The option requires Clang (GNU builds are configured with `--coverage` flags, but the
report needs `llvm-profdata` and `llvm-cov`, which the script looks for next to the
compiler and then on `PATH`). The default build directory is build/coverage; the
generator, compilers and toolchain are copied from the main build when it exists.
`--show` prints annotated sources instead of the summary.

Exit codes: 0 report produced, 1 test or coverage failure, 77 LLVM tools missing,
2 usage problem.

## Driver capability checks

### check_vk_extensions.py

Lists the KHR and EXT extensions of the installed driver and compares them with every
`VK_KHR_*` and `VK_EXT_*` name written in `PLAN.md` and `AGENTS.md`. Run it after every
driver or SDK update and record the counts in `PLAN.md`, section 5.8.

```text
python tools/check_vk_extensions.py [--sdk DIR] [--dump FILE] [--list-unadjudicated]
```

`vulkaninfoSDK` is found through `--sdk`, the `VULKAN_SDK` variable or the newest
directory under `C:/VulkanSDK`. `--dump` reads a saved output instead. A name ending
in an underscore stands for a family (`VK_KHR_video_` matches every video extension).

Output: the number of device extensions, how many of them are KHR or EXT, how many
doc-named names exist and how many are device-level, and with `--list-unadjudicated`
every device extension that no document names. A name that the driver lacks entirely
is listed under `MISSING from the driver entirely`.

Exit codes: 0 every doc-named extension is available, 1 a doc-named extension is
missing, 2 the environment is unusable.

### check_slang_ngsm.py

Compiles five small kernels with the SDK's `slangc` and lists the SPIR-V capabilities
they emit. It answers whether the Slang compiler can already use the next-generation
shader-model extensions (`VK_KHR_shader_untyped_pointers`,
`VK_EXT_shader_replicated_composites`, `VK_EXT_shader_64bit_indexing`,
`VK_EXT_shader_long_vector`). Each kernel is checked with `spirv-val`; when available,
`spirv-dis` supplies the capability list.

```text
python tools/check_slang_ngsm.py [--sdk DIR]
```

Without `--sdk` the script reads the `VULKAN_SDK` variable. It expects the Windows SDK
layout, with the tools in the `Bin` directory of the SDK.

Output: one line per probe with its status (`REJECTED by slangc`, `spirv-val FAILED`
or `compiled+valid`), then a matrix for the record in `PLAN.md`, section 5.8.

Exit code: 0 when the probe ran, whatever the individual results are.

## Reference values for two tests

### sms_curved_oracle.py and curved_oracle.json

Solves a curved-glass chain in double precision with NumPy: a receiver, two
refractions through one sphere and a light. The result includes the two surface
points with their derivatives and the geometric factor obtained by finite
differences. The values in `curved_oracle.json` are the independent reference of the
curved geometric-term test in `test_sms.cpp`.

```text
python tools/sms_curved_oracle.py [output.json]
```

Without an argument the script overwrites `curved_oracle.json` in this directory.

### dn3m4_receipt.py

Evaluates the production-scene convergence test of the denoiser
(`SceneOutputCopyPass_DN3M4ProductionSceneConvergence` in
`test_scene_output_copy_pass_dn3m4.cpp`) against its acceptance limits. The test is
skipped unless `HARMONIA_DN3M4_DIR` names a prepared directory, so the default suite
does not depend on it.

1. Prepare the directory with raw Theia captures named `<scene>_raw_<spp>.exr` for
   `dragon_teapot` and `cornell_classic` at 16, 64, 256, 1024 and 4096 samples per
   pixel. A capture is raw accumulation because `--output` disables the denoiser.
2. Run the component test with `HARMONIA_DN3M4_DIR` set to the directory. It runs the
   shared denoiser pass over every image in two modes and writes
   `out/<scene>_cw_<spp>.exr` (the default constant-weight blend) and
   `out/<scene>_cv_<spp>.exr` (the converging blend).
3. Evaluate:

```text
python tools/dn3m4_receipt.py --dir <HARMONIA_DN3M4_DIR>
```

The reference is the raw 4096-sample image of the same scene. All errors are Rel-MSE
against it. Acceptance, per scene:

1. The raw Rel-MSE falls about as 1/samples: the log-log slope over 16 to 1024 samples
   lies in [-1.3, -0.7], and the values do not increase from one sample count to the
   next (a rise of up to 5 percent is tolerated).
2. The default constant-weight blend crosses over: its Rel-MSE reaches at least 1.1
   times the raw Rel-MSE at 256 or 1024 samples, and at 4096 samples it is at least
   5 times the converging blend's Rel-MSE.
3. The converging blend is never worse than 1.1 times raw up to 1024 samples and ends
   below half the constant-weight blend at 4096 samples.

Output: one summary line per scene with the measured values and `PASS` or `FAIL`, then
the overall result. In the `out` directory it writes `<scene>_curves.csv` (mode,
sample count, Rel-MSE), signed difference images for both modes at 1024 and 4096
samples, and tone-mapped crops of the three modes at 4096 samples.

Exit code: 0 when every check passes for every scene, 1 otherwise.
