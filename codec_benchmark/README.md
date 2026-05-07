# Codec Benchmark

Build:

```bash
make -C codec_benchmark
```

Generate sample inputs and run all codecs:

```bash
make -C codec_benchmark check
```

Manual use:

```bash
./codec_benchmark/build/compressor_benchmark zlib <source_folder>
./codec_benchmark/build/compressor_benchmark heatshrink <source_folder>
./codec_benchmark/build/compressor_benchmark g5 <source_folder>
```

Inputs use resolution-bearing bitstream names:

```text
<stem>.<width>x<height>.bs-od
<stem>.<width>x<height>.bs-1bppstreams
```

`zlib` and `heatshrink` consume `.bs-od`. `g5` consumes `.bs-1bppstreams`.
