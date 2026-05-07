# Bitstream Converter

This tool converts PNG source images into the two bitstream formats consumed by
`codec_benchmark`:

```text
<stem>.<scheme>.<width>x<height>.bs-od
<stem>.<scheme>.<width>x<height>.bs-1bppstreams
```

Use it after source images have been gathered or generated, and before running
the compressor benchmark:

```text
gather source images
  -> run source/stress generator tools to produce PNG variations
  -> run this bitstream converter
  -> run codec_benchmark to produce compressed .dat variants
```

Only PNG files are processed. JPEG originals are intentionally ignored.
Any PNG below a folder named `originals` is also skipped; those files are source
masters, not benchmark-ready target-resolution images.

## Usage

```bash
python3 tools/bitstream-converter/convert_pngs.py image_sources
python3 tools/bitstream-converter/convert_pngs.py image_sources/photos/800x480/landscape-cloud-texture.png
python3 tools/bitstream-converter/convert_pngs.py image_sources --scheme gray16
python3 tools/bitstream-converter/convert_pngs.py image_sources --scheme all
```

## Scheme Selection

The converter uses this order:

1. `--scheme mono|gray4|gray16|bwr|bwy|bwry|bwgbry|all`.
2. Scheme token in filename.
3. Lossless exact-color mode if the PNG has 16 or fewer colors.
4. For full-color PNGs without scheme tokens, generate `mono`, `gray4`, and `gray16`.

Full-color images are Burkes-dithered into the target palette. Exact-color PNGs
are packed losslessly with palette indices assigned in row-major first-seen
order.

## Packing

`.bs-od` uses the same row-major MSB-first 1/2/4bpp packing rules as
`py-opendisplay`.

`.bs-1bppstreams` splits the final packed palette values into strict 1bpp
planes, LSB plane first, for G5 benchmarking.
