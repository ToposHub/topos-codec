# Topos Video Codec

A native C11 mezzanine video codec engine with intra profiles and an optional LP inter-frame tier (10/12-bit baseline, 16-bit extension; 4:2:2 / 4:4:4 / GBR, optional alpha). Licensed under the **Apache License 2.0**.

- **Video whitepaper**: [Topos_Codec_Whitepaper.md](Topos_Codec_Whitepaper.md) (English) · [Topos_Codec_白皮书.md](Topos_Codec_白皮书.md) (中文) — format overview, profile tiers, bitrate tables, performance
- **Image whitepaper (.toos)**: [Topos_Image_Whitepaper.md](Topos_Image_Whitepaper.md) (English) · [Topos_Image_白皮书.md](Topos_Image_白皮书.md) (中文)
- **Integration guide**: `docs/SDK.md` · bitstream specs: `docs/bitstream_spec_v1.md` / `docs/bitstream_spec_v2.md` · container: `docs/container_spec_v1.md` · ADRs: `docs/ADR-INDEX.md`

## Profile family at a glance

Six intra tiers named to map onto common mezzanine workflows. 422 Proxy / LT / 422 / 422 HQ share one 4:2:2 bitstream profile (profile=3) and differ only in target bitrate and alpha policy; 4444 (profile=5) and 4444 XQ (profile=6) are independent bitstream profiles. All tiers can optionally carry alpha (a8/a10/a12/a16).

| Tier | Pixel format | Bit depth | ≈ 1080p25 | ≈ 2160p25 (4K) | Primary use |
| --- | --- | --- | ---: | ---: | --- |
| Topos 422 Proxy | YUV 4:2:2 | 10 | 31 Mbps | 126 Mbps | offline proxies, remote editing |
| Topos 422 LT | YUV 4:2:2 | 10 | 64 Mbps | 256 Mbps | light intermediates, rough cuts |
| Topos 422 *(default)* | YUV 4:2:2 | 10 | 97 Mbps | 389 Mbps | general editing, render caches |
| Topos 422 HQ | YUV 4:2:2 / 4:4:4 / GBR | 10/12 | 143 Mbps | 571 Mbps | grading, intermediate mastering |
| Topos 4444 | YUV/GBR 4:4:4 | 10/12 | 213 Mbps | 852 Mbps | VFX, motion graphics, compositing |
| Topos 4444 XQ | YUV/GBR 4:4:4 | 12 | 351 Mbps | 1402 Mbps | HDR and multi-generation work |

Bitrates are tier targets at 1080p25 / 2160p25, scaling linearly as `bitrate ≈ bpp × width × height × fps`; any tier can also be driven with a fixed QP (0–63) instead of a bitrate target. The full specification table (reference data-rate ranges, alpha budgets, bitstream profile bytes) is in the whitepaper §4.

## Key features

- **Intra profiles and optional LP tier** — frame-independent intra profiles support indexed random access; LP uses an IP-2 micro-GOP
- **Deterministic rate control** — frame-level exact-QP search (sized mode) or fixed QP; byte-reproducible output pinned by frozen golden vectors
- **V2 canonical VLC entropy coding** — 23–32% bitrate savings at equal quality over V1 Rice; self-describing bitstream, old files decode forever
- **10/12-bit baseline plus 16-bit extension, 4:2:2 / 4:4:4 / GBR** — GBR pass-through avoids YUV round trips for RGB compositing sources; full/limited range, BT.709/BT.2020-class metadata in-band
- **Runtime-dispatched SIMD** — AVX2 and NEON paths selected via CPU feature detection; slice-parallel encode and decode with bounded thread pool
- **Zero third-party dependencies** — pure C11 + libc + pthread; first-class Linux / macOS (universal) / Windows (MSVC & MinGW) CI
- **FFmpeg-friendly** — optional libavcodec patch decodes Topos .mov in FFmpeg/ffplay; MOV-derived container with FastStart

## Topos Image (.toos) — still-image format on the same core

`.toos` is a still-image intermediate format built on this same codec core: a TPIM envelope + one complete TPIC intra-frame packet, with pixel coding **100% shared with libtopos_codec** (the spec freezes a 'no second implementation' rule). GBR 4:4:4 pass-through, explicit three-state alpha, atomic writes, first-class render sequence support — and decoding 2.3× faster than PNG.

| Tier (`toos encode --tier`) | Format | ≈MB/frame (1080p) | Use |
| --- | --- | ---: | --- |
| Topos 422 Low | YUV 4:2:2 10-bit | 1.11 | preview / review / proxies |
| Topos 422 Medium | YUV 4:2:2 10-bit | 1.29 | smaller than PNG 8-bit on the same material |
| Topos 422 High | YUV 4:2:2 10-bit | 1.71 | the PNG-anchored tier |
| Topos 422 Ultra *(default)* | YUV 4:2:2 10-bit | 2.86 | visually lossless class |
| Topos 444 High | GBR 4:4:4 12-bit | 5.10 | high-fidelity compositing |
| Topos 444 Ultra | GBR 4:4:4 12-bit | 5.79 | 12-bit visually lossless class |

Mathematically lossless output is `--qp 28` (10-bit) or a lossless container (PNG/EXR); full color-mode matrix, alpha semantics and sequence grammar are in the image whitepaper.

## Quick start

```bash
# 1. Build the native library (or: bash native/run_tests.sh for the full gate)
cmake -S native -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j

# 2. Use the Python bindings (either way)
TOPOS_CODEC_LIB=$(ls build/release/topos_codec*dylib* build/release/*.so 2>/dev/null | head -1) \
    PYTHONPATH=python python3 native/examples/topos_roundtrip.py
# ...or drop the library into python/topos_codec/lib/ for configuration-free use:
cp build/release/topos_codec* python/topos_codec/lib/
pip install .
```

For a bundled platform wheel, build the native library first, then regenerate with `--include-lib` and run `python -m pip wheel .`. Each bundled wheel is for its build platform; source-only wheels require `TOPOS_CODEC_LIB` or a library in `topos_codec/lib/`.

## Layout

| Path | Contents |
| --- | --- |
| `native/` | C11 codec core (src/include/tests/tools/examples/scripts + CMake) |
| `python/topos_codec/` | ctypes bindings, tier/capability declarations, high-level encoder wrapper |
| `python/tests/` | binding tests (`pytest python/tests`) |
| `docs/` | SDK guide, bitstream/container specs, ADRs, benchmark reports (whitepapers at repo root) |
| `.github/workflows/` | Linux/macOS/Windows CI matrix |

## Quality

31 native unit suites, 7 frozen golden vector groups, fuzzing entry points with deterministic replay, and an ABI symbol manifest gate. One-shot gate: `bash native/run_tests.sh`.

## Trademarks

Topos Video Codec is an independent, originally developed format. It is not affiliated with, sponsored by, or endorsed by any other codec vendor. Apple, ProRes and QuickTime are trademarks of Apple Inc.; DNxHR is a trademark of Avid Technology, Inc.; DaVinci Resolve is a trademark of Blackmagic Design Pty Ltd. Those names appear in the documentation solely to identify and compare the respective technologies; comparative figures are measured against FFmpeg's open-source software implementations.

## License

Apache License 2.0 — see `LICENSE` and `NOTICE`.
