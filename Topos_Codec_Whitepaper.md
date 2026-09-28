# Topos Video Codec White Paper

**All-Intra Mezzanine Codec Engine · V1 Preview (10/12/16-bit 4:2:2 / 4:4:4 / GBR)**

Version 1.1 · 2026-09-21 · Released under the Apache License 2.0

---

> **Trademark & affiliation notice.** Topos Video Codec is an independent,
> originally developed codec format. It is not affiliated with, sponsored by,
> or endorsed by Apple Inc., Avid Technology, Inc., or Blackmagic Design Pty
> Ltd. The names Apple, ProRes and QuickTime (trademarks of Apple Inc.,
> registered in the U.S. and other countries), DNxHR (trademark of Avid
> Technology, Inc.) and DaVinci Resolve (trademark of Blackmagic Design Pty
> Ltd.) appear in this document solely to identify and compare the respective
> technologies. All comparative figures in this paper are measured against
> FFmpeg's open-source software implementations under the bundled benchmark
> protocol — not against any vendor's official implementation.

## 1. Introduction

Topos Video Codec (`libtopos_codec`) is an **all-intra video codec engine for
mezzanine (intermediate) production workflows**, occupying the same ecosystem
role as Apple ProRes and Avid DNxHR: providing an intermediate bitstream with
**consistent quality, frame-independent access, inexpensive decoding, and
multi-generation stability** for editing, grading, compositing, and
multi-pass rendering.

Unlike comparable formats, Topos was designed from day one for
**real-time, high-resolution workflows**:

- **Pure C11, zero third-party dependencies** — the library links only libc
  and pthread/Win32 threading primitives. There is no FFmpeg or third-party
  library dependency, so it can be embedded in any host application without
  transitive dependencies.
- **Explicitly queryable quality and bitrate** — the seven profiles are exposed
  as machine-readable *capability declarations*, not implicit parameters;
  bitrate targets, bit depth, and alpha policy are all introspectable.
- **Deterministic frame-level rate control** — given a target bitrate, each
  frame independently performs an exact QP search with reproducible output.
- **Self-describing bitstream** — decoders negotiate the format from the
  frame header; files written by older versions remain readable forever.

This paper is written for integrators, workflow engineers, and format
evaluators. It describes the design goals, the profile family, technical
specifications, and usage. Deeper detail is available in the companion
specifications (`docs/bitstream_spec_v2.md`, `docs/container_spec_v1.md`, `docs/SDK.md`).

## 2. Design Goals

| Goal | Description |
| --- | --- |
| Production-grade quality | 10/12/16-bit quantization, exactly invertible integer transform, frozen quantization matrix (QM v1.1) — no quality drift across generations |
| Real-time decoding | All-intra + O(1) indexed random access: ≈ 5 ms/frame at 1080p, ≈ 13 ms/frame at 4K (see §8) |
| Deterministic output | Same input plus same parameters always yields the same bitstream; frozen golden vectors verify byte-exact reproducibility |
| Portable, zero dependencies | C11 + libc + pthread; first-class CI on Linux / macOS (universal) / Windows (MSVC & MinGW) |
| Explicit capability negotiation | Profiles are queryable capabilities (`tc_query_support` / capability manifest); implicit parameter guessing is disallowed |
| Mezzanine purity | Pixel format and color metadata travel with the stream; v1.1 adds an optional single audio track (lossless lpcm / AAC delivery, ADR-C051); the video-only file form is unchanged |

## 3. Technical Overview

**Encoding pipeline** (per frame, no inter-frame prediction):

```
Input planes (YUV 4:2:2/4:4:4, GBR; 10/12/16-bit; optional alpha plane)
  → exactly invertible integer transform (no floating-point drift)
  → frozen quantization matrix QM v1.1 + per-tier target-bitrate QP search
    (or explicit QP)
  → entropy coding: V2 canonical VLC (default) / bounded Rice (V1, selectable)
  → slice-parallel bitstream assembly (bounded resident thread pool,
    runtime CPU-feature dispatch)
  → MOV-derived container (.mov, FastStart metadata, O(1) sample index)
```

Key characteristics:

- **Dual-mode entropy coding**: the default V2 canonical VLC saves
  **23–32% bitrate at equal quality** versus the V1 bounded Rice coder; the
  bitstream is self-describing, so V1 files remain decodable forever.
- **Runtime-dispatched SIMD**: AVX2 (transform + RGB↔YUV color conversion)
  and NEON (Apple silicon), selected via runtime CPU feature detection — no
  multi-variant binaries.
- **Exact color conversion**: RGB↔YUV with full/limited range and
  BT.709/BT.2020-class matrices, bit-aligned against the reference floating
  point implementation.
- **Slice-level concealment**: on localized bitstream damage the decoder
  conceals affected slices and returns `TC_WARN_CONCEALED` instead of failing
  the frame.
- **Streaming decoder**: a persistent decoder context supports frame-by-frame
  and batch decoding with zero-copy plane output.
- **Alpha channel**: every intra tier can optionally carry alpha
  (approximation mode with 12-bit internal fidelity by default, delivered as
  a8/a10/a12/a16) with an independent closed-loop bitrate budget (per-tier
  budget ratio and hard cap); the LP inter-frame tier is explicitly
  no-alpha.

## 4. The Topos Profile Family

The eight tiers mirror the positioning of the ProRes family to make workflow
mapping easy. **422 Net Proxy / 422 Proxy / 422 LT / 422 / 422 HQ** share a
single 4:2:2 bitstream profile (profile=3) and differ only in target bitrate
and alpha policy (Net Proxy is the low-rate network tier, not a
ProRes-comparable tier); **4444** (profile=5) and **4444 XQ** (profile=6)
are independent bitstream profiles distinguished by the profile byte in the
frame header; **422 LP** (v1.8, ADR-C056) is the first non-intra tier — an
inter-frame micro-GOP (zero-motion IP-2, bitstream carrier V7-R3) with fixed
anchor qp=72, no alpha, and no rate control; its size band sits at ≈89–94%
of LT and it encodes in real time. Tier (quality) is orthogonal to
inter-frameness: inter-frameness is self-described by the packet header,
while the tier only describes quality-tier semantics. The video **RAW** tier
(v1.9, ADR-C058, container tier 8) is declared ahead of its encode chain and
activates with the video RAW productization.
The `Topos` brand always precedes the tier name: these are distinct,
self-contained Topos profiles, not interoperable with any other vendor's
format.

### 4.1 Tier Specification Table

| Tier | Pixel format | Bit depth | Reference data rate @1080p25¹ | Target bpp² | Alpha budget / hard cap | Primary use |
| --- | --- | --- | --- | --- | --- | --- |
| **Topos 422 Net Proxy** | YUV 4:2:2 | 10 | 15 Mbps | 0.291 | 20% / 25% | Low-bandwidth remote preview, network transcode (not ProRes-comparable) |
| **Topos 422 Proxy** | YUV 4:2:2 | 10 | 33 Mbps | 0.645 | 20% / 25% | Offline proxies, remote editing |
| **Topos 422 LT** | YUV 4:2:2 | 10 | 70 Mbps | 1.352 | 20% / 25% | Lightweight intermediates, rough cuts |
| **Topos 422** (default) | YUV 4:2:2 | 10 | 109 Mbps | 2.112 | 25% / 30% | General editing, render caches |
| **Topos 422 HQ** | YUV 4:2:2 / 4:4:4 / GBR | 10/12/16 | 163 Mbps | 3.151 | 25% / 30% | Grading, intermediate mastering |
| **Topos 4444** | YUV/GBR 4:4:4 | 10/12/16 | 221 Mbps | 4.263 | 25% / 30% | VFX, motion graphics, compositing |
| **Topos 4444 XQ** | YUV/GBR 4:4:4 | 12 | 326 Mbps | 6.288 | 30% / 30% | High-dynamic-range and multi-generation work |
| **Topos 422 LP** (inter-frame) | YUV 4:2:2 | 10 | ≈42–64 Mbps (fixed anchor qp72) | — (no rate control) | no-alpha | Real-time rough-cut caches, fast transcodes |

> ¹ Reference value from the same-master ProRes-parity corpus (2026-09-21
> recalibration, v1.9). LP is a derived reference band: the actual rate
> floats with content redundancy.
> ² bits-per-pixel anchoring value: the target bitrate scales linearly with
> resolution and frame rate, i.e. `bitrate ≈ bpp × width × height × fps`.
> Tiers are tuning targets, not format ceilings — callers may substitute a
> fixed QP (0–95) for the tier bitrate target, or a byte-exact budget
> (`max_video_bytes` / `max_file_bytes`) for hard file-size caps.
> Tier declarations have a single source: `src/shared/codec/topos_profiles.py`
> (main repo) / `python/topos_codec/topos_profiles.py` (release package),
> cross-pinned against `docs/capability_manifest.json`.
> ³ v1.10 quantization matrices: the 4:2:2 tiers use `qmatrix_id=4`
> "Edge-Balanced" — LF slightly coarser, HF finer (HF:LF step ratio ≈ 0.4) —
> trading the flat-matrix LF surplus for edge fidelity. Measured at constant
> file size (487-frame same-master corpus): edge PSNR +0.66/+0.54/+0.29 dB
> (2K Proxy/Standard/HQ), +1.01/+0.78 dB (4K Proxy/LT); encode/decode speed
> unchanged. At ≥3840 width the Standard/HQ operating points fall back to
> flat (id=0) — measured negative there (Y≈52–60 dB near-transparent).
> See `docs/topos_edge_jag_diagnosis_2026-09-21.md`.
> ⁴ v1.11 output features (2026-09-21, "V7-R4/R5"): frame-header **flags**
> bit 2 = *output deblocking* (post-loop, decoder-side 8×8-grid filter with
> QP-adaptive strength and true-edge protection; spec in
> `src/codec/deblock.h`) — enabled on all 4:2:2 10-bit video tiers. Flag
> bit 3 = *refined QP-scale table* (QP ≥ 64 doubling rate slowed from
> every 4 to every 6 QP; QP ≤ 63 bit-identical to the frozen table) —
> enabled only on 4K Standard/HQ (A/B: +0.53 dB PSNR at −0.1% size; the
> 2K/4K-proxy domain measured negative and keeps the classic table).
> Features are carried in frame `reserved[5]` at the encoder API and ride
> the previously-reserved flags bits in the bitstream: legacy decoders
> cleanly reject the stream (reserved-bits rule), never misread it.
> Target-size (scaled) delivery is not filtered in this release.
>
> **v1.9 recalibration (2026-09-21)**: the 422-family tiers moved from
> equal-quality anchoring to ProRes-capacity defaults (same-master measured
> ProRes bpp × 0.98 × 0.95). The former low-rate Proxy calibration is
> preserved as the separate **Net Proxy** tier (network proxy, not a
> ProRes-comparable tier). Corpus files land at 80–94% of the same-tier
> ProRes file size, with the 98% cap enforced by the budget interface.

### 4.2 Typical Target Bitrates (from the bpp anchoring)

| Tier | 1080p25 | 2160p25 (4K) |
| --- | ---: | ---: |
| Topos 422 Net Proxy | ≈ 15 Mbps | ≈ 60 Mbps |
| Topos 422 Proxy | ≈ 33 Mbps | ≈ 134 Mbps |
| Topos 422 LT | ≈ 70 Mbps | ≈ 280 Mbps |
| Topos 422 | ≈ 109 Mbps | ≈ 438 Mbps |
| Topos 422 HQ | ≈ 163 Mbps | ≈ 653 Mbps |
| Topos 4444 | ≈ 221 Mbps | ≈ 884 Mbps |
| Topos 4444 XQ | ≈ 326 Mbps | ≈ 1304 Mbps |
| Topos 422 LP | ≈ 42–64 Mbps (anchored, no scaling) | ≈ 168–256 Mbps |

### 4.3 Workflow Mapping (informative)

The table below maps Topos tiers to commonly known mezzanine tiers for
orientation only.

| Topos tier | Comparable tier | Bitstream relationship |
| --- | --- | --- |
| Topos 422 Proxy | low-rate 4:2:2 offline tier | low-rate preset of the 422 profile |
| Topos 422 LT | light 4:2:2 intermediate tier | preset of the 422 profile |
| Topos 422 | standard 4:2:2 tier | preset of the 422 profile (family default) |
| Topos 422 HQ | high-rate 4:2:2 mastering tier | high-rate preset; may cross to 444/GBR formats |
| Topos 4444 | 4:4:4 compositing tier with alpha | independent 4:4:4 profile + alpha |
| Topos 4444 XQ | highest-rate 12-bit 4:4:4 tier | independent 12-bit 4:4:4 profile |
| Topos 422 LP | — (size band ≈ the 4:2:2 light intermediate tier) | inter-frame micro-GOP (IP-2), not an intra preset |

> This mapping describes positioning and intended use only; it is not a
> bitrate-equivalence claim. The reference frame rates differ between the
> two families (the Topos reference frame is 1080p25), although the mid-range
> data rates of corresponding tiers are close.

### 4.4 Bitstream Format Evolution

| Version | Frame-header minor | New capability |
| --- | --- | --- |
| v1.0 | 0 | YUV 4:2:2 10-bit (profile 3) |
| v1.2 | 1 | 12-bit enumeration extension |
| v1.3 | 2 | YUV 4:4:4 enumeration extension |
| v1.4 | 3 | GBR 4:4:4 enumeration extension; profiles 5/6 activated |
| v1.5 | — | qp domain extended to 0–95 (intra tier anchors re-anchored, ADR-C031/C043) |
| V7-R3 | (entropy carrier em 8) | inter-frame micro-GOP carrier (LP tier; frame-header profile stays 3; ADR-C047/C048) |

All extensions are backward compatible: decoders negotiate from the
self-describing frame header, and older files remain readable.

### 4.5 Naming Rules (product tier identification guide)

All output-specification surfaces (export panels / Quick Export / CLI /
probe display / container metadata labels) derive from a **single tier
registry** (N-7); every entry point shows identical tier names. Naming
convention:

| Rule | Content | Example |
| --- | --- | --- |
| N-1 Family prefix | video line `Topos` (capitalized, MOV container); image line `toos` (lowercase, `.toos` files) | `Topos 422 HQ` / `toos High 12bit` |
| N-2 Video intra tiers | `Topos 422 <suffix>` / `Topos 4444 <suffix>`: Proxy / LT / (default, no suffix) / HQ / 4444 / 4444 XQ / LP | `Topos 422 LT`, `Topos 4444 XQ`, `Topos 422 LP` |
| N-3 RAW tiers (spec name) | `Topos RAW <bd>-bit <N>:1`: bit depth always carries `-bit`, ratio always carries `:1`, N ∈ {2,4,6,8,12,16}; mode ≡ bit depth (12-bit = log production, 16-bit = linear archival; Log/Linear never enters tier names); video/image share one 12-tier table and qp anchors | `Topos RAW 12-bit 4:1` |
| N-4 Image integer tiers | `toos <Quality> <bd>bit`, Quality ∈ {Low, Medium, High, Ultra} (image line only; see image whitepaper §5.1) | `toos High 12bit` |
| N-5 Image float tiers | `toos <Quality> <bd>bit float` (HALF domain; a future 32-bit tier only swaps the number) | `toos Ultra 16bit float` |
| N-6 Image RAW display name (TMET) | `toos RAW <bd>bit <N>:1` — lowercase image family, no hyphen in bit depth; spec-level docs always use the N-3 hyphenated form | `toos RAW 12bit 4:1` |
| N-7 Single registry | every output-spec surface derives from one tier registry, identical names at every entry point (opening paragraph) | — |
| N-8 Words tier names never carry | Log/Linear, lossless, experimental and similar positioning words live in prose and docs only, never in tier names | — |

Identification guide: the **family prefix** selects the product line
(`Topos` = MOV video / `toos` = `.toos` image files); **bit-depth and ratio
words** select the capability domain (`422` = 4:2:2 chroma subsampling,
`4444`/`444` = 4:4:4 full chroma, `RAW <N>:1` = compressed camera-RAW CFA
tier); the **suffix** (Proxy/LT/HQ/XQ/LP or Low..Ultra) selects the quality
tier. RAW ratios (N:1) are CQ positioning labels — size floats with content
and no exact ratio is promised. Video RAW tiers (`Topos RAW 12-bit 2:1` …
`16-bit 16:1`, container tier 8) land with the video RAW productization
(ADR-C058), using the same spec vocabulary as this section.

### 4.6 Video RAW Tier (TRAW, active in v1.9)

Video RAW is a **bit-depth × ratio parameterized tier family** on a single
tier (container tier id 8): the payload is the Bayer CFA 4 phase planes
(R/Gr/Gb/B, each ceil(W/2)×ceil(H/2), uint16 full scale), encoded
all-intra, with the quantization matrix frozen flat (qm0), full range,
matrix=0 identity, and primaries placeholder 1 (real color semantics are
carried by the application-side RAW parameters/debayer; no camera-native
enum is invented in the shared bitstream domain).

| Spec name | Bit depth | Transfer (frozen pair) | Ratio label | qp anchors (12 tiers shared with the image line) | Carrier |
| --- | --- | --- | --- | --- | --- |
| Topos RAW 12-bit 2:1 … 16:1 | 12 | PWL-log12 (LOG0, code 20) | 2/4/6/8/12/16:1 | 45/59/64/66/68/70 | V1 minor 5 extension; rANS (V7-R2, default) or V1 entropy |
| Topos RAW 16-bit 2:1 … 16:1 | 16 | Linear (code 8) | same | 51/72/76/78/80/82 | V1 minor 6 extension; bd≥13 requires wide-domain entropy (rANS mandatory) |

> ¹ The ratio (N:1) is a CQ positioning label: nominal anchors are derived
> from per-frame 1080p size (16-bit container nominal 12.44 MB/frame ÷ N);
> actual size floats with content redundancy and is not a ratio promise.
> ² TRAW and alpha are mutually exclusive (no-alpha); color metadata is
> frozen (caller tags never enter the bitstream). RAW→RAW direct-out
> semantics (R2): CFA phase planes reach the encoder untouched — any
> pixel-level processing (debayer/grading/geometry) lives outside the
> direct lane, keeping the deliverable in the same domain as the source
> bit-for-bit.

## 5. Color and Alpha

- **Color metadata travels with the stream**: primaries / transfer /
  matrix / range are written into the frame header (BT.709, BT.2020, PQ,
  HLG, DCI-P3 and more) and into the `tpcC`-derived container metadata; the
  application layer can map SMPTE ST 2086 mastering display and
  MaxCLL/MaxFALL into the container.
- **GBR pass-through**: the 4444 / 4444 XQ tiers support GBR 4:4:4
  (matrix=0), so RGB compositing sources avoid a YUV round trip.
- **Alpha**: optional on every tier; the default approximation mode (mode 2)
  runs at 12-bit internal fidelity, and measured bitrate share is virtually
  identical to the lossless mode. The encoder runs a closed-loop *alpha
  budget*: the alpha stream share is steered toward the tier-declared budget
  ratio, converging automatically with reported statistics on overrun.

## 6. Container and Interoperability

- **MOV-derived container (.mov)**: self-contained mux/demux with zero I/O
  dependencies (I/O is driven by user callbacks); **FastStart** places
  metadata ahead of media for instant network playback; the sample index is
  O(1), so random access performs identically to sequential reads.
- **FFmpeg ecosystem**: the bundled patch registers a `libtopos` decoder with
  libavcodec (linking `libtopos_codec`), letting FFmpeg/ffplay decode Topos
  .mov files directly; build script in `packaging/ffmpeg/` (release repo).
- **Capability manifest**: `docs/capability_manifest.json` declares every tier,
  format, and version in machine-readable form for runtime validation by
  integrators (cross-pinned against the native query API).
- **Audio track (v1.1)**: optional single audio track (lossless lpcm /
  AAC delivery, 2.0/5.1/7.1); encoding lives in the application layer
  (native stores packets only). Playback caches and proxies remain
  video-only (ADR-C051 boundary).
- **Audio edit list + re-import (v1.4)**: the AAC track carries a minimal
  single-entry `edts/elst` whose media_time equals the encoder-reported
  priming, so decoders trim the encoder lead-in and A/V lands sample-exact
  (ADR-C052; elst is audio-trak-only — the video track keeps its
  no-gap pts contract). An exported Topos file re-imported into the
  timeline IS the master: playback reads its own audio track via the
  generic MOV demux path; render caches and proxies stay video-only
  (ADR-C008 as revised).
- **Timecode track (v1.5)**: professional-interchange start timecode —
  a trailing `tmcd` track (one 4-byte big-endian start-frame-count sample,
  referenced from the video track via `tref`) with QuickTime semantics
  byte-aligned against `ffmpeg -write_tmcd` output. Drop-frame follows
  SMPTE 12M (2 frames/minute at 30DF, 4 at 60DF); per-frame timecodes are
  derived in the binding layer from the single sample (O(1), no per-frame
  table). Timeline exports carry the sequence start timecode when set;
  files without timecode stay byte-identical to v1.4 (ADR-C055).
- **Multi-track stems (v1.6)**: up to 16 discrete audio tracks
  (declaration order, ids 2..N+1; the timecode track stays last) with
  per-track format and QuickTime track names (udta/©nam) — the
  "Roles as Multitrack QuickTime" interchange shape. Mono layout joins
  stereo/5.1/7.1; the v1.1 single-track APIs remain valid as the
  track-0 view, and single-track files stay byte-identical (ADR-C054).

## 7. Usage

### 7.1 Building (no external dependencies)

```bash
cmake -S native -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j
# or build + run the full gate (Debug/ASan/UBSan/TSan/Fuzz) in one step:
bash native/run_tests.sh
```

### 7.2 C API (excerpt)

```c
#include "topos_codec.h"

/* Capability negotiation: explicit query; unsupported combos return an error */
tc_query_support(3, 0, 10, 2);

/* Frame coding: plain mode (direct QP) or sized mode (per-frame target bytes) */
tc_frame_encode(&cfg, planes, &packet, &stats);
tc_frame_encode_sized(&cfg, planes, target_bytes, &packet, &stats);

/* Container: mux (with alpha budget) → FastStart → random-access demux */
tc_mux_create(...); tc_mux_add_packet(...); tc_mux_finish(...);
tc_movie_open(path, &movie); tc_movie_packet(&movie, pts, ...);
```

Complete compilable examples: `native/examples/encode_decode.c` (C) and
`native/examples/topos_roundtrip.py` (Python).

### 7.3 Python Bindings (ctypes — no CPython extension, Python-version agnostic)

```python
from topos_codec import ToposCodec, ToposMuxFile, ToposMovieFile

codec = ToposCodec()                       # locates the library automatically
codec.query_support(3, 0, 10, 2)           # explicit profile/format/depth/alpha

mux = ToposMuxFile(codec, "out.mov", codec.movie_config(width=1920, height=1080))
for i, planes in enumerate(frames):        # planes: compact u16 plane bytes
    packet, stats = codec.encode_frame(fc, planes)
    mux.add_packet(packet, i * 1000, 1000)
mux.finish(); mux.close()

movie = ToposMovieFile(codec, "out.mov")
frame = codec.decode(movie.packet(0))      # O(1) random access
```

Library resolution order: explicit path / `TOPOS_CODEC_LIB` environment
variable → the package's own `lib/` directory → repository build directories.
Drop the dylib/so/dll into `python/topos_codec/lib/` of the release tree for
configuration-free use; `pip install ./python` exposes the `topos_codec`
package.

### 7.4 CLI Tools

| Tool | Purpose |
| --- | --- |
| `topos_encoder_cli` / `topos_decoder_cli` | command-line encode / decode |
| `topos_probe_cli` | bitstream / container probing |
| `topos_inspect` | packet-level diagnostics (ships with the library) |
| `topos_quality` | quality / bitrate / speed measurement and QM tuning (`perf quick` one-shot benchmark) |

### 7.5 High-Level Encoder Wrapper

`topos_encoder.ToposVideoEncoder` (bundled with the release) provides an
application-grade closed loop: tier → bpp → frame-level sized rate control,
alpha budget closure, atomic temp-file publication, slice threading policy,
and color metadata (SDR/HDR10/HLG/DCI-P3) writing.

## 8. Performance

Measured on one machine (Apple-M1-Pro-class 8-core, release build `-O3`, p50;
full data and protocol in `docs/bench_*_2026-08-31.md`).

**Decoding** (full product path: container read + decode + plane assembly,
8 threads):

| Resolution | Per frame | Throughput |
| --- | ---: | ---: |
| 1080p | 4.93 ms | ≈ 203 fps |
| 4K (3840×2160) | 13.40 ms | ≈ 75 fps |

All-intra + O(1) index: random access matches sequential read performance;
4→8 thread scaling gains −36% at 4K.

**Encoding** (qp20; plain = direct QP, sized = per-frame target rate control
including the exact QP search, ≈ 5 probes on average):

| Configuration | plain | sized (rate-controlled) |
| --- | ---: | ---: |
| 1080p | 26.4 ms/frame (≈ 38 fps) | 50.2 ms/frame (≈ 20 fps) |
| 4K | 90.2 ms/frame (≈ 11 fps) | 180.4 ms/frame (≈ 5.5 fps) |

Same-machine comparison against FFmpeg's software ProRes/DNxHR encoders
(equal-visual-quality protocol): Topos encodes roughly **1.4–3.4×** faster,
with frame-deterministic, reproducible rate control. The cumulative SIMD
optimization trajectory: 1080p plain 2.58×, sized 4.10× (see
`docs/bench_topos_encode_2026-08-31.md`).

> Absolute numbers vary with CPU and memory subsystem; re-measure on your
> target hardware with the bundled `topos_quality perf quick`.

## 9. Quality Assurance

Engineering gates are tied to format freezes:

- **Unit tests**: 31 native test suites (ABI/bitstream/transform/quant/
  container/robustness/OOM/stress);
- **Golden consistency**: 7 frozen vector groups (1,380 transform vectors,
  56 bitstream vectors, full encode chain, dual-frozen container, profile
  matrix, VLC extensions) — any silent drift fails the gate immediately;
- **Fuzzing**: 5 entry points (primitives/bitstream/codec/mov/encode) +
  deterministic replay drivers + seed-corpus self-checks;
- **ABI pinning**: public symbol manifest gate (`public_symbols_v1.txt`) with
  a C++ compatibility test;
- **Three-platform CI matrix**: Linux (GCC) / macOS (universal) /
  Windows (MSVC required + MinGW).

## 10. Licensing

Topos Video Codec is released under the **Apache License 2.0** (see
`LICENSE`). Apache-2.0 was chosen for its explicit **patent grant**: for
codec technology, the mutual patent license and termination protections
matter greatly, and they are absent from MIT/BSD, while commercial use stays
unrestricted and GPLv3 remains one-way compatible.

**Trademarks.** Apple, ProRes and QuickTime are trademarks of Apple Inc.,
registered in the U.S. and other countries. DNxHR is a trademark of Avid
Technology, Inc. DaVinci Resolve is a trademark of Blackmagic Design Pty
Ltd. These names appear in this documentation solely to identify and compare
the respective technologies (nominative use) and imply no affiliation,
sponsorship, or endorsement.

## 11. Specification Index

| Document | Contents |
| --- | --- |
| `docs/bitstream_spec_v1.md` / `docs/bitstream_spec_v2.md` | bitstream specifications (V1 Rice / V2 canonical VLC) |
| `docs/container_spec_v1.md` | MOV-derived container specification |
| `docs/SDK.md` | integration guide for standalone integrators |
| `docs/capability_manifest.json` | machine-readable capability manifest |
| `docs/ADR-INDEX.md` (ADR-C001…C028) | all architecture decision records |
| `docs/benchmark_protocol.md` | performance measurement protocol |

---

*Topos Video Codec · Copyright (c) 2026 Topos Color project authors · Apache License 2.0*
