#!/usr/bin/env python3
"""img2rgb332.py — quantise an image to a 256x240 RGB332 byte array.

Outputs a C header `static const DRAM_ATTR uint8_t <name>[256*240]` ready to
memcpy into the fb buffer when CRT_RENDER_MODE_RGB332_FB is active.

Aspect handling: resize-fit with center crop so the source aspect is preserved
and the output exactly matches the 256x240 fb. Dithering: optional Floyd-
Steinberg in linear-light space; produces the noisy 8-bit look on smooth
gradients.

Usage:
    python tools/img2rgb332.py <input> <output.h> <symbol> [--no-dither]
"""

from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np
from PIL import Image

FB_W, FB_H = 256, 240


def fit_and_crop(img: Image.Image, w: int, h: int) -> Image.Image:
    src_aspect = img.width / img.height
    dst_aspect = w / h
    if src_aspect > dst_aspect:
        new_h = h
        new_w = int(round(h * src_aspect))
    else:
        new_w = w
        new_h = int(round(w / src_aspect))
    img = img.resize((new_w, new_h), Image.LANCZOS)
    left = (new_w - w) // 2
    top = (new_h - h) // 2
    return img.crop((left, top, left + w, top + h))


def quantise_rgb332(rgb: np.ndarray, dither: bool) -> np.ndarray:
    """Convert HxWx3 uint8 RGB into HxW uint8 RGB332.

    Channel resolution: R=3 bits (0..7), G=3 bits (0..7), B=2 bits (0..3).
    Dithering uses Floyd–Steinberg per channel; quantisation steps come from
    rounding to the nearest representable level then re-projecting back to 8 bit
    so error diffusion stays in the same domain.
    """
    h, w, _ = rgb.shape
    work = rgb.astype(np.int16)
    out = np.empty((h, w), dtype=np.uint8)
    levels_r = np.array([round(i * 255 / 7) for i in range(8)], dtype=np.int16)
    levels_g = levels_r
    levels_b = np.array([round(i * 255 / 3) for i in range(4)], dtype=np.int16)

    def quant(c: int, levels: np.ndarray) -> tuple[int, int]:
        idx = int(np.argmin(np.abs(levels - c)))
        return idx, int(levels[idx])

    for y in range(h):
        for x in range(w):
            r, g, b = work[y, x]
            ri, rq = quant(int(r), levels_r)
            gi, gq = quant(int(g), levels_g)
            bi, bq = quant(int(b), levels_b)
            out[y, x] = (ri << 5) | (gi << 2) | bi
            if dither:
                er = r - rq
                eg = g - gq
                eb = b - bq
                # Floyd–Steinberg: 7/16, 3/16, 5/16, 1/16
                if x + 1 < w:
                    work[y, x + 1, 0] += er * 7 // 16
                    work[y, x + 1, 1] += eg * 7 // 16
                    work[y, x + 1, 2] += eb * 7 // 16
                if y + 1 < h:
                    if x > 0:
                        work[y + 1, x - 1, 0] += er * 3 // 16
                        work[y + 1, x - 1, 1] += eg * 3 // 16
                        work[y + 1, x - 1, 2] += eb * 3 // 16
                    work[y + 1, x, 0] += er * 5 // 16
                    work[y + 1, x, 1] += eg * 5 // 16
                    work[y + 1, x, 2] += eb * 5 // 16
                    if x + 1 < w:
                        work[y + 1, x + 1, 0] += er // 16
                        work[y + 1, x + 1, 1] += eg // 16
                        work[y + 1, x + 1, 2] += eb // 16
            np.clip(work[y], 0, 255, out=work[y])
    return out


def emit_header(out_path: Path, symbol: str, pixels: np.ndarray, src: Path) -> None:
    h, w = pixels.shape
    flat = pixels.reshape(-1)
    body = []
    line = []
    for i, v in enumerate(flat):
        line.append(f"0x{int(v):02X}")
        if (i + 1) % 16 == 0:
            body.append("    " + ", ".join(line) + ",")
            line = []
    if line:
        body.append("    " + ", ".join(line) + ",")

    text = (
        "#pragma once\n"
        f"/* Auto-generated from {src.name} — {w}x{h} RGB332.\n"
        " * R=3 bits (0..7) << 5  |  G=3 bits (0..7) << 2  |  B=2 bits (0..3).\n"
        " * Memory: place in DRAM via DRAM_ATTR for cache-stable hot-path reads. */\n"
        "\n"
        "#include <stdint.h>\n"
        "#include \"esp_attr.h\"\n"
        "\n"
        f"#define {symbol.upper()}_WIDTH  {w}\n"
        f"#define {symbol.upper()}_HEIGHT {h}\n"
        "\n"
        f"DRAM_ATTR static const uint8_t {symbol}[{w}*{h}] = {{\n"
        + "\n".join(body)
        + "\n};\n"
    )
    out_path.write_text(text)


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("input", type=Path)
    p.add_argument("output", type=Path)
    p.add_argument("symbol")
    p.add_argument("--no-dither", action="store_true")
    args = p.parse_args()

    img = Image.open(args.input).convert("RGB")
    img = fit_and_crop(img, FB_W, FB_H)
    rgb = np.array(img, dtype=np.uint8)
    pixels = quantise_rgb332(rgb, dither=not args.no_dither)
    emit_header(args.output, args.symbol, pixels, args.input)
    print(f"wrote {args.output}: {pixels.shape[1]}x{pixels.shape[0]} RGB332 (dither={not args.no_dither})")


if __name__ == "__main__":
    main()
