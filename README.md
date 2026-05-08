# OpenDisplay Compression Benchmark

This repository is for testing compression candidates for OpenDisplay image transfer.

The current firmware/client flow uses zlib-compatible compressed payloads. The goal of this benchmark is to compare that baseline against lower-memory options while keeping the input data close to what firmware would actually receive.

## Layout

```text
codec_benchmark/      C++ benchmark CLI and codec integrations
image_sources/        RGB source image corpus for benchmark conversion
tools/                Deterministic image source generators
results/              Suggested place for copied benchmark outputs/summaries
```

Generated bitstreams are written beside their source PNGs under `image_sources/` so it is easy to compare the source image, packed payloads, and compressed outputs for the same asset.

## Image Sources

The prepared source corpus lives in `image_sources/`. These are normal RGB PNG/JPEG source images; palette quantization, OpenDisplay packing, and 1bpp plane extraction are handled by the bitstream converter before compression benchmarking.

The corpus currently contains `493` source files:

```text
image_sources/generated/originals/    9 deterministic 2560x1440 generated masters
image_sources/generated/<resolution>/ 9 generated images resized into each target resolution
image_sources/photos/originals/       4 free photo originals
image_sources/photos/<resolution>/    4 photo crops resized into each target resolution
image_sources/stress/<resolution>/    19 deterministic stress images generated per resolution
```

Target resolutions:

```text
2560x1440
1872x1404
1600x1200
1360x480
1024x576
960x640
800x480
648x480
600x448
400x300
296x128
200x200
152x296
128x296
122x250
```

`2560x1440` is the largest regular source size. Generated regular-use scenes are authored at that size and resized down. Stress scenes are generated independently at every target resolution so they keep exact 1px, 2px, random, and interleaved patterns at the final dimensions.

Generated regular-use masters:

```text
dashboard-mixed-density.png
status-panel-large-numbers.png
price-label-barcode-layout.png
price-tag-electronics-promo.png
price-tag-grocery-shelf.png
price-tag-warehouse-markdown.png
menu-board-text-heavy.png
floorplan-thin-lines.png
text-glyph-density-grid.png
```

The three `price-tag-*` images are deterministic electronic shelf label content scenes with large price text, promo blocks, dense barcodes, QR-like blocks, product-like graphics, and hard color boundaries. They intentionally exclude the physical device frame, bezel, flex cable, and housing so the benchmark input represents display content rather than product photography.

Photo originals are free Wikimedia-hosted images, stored under `image_sources/photos/originals/` with descriptive filenames:

```text
foliage-high-frequency.jpg
interior-room-soft-tones.jpg
landscape-cloud-texture.jpg
product-shelf-detail.jpg
```

Stress images cover patterns expected to either compress unusually well or unusually badly after palette conversion:

```text
checkerboard-mono-1px
checkerboard-mono-2px
checkerboard-bwr-1px
checkerboard-bwy-1px
checkerboard-bwry-1px
checkerboard-bwgbry-1px
checkerboard-gray16-1px
random-noise-mono
random-noise-bwr
random-noise-bwy
random-noise-bwry
random-noise-bwgbry
random-noise-gray16
diagonal-hatch-multi-angle-mono
color-plane-interleave-bwr
color-plane-interleave-bwy
color-plane-interleave-bwry
mixed-entropy-bands-gray16
smooth-gradient-dither-trap
```

The palette names map to the display color schemes we want to exercise later: mono, 16-level gray, black-white-red, black-white-yellow, black-white-red-yellow, and black-white-green-blue-red-yellow. The source images remain RGB so the preprocessor can test multiple quantizers and bitstream layouts from the same corpus.

Regenerate deterministic image sources from this folder with:

```bash
dotnet run --project tools/source-generator/SourceGenerator.csproj
dotnet run --project tools/stress-generator/StressGenerator.csproj
```

## Benchmark Pipeline

Use the tools in this order:

1. Gather or generate source images under `image_sources/`.
2. Run one of the source generator tools when you need deterministic resized/stress PNG variations.
3. Run the PNG bitstream converter:

```bash
python3 tools/bitstream-converter/convert_pngs.py image_sources
```

4. Run the codec benchmark on folders containing generated `.bs-*` files:

```bash
./codec_benchmark/build/compressor_benchmark zlib image_sources/photos/800x480
./codec_benchmark/build/compressor_benchmark heatshrink image_sources/photos/800x480
./codec_benchmark/build/compressor_benchmark g5 image_sources/photos/800x480
```

The bitstream converter processes PNG files only and skips any path below an
`originals` folder. It emits `.bs-od` files for zlib/heatshrink and
`.bs-1bppstreams` files for G5.

## Full Benchmark Workflow

Run the full benchmark in two phases.

Phase 1 is preparation: build/update the tools and make sure `.bs-*` files exist
beside the PNGs. Do not start the full compression pass until the bitstream
corpus is ready.

Phase 2 is the actual run. Create or reuse a folder under `results/`, run the
benchmark runner over folders containing `.bs-*` files, and write JSONL results:

```bash
mkdir -p results/run-YYYYMMDD-HHMMSS
python3 tools/run_benchmark.py --results-dir results/run-YYYYMMDD-HHMMSS --runs 1
```

`tools/run_benchmark.py` appends to existing result files by default and refreshes
`summary.csv` from the accumulated `compression.jsonl`. Use `--replace` only when
you intentionally want to clear a result directory before running. To add results
for a new algorithm later, add it to `compressor_benchmark` and run:

```bash
python3 tools/run_benchmark.py --results-dir results/run-YYYYMMDD-HHMMSS --runs 1 --algorithm newalg
```

`--runs` defaults to `1`. The reported `avg_ms` is always the arithmetic mean of
the requested runs; with one run it is the single measured runtime. G5 flip-limit
failures are emitted as JSONL failure rows instead of aborting the whole run.

## Input Bitstreams

The codec benchmark consumes bitstreams, not images directly.

Use these filename forms:

```text
<stem>.<scheme>.<width>x<height>.bs-od
<stem>.<scheme>.<width>x<height>.bs-1bppstreams
```

`bs-od` is the current OpenDisplay packed pixel stream. It is used by zlib and heatshrink.

`bs-1bppstreams` is one or more strict 1bpp planes concatenated together. It is used by G5. The benchmark infers plane count from file size:

```text
plane_size = ((width + 7) / 8) * height
plane_count = file_size / plane_size
```

Supported G5 plane counts are `1`, `2`, and `4`.

## Build

```bash
make -C codec_benchmark
```

The Makefile fetches heatshrink and G5 into `codec_benchmark/_deps/`. zlib is expected from the system development package.

There is also a CMake project in `codec_benchmark/`, but this environment only verified the Makefile path.

## Quick Verification

Generate tiny 1bpp, 2bpp, and 4bpp fixture bitstreams, then run every codec:

```bash
make -C codec_benchmark check
```

This is a functionality check only. The generated 16x16 fixtures are too small to represent real OpenDisplay image compression ratios.

## Manual Use

```bash
./codec_benchmark/build/compressor_benchmark [--runs N] [--jsonl results/run/compression.jsonl] zlib <bitstream_folder>
./codec_benchmark/build/compressor_benchmark [--runs N] [--jsonl results/run/compression.jsonl] heatshrink <bitstream_folder>
./codec_benchmark/build/compressor_benchmark [--runs N] [--jsonl results/run/compression.jsonl] g5 <bitstream_folder>
```

The selected algorithm runs all configured variants and writes `.dat` files beside the source bitstreams:

```text
<stem>.<width>x<height>.<algorithm>.<variant>.dat
```

## Variants

zlib:

```text
zlib.current       level 6, windowBits 15; current Python zlib.compress behavior
zlib.l9-ws9       level 9, 512 B decoder window
zlib.l9-ws12      level 9, 4 KB decoder window
zlib.l9-ws15      level 9, 32 KB decoder window
```

heatshrink:

```text
heatshrink.w9-l4
heatshrink.w11-l5
heatshrink.w13-l6
```

G5:

```text
g5.virtual-f64
g5.perplane-f128
g5.perplane-f256
```

For G5, `fNN` is the compiled `MAX_IMAGE_FLIPS` limit used for that variant. This lets the benchmark catch streams that would exceed a target MCU decoder-state budget.

## Output

Each run prints:

```text
algorithm variant file resolution input_bytes compressed_bytes ratio avg_ms
```

It then prints an average compressed/input ratio over successful outputs.

Compression defaults to one run. The benchmark writes one compressed output and verifies decompression against the exact codec input.
