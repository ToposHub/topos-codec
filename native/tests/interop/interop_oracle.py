#!/usr/bin/env python3
"""R5 外部 oracle：Topos MOV 互操作验证（container_spec_v1.md §9）。

三方 oracle，逐文件断言到 sample 级——不再是"未知 codec 不崩溃"：

  A. 独立 atom parser（纯 struct 实现，不依赖 reader/FFmpeg）
     - 树结构：ftyp 品牌/qt、单一 moov/mdat、布局（standard/faststart）
     - stsd 单 TPIC entry；tpcC 存在且 zlib.crc32 通过
     - 采样表交叉一致：stts 展开 == stsz 计数 == stsc×stco 展开
     - 每个样本绝对偏移落在 mdat 载荷内且以 b"TPIC" 开头
  B. PyAV（av 降级到 demux 层：未知 codec 下的容器互操作）
     - demux 包数 == 样本数；逐包 size == stsz 值；duration 合计 == 期望 tick
  C. ffprobe（可选，未安装时显式 SKIP）
     - -show_packets 包数/size；-show_streams codec_tag_string == TPIC

fixtures（本脚本自产）：
  standard_vfr   标准布局 + VFR（dur 1/2/3 循环）+ SAR 5/4
  faststart      CLI 产出的 FastStart 布局（--encoder-cli 传入时）
  hand_co64      standard_vfr 的 stco 手工升级 co64（独立 parser 变换）

用法：
  python interop_oracle.py [--lib <libtopos_codec>] [--encoder-cli <path>]
                            [--ffprobe <path>]
退出码：0 全过；1 任一硬 oracle 失败（ffprobe 缺失不算失败）。
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Tuple

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT))

U32 = ">I"
U64 = ">Q"


class OracleError(RuntimeError):
    """oracle 断言失败。"""


# ── 独立 atom parser（oracle A）──────────────────────────────────────────


@dataclass
class Atom:
    off: int
    size: int
    type: bytes
    body: int

    @property
    def end(self) -> int:
        return self.off + self.size


def _read_atom(buf: bytes, off: int, limit: int) -> Atom:
    if off + 8 > limit:
        raise OracleError(f"atom 头越界 @{off} (limit {limit})")
    size, typ = struct.unpack(">I4s", buf[off:off + 8])
    body = off + 8
    if size == 1:
        if off + 16 > limit:
            raise OracleError(f"ext atom 头越界 @{off}")
        size = struct.unpack(U64, buf[off + 8:off + 16])[0]
        body = off + 16
    elif size == 0:
        size = limit - off
    if size < 8 or off + size > limit:
        raise OracleError(f"atom size 越界 type={typ!r} @{off} size={size}")
    return Atom(off, size, typ, body)


def _children(buf: bytes, start: int, end: int) -> List[Atom]:
    out: List[Atom] = []
    o = start
    while o < end:
        a = _read_atom(buf, o, end)
        out.append(a)
        o = a.end
    if o != end:
        raise OracleError(f"子 atom 不闭合: o={o} end={end} (差 {end - o})")
    return out


def _find(kids: List[Atom], typ: bytes) -> Atom:
    hits = [a for a in kids if a.type == typ]
    if len(hits) != 1:
        raise OracleError(f"期望恰好 1 个 {typ!r}，实得 {len(hits)}")
    return hits[0]


@dataclass
class ParsedMovie:
    layout: str
    faststart: bool
    timescale: int
    durs: List[int]
    sizes: List[int]
    offsets: List[int]
    has_pasp: bool = False
    unknown_seen: List[str] = field(default_factory=list)


def parse_topos_mov(data: bytes, expect_layout: Optional[str] = None) -> ParsedMovie:
    """独立实现的结构校验（与 native reader 无共享代码）。"""
    top = _children(data, 0, len(data))
    types = [a.type for a in top]
    if types[0] != b"ftyp":
        raise OracleError(f"首个顶层 atom 非 ftyp: {types[0]!r}")
    if struct.unpack(">I", data[4:8])[0] < 8 or data[8:12] != b"qt  ":
        raise OracleError("ftyp major brand 非 qt")
    moov = _find(top, b"moov")
    mdat = _find(top, b"mdat")
    faststart = moov.off < mdat.off
    layout = "faststart" if faststart else "standard"
    if expect_layout is not None and layout != expect_layout:
        raise OracleError(f"布局 {layout} ≠ 期望 {expect_layout}")

    moov_kids = _children(data, moov.body, moov.end)
    traks = [a for a in moov_kids if a.type == b"trak"]
    if len(traks) != 1:
        raise OracleError(f"trak 数 {len(traks)} ≠ 1")
    trak = traks[0]

    trak_kids = _children(data, trak.body, trak.end)
    mdia = _find(trak_kids, b"mdia")
    mdia_kids = _children(data, mdia.body, mdia.end)
    mdhd = _find(mdia_kids, b"mdhd")
    timescale = struct.unpack(U32, data[mdhd.body + 12:mdhd.body + 16])[0]
    if timescale == 0:
        raise OracleError("mdhd timescale=0")
    hdlr = _find(mdia_kids, b"hdlr")
    if data[hdlr.body + 8:hdlr.body + 12] != b"vide":
        raise OracleError("hdlr 非 vide")
    minf = _find(mdia_kids, b"minf")
    minf_kids = _children(data, minf.body, minf.end)
    stbl = _find(minf_kids, b"stbl")

    stbl_kids = _children(data, stbl.body, stbl.end)
    known = {b"stsd", b"stts", b"stsz", b"stsc", b"stco", b"co64", b"stss"}
    unknown_seen = [a.type.decode("latin1") for a in stbl_kids if a.type not in known]

    # stsd → TPIC entry → tpcC（CRC）
    stsd = _find(stbl_kids, b"stsd")
    entry_count = struct.unpack(U32, data[stsd.body + 4:stsd.body + 8])[0]
    if entry_count != 1:
        raise OracleError(f"stsd entry_count={entry_count} ≠ 1")
    entry = _read_atom(data, stsd.body + 8, stsd.end)
    if entry.type != b"TPIC":
        raise OracleError(f"entry {entry.type!r} ≠ TPIC")
    entry_kids = _children(data, entry.body + 78, entry.end)
    tpcc = _find(entry_kids, b"tpcC")
    tpcc_bytes = data[tpcc.body:tpcc.end]
    if len(tpcc_bytes) != 28 or tpcc_bytes[:4] != b"TPCC":
        raise OracleError("tpcC 长度/magic 不符")
    if zlib.crc32(tpcc_bytes[:24]) != struct.unpack(U32, tpcc_bytes[24:28])[0]:
        raise OracleError("tpcC CRC 不符（独立 parser）")
    has_pasp = any(a.type == b"pasp" for a in entry_kids)

    # stts（VFR 多 run）
    stts = _find(stbl_kids, b"stts")
    n_runs = struct.unpack(U32, data[stts.body + 4:stts.body + 8])[0]
    durs: List[int] = []
    o = stts.body + 8
    for _ in range(n_runs):
        cnt, dlt = struct.unpack(">II", data[o:o + 8])
        if dlt == 0:
            raise OracleError("stts delta=0")
        durs.extend([dlt] * cnt)
        o += 8

    # stsz
    stsz = _find(stbl_kids, b"stsz")
    uniform = struct.unpack(U32, data[stsz.body + 4:stsz.body + 8])[0]
    count = struct.unpack(U32, data[stsz.body + 8:stsz.body + 12])[0]
    if len(durs) != count:
        raise OracleError(f"stts 展开 {len(durs)} ≠ stsz 计数 {count}")
    if uniform != 0:
        sizes = [uniform] * count
    else:
        sizes = list(struct.unpack(f">{count}I", data[stsz.body + 12:stsz.body + 12 + 4 * count]))
    if count and (min(sizes) == 0 if sizes else True):
        raise OracleError("stsz 含 0")

    # stsc × stco/co64 → 绝对偏移
    stsc = _find(stbl_kids, b"stsc")
    n_sc = struct.unpack(U32, data[stsc.body + 4:stsc.body + 8])[0]
    runs = [struct.unpack(">III", data[stsc.body + 8 + 12 * i:stsc.body + 20 + 12 * i])
            for i in range(n_sc)]
    use_co64 = any(a.type == b"co64" for a in stbl_kids)
    stco = _find(stbl_kids, b"co64" if use_co64 else b"stco")
    n_chunks = struct.unpack(U32, data[stco.body + 4:stco.body + 8])[0]
    esz = 8 if use_co64 else 4
    fmt = f">{n_chunks}Q" if use_co64 else f">{n_chunks}I"
    chunk_bases = list(struct.unpack(fmt, data[stco.body + 8:stco.body + 8 + esz * n_chunks]))

    offsets: List[int] = []
    si = 0
    chunk_i = 0
    for e, (first, spc, _) in enumerate(runs):
        if first != chunk_i + 1 or spc == 0:
            raise OracleError(f"stsc run {e} 非法 (first={first} spc={spc})")
        if e + 1 < len(runs):
            nxt = runs[e + 1][0]
            if nxt <= first:
                raise OracleError("stsc first_chunk 非严格递增")
            run_chunks = nxt - first
        else:
            run_chunks = n_chunks - chunk_i  # 末 run
        for _ in range(run_chunks):
            if chunk_i >= n_chunks:
                raise OracleError("stco 条目不足")
            base = chunk_bases[chunk_i]
            chunk_i += 1
            pos = base
            for _ in range(spc):
                if si >= count:
                    break
                offsets.append(pos)
                pos += sizes[si]
                si += 1
    if si != count or chunk_i != n_chunks:
        raise OracleError(f"stsc/stco 展开 {si}/{chunk_i} ≠ {count}/{n_chunks}")

    # 样本必须落在 mdat 载荷内且以 TPIC 开头
    payload_lo, payload_hi = mdat.body, mdat.end
    for i, (off, sz) in enumerate(zip(offsets, sizes)):
        if not (payload_lo <= off and off + sz <= payload_hi):
            raise OracleError(f"sample {i} 偏移 {off}+{sz} 不在 mdat 载荷 [{payload_lo},{payload_hi})")
        if data[off:off + 4] != b"TPIC":
            raise OracleError(f"sample {i} 魔数非 TPIC")

    return ParsedMovie(layout=layout, faststart=faststart, timescale=timescale,
                       durs=durs, sizes=sizes, offsets=offsets,
                       has_pasp=has_pasp, unknown_seen=unknown_seen)


# ── 手工 co64 变换（fixture 生成，与 native 单测同构）────────────────────


def hand_upgrade_co64(data: bytes) -> bytes:
    """stco → co64：条目 4B→8B 原值拓宽，祖先 size 同步 +delta。

    仅用于 standard 布局（mdat 在 moov 前，条目值不变）。
    """
    def ancestors(buf: bytes) -> List[Tuple[int, int]]:
        anc: List[Tuple[int, int]] = []
        path = (b"moov", b"trak", b"mdia", b"minf", b"stbl")
        def walk(kids, depth):
            for a in kids:
                if a.type == path[depth]:
                    anc.append((a.off, a.size))
                    if depth + 1 < len(path):
                        walk(_children(buf, a.body, a.end), depth + 1)
                    break
        walk(_children(buf, 0, len(buf)), 0)
        return anc

    anc = ancestors(data)
    if len(anc) != 5:
        raise OracleError(f"祖先链不完整: {len(anc)}")
    stbl_off = anc[-1][0]
    stbl_kids = _children(data, stbl_off + 8, stbl_off + anc[-1][1])
    stco = _find(stbl_kids, b"stco")
    count = struct.unpack(U32, data[stco.body + 4:stco.body + 8])[0]
    delta = 4 * count
    out = bytearray()
    out += data[:stco.off]
    out += struct.pack(">I4s", stco.size + delta, b"co64")
    out += data[stco.body:stco.body + 8]
    for i in range(count):
        out += struct.pack(U64, struct.unpack(U32, data[stco.body + 8 + 4 * i:stco.body + 12 + 4 * i])[0])
    out += data[stco.end:]
    for (o, s) in anc:
        out[o:o + 4] = struct.pack(U32, s + delta)
    return bytes(out)


# ── PyAV / ffprobe oracle（B / C）────────────────────────────────────────


def pyav_demux_check(path: Path, pm: ParsedMovie) -> None:
    import av  # noqa: PLC0415 —— 延迟导入，缺库时报清晰错误
    container = av.open(str(path))
    try:
        streams = container.streams
        if len(streams.video) != 1 or streams.audio:
            raise OracleError(f"流拓扑异常: video={len(streams.video)} audio={len(streams.audio)}")
        all_pkts = [p for p in container.demux(streams.video[0])]
        # FFmpeg mov demuxer 对未知 codec 会在流尾补一个 0 字节虚包
        # （承载末帧 duration 的 EOS 语义）——真实样本不可能是 0 字节
        # （stsz size=0 被 spec §7 拒绝），按非空过滤后对齐。
        pkts = [p for p in all_pkts if p.size]
        if len(all_pkts) - len(pkts) > 1:
            raise OracleError(f"流尾虚包 {len(all_pkts) - len(pkts)} 个 > 1")
        if len(pkts) != len(pm.sizes):
            raise OracleError(f"PyAV 包数 {len(pkts)} ≠ 期望 {len(pm.sizes)}")
        for i, p in enumerate(pkts):
            size = p.size if p.size is not None else 0
            if size != pm.sizes[i]:
                raise OracleError(f"PyAV 包 {i} size {size} ≠ stsz {pm.sizes[i]}")
        dur_sum = sum((p.duration or 0) for p in pkts)
        if dur_sum and dur_sum != sum(pm.durs):
            raise OracleError(f"PyAV duration 合计 {dur_sum} ≠ 期望 {sum(pm.durs)}")
    finally:
        container.close()


def ffprobe_check(ffprobe: str, path: Path, pm: ParsedMovie) -> None:
    r = subprocess.run(
        [ffprobe, "-v", "error", "-show_streams", "-show_packets", "-of", "json", str(path)],
        capture_output=True, text=True, timeout=30,
    )
    if r.returncode != 0:
        raise OracleError(f"ffprobe 失败: {r.stderr.strip()[:200]}")
    j = json.loads(r.stdout)
    vstreams = [s for s in j.get("streams", []) if s.get("codec_type") == "video"]
    if len(vstreams) != 1:
        raise OracleError(f"ffprobe video 流 {len(vstreams)} ≠ 1")
    if vstreams[0].get("codec_tag_string") != "TPIC":
        raise OracleError(f"codec_tag_string={vstreams[0].get('codec_tag_string')!r} ≠ TPIC")
    pkts = [p for p in j.get("packets", []) if p.get("codec_type") == "video"]
    if len(pkts) != len(pm.sizes):
        raise OracleError(f"ffprobe 包数 {len(pkts)} ≠ 期望 {len(pm.sizes)}")
    for i, p in enumerate(pkts):
        if int(p.get("size", -1)) != pm.sizes[i]:
            raise OracleError(f"ffprobe 包 {i} size {p.get('size')} ≠ stsz {pm.sizes[i]}")


# ── fixture 生成（binding mux）───────────────────────────────────────────


def make_standard_vfr(lib: Optional[str], out: Path) -> Tuple[List[int], List[int]]:
    """96×64，9 帧，dur 1/2/3 循环，SAR 5/4 → 标准布局。返回 (durs, sizes)。"""
    if lib:
        os.environ["TOPOS_CODEC_LIB"] = lib
    import ctypes  # noqa: PLC0415

    from src.shared.codec.topos_binding import (  # noqa: PLC0415
        ToposCodec, ToposMuxFile, _CFrameConfig,
    )

    codec = ToposCodec()
    w, h = 96, 64
    cw = w // 2

    def plane(n: int, seed: int) -> bytes:
        vals = [((seed + k * 37) % 941 + 40) & 0x3FF for k in range(n)]
        return struct.pack(f"<{n}H", *vals)

    fc = _CFrameConfig()
    fc.struct_size = ctypes.sizeof(_CFrameConfig)
    fc.abi_version = codec.abi_version
    fc.visible_width, fc.visible_height = w, h
    fc.qp_base = 24
    fc.qmatrix_id = 1
    fc.color_range = 1
    fc.color_primaries = 1
    fc.color_transfer = 1
    fc.color_matrix = 1
    fc.sar_num, fc.sar_den = 5, 4

    y0 = plane(w * h, 3)
    u = plane(cw * h, 5)
    v = plane(cw * h, 7)
    durs = [(i % 3) + 1 for i in range(9)]
    sizes: List[int] = []
    packets = []
    for i in range(9):
        y = y0 if i == 0 else bytes((b + i) & 0xFF for b in y0)  # 逐帧变化
        pkt, _stats = codec.encode_frame(fc, [y, u, v])
        packets.append(pkt)
        sizes.append(len(pkt))
    movie_cfg = codec.movie_config(width=w, height=h, qp=24, qmatrix=1,
                                   sar_num=5, sar_den=4, timescale=24)
    mux = ToposMuxFile(codec, str(out), movie_cfg)
    pts = 0
    for i, pkt in enumerate(packets):
        mux.add_packet(pkt, pts, durs[i])
        pts += durs[i]
    mux.finish()
    mux.close()
    return durs, sizes


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    here = Path(__file__).resolve().parent
    # 产物名 = CMake OUTPUT_NAME topos_codec + PREFIX ""（陈旧 build 目录
    # 可能仍有 lib 前缀旧名，做双名回退）
    default_lib = here.parents[1] / "build" / "debug" / (
        "topos_codec.dylib" if sys.platform == "darwin" else "topos_codec.so")
    if not default_lib.exists():
        default_lib = here.parents[1] / "build" / "debug" / (
            "libtopos_codec.dylib" if sys.platform == "darwin" else "libtopos_codec.so")
    ap.add_argument("--lib", default=str(default_lib))
    ap.add_argument("--encoder-cli", default="")
    ap.add_argument("--ffprobe", default=shutil.which("ffprobe") or "")
    ap.add_argument("--work", default=tempfile.mkdtemp(prefix="topos_interop_"))
    args = ap.parse_args()

    work = Path(args.work)
    work.mkdir(parents=True, exist_ok=True)
    failures: List[str] = []

    # fixture 1：binding 产 standard VFR+SAR
    f1 = work / "standard_vfr.mov"
    durs, sizes = make_standard_vfr(args.lib, f1)
    # fixture 2：手工 co64（standard 布局）
    f2 = work / "hand_co64.mov"
    f2.write_bytes(hand_upgrade_co64(f1.read_bytes()))

    cases = [("standard_vfr", f1, durs, sizes, "standard"),
             ("hand_co64", f2, durs, sizes, "standard")]

    # fixture 3：CLI 产 faststart（CFR，dur=1）
    if args.encoder_cli and Path(args.encoder_cli).exists():
        raw = work / "fs.raw"
        rawgen = Path(args.encoder_cli).parent / "topos_rawgen"
        if rawgen.exists():
            subprocess.run([str(rawgen), "--width", "96", "--height", "64",
                            "--frames", "4", "--kind", "2", "--out", str(raw)],
                           check=True, capture_output=True)
            f3 = work / "faststart.mov"
            subprocess.run([args.encoder_cli, "--width", "96", "--height", "64",
                            "--fps", "24", "--qp", "24", "--qm", "1",
                            "--input", str(raw), "--output", str(f3),
                            "--faststart"], check=True, capture_output=True)
            # CLI 产 CFR：--fps 24 → timescale 24000、每帧 1000 tick
            cases.append(("faststart", f3, [1000] * 4, None, "faststart"))

    ffprobe = args.ffprobe
    for name, path, exp_durs, exp_sizes, layout in cases:
        data = path.read_bytes()
        pm = parse_topos_mov(data, expect_layout=layout)
        if pm.durs != exp_durs:
            failures.append(f"{name}: stts durs {pm.durs[:8]}… ≠ 期望 {exp_durs[:8]}…")
        if exp_sizes is not None and pm.sizes != exp_sizes:
            failures.append(f"{name}: stsz sizes ≠ 编码侧记录")
        tag = f"{name}: parser OK ({len(pm.sizes)} samples, {pm.timescale} tsc" \
              f"{', pasp' if pm.has_pasp else ''})" + \
              (f", unknown atoms: {pm.unknown_seen}" if pm.unknown_seen else "")
        print(tag)
        try:
            pyav_demux_check(path, pm)
            print(f"{name}: PyAV demux OK ({len(pm.sizes)} 包)")
        except Exception as exc:  # noqa: BLE001 —— oracle 汇总所有失败再退出
            failures.append(f"{name}: PyAV 失败: {exc}")
        if ffprobe:
            try:
                ffprobe_check(ffprobe, path, pm)
                print(f"{name}: ffprobe OK (tag=TPIC, {len(pm.sizes)} 包)")
            except Exception as exc:  # noqa: BLE001
                failures.append(f"{name}: ffprobe 失败: {exc}")
        else:
            print(f"{name}: ffprobe SKIP（未安装）")

    if failures:
        print("FAIL:")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("interop oracle: ALL OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
