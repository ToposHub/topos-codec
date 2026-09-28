# Topos Image (.toos) White Paper

**A Still-Image Intermediate Format Built on the Topos Video Codec Core · V1 Preview**

Version 1.0 · 2026-09-05 · Released under the Apache License 2.0

---

## 1. Introduction

**Topos Image (`.toos`)** is a still-image intermediate format built on top of the Topos Video Codec core: one `.toos` file = a **TPIM file envelope** + **one complete TPIC intra-frame packet** — the exact same single-frame coded packet used by the video pipeline.

The format rests on one load-bearing constraint, which is also its main reliability guarantee: **the file layer (TPIM) performs no pixel codec mathematics whatsoever**. Beyond envelope, directory, CRC and image-semantic metadata, pixel coding is 100% shared with the same `libtopos_codec` core (exact integer transform, quantization matrices, V2 canonical VLC entropy coding). The specification freezes a "no second implementation" rule, which structurally prevents divergence between the video and image decode paths.

Positioning: a high-fidelity image intermediate format for VFX compositing, grading, texturing and render caches — **high quality + self-describing metadata + atomic writes + sequence-friendly** — while decoding faster than PNG.

## 2. Design Goals

| Goal | Description |
| --- | --- |
| One core with video | Pixel coding 100% shared with the video codec; the same golden gates cover both; zero implementation fork |
| Deterministic output | Identical inputs produce byte-identical files; `tc_image_write` is reproducible by contract |
| Verifiable structure | 64-byte preamble + directory + per-chunk CRC-32; `toos verify --deep` streams the full check |
| Self-describing metadata | ICC profile, OCIO config name, EXIF, XMP and a SHA-256 fingerprint travel with the image |
| Alpha as a first-class citizen | straight (lossless A16) and premultiplied (bounded, declared error) are explicitly distinct semantics |
| Atomic persistence | temp file in the same directory → fully written → validated → renamed; failures clean up — never a half-written image |
| Explicit capability bounds | capabilities are machine-readable (manifest + `tc_image_query_capabilities`); unsupported means rejected, never guessed |

## 3. File Format Essentials

- **Extension**: `.toos` (the only one); MIME `image/x-toos`; file type is determined by the `TPIM` magic. The inner coded packet keeps the video pipeline's `TPIC` magic.
- **Container**: 64-byte preamble (version / file size / directory offset / CRC-32) → 32-byte directory entries × n (chunk FourCC / flags / offset / size / crc32) → chunks; **all big-endian**; serializing raw C structs is forbidden.
- **Chunk system**:
  - critical: `IDSC` (128-byte image description, frozen field-by-field) + `PIXL` (the complete TPIC packet, ≤ 256 MiB);
  - optional: `ICCP` (ICC profile) / `OCIO` (config name) / `EXIF` / `XMP` / `THMB` (dispensable thumbnail) / `HASH` (SHA-256);
  - `TILE` / `MIPM` etc. are v1-reserved and handled as unknown (unknown *critical* chunks refuse to decode — forward-safe).
- **Consistency gate**: ten cross-checks between `IDSC` and `PIXL` (dimensions / profile / bit depth / alpha / CICP / SAR / header CRC …), plus `tc_image_derive_idsc` to derive the description from the packet — hand-assembling an inconsistent one is impossible by design.
- **Hard limits**: 8192×8192 pixels; bounded parsing allocations (directory + IDSC ≤ 64 KiB).
- **Explicitly out of scope for v1**: half/float precision, tiles/ROI, mipmaps, multi-image, in-browser decoding.

## 4. Color Modes

| ID | Name | Pixel format | Bit depth | Status & positioning |
| --- | --- | --- | --- | --- |
| 0 | Image Preview | YUV 4:2:2 | 10/12 | size-first; chroma subsampled (explicitly not color-lossless) |
| 1 | Image HQ | GBR 4:4:4 | 10 | **the main tier for VFX beauty / textures / render caches**; RGB pass-through, no YUV round trip |
| 2 | Image XQ | GBR 4:4:4 | 12 | high-precision integer intermediate |
| 3 | Image RAW (TRAW) | Bayer CFA 4 phase planes (R/Gr/Gb/B, each W/2×H/2) | 12/16 | camera-RAW production tier: 12-bit Log (transfer=TRAW_LOG0) production / 16-bit Linear archival; the decoder passes phase planes through and debayer/ISP is consumed by the application. Re-assigned from reserved value 3 (Image Lossless) on 2026-09-13 — lossless semantics are carried by the RAW Linear mode |
| 4 | Image HF (float) | GBR 4:4:4 | 16 (float16) | HALF sample domain (spec §15): HDR linear compositing intermediate; the sample domain is self-described by IDSC sample_kind and enters the same integer encode kernel through a frozen monotone map |

The GBR 4:4:4 path (identity matrix) lets RGB compositing sources avoid any YUV round trip; the RAW path keeps native camera CFA data unbaked until a non-destructive ISP pass.

## 5. Encoding Tier System

All 22 tiers (`toos encode --tier`; aligned three-way with the CLI preset table and `docs/capability_manifest.json` as of 2026-09-21 — application/UI names derive from the same registry). Anchors are CLI-measured per-frame file sizes:

**Integer families** (YUV 4:2:2 and GBR 4:4:4):

| Tier | Format | Q-matrix | qp | ≈MB/frame (1080p) | Fidelity & use |
| --- | --- | --- | ---: | ---: | --- |
| **Topos 422 Low** | YUV 4:2:2 10-bit | 422 Low Compact | 63 | 1.08 | preview / review / proxies |
| **Topos 422 Medium** | YUV 4:2:2 10-bit | Standard | 61 | 1.29 | smaller than PNG 8-bit on the same material |
| **Topos 422 High** | YUV 4:2:2 10-bit | Standard | 58 | 1.71 | the PNG-anchored tier (PNG 8-bit ≈ 2.04 MB) |
| **Topos 422 Ultra** (default) | YUV 4:2:2 10-bit | flat | 58 | 2.86 | visually lossless class |
| **Topos 444 High** | GBR 4:4:4 10-bit | Standard | 63 | 1.30 | full-chroma compositing compact tier (revision 6: ProRes 4444 depth parity) |
| **Topos 444 Ultra** | GBR 4:4:4 10-bit | flat | 58 | 3.40 | full-chroma visually lossless class |

**Float family** (GBR 4:4:4 float16, image_profile 4 + linear transfer; v1.8 product naming ADR-C057 — **not lossless-chasing**: smaller files + very good quality + real-time encode/decode; anchors = mean of three real 2K video frames, spec §15.3):

| Tier | Format | qp | ≈MB/frame (1080p) | Fidelity & use |
| --- | --- | ---: | ---: | --- |
| **toos Low 16bit float** | GBR float16 | 82 | 0.38 | review proxy (PSNR ≈ 36 dB) |
| **toos Medium 16bit float** | GBR float16 | 72 | 0.99 | proxy (PSNR ≈ 44 dB) |
| **toos High 16bit float** | GBR float16 | 58 | 4.45 | high quality (PSNR ≈ 71 dB, 2.6–3.2:1) |
| **toos Ultra 16bit float** | GBR float16 | 44 | 5.75 | visually transparent (±2 half-ULP; smaller than EXR half zip16 on the same frame) |

**RAW family** (TRAW: pf=3 CFA 4 phase planes + profile 7 + frozen qm0; input is 4 phase planes R/Gr/Gb/B concatenated as uint16 LE. Ratio names are CQ positioning labels — size floats with content, no rate control. 12-bit = Log production (transfer=TRAW_LOG0), 16-bit = Linear archival):

| Tier | Format | qp | Nominal anchor¹ (≈MB/frame 1080p) | Positioning |
| --- | --- | ---: | ---: | --- |
| **Topos RAW 12-bit 2:1** | CFA 12-bit Log | 45 | 2.07 | near-lossless production |
| **Topos RAW 12-bit 4:1** (RAW default) | CFA 12-bit Log | 59 | 1.04 | transparency-line production tier |
| **Topos RAW 12-bit 6:1** | CFA 12-bit Log | 64 | 0.69 | production tier |
| **Topos RAW 12-bit 8:1** | CFA 12-bit Log | 66 | 0.52 | production tier |
| **Topos RAW 12-bit 12:1** | CFA 12-bit Log | 68 | 0.35 | production tier |
| **Topos RAW 12-bit 16:1** | CFA 12-bit Log | 70 | 0.26 | review proxy |
| **Topos RAW 16-bit 2:1** | CFA 16-bit Linear | 51 | 2.07 | near-lossless archival (measured 1.5–2.0:1) |
| **Topos RAW 16-bit 4:1** (RAW default) | CFA 16-bit Linear | 72 | 1.04 | archival default (measured 3.0–6.2:1) |
| **Topos RAW 16-bit 6:1** | CFA 16-bit Linear | 76 | 0.69 | archival (measured 3.8–10.8:1) |
| **Topos RAW 16-bit 8:1** | CFA 16-bit Linear | 78 | 0.52 | archival (measured 4.4–14.7:1) |
| **Topos RAW 16-bit 12:1** | CFA 16-bit Linear | 80 | 0.35 | archival (measured 5.2–21.1:1) |
| **Topos RAW 16-bit 16:1** | CFA 16-bit Linear | 82 | 0.26 | review proxy (measured 6.5–31.9:1) |

> ¹ RAW anchors are derived from the nominal ratio (CFA single-plane 16-bit container nominal 4.15 MB per 1080p frame ÷ N), not a measured promise — actual ratios float with content.

### 5.1 Naming Rules (unified across the product line)

| Rule | Content | Example |
| --- | --- | --- |
| N-1 Family prefix | video line `Topos` (capitalized, MOV container); image line `toos` (lowercase, `.toos` files) | `Topos 422 HQ` / `toos High 12bit` |
| N-4 Image integer tiers | `toos <Quality> <bd>bit`, Quality ∈ {Low, Medium, High, Ultra} (422 quality ladder; the 444 family is not in the ladder and falls back to the Image HQ class name in-file) | `toos High 12bit` |
| N-5 Image float tiers | `toos <Quality> <bd>bit float` (HALF domain; a future 32-bit tier only changes the depth number) | `toos Ultra 16bit float` |
| N-6 Image RAW tier (in-file/TMET name) | `toos RAW <bd>bit <N>:1` — lowercase image-family prefix, bit depth without hyphen; spec/whitepaper prose always uses the hyphenated N-3 form `Topos RAW <bd>-bit <N>:1` | `toos RAW 12bit 4:1` |
| N-7 Single registry | every output-specification surface (image-sequence export / Quick Export / CLI `--tier` / probe display / TMET label) derives from one tier registry (`src/shared/codec/topos_profiles.py`, `TOPOS_IMAGE_TIERS`) | — |
| N-8 Words tier names never carry | Log/Linear, lossless, experimental and similar positioning words live in prose and docs only | — |

Product semantics, stated honestly:

- The tiers target **"excellent quality at a relatively small size" — there is no mathematically lossless tier**; true lossless is `--qp 28` (10-bit), `--half --qp 20/32` (float domain, spec §15), or a lossless container (PNG/EXR/TIFF).
- The qp domain is 0–95 (v1.5 extension); float/RAW tier anchors sit at 44–82.
- **The size gap versus the video tiers is structural**: image frames are self-contained (random access + metadata + atomic writes) and lack the video pipeline's per-frame bitrate closed loop — a property, not a defect; the two size tables must not be compared directly.
- For proxy workflows a half-resolution pipeline is recommended: 960×540 + 422 Low ≈ 0.4 MB/frame (≈ 30% of full-resolution Low).
- Tiers are **explicitly queryable capability declarations**: with no `--tier`, the integer default is 422 Ultra (qm0 qp58); RAW defaults to raw12-4 (log production transparency line).

## 6. Alpha and Metadata

- **Three explicit states**: absent / straight (A16 container, lossless pass-through 0..65535) / premultiplied (bounded, must declare `alpha_bit_depth` ∈ {8,10,12} and the measured `alpha_max_abs_err`).
- Alpha is a full-resolution independent plane: never subsampled, never pushed into the color dead-zone; RGB coded values at alpha=0 are preserved verbatim.
- Metadata chunks are read with validation (ICC must contain the `acsp` signature, OCIO names must be printable UTF-8); conflicts are hard errors, never silently swallowed.

## 7. Sequence Support

First-class render-sequence support (`ToposImageSequence` / MediaSource protocol):

- **Frozen frame-number grammar**: `<prefix><frame><suffix>`, negative numbers allowed, optional zero padding; padding width is strict (`frame_1` and `frame_0001` are different sequences).
- **Step anchor semantics**: `(frame - anchor) % step == 0`, anchored to absolute frame numbers — independent of which files happen to exist in the directory.
- **Missing-frame policies**: skip (jump to nearest successor) / error (hole in range fails) / hold (reuse nearest predecessor, bounded consecutive holes) — a missing frame is never silently faked.
- **Bounded resources**: one background prefetch thread + generation cancel (seek invalidates), an LRU byte budget for decoded planes; frame-level parallelism = 1 × slice-level parallelism, never oversubscribing the CPU.

## 8. Usage

### 8.1 Building

The image layer builds together with `libtopos_codec` (same CMake project, no extra dependencies):

```bash
cmake -S native -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j          # produces libtopos_codec + the toos CLI
```

### 8.2 C API (`topos_image.h`, ABI v1, 38-symbol export gate)

```c
tc_image_probe(file, &info);                       /* header-only probe, zero pixel allocation */
tc_image_validate(file, TC_IMG_VALIDATE_DEEP);     /* structure + streamed per-chunk CRC */
tc_image_decode(file, planes);                     /* caller-provided planes, zero-copy */
tc_image_query_capabilities(&caps);                /* capability negotiation */
tc_image_derive_idsc(packet, &idsc);               /* derive a consistent description */
tc_image_write(file, &idsc, planes, &meta_io);     /* deterministic atomic write */
```

### 8.3 CLI (`toos`)

```bash
toos probe   image.toos                    # parse and print the image description
toos verify  image.toos --deep             # structure + full CRC
toos encode in.raw -o image.toos --width 1920 --height 1080 \
            --pixel-format 1 --bit-depth 10 --tier 444-high
toos benchmark image.toos --iters 20       # probe/validate/decode percentile timings
```

Without `--tier` the default is 422 Ultra (qm0 qp58, VLC + AQ/RDO); explicit parameters override the tier.

### 8.4 Python

```python
from topos_codec.topos_image_binding import ToposImageCodec   # release package; app repo: src.shared.codec.topos_image_binding
img = ToposImageCodec()
info = img.probe("image.toos")
planes = img.decode("image.toos")            # zero-copy numpy
```

Application-side, `ToposImageSource` (single image) and `ToposImageSequence` implement the MediaSource protocol; pixel-format names and metadata keys are identical to the video pipeline.

### 8.5 PNG Interchange

```bash
python scripts/toos_tools.py decode image.toos -o image.png     # 16-bit PNG
python scripts/toos_tools.py encode image.png -o image.toos \
       --bit-depth 10 --image-profile 1
```

YUV → PNG conversion is explicitly rejected (sRGB is never silently guessed), preventing color-semantic mistakes.

## 9. Performance

Release build, 1080p photographic material (ADR-I010 protocol):

| Metric | Value |
| --- | --- |
| Encode (in-process, conservative bounds) | 16–33 ms/frame |
| Decode (V2 VLC) | ≈ 13.9 ms/frame |
| Reference: PNG 8-bit decode | 31.5 ms (16-bit 56 ms) — **toos decodes 2.3× faster** |
| Per-sequence size (post VLC default) | 12.3 → 3.28 MB/frame (−73%), 1.6× PNG 8-bit (≈ 1.2× at the qp61 tier) |
| probe (header-only) | ≈ 0.017 ms |

Envelope overhead is ≈2% (4K) to 8% (1080p) of whole-image decode; plain pread already reaches ≈ 2.4 GB/s, so mmap stays off.

## 10. Completeness and Boundaries (V1 Preview, stated honestly)

- **Done**: TPIM parser/writer + security infrastructure, GBR 10-bit, 12-bit + metadata, full ABI + CLI + atomic write + PNG interchange, first performance measurements. Gates: unit tests + ASan/UBSan + fuzz + 38-symbol ABI pinning + 5 image golden vectors + 68 image pytest cases, with zero regression on the video golden suite.
- **Delivered with deferrals**: three application wiring points (export / thumbnails / import probing), UI/playback integration, Windows/Linux builds and ABI smoke, 1000+ frame stress, OIIO plugin round-trip acceptance (source delivered, awaiting an SDK environment).
- **Explicitly out of scope**: Nuke / Photoshop / After Effects plug-ins (separate future projects).
- **Open risks (title-level)**: high-entropy 8K RGBA approaching the 256 MiB packet cap; upstream codec evolution vs image golden vectors (covered by the golden gate).

## 11. Licensing

Released together with Topos Video Codec under the **Apache License 2.0** (see `LICENSE`). The image layer shares the library and the license.

**Trademarks**: PNG and TIFF are the names of their respective public formats; OpenEXR and OpenImageIO are the names of their respective projects. Those names appear here solely for factual interoperability and comparison.

## 12. Specification Index

| Document | Contents |
| --- | --- |
| `docs/topos_image_file_spec_v0.md` | TPIM file format specification (v0 frozen) |
| `docs/SDK.md` | integration guide |
| `docs/capability_manifest.json` | machine-readable capability manifest (color modes / tiers) |
| `docs/ADR-I001…I011` | all image-layer architecture decision records |
| `docs/benchmark_protocol.md` | performance measurement protocol |

---

*Topos Image · Copyright (c) 2026 Topos Color project authors · Apache License 2.0*
