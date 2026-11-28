"""Generates the source image and the header-only sRGB variant of the DDS fixture.

The source image is a pure function of (x, y): a smooth per-channel gradient, four
saturated 4x4 corner patches and a block-aligned grey checker in the centre. Every
feature is aligned to the 4x4 block grid, so BC7 reproduces it within a small, statable
error (see README.md). The block-compressed and reference DDS files are then produced
with DirectXTex texconv; run this script and then the commands in README.md.
"""

import struct
import sys
from pathlib import Path

W = H = 16


def pixel(x, y):
    r = (x * 17 + y * 5) % 256
    g = (y * 13 + x * 3) % 256
    b = ((x * x + y * y) * 3) % 256
    if x < 4 and y < 4:
        r, g, b = 255, 0, 0
    if x >= 12 and y < 4:
        r, g, b = 0, 255, 0
    if x < 4 and y >= 12:
        r, g, b = 0, 0, 255
    if x >= 12 and y >= 12:
        r, g, b = 255, 255, 255
    if 4 <= x < 12 and 4 <= y < 12:
        v = 224 if ((x // 4) + (y // 4)) % 2 == 0 else 32
        r, g, b = v, v, v
    return r, g, b


def write_source(out_dir):
    pixels = bytearray()
    for y in range(H):  # top-left origin (descriptor bit 5 set): row 0 is the top row
        for x in range(W):
            r, g, b = pixel(x, y)
            pixels += bytes((b, g, r))
    # Uncompressed 24-bit BGR TGA, top-left origin, no colour map.
    header = struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0, W, H, 24, 0x20)
    path = Path(out_dir) / "bc7_checker16.tga"
    path.write_bytes(header + bytes(pixels))
    print("wrote", path)


def write_srgb_variant(out_dir):
    """Copies BC7_UNORM to BC7_UNORM_SRGB without re-encoding any compressed block."""
    source = Path(out_dir) / "bc7_checker16.dds"
    data = bytearray(source.read_bytes())
    if len(data) < 148 or data[:4] != b"DDS " or data[84:88] != b"DX10":
        raise ValueError(f"{source} is not a DX10 DDS file")
    if struct.unpack_from("<I", data, 128)[0] != 98:  # DXGI_FORMAT_BC7_UNORM
        raise ValueError(f"{source} is not BC7_UNORM")
    struct.pack_into("<I", data, 128, 99)  # DXGI_FORMAT_BC7_UNORM_SRGB
    path = source.with_name("bc7_checker16_srgb.dds")
    path.write_bytes(data)
    print("wrote", path, "(identical BC7 payloads; only the DXGI format changed)")


if __name__ == "__main__":
    directory = sys.argv[1] if len(sys.argv) > 1 else "."
    if len(sys.argv) == 3 and sys.argv[2] == "--srgb-variant":
        write_srgb_variant(directory)
    elif len(sys.argv) <= 2:
        write_source(directory)
    else:
        raise SystemExit(f"usage: {sys.argv[0]} [out_dir] [--srgb-variant]")
