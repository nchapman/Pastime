# libwebp 1.5.0 — vendored decoder

Decoder-only subset of upstream libwebp, vendored for Downplay's
thumbnail pipeline. Imported from the v1.5.0 release tag at
https://github.com/webmproject/libwebp.

## What's included

- `src/dec/` — full decoder
- `src/dsp/` — DSP primitives, decode-only (encoder DSP and per-arch
  encoder variants are excluded; `cost*.c`, `enc*.c`, `lossless_enc*.c`,
  `ssim.c` are not present)
- `src/utils/` — utilities used by the decoder (encoder utilities like
  `bit_writer_utils`, `huffman_encode_utils`, `quant_levels_utils` are
  not present)
- `src/webp/` — public headers (`decode.h`, `encode.h`, `format_constants.h`,
  `mux_types.h`, `types.h`)

The `mux/`, `demux/`, and `enc/` directories from upstream are not
vendored — Downplay decodes single still images, not animated WebP or
encoded streams.

## License

BSD-3-clause (see `COPYING`) plus `PATENTS` grant.

## Re-syncing

To bump to a newer libwebp release, run the file-list copy in the
prefetch-optimization commit's history (search `git log` for
"vendor libwebp"). The decoder file list rarely changes between
releases; check upstream `src/dsp/Makefile.am` `COMMON_SOURCES` /
`libwebpdspdecode_*_la_SOURCES` and `src/utils/Makefile.am`
`COMMON_SOURCES` for any new entries.
