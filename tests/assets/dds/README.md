# DDS test fixtures

Block-compressed DDS files for the texture loader tests. They are made with Microsoft
DirectXTex `texconv` (DirectX Texture Converter 2026.5.8.1, CPU codecs, `-nogpu`) from one
16x16 source image, so the tests run against what the reference encoder writes. `texconv`
is not a runtime dependency of Harmonia and is not needed to run the tests: the outputs are
committed. Only regenerating the fixtures needs it.

| File | Format | Purpose |
|------|--------|---------|
| `bc7_checker16.tga` | 24-bit BGR, top-left origin | The uncompressed source image. |
| `bc7_checker16.dds` | DX10, `BC7_UNORM`, 5 levels | The block-compressed upload under test. |
| `bc7_checker16_srgb.dds` | DX10, `BC7_UNORM_SRGB`, 5 levels | The same blocks in a file that declares sRGB. |
| `bc7_checker16_ref.dds` | DX10, `R8G8B8A8_UNORM`, 5 levels | DirectXTex's own decode of `bc7_checker16.dds`, level by level: the independent CPU oracle of every level. It is uncompressed, so the compressed loader must reject it. |
| `bc7_checker16_bc1.dds` | legacy header, FourCC `DXT1`, 5 levels | The legacy (pre-DX10) header and 8-byte blocks. |
| `bc7_checker16_bc5.dds` | DX10, `BC5_UNORM`, 5 levels | A family without an sRGB variant. |

## The source image

`make_fixture.py` writes the TGA; the image is a pure function of the pixel position: a
per-channel gradient that wraps around, four saturated 4x4 corner patches (red, green,
blue, white) and a grey checker in the centre whose cells are 4x4. Every feature is
aligned to the block grid on purpose. Row 0 is the top row, as in the DDS files.

## Regenerating

```text
python make_fixture.py <tmp>
texconv -nologo -y --single-proc -nogpu -o <tmp> -f BC7_UNORM -m 0 <tmp>\bc7_checker16.tga
python make_fixture.py <tmp> --srgb-variant
texconv -nologo -y --single-proc -nogpu -o <tmp> -f R8G8B8A8_UNORM -dx10 -m 0 -sx _ref <tmp>\bc7_checker16.dds
texconv -nologo -y --single-proc -nogpu -o <tmp> -f BC1_UNORM -m 0 -sx _bc1 <tmp>\bc7_checker16.tga
texconv -nologo -y --single-proc -nogpu -o <tmp> -f BC5_UNORM -m 0 -dx10 -sx _bc5 <tmp>\bc7_checker16.tga
```

The sRGB variant is made by changing only the DX10 DXGI format field of the UNORM file.
Re-encoding the source as `BC7_UNORM_SRGB` can choose different BC7 endpoints, which would
break the independent `_ref.dds` comparison; the header-only variant keeps every block equal.

`-m 0` writes the complete chain: five levels for 16x16. The layout of every file follows
one rule, which the unit tests pin with literal offsets: level *i* covers
ceil(w / 2^i) x ceil(h / 2^i) texels as whole 4x4 blocks, one level after another from the
top level, starting at byte 148 (DX10) or 128 (legacy). `bc7_checker16.dds` is therefore
148 + (16 + 4 + 1 + 1 + 1) x 16 = 516 bytes.

## Stated BC7 error

Against its uncompressed source, `bc7_checker16.dds` at level 0 has a maximum channel error
of 76/255 and a mean error of 4.83/255 (27.77 dB), measured with DirectXTex's decoder. The
gradient wraps around, which is hard content for 4x4 blocks, so this bound is loose. The
tight oracle of the tests is the comparison of the GPU decode with `_ref.dds` at every
level: two decoders of the same blocks agree to within 1 LSB.

## What is checked where

- `tests/unit/test_dds_image.cpp` (CPU): the parser on these files and on synthetic
  headers, including every rejection.
- `tests/component/test_host_image_upload_dds.cpp` (GPU): the upload of
  `bc7_checker16.dds` and `bc7_checker16_srgb.dds`, compared with the source through the
  RGBA8 path and with `_ref.dds` at every level.
