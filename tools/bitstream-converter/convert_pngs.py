#!/usr/bin/env python3
"""Convert PNG source images into OpenDisplay benchmark bitstreams.

The 1/2/4bpp packing mirrors py-opendisplay's encoding/images.py behavior:
row-major, MSB-first, palette index values packed directly into the stream.
"""

from __future__ import annotations

import argparse
import re
import sys
from collections.abc import Iterable
from pathlib import Path

from PIL import Image


SCHEME_BITS: dict[str, int] = {
    "mono": 1,
    "gray4": 2,
    "gray16": 4,
    "bwr": 2,
    "bwy": 2,
    "bwry": 2,
    "bwgbry": 4,
}

PALETTES: dict[str, list[tuple[int, int, int]]] = {
    "mono": [(0, 0, 0), (255, 255, 255)],
    "gray4": [(0, 0, 0), (85, 85, 85), (170, 170, 170), (255, 255, 255)],
    "gray16": [(i * 17, i * 17, i * 17) for i in range(16)],
    "bwr": [(0, 0, 0), (255, 255, 255), (220, 0, 0)],
    "bwy": [(0, 0, 0), (255, 255, 255), (220, 190, 0)],
    "bwry": [(0, 0, 0), (255, 255, 255), (220, 190, 0), (220, 0, 0)],
    # Firmware value 4 is intentionally unused for BWGBRY; index 5 maps to value 6.
    "bwgbry": [(0, 0, 0), (255, 255, 255), (0, 170, 0), (0, 80, 220), (220, 0, 0), (220, 190, 0)],
}

BWGBRY_VALUE_MAP = {0: 0, 1: 1, 2: 2, 3: 3, 4: 5, 5: 6}
SCHEME_PATTERN = re.compile(r"(^|[-_.])(bwgbry|gray16|gray4|bwry|bwr|bwy|mono)([-_.]|$)", re.IGNORECASE)


def composite_over_white(path: Path) -> Image.Image:
    image = Image.open(path).convert("RGBA")
    background = Image.new("RGBA", image.size, (255, 255, 255, 255))
    background.alpha_composite(image)
    return background.convert("RGB")


def colors_first_seen(image: Image.Image, limit: int = 17) -> list[tuple[int, int, int]]:
    seen: dict[tuple[int, int, int], None] = {}
    for color in image.getdata():
        rgb = tuple(color)
        if rgb not in seen:
            seen[rgb] = None
            if len(seen) >= limit:
                break
    return list(seen.keys())


def indexed_from_exact_colors(image: Image.Image, colors: list[tuple[int, int, int]]) -> list[int]:
    mapping = {color: idx for idx, color in enumerate(colors)}
    return [mapping[tuple(color)] for color in image.getdata()]


def nearest_palette_index(pixel: tuple[float, float, float], palette: list[tuple[int, int, int]]) -> int:
    best_idx = 0
    best_dist = float("inf")
    r, g, b = pixel
    for idx, (pr, pg, pb) in enumerate(palette):
        dr = r - pr
        dg = g - pg
        db = b - pb
        dist = dr * dr + dg * dg + db * db
        if dist < best_dist:
            best_idx = idx
            best_dist = dist
    return best_idx


def dither_burkes(image: Image.Image, palette: list[tuple[int, int, int]]) -> list[int]:
    """Burkes error diffusion into palette indices.

    Error distribution:
      x+1 8/32, x+2 4/32
      next row x-2 2/32, x-1 4/32, x 8/32, x+1 4/32, x+2 2/32
    """

    width, height = image.size
    pixels = [[list(map(float, image.getpixel((x, y)))) for x in range(width)] for y in range(height)]
    out = [0] * (width * height)

    def add_error(x: int, y: int, error: tuple[float, float, float], weight: float) -> None:
        if x < 0 or x >= width or y < 0 or y >= height:
            return
        row_pixel = pixels[y][x]
        row_pixel[0] += error[0] * weight
        row_pixel[1] += error[1] * weight
        row_pixel[2] += error[2] * weight

    for y in range(height):
        for x in range(width):
            old = tuple(max(0.0, min(255.0, c)) for c in pixels[y][x])
            idx = nearest_palette_index(old, palette)
            new = palette[idx]
            out[y * width + x] = idx
            error = (old[0] - new[0], old[1] - new[1], old[2] - new[2])
            add_error(x + 1, y, error, 8 / 32)
            add_error(x + 2, y, error, 4 / 32)
            add_error(x - 2, y + 1, error, 2 / 32)
            add_error(x - 1, y + 1, error, 4 / 32)
            add_error(x, y + 1, error, 8 / 32)
            add_error(x + 1, y + 1, error, 4 / 32)
            add_error(x + 2, y + 1, error, 2 / 32)

    return out


def infer_exact_scheme(color_count: int) -> tuple[str, int]:
    if color_count <= 2:
        return "indexed1", 1
    if color_count <= 4:
        return "indexed2", 2
    return "indexed4", 4


def detect_scheme_from_name(path: Path) -> str | None:
    match = SCHEME_PATTERN.search(path.stem)
    return match.group(2).lower() if match else None


def pack_od(indices: list[int], width: int, height: int, bpp: int, scheme: str) -> bytes:
    pixels_per_byte = 8 // bpp
    pitch = (width + pixels_per_byte - 1) // pixels_per_byte
    out = bytearray(pitch * height)

    for y in range(height):
        for x in range(width):
            value = indices[y * width + x] & ((1 << bpp) - 1)
            if scheme == "bwgbry":
                value = BWGBRY_VALUE_MAP.get(value, 0)
            byte_idx = y * pitch + x // pixels_per_byte
            shift = (pixels_per_byte - 1 - (x % pixels_per_byte)) * bpp
            out[byte_idx] |= value << shift

    return bytes(out)


def pack_1bpp_streams(indices: list[int], width: int, height: int, bpp: int, scheme: str) -> bytes:
    pitch = (width + 7) // 8
    plane_size = pitch * height
    out = bytearray(plane_size * bpp)

    for y in range(height):
        for x in range(width):
            value = indices[y * width + x] & ((1 << bpp) - 1)
            if scheme == "bwgbry":
                value = BWGBRY_VALUE_MAP.get(value, 0)
            byte_offset = y * pitch + x // 8
            bit = 1 << (7 - (x % 8))
            for plane in range(bpp):
                if value & (1 << plane):
                    out[plane * plane_size + byte_offset] |= bit

    tail_bits = width % 8
    if tail_bits:
        tail_mask = (1 << (8 - tail_bits)) - 1
        for plane in range(bpp):
            plane_offset = plane * plane_size
            for y in range(height):
                out[plane_offset + y * pitch + pitch - 1] |= tail_mask

    return bytes(out)


def schemes_for_image(path: Path, image: Image.Image, requested_scheme: str | None) -> list[tuple[str, int, list[int]]]:
    colors = colors_first_seen(image)
    if requested_scheme:
        requested = list(SCHEME_BITS) if requested_scheme == "all" else [requested_scheme]
        return [(scheme, SCHEME_BITS[scheme], dither_burkes(image, PALETTES[scheme])) for scheme in requested]

    detected = detect_scheme_from_name(path)
    if detected:
        return [(detected, SCHEME_BITS[detected], dither_burkes(image, PALETTES[detected]))]

    if len(colors) <= 16:
        scheme, bpp = infer_exact_scheme(len(colors))
        return [(scheme, bpp, indexed_from_exact_colors(image, colors))]

    defaults = ["mono", "gray4", "gray16"]
    return [(scheme, SCHEME_BITS[scheme], dither_burkes(image, PALETTES[scheme])) for scheme in defaults]


def output_prefix(path: Path, scheme: str, width: int, height: int) -> Path:
    return path.with_name(f"{path.stem}.{scheme}.{width}x{height}")


def convert_png(path: Path, requested_scheme: str | None) -> int:
    image = composite_over_white(path)
    width, height = image.size
    exact_colors = colors_first_seen(image)
    variants = schemes_for_image(path, image, requested_scheme)

    for scheme, bpp, indices in variants:
        prefix = output_prefix(path, scheme, width, height)
        od = pack_od(indices, width, height, bpp, scheme)
        planes = pack_1bpp_streams(indices, width, height, bpp, scheme)
        od_path = prefix.parent / f"{prefix.name}.bs-od"
        g5_path = prefix.parent / f"{prefix.name}.bs-1bppstreams"
        od_path.write_bytes(od)
        g5_path.write_bytes(planes)
        print(
            f"{path} scheme={scheme} resolution={width}x{height} "
            f"source_colors={len(exact_colors) if len(exact_colors) < 17 else '>16'} "
            f"bpp={bpp} od_bytes={len(od)} g5_bytes={len(planes)}"
        )

    return len(variants)


def iter_pngs(paths: Iterable[Path]) -> Iterable[Path]:
    for path in paths:
        if path.is_dir():
            yield from sorted(p for p in path.rglob("*.png") if p.is_file() and "originals" not in p.parts)
        elif path.is_file() and path.suffix.lower() == ".png":
            if "originals" in path.parts:
                continue
            yield path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Convert PNG images to OpenDisplay benchmark bitstreams.")
    parser.add_argument("paths", nargs="+", type=Path, help="PNG file or directory containing PNG files")
    parser.add_argument(
        "--scheme",
        choices=["mono", "gray4", "gray16", "bwr", "bwy", "bwry", "bwgbry", "all"],
        help="Force one scheme, or all schemes, instead of inferring from the PNG/name.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    pngs = list(iter_pngs(args.paths))
    if not pngs:
        print("no PNG files found", file=sys.stderr)
        return 2

    converted = 0
    failed = 0
    for png in pngs:
        try:
            converted += convert_png(png, args.scheme)
        except Exception as exc:  # noqa: BLE001 - CLI should continue through bad corpus entries.
            failed += 1
            print(f"{png}: failed: {exc}", file=sys.stderr)

    print(f"converted_variants={converted} failed_images={failed}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
