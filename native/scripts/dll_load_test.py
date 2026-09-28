#!/usr/bin/env python3
"""dll_load_test.py — 独立 codec 加载/功能冒烟（R7，stdlib-only）。

Windows DLL 打包加载测试 / PyInstaller 干净机 codec 验证 / CI 跨平台
冒烟共用：不依赖 numpy 与仓库 Python 包，直接经 ctypes 走 C ABI 做
load → abi 协商 → 编码 → 解码 → 数值校验 的最小 roundtrip。

用法：
    python dll_load_test.py [lib-path]      # 缺省取 TOPOS_CODEC_LIB，再退
                                          # 回 build/debug 平台默认名

退出码：0 = 加载且 roundtrip 数值通过；1 = 任何一步失败。
"""
from __future__ import annotations

import ctypes
import os
import sys
from ctypes import POINTER, Structure, byref, c_int32, c_size_t, c_uint8, \
    c_uint16, c_uint32, c_uint64, c_void_p
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
TC_OK = 0
W, H = 64, 48


class FrameConfig(Structure):
    _fields_ = [
        ("struct_size", c_uint32), ("abi_version", c_uint32),
        ("visible_width", c_uint16), ("visible_height", c_uint16),
        ("profile", c_uint8), ("pixel_format", c_uint8), ("bit_depth", c_uint8),
        ("qmatrix_id", c_uint8), ("qp_base", c_uint8),
        ("qp_delta_luma", ctypes.c_int8), ("qp_delta_chroma", ctypes.c_int8),
        ("slice_rows", c_uint8), ("alpha_mode", c_uint8),
        ("alpha_bit_depth", c_uint8), ("alpha_premultiplied", c_uint8),
        ("color_range", c_uint8), ("color_primaries", c_uint8),
        ("color_transfer", c_uint8), ("color_matrix", c_uint8),
        ("chroma_siting", c_uint8),
        ("sar_num", c_uint16), ("sar_den", c_uint16),
        ("reserved", c_uint32 * 8),
    ]


class FrameInput(Structure):
    _fields_ = [
        ("struct_size", c_uint32), ("abi_version", c_uint32),
        ("planes", POINTER(c_uint16) * 4),
        ("strides", c_size_t * 4),
        ("reserved", c_uint32 * 8),
    ]


class FrameStats(Structure):
    _fields_ = [
        ("struct_size", c_uint32), ("abi_version", c_uint32),
        ("packet_size", c_uint32), ("color_payload_bytes", c_uint32),
        ("alpha_payload_bytes", c_uint32),
        ("color_header_bytes", c_uint32), ("alpha_header_bytes", c_uint32),
        ("slice_count", c_uint16), ("qp_base", c_uint8),
        ("reserved8", c_uint8), ("alpha_max_abs_error", c_uint16),
        ("reserved16", c_uint16),
        ("reserved", c_uint32 * 8),
    ]


class FrameOutput(Structure):
    _fields_ = [
        ("struct_size", c_uint32), ("abi_version", c_uint32),
        ("visible_width", c_uint16), ("visible_height", c_uint16),
        ("coded_width", c_uint16), ("coded_height", c_uint16),
        ("plane_count", c_uint8), ("bit_depth", c_uint8),
        ("profile", c_uint8), ("pixel_format", c_uint8),
        ("alpha_mode", c_uint8), ("alpha_bit_depth", c_uint8),
        ("color_range", c_uint8), ("color_primaries", c_uint8),
        ("color_transfer", c_uint8), ("color_matrix", c_uint8),
        ("chroma_siting", c_uint8),
        ("concealed_slices", c_uint16), ("slice_count", c_uint16),
        ("sar_num", c_uint16), ("sar_den", c_uint16),
        ("slice_status", c_uint8 * 512),
        ("reserved", c_uint32 * 8),
    ]


def find_lib() -> Path:
    if len(sys.argv) > 1:
        return Path(sys.argv[1])
    env = os.environ.get("TOPOS_CODEC_LIB")
    if env:
        return Path(env)
    for name in ("topos_codec.dylib", "topos_codec.so", "topos_codec.dll",
                 "libtopos_codec.dylib", "libtopos_codec.so"):
        p = REPO / "build" / "debug" / name
        if p.is_file():
            return p
    print("找不到 libtopos_codec（传参 / TOPOS_CODEC_LIB / build/debug）")
    sys.exit(1)


def main() -> int:
    lib_path = find_lib()
    try:
        lib = ctypes.CDLL(str(lib_path))
    except OSError as exc:
        print(f"加载失败 {lib_path}: {exc}")
        return 1

    lib.tc_abi_version.restype = c_int32
    lib.tc_abi_version.argtypes = []
    abi = lib.tc_abi_version()
    if abi != 2:
        print(f"ABI 不匹配: lib={abi} expected=2")
        return 1

    lib.tc_frame_packet_bound.restype = c_size_t
    lib.tc_frame_packet_bound.argtypes = [POINTER(FrameConfig)]
    lib.tc_frame_encode.restype = c_int32
    lib.tc_frame_encode.argtypes = [
        POINTER(FrameConfig), POINTER(FrameInput),
        POINTER(c_uint8), c_size_t, POINTER(FrameStats)]
    lib.tc_frame_decode.restype = c_int32
    lib.tc_frame_decode.argtypes = [
        POINTER(c_uint8), c_size_t,
        POINTER(POINTER(c_uint16)), POINTER(c_size_t), POINTER(FrameOutput)]
    lib.tc_frame_plane_geometry.restype = c_int32
    lib.tc_frame_plane_geometry.argtypes = [
        POINTER(FrameOutput), c_uint32, POINTER(c_uint32), POINTER(c_uint32)]

    cfg = FrameConfig(struct_size=ctypes.sizeof(FrameConfig), abi_version=2,
                      visible_width=W, visible_height=H,
                      profile=3, pixel_format=0, bit_depth=10,
                      qmatrix_id=1, qp_base=20)
    cw, ch = W // 2, H  # 4:2:2 色度几何
    y = (c_uint16 * (W * H))()
    u = (c_uint16 * (cw * ch))()
    v = (c_uint16 * (cw * ch))()
    for i in range(W * H):
        y[i] = (i * 4 + (i // W) * 7) % 1024
    for i in range(cw * ch):
        u[i] = (i * 3) % 1024
        v[i] = (i * 5 + 13) % 1024
    fi = FrameInput(struct_size=ctypes.sizeof(FrameInput), abi_version=2)
    fi.planes[0] = y
    fi.planes[1] = u
    fi.planes[2] = v

    cap = lib.tc_frame_packet_bound(byref(cfg))
    pkt = (c_uint8 * cap)()
    st = FrameStats(struct_size=ctypes.sizeof(FrameStats), abi_version=2)
    rc = lib.tc_frame_encode(byref(cfg), byref(fi), pkt, cap, byref(st))
    if rc != TC_OK:
        print(f"编码失败 rc={rc}")
        return 1

    info = FrameOutput()
    rc = lib.tc_frame_decode(pkt, st.packet_size, None, None, byref(info))
    if rc != TC_OK or info.plane_count != 3 or info.bit_depth != 10 \
            or info.visible_width != W or info.visible_height != H:
        print(f"解析失败 rc={rc} planes={info.plane_count} "
              f"bd={info.bit_depth} {info.visible_width}x{info.visible_height}")
        return 1

    outs = []
    strides = (c_size_t * 4)(0, 0, 0, 0)
    plane_ptrs = (POINTER(c_uint16) * 4)()
    geoms = []
    for p in range(3):
        gw, gh = c_uint32(), c_uint32()
        if lib.tc_frame_plane_geometry(byref(info), p, byref(gw), byref(gh)) != TC_OK:
            print(f"几何查询失败 plane={p}")
            return 1
        buf = (c_uint16 * (gw.value * gh.value))()
        outs.append(buf)
        plane_ptrs[p] = buf
        geoms.append((gw.value, gh.value))
    rc = lib.tc_frame_decode(pkt, st.packet_size, plane_ptrs, strides, byref(info))
    if rc != TC_OK or info.concealed_slices != 0:
        print(f"解码失败 rc={rc} concealed={info.concealed_slices}")
        return 1
    if geoms[0] != (W, H) or geoms[1] != (cw, ch) or geoms[2] != (cw, ch):
        print(f"几何不符: {geoms}")
        return 1

    # 有损 qp20/10-bit：逐平面最大绝对误差须在受限近似容差内
    refs = (y, u, v)
    for p, buf in enumerate(outs):
        diff = max(abs(int(a) - int(b)) for a, b in zip(buf, refs[p]))
        if diff > 64:
            print(f"plane{p} 最大误差 {diff} 超容差 64")
            return 1

    print(f"codec roundtrip: OK（{lib_path.name}, abi={abi}, "
          f"pkt={st.packet_size}B, qp20@10bit maxdiff≤64）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
