"""Topos Video Codec — ctypes 绑定层（阶段 1 骨架）。

ADR-C004：经稳定 C ABI 用标准库 ctypes 消费 libtopos_codec，不编译 CPython
扩展模块（规避 .venv 3.12 / 系统 3.13 双版本分裂）。镜像结构与 C 侧
sizeof / abi_version 一致性在 ToposCodec 初始化时强制校验（风险 R-18）。
"""
from __future__ import annotations

import ctypes
import logging
import os
import sys
import threading
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional

_LOG = logging.getLogger(__name__)

# —— 常量镜像（native/topos_codec/include/topos_codec.h，值冻结） ——
TOPOS_CODEC_ABI_VERSION = 2

TC_OK = 0
TC_WARN_CONCEALED = 1
TC_ERR_INVALID_ARGUMENT = -1
TC_ERR_OUT_OF_MEMORY = -2
TC_ERR_UNSUPPORTED_VERSION = -3
TC_ERR_UNSUPPORTED_PROFILE = -4
TC_ERR_UNSUPPORTED_PIXEL_FORMAT = -5
TC_ERR_UNSUPPORTED_MATRIX = -6
TC_ERR_UNSUPPORTED_ALPHA_MODE = -7
TC_ERR_LIMIT_EXCEEDED = -8
TC_ERR_MALFORMED = -9
TC_ERR_TRUNCATED = -10
TC_ERR_CHECKSUM_MISMATCH = -11
TC_ERR_STATE = -12
TC_ERR_CANCELLED = -13
TC_ERR_IO = -14
TC_ERR_BUFFER_TOO_SMALL = -15
TC_ERR_NOT_IMPLEMENTED = -16

TC_DECODE_SCALE_FULL = 0
TC_DECODE_SCALE_HALF = 1
TC_DECODE_SCALE_THIRD = 2
TC_DECODE_SCALE_QUARTER = 3
TC_DECODE_SCALE_EIGHTH = 4

TC_DECODE_MODE_FULL = 0
TC_DECODE_MODE_SCALED = 1
TC_DECODE_MODE_REDUCED = 2
TC_DECODE_MODE_AUTO_2K = 3

TC_DECODE_QUALITY_DEFAULT = 0
TC_DECODE_QUALITY_FAST = 1
TC_DECODE_QUALITY_BALANCED = 2
TC_DECODE_QUALITY_HIGH = 3
TC_DECODE_FLAG_DROP_ALPHA = 1 << 0

TOPOS_CPU_X86_AVX2 = 1 << 0
TOPOS_CPU_X86_AVX512F = 1 << 1
TOPOS_CPU_ARM_NEON = 1 << 2
TOPOS_CPU_X86_FMA = 1 << 3

TC_CODEC_CAP_FULL_DECODE = 1 << 0
TC_CODEC_CAP_FIXED_REDUCED_DECODE = 1 << 1
TC_CODEC_CAP_AUTO_2K_DECODE = 1 << 2
TC_CODEC_CAP_CPU_SURFACE_OUTPUT = 1 << 3
TC_CODEC_CAP_HOST_VISIBLE_GPU_SURFACE = 1 << 4
TC_CODEC_CAP_V7A_BAND_DECODE = 1 << 5  # 已退役（2026-09-13 归档）：native 不再广播，v7a_band_decode 恒 False
TC_CODEC_CAP_V7B_SCALABLE_BASE = 1 << 6
TC_CODEC_CAP_DECODE_DROP_ALPHA = 1 << 7
TC_CODEC_MAX_DIM = 16384
TC_CODEC_AUTO_2K_MAX_DIM = 2048
# v1.5 qp 域上限（ADR-C031；native TC_QP_MAX）。qp≥64 需帧头 minor=4
# （V1）/ major=2（V2），产品层编码/码控钳位统一用本常量。
TOPOS_QP_MAX = 95

# V7-B scalable 发布四态（topos_codec.h topos_scalable_status，P0-04）
TC_SCALABLE_STATUS_EXPERIMENTAL = 0
TC_SCALABLE_STATUS_REFERENCE = 1
TC_SCALABLE_STATUS_ELIGIBLE = 2
TC_SCALABLE_STATUS_DEFAULT = 3

# —— 色彩码表（bitstream_spec §A.7，H.273 对齐；str → 码值正向映射，
# 解码侧逆向映射见 src/shared/media/color_metadata.py 的 map_* 系列。
# 阶段 8 单一真相源：ToposVideoEncoder（export）与 TPIC 代理生成
# （media）共用，未知标签必须显式失败，不许默认值掩盖） ——
TOPOS_PRIMARIES_CODES = {
    'bt709': 1,
    'smpte170m': 6,
    'bt2020': 9,
    'smpte431': 12,
}
TOPOS_TRANSFER_CODES = {
    'bt709': 1,
    'linear': 8,
    'iec61966_2_1': 13,   # H1：sRGB EOTF（v1.8 minor≥7 载体）
    'smpte2084': 16,
    'arib-std-b67': 18,
}
TOPOS_MATRIX_CODES = {
    'bt709': 1,
    'smpte170m': 5,
    'bt601': 5,
    'bt470bg': 5,
    'bt2020': 9,
    'bt2020nc': 9,
}

# —— 容器能力声明（R5；与 container_spec_v1.md §10 非目标一致）。
# v1.1（2026-09-19，ADR-C051）：音频轨解冻——单轨 lpcm/mp4a，时间线导出
# 携带；回放缓存/代理仍纯视频（ADR-C008 不变）。
# v1.4（2026-09-20，ADR-C052）：edit list 解冻音频 trak 最小子集——仅
# mp4a priming 对齐（单条目 elst）；视频 trak pts 无空洞约束不变。
TOPOS_MOVIE_CAPS = {
    'video': True,
    'audio_tracks': True,   # v1.1：单音频轨（lpcm/mp4a，见 TC_AUDIO_*）
    'field_order': False,
    'edit_lists': 'audio_only',
}

# CMake 目标 OUTPUT_NAME=topos_codec、PREFIX=""（三平台统一无 lib 前缀命名）；
# libtopos_codec.* 仅为历史构建的陈旧产物，排在新名之后兜底。
_LIB_NAMES = ("topos_codec.dylib", "topos_codec.so", "topos_codec.dll",
              "libtopos_codec.dylib", "libtopos_codec.so")
_BUILD_CONFIGS = ("release", "debug", "asan", "ubsan", "fuzz")

# Windows 无 os.pread/os.pwrite（POSIX 独有）——IO 回调按平台分派。
# 注意：Windows 上还必须逐 fd 强制 O_BINARY——CPython 的 os.open 默认
# 继承 CRT 文本模式（_fmode=TEXT），os.write 会做 LF→CRLF 翻译（多写
# 字节、mdat/moov 错位），os.read 会做 CRLF→LF 折叠（短读/数据错）。
# 已实测与 native DLL 无关，属平台默认行为；io.open 不受影响，但本模块
# 的低层回调全部基于 os.open/os.read/os.write。
_HAS_PREAD = hasattr(os, "pread")
_HAS_PWRITE = hasattr(os, "pwrite")
_IS_WINDOWS = sys.platform == "win32"


def _open_binary(path: str, flags: int, mode: int = 0o666) -> int:
    """os.open + Windows 逐 fd 强制 BINARY（见模块头注释）。"""
    fd = os.open(path, flags, mode)
    if _IS_WINDOWS:
        import msvcrt

        msvcrt.setmode(fd, os.O_BINARY)
    return fd


def _pread_full(fd: int, length: int, off: int, lock=None) -> bytes:
    """os.pread 或其 Windows 语义模拟（不移动文件指针；lock 可选互斥）。"""
    if _HAS_PREAD:
        return os.pread(fd, length, off)
    if lock is not None:
        lock.acquire()
    try:
        pos = os.lseek(fd, 0, os.SEEK_CUR)
        os.lseek(fd, off, os.SEEK_SET)
        data = os.read(fd, length)
        os.lseek(fd, pos, os.SEEK_SET)
        return data
    finally:
        if lock is not None:
            lock.release()


def _pwrite_full(fd: int, data: bytes, off: int, lock=None) -> int:
    """os.pwrite 或其 Windows 语义模拟（写后恢复文件指针）。"""
    if _HAS_PWRITE:
        return os.pwrite(fd, data, off)
    if lock is not None:
        lock.acquire()
    try:
        pos = os.lseek(fd, 0, os.SEEK_CUR)
        os.lseek(fd, off, os.SEEK_SET)
        n = os.write(fd, data)
        os.lseek(fd, pos, os.SEEK_SET)
        return n
    finally:
        if lock is not None:
            lock.release()


class ToposCodecError(RuntimeError):
    """绑定层/库交互错误（库缺失、ABI 镜像不一致、调用失败）。"""


try:  # 零拷贝平面（阶段 3a）：numpy 可选依赖
    import numpy as _np
except ImportError:  # pragma: no cover
    _np = None


def _plane_nbytes(pl) -> int:
    """平面字节数（bytes 按长度；ndarray 按 nbytes）。"""
    if isinstance(pl, (bytes, bytearray, memoryview)):
        return len(pl)
    if _np is not None and isinstance(pl, _np.ndarray):
        return int(pl.nbytes)
    raise ToposCodecError(f"不支持的平面类型 {type(pl).__name__}（bytes 或 numpy uint16 数组）")


def _fill_frame_input(cin, planes) -> list:
    """填 _CFrameInput 平面指针：bytes 零转换；C 连续小端 uint16 数组零拷贝。

    返回 keep 列表——调用期间必须持有（防数组 GC 释放底层缓冲）；
    调用方挂在 cin._keep 上，与 cin 同生命周期。
    """
    keep = []
    for i, pl in enumerate(planes):
        if isinstance(pl, (bytes, bytearray, memoryview)):
            if not isinstance(pl, bytes):
                pl = bytes(pl)  # bytearray/memoryview 归一（罕见路径，拷贝可忽略）
            keep.append(pl)
            cin.planes[i] = ctypes.cast(ctypes.c_char_p(pl), ctypes.POINTER(ctypes.c_uint16))
        else:
            if _np is None or not isinstance(pl, _np.ndarray):
                raise ToposCodecError(
                    f"平面 {i} 类型 {type(pl).__name__} 不支持（bytes 或 numpy uint16 数组）")
            if pl.dtype != _np.uint16 or not pl.flags['C_CONTIGUOUS']:
                raise ToposCodecError(
                    f"平面 {i} 须为 C 连续小端 uint16 数组（得到 dtype={pl.dtype}, "
                    f"contiguous={bool(pl.flags['C_CONTIGUOUS'])}）——先 ascontiguousarray")
            keep.append(pl)
            cin.planes[i] = pl.ctypes.data_as(ctypes.POINTER(ctypes.c_uint16))
        cin.strides[i] = 0  # tight
    return keep


class _CVersionInfo(ctypes.Structure):
    """镜像 topos_version_info（字段顺序/宽度不得改动，受 sizeof 校验保护）。"""

    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("version_major", ctypes.c_uint32),
        ("version_minor", ctypes.c_uint32),
        ("version_patch", ctypes.c_uint32),
        ("git_commit", ctypes.c_char_p),
        ("build_target", ctypes.c_char_p),
        ("reserved", ctypes.c_uint32 * 4),
    ]


class _CCpuFeatures(ctypes.Structure):
    """镜像 topos_cpu_features。"""

    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32 * 5),
    ]


class _CCodecCapabilities(ctypes.Structure):
    """镜像 topos_codec_capabilities（RD7-01 truthful feature gate）。"""

    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("max_width", ctypes.c_uint32),
        ("max_height", ctypes.c_uint32),
        ("auto2k_max_dim", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32 * 4),
    ]


# —— 阶段 4 镜像（字段顺序/宽度不得改动，struct_size 由 C 侧校验） ——

TC_FRAME_MAX_PLANES = 4
TC_FRAME_SLICE_OK = 0
TC_FRAME_SLICE_CONCEALED = 1
TC_MAX_SLICES = 512


class _CFrameConfig(ctypes.Structure):
    """镜像 topos_frame_config。"""

    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("visible_width", ctypes.c_uint16),
        ("visible_height", ctypes.c_uint16),
        ("profile", ctypes.c_uint8),
        ("pixel_format", ctypes.c_uint8),
        ("bit_depth", ctypes.c_uint8),
        ("qmatrix_id", ctypes.c_uint8),
        ("qp_base", ctypes.c_uint8),
        ("qp_delta_luma", ctypes.c_int8),
        ("qp_delta_chroma", ctypes.c_int8),
        ("slice_rows", ctypes.c_uint8),
        ("alpha_mode", ctypes.c_uint8),
        ("alpha_bit_depth", ctypes.c_uint8),
        ("alpha_premultiplied", ctypes.c_uint8),
        ("color_range", ctypes.c_uint8),
        ("color_primaries", ctypes.c_uint8),
        ("color_transfer", ctypes.c_uint8),
        ("color_matrix", ctypes.c_uint8),
        ("chroma_siting", ctypes.c_uint8),
        ("sar_num", ctypes.c_uint16),
        ("sar_den", ctypes.c_uint16),
        ("reserved", ctypes.c_uint32 * 8),
    ]


class _CFrameInput(ctypes.Structure):
    """镜像 topos_frame_input。"""

    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("planes", ctypes.POINTER(ctypes.c_uint16) * TC_FRAME_MAX_PLANES),
        ("strides", ctypes.c_size_t * TC_FRAME_MAX_PLANES),
        ("reserved", ctypes.c_uint32 * 8),
    ]


class _CFrameStats(ctypes.Structure):
    """镜像 topos_frame_stats。"""

    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("packet_size", ctypes.c_uint32),
        ("color_payload_bytes", ctypes.c_uint32),
        ("alpha_payload_bytes", ctypes.c_uint32),
        ("color_header_bytes", ctypes.c_uint32),
        ("alpha_header_bytes", ctypes.c_uint32),
        ("slice_count", ctypes.c_uint16),
        ("qp_base", ctypes.c_uint8),
        ("reserved8", ctypes.c_uint8),
        ("alpha_max_abs_error", ctypes.c_uint16),
        ("reserved16", ctypes.c_uint16),
        ("reserved", ctypes.c_uint32 * 8),
    ]


class _CFrameOutput(ctypes.Structure):
    """镜像 topos_frame_output。"""

    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("visible_width", ctypes.c_uint16),
        ("visible_height", ctypes.c_uint16),
        ("coded_width", ctypes.c_uint16),
        ("coded_height", ctypes.c_uint16),
        ("plane_count", ctypes.c_uint8),
        ("bit_depth", ctypes.c_uint8),
        ("profile", ctypes.c_uint8),
        ("pixel_format", ctypes.c_uint8),
        ("alpha_mode", ctypes.c_uint8),
        ("alpha_bit_depth", ctypes.c_uint8),
        ("color_range", ctypes.c_uint8),
        ("color_primaries", ctypes.c_uint8),
        ("color_transfer", ctypes.c_uint8),
        ("color_matrix", ctypes.c_uint8),
        ("chroma_siting", ctypes.c_uint8),
        ("concealed_slices", ctypes.c_uint16),
        ("slice_count", ctypes.c_uint16),
        ("sar_num", ctypes.c_uint16),
        ("sar_den", ctypes.c_uint16),
        ("slice_status", ctypes.c_uint8 * TC_MAX_SLICES),
        ("reserved", ctypes.c_uint32 * 8),
    ]


class _CPlaneView(ctypes.Structure):
    """镜像 topos_plane_view（M2 持久 decoder context）。"""

    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("pixels", ctypes.c_void_p),
        ("stride", ctypes.c_size_t),
    ]


TC_DECODE_MEMORY_CPU = 0
TC_DECODE_MEMORY_HOST_VISIBLE_GPU = 1


class _CDecodeRequest(ctypes.Structure):
    """镜像 topos_decode_request（RD1 统一解码请求）。"""

    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("mode", ctypes.c_uint32),
        ("scale", ctypes.c_uint32),
        ("target_width", ctypes.c_uint32),
        ("target_height", ctypes.c_uint32),
        ("quality", ctypes.c_uint32),
        ("memory_type", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32 * 4),
    ]


ToposDecodeRequest = _CDecodeRequest


class _CDecodeSurface(ctypes.Structure):
    """镜像 topos_decode_surface；planes 指向 caller-owned 映射缓冲。"""

    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("memory_type", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32),
        ("planes", _CPlaneView * TC_FRAME_MAX_PLANES),
    ]


class _CDecoderConfig(ctypes.Structure):
    """镜像 topos_decoder_config（M2；全 0 保留字段即合法）。

    max_slice_workers（M10-4，原 reserved[7]）：本 context 每帧 slice 批次
    的 worker 预算，0 = 默认（进程线程数）。顺序流水两路各配半池。
    """

    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("max_slice_workers", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32 * 7),
    ]


class _CGopFrameInfo(ctypes.Structure):
    """镜像 topos_gop_frame_info（V9 GOP context，micro-gop 计划批 1）。"""
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("frame_type", ctypes.c_uint8),
        ("ref_distance", ctypes.c_uint8),
        ("gop_id", ctypes.c_uint16),
        ("sample_index", ctypes.c_uint32),
        ("ref_state", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32 * 3),
    ]


# topos_gop_ref_state（与 topos_codec.h 枚举一致）
TOPOS_GOP_NO_REF = 0
TOPOS_GOP_REF_READY = 1
TOPOS_GOP_REF_INVALID = 2

# V9 错误码（topos_codec.h；§3.8 参考失效语义）
TC_ERR_REFERENCE_INVALID = -17


class _CEncodeStageStats(ctypes.Structure):
    """镜像 tc_encode_stage_stats（E1 诊断，native codec.h）。"""

    _fields_ = [
        ("pad_ns", ctypes.c_uint64),
        ("fill_ns", ctypes.c_uint64),
        ("entropy_ns", ctypes.c_uint64),
        ("crc_ns", ctypes.c_uint64),
        ("copy_ns", ctypes.c_uint64),
        ("asm_ns", ctypes.c_uint64),
        ("slices", ctypes.c_uint32),
        ("blocks", ctypes.c_uint32),
        ("dct_ns", ctypes.c_uint64),
        ("probe_fill_ns", ctypes.c_uint64),
        ("prep_wall_ns", ctypes.c_uint64),
        ("probe_wall_ns", ctypes.c_uint64),
        ("final_wall_ns", ctypes.c_uint64),
        ("check_wall_ns", ctypes.c_uint64),
    ]


class _CDecodeStageStats(ctypes.Structure):
    """镜像 tc_decode_stage_stats（RD0-03；内部诊断 ABI）。"""

    _fields_ = [
        ("scan_ns", ctypes.c_uint64),
        ("crc_ns", ctypes.c_uint64),
        ("entropy_ns", ctypes.c_uint64),
        ("dequant_idct_ns", ctypes.c_uint64),
        ("output_ns", ctypes.c_uint64),
        ("alloc_ns", ctypes.c_uint64),
        ("blocks", ctypes.c_uint32),
        ("nonzero_ac", ctypes.c_uint32),
        ("dc_only_blocks", ctypes.c_uint32),
        ("slices", ctypes.c_uint32),
        ("packet_bytes_read", ctypes.c_uint64),
        ("segments_parsed", ctypes.c_uint64),
        ("segments_skipped", ctypes.c_uint64),
        ("entropy_symbols", ctypes.c_uint64),
        ("coefficients_skipped", ctypes.c_uint64),
        ("idct_samples", ctypes.c_uint64),
        ("upload_bytes", ctypes.c_uint64),
        ("frames", ctypes.c_uint64),
        ("wall_ns", ctypes.c_uint64),
        ("p50_ns", ctypes.c_uint64),
        ("p95_ns", ctypes.c_uint64),
        ("p99_ns", ctypes.c_uint64),
    ]


class _CBatchPacket(ctypes.Structure):
    """镜像 topos_batch_packet（阶段4 跨帧批量解码）。"""

    _fields_ = [
        ("data", ctypes.c_void_p),
        ("size", ctypes.c_size_t),
    ]


@dataclass(frozen=True)
class ToposVersion:
    major: int
    minor: int
    patch: int
    git_commit: str
    build_target: str
    abi_version: int
    struct_size: int


@dataclass(frozen=True)
class ToposCpuFeatures:
    flags: int
    abi_version: int
    struct_size: int

    @property
    def avx2(self) -> bool:
        return bool(self.flags & TOPOS_CPU_X86_AVX2)

    @property
    def avx512f(self) -> bool:
        return bool(self.flags & TOPOS_CPU_X86_AVX512F)

    @property
    def neon(self) -> bool:
        return bool(self.flags & TOPOS_CPU_ARM_NEON)

    @property
    def fma(self) -> bool:
        return bool(self.flags & TOPOS_CPU_X86_FMA)


@dataclass(frozen=True)
class ToposCodecCapabilities:
    flags: int
    max_width: int
    max_height: int
    auto2k_max_dim: int
    abi_version: int
    struct_size: int

    @property
    def full_decode(self) -> bool:
        return bool(self.flags & TC_CODEC_CAP_FULL_DECODE)

    @property
    def fixed_reduced_decode(self) -> bool:
        return bool(self.flags & TC_CODEC_CAP_FIXED_REDUCED_DECODE)

    @property
    def auto2k_decode(self) -> bool:
        return bool(self.flags & TC_CODEC_CAP_AUTO_2K_DECODE)

    @property
    def cpu_surface_output(self) -> bool:
        return bool(self.flags & TC_CODEC_CAP_CPU_SURFACE_OUTPUT)

    @property
    def host_visible_gpu_surface(self) -> bool:
        return bool(self.flags & TC_CODEC_CAP_HOST_VISIBLE_GPU_SURFACE)

    @property
    def v7a_band_decode(self) -> bool:
        return bool(self.flags & TC_CODEC_CAP_V7A_BAND_DECODE)

    @property
    def v7b_scalable_base(self) -> bool:
        return bool(self.flags & TC_CODEC_CAP_V7B_SCALABLE_BASE)

    @property
    def decode_drop_alpha(self) -> bool:
        return bool(self.flags & TC_CODEC_CAP_DECODE_DROP_ALPHA)


@dataclass(frozen=True)
class ToposFrameStats:
    """tc_frame_encode 的统计输出（spec §11.3）。"""

    packet_size: int
    color_payload_bytes: int
    alpha_payload_bytes: int
    color_header_bytes: int
    alpha_header_bytes: int
    slice_count: int
    qp_base: int
    alpha_max_abs_error: int


@dataclass(frozen=True)
class ToposEncodeStageStats:
    """编码阶段 profile 累计值（纳秒；worker 字段是 CPU 累计）。"""

    pad_ns: int
    fill_ns: int
    entropy_ns: int
    crc_ns: int
    copy_ns: int
    asm_ns: int
    slices: int
    blocks: int
    dct_ns: int
    probe_fill_ns: int
    prep_wall_ns: int
    probe_wall_ns: int
    final_wall_ns: int
    check_wall_ns: int


#: worker CPU 累计字段（slice 并行下各任务局部计时 join 求和，可超墙钟）
ENCODE_STAGE_CPU_FIELDS = (
    "pad_ns", "fill_ns", "entropy_ns", "crc_ns", "copy_ns", "asm_ns",
    "dct_ns", "probe_fill_ns",
)
#: native 调用线程墙钟分桶（prep+probe+final+check ≈ sized 总墙钟）
ENCODE_STAGE_WALL_FIELDS = (
    "prep_wall_ns", "probe_wall_ns", "final_wall_ns", "check_wall_ns",
)
ENCODE_STAGE_COUNTER_FIELDS = ("slices", "blocks")


def group_native_encode_stage_stats(stats: Any) -> Dict[str, Any]:
    """把 ``ToposEncodeStageStats``（或同名字段对象/映射）分组为 JSON 报告。

    T-P1-11 步骤 5：报告中墙钟与多线程累计 CPU time 必须显式区分——
    worker CPU 字段在 slice 并行时会超过墙钟，直接相加/对比会误导调参。
    """
    def _get(name: str) -> int:
        if isinstance(stats, dict):
            value = stats.get(name, 0)
        else:
            value = getattr(stats, name, 0)
        try:
            return int(value)
        except (TypeError, ValueError):
            return 0

    grouped: Dict[str, Any] = {
        "worker_cpu_ns": {name: _get(name) for name in ENCODE_STAGE_CPU_FIELDS},
        "wall_ns": {name: _get(name) for name in ENCODE_STAGE_WALL_FIELDS},
        "counters": {name: _get(name) for name in ENCODE_STAGE_COUNTER_FIELDS},
    }
    grouped["worker_cpu_ns_total"] = sum(
        grouped["worker_cpu_ns"].values())
    grouped["wall_ns_total"] = sum(grouped["wall_ns"].values())
    return grouped


@dataclass(frozen=True)
class ToposDecodeStageStats:
    """解码阶段 profile 累计值与 reduced 路径读/解/算/写计数。"""

    scan_ns: int
    crc_ns: int
    entropy_ns: int
    dequant_idct_ns: int
    output_ns: int
    alloc_ns: int
    blocks: int
    nonzero_ac: int
    dc_only_blocks: int
    slices: int
    packet_bytes_read: int
    segments_parsed: int
    segments_skipped: int
    entropy_symbols: int
    coefficients_skipped: int
    idct_samples: int
    upload_bytes: int
    frames: int
    wall_ns: int
    p50_ns: int
    p95_ns: int
    p99_ns: int


@dataclass(frozen=True)
class ToposFrameInfo:
    """tc_frame_decode 的几何/状态输出。"""

    visible_width: int
    visible_height: int
    plane_count: int
    bit_depth: int
    profile: int
    pixel_format: int   # R4.2：0 = 4:2:2；1 = 4:4:4（几何按此分派）
    alpha_mode: int
    concealed_slices: int
    slice_count: int
    slice_status: tuple


class ToposFrame:
    """解码帧：planar uint16 little-endian bytes（Y/U/V[/A]，tight）。"""

    __slots__ = ("planes", "info")

    def __init__(self, planes: List[bytes], info: ToposFrameInfo) -> None:
        self.planes = planes
        self.info = info


def _make_decode_request(
    mode: int,
    scale: int = TC_DECODE_SCALE_FULL,
    target_size: Optional[tuple[int, int]] = None,
    quality: int = TC_DECODE_QUALITY_DEFAULT,
    memory_type: int = TC_DECODE_MEMORY_CPU,
    flags: int = 0,
) -> _CDecodeRequest:
    """构造并校验 native ``topos_decode_request`` 镜像。"""
    if int(mode) not in (
        TC_DECODE_MODE_FULL,
        TC_DECODE_MODE_SCALED,
        TC_DECODE_MODE_REDUCED,
        TC_DECODE_MODE_AUTO_2K,
    ):
        raise ToposCodecError(f"不支持的 decode mode: {mode}")
    if int(scale) not in (
        TC_DECODE_SCALE_FULL,
        TC_DECODE_SCALE_HALF,
        TC_DECODE_SCALE_THIRD,
        TC_DECODE_SCALE_QUARTER,
        TC_DECODE_SCALE_EIGHTH,
    ):
        raise ToposCodecError(f"不支持的 decode scale: {scale}")
    if int(quality) not in (
        TC_DECODE_QUALITY_DEFAULT,
        TC_DECODE_QUALITY_FAST,
        TC_DECODE_QUALITY_BALANCED,
        TC_DECODE_QUALITY_HIGH,
    ):
        raise ToposCodecError(f"不支持的 decode quality: {quality}")
    if int(memory_type) not in (
        TC_DECODE_MEMORY_CPU,
        TC_DECODE_MEMORY_HOST_VISIBLE_GPU,
    ):
        raise ToposCodecError(f"不支持的 decode memory_type: {memory_type}")
    if int(flags) & ~TC_DECODE_FLAG_DROP_ALPHA:
        raise ToposCodecError(f"不支持的 decode flags: {flags}")
    if target_size is None:
        width = height = 0
    else:
        if len(target_size) != 2:
            raise ToposCodecError("target_size 必须是 (width, height)")
        width, height = (int(target_size[0]), int(target_size[1]))
        if width <= 0 or height <= 0:
            raise ToposCodecError("target_size 必须为正数")
    return _CDecodeRequest(
        struct_size=ctypes.sizeof(_CDecodeRequest),
        abi_version=TOPOS_CODEC_ABI_VERSION,
        mode=int(mode),
        scale=int(scale),
        target_width=width,
        target_height=height,
        quality=int(quality),
        memory_type=int(memory_type),
        flags=int(flags),
    )


def _frame_info_from_c(info: _CFrameOutput) -> ToposFrameInfo:
    return ToposFrameInfo(
        visible_width=info.visible_width,
        visible_height=info.visible_height,
        plane_count=info.plane_count,
        bit_depth=info.bit_depth,
        profile=info.profile,
        pixel_format=info.pixel_format,
        alpha_mode=info.alpha_mode,
        concealed_slices=info.concealed_slices,
        slice_count=info.slice_count,
        slice_status=tuple(info.slice_status[: info.slice_count]),
    )


class ToposDecoder:
    """持久 decoder context（M2）：单次原生调用整帧解码，输出直写调用方平面。

    - 单实例不可重入（一次仅一个 decode 在飞）；不同实例可并发；
    - context 持有 grow-only 内部缓冲（alpha 行缓冲池），几何稳定时稳态零分配；
    - decode_views 接受预填充的 _CPlaneView 数组（plane_count 项），配合
      上层平面池实现零拷贝输出；
    - 旧 dylib 无 tc_decoder_* 符号时构造抛 ToposCodecError（调用方回退旧路径）。
    """

    def __init__(self, codec: "ToposCodec",
                 max_slice_workers: int = 0) -> None:
        if not getattr(codec, "_has_decoder_ctx", False):
            raise ToposCodecError(
                "libtopos_codec 缺少 tc_decoder_* 符号（需重新构建 native 库）"
            )
        self._codec = codec
        cfg = _CDecoderConfig(
            struct_size=ctypes.sizeof(_CDecoderConfig),
            abi_version=TOPOS_CODEC_ABI_VERSION,
            max_slice_workers=int(max_slice_workers) if max_slice_workers else 0,
        )
        handle = ctypes.c_void_p()
        status = int(codec._lib.tc_decoder_create(ctypes.byref(cfg), ctypes.byref(handle)))
        if status != TC_OK:
            raise ToposCodecError(f"tc_decoder_create 失败 [{status}]: {codec.last_error()}")
        self._handle = handle

    def close(self) -> None:
        """释放原生 context（幂等）。"""
        if getattr(self, "_handle", None) is not None:
            self._codec._lib.tc_decoder_destroy(self._handle)
            self._handle = None

    def set_max_slice_workers(self, workers: int) -> int:
        """更新本 context 的 slice worker 预算并返回生效请求值。

        ``0`` 表示恢复为进程级线程池默认值。调用方应在 context 空闲时
        使用；native 以原子值发布，正在执行的帧保持其已采样预算。
        """
        self._require_open()
        if not getattr(self._codec, "_has_decoder_worker_budget", False):
            raise ToposCodecError(
                "libtopos_codec 缺少 tc_decoder_set_max_slice_workers 符号"
            )
        value = max(0, int(workers))
        status = int(self._codec._lib.tc_decoder_set_max_slice_workers(
            self._handle, ctypes.c_uint32(value)))
        if status != TC_OK:
            raise ToposCodecError(
                f"tc_decoder_set_max_slice_workers 失败 [{status}]: "
                f"{self._codec.last_error()}"
            )
        return value

    def __enter__(self) -> "ToposDecoder":
        return self

    def __exit__(self, *_exc) -> None:
        self.close()

    def prepare(self, packet, size: Optional[int] = None) -> "_CFrameOutput":
        """结构解析并返回几何信息（不做 payload CRC；不承诺 payload 完整性）。

        size 缺省取 len(packet)；packet 为复用 ctypes 缓冲时必须显式传实际
        字节数（len 返回的是缓冲容量）。
        """
        self._require_open()
        n = len(packet) if size is None else size
        info = _CFrameOutput()
        status = int(self._codec._lib.tc_decoder_prepare(
            self._handle, packet, ctypes.c_size_t(n), ctypes.byref(info)))
        if status != TC_OK:
            raise ToposCodecError(f"tc_decoder_prepare 失败 [{status}]: {self._codec.last_error()}")
        return info

    @staticmethod
    def make_request(
        mode: int,
        scale: int = TC_DECODE_SCALE_FULL,
        target_size: Optional[tuple[int, int]] = None,
        quality: int = TC_DECODE_QUALITY_DEFAULT,
        memory_type: int = TC_DECODE_MEMORY_CPU,
        flags: int = 0,
    ) -> _CDecodeRequest:
        """构造 request；返回对象可复用于 prepare/decode/batch。"""
        return _make_decode_request(
            mode, scale, target_size, quality, memory_type, flags)

    def prepare_request(
        self,
        packet,
        request: _CDecodeRequest,
        size: Optional[int] = None,
    ) -> "_CFrameOutput":
        """按 request 查询最终输出几何，不读取 payload。"""
        if not getattr(self._codec, "_has_decoder_request", False):
            raise ToposCodecError(
                "libtopos_codec 缺少 tc_decoder_prepare_request 符号（需重新构建 native 库）"
            )
        if not isinstance(request, _CDecodeRequest):
            raise ToposCodecError("request 必须由 ToposDecoder.make_request 构造")
        self._require_open()
        n = len(packet) if size is None else size
        info = _CFrameOutput()
        status = int(self._codec._lib.tc_decoder_prepare_request(
            self._handle, packet, ctypes.c_size_t(n), ctypes.byref(request),
            ctypes.byref(info),
        ))
        if status != TC_OK:
            raise ToposCodecError(
                f"tc_decoder_prepare_request 失败 [{status}]: {self._codec.last_error()}"
            )
        return info

    def decode_views(self, packet, views, plane_count: int,
                     size: Optional[int] = None) -> ToposFrameInfo:
        """整帧解码进 views[0..plane_count)（须已填 struct_size/abi_version/pixels）。

        packet 为 bytes 或 ctypes 缓冲（调用期间须保持存活）；复用缓冲时
        显式传 size（len 返回容量而非实际字节数）。
        坏 slice 按 §9 conceal，帧仍交付（返回 info.concealed_slices > 0）。
        """
        self._require_open()
        n = len(packet) if size is None else size
        info = _CFrameOutput()
        status = int(self._codec._lib.tc_decoder_decode(
            self._handle, packet, ctypes.c_size_t(n), views, ctypes.byref(info)))
        if status not in (TC_OK, TC_WARN_CONCEALED):
            raise ToposCodecError(f"tc_decoder_decode 失败 [{status}]: {self._codec.last_error()}")
        return ToposFrameInfo(
            visible_width=info.visible_width,
            visible_height=info.visible_height,
            plane_count=info.plane_count,
            bit_depth=info.bit_depth,
            profile=info.profile,
            pixel_format=info.pixel_format,
            alpha_mode=info.alpha_mode,
            concealed_slices=info.concealed_slices,
            slice_count=info.slice_count,
            slice_status=tuple(info.slice_status[: info.slice_count]),
        )

    def decode_views_request(
        self,
        packet,
        views,
        request: _CDecodeRequest,
        size: Optional[int] = None,
    ) -> ToposFrameInfo:
        """按 request 直接解码到目标尺寸 views。"""
        if not getattr(self._codec, "_has_decoder_request", False):
            raise ToposCodecError(
                "libtopos_codec 缺少 tc_decoder_decode_request 符号（需重新构建 native 库）"
            )
        if not isinstance(request, _CDecodeRequest):
            raise ToposCodecError("request 必须由 ToposDecoder.make_request 构造")
        self._require_open()
        n = len(packet) if size is None else size
        info = _CFrameOutput()
        status = int(self._codec._lib.tc_decoder_decode_request(
            self._handle, packet, ctypes.c_size_t(n), ctypes.byref(request),
            views, ctypes.byref(info),
        ))
        if status not in (TC_OK, TC_WARN_CONCEALED):
            raise ToposCodecError(
                f"tc_decoder_decode_request 失败 [{status}]: {self._codec.last_error()}"
            )
        return _frame_info_from_c(info)

    def decode_surface(self, packet, views, memory_type: int = TC_DECODE_MEMORY_CPU,
                       size: Optional[int] = None) -> ToposFrameInfo:
        """直接解码到 caller-owned surface plane views。

        ``memory_type=TC_DECODE_MEMORY_HOST_VISIBLE_GPU`` 只表示上层已经完成
        GPU 资源映射和同步准备；native codec 仍按 uint16 plane 指针写入，
        不创建中间帧。``views`` 须为 ``make_views`` 生成的 4 项数组。
        """
        if not getattr(self._codec, "_has_decoder_surface", False):
            raise ToposCodecError(
                "libtopos_codec 缺少 tc_decoder_decode_surface 符号"
            )
        if int(memory_type) not in (TC_DECODE_MEMORY_CPU,
                                    TC_DECODE_MEMORY_HOST_VISIBLE_GPU):
            raise ToposCodecError(f"不支持的 surface memory_type: {memory_type}")
        self._require_open()
        surface = _CDecodeSurface(
            struct_size=ctypes.sizeof(_CDecodeSurface),
            abi_version=TOPOS_CODEC_ABI_VERSION,
            memory_type=int(memory_type),
        )
        for p in range(TC_FRAME_MAX_PLANES):
            surface.planes[p] = views[p]
        info = _CFrameOutput()
        n = len(packet) if size is None else size
        status = int(self._codec._lib.tc_decoder_decode_surface(
            self._handle, packet, ctypes.c_size_t(n), ctypes.byref(surface),
            ctypes.byref(info),
        ))
        if status not in (TC_OK, TC_WARN_CONCEALED):
            raise ToposCodecError(
                f"tc_decoder_decode_surface 失败 [{status}]: {self._codec.last_error()}"
            )
        return ToposFrameInfo(
            visible_width=info.visible_width,
            visible_height=info.visible_height,
            plane_count=info.plane_count,
            bit_depth=info.bit_depth,
            profile=info.profile,
            pixel_format=info.pixel_format,
            alpha_mode=info.alpha_mode,
            concealed_slices=info.concealed_slices,
            slice_count=info.slice_count,
            slice_status=tuple(info.slice_status[: info.slice_count]),
        )

    def decode_surface_request(
        self,
        packet,
        views,
        request: _CDecodeRequest,
        size: Optional[int] = None,
    ) -> ToposFrameInfo:
        """按 request 直接写入 caller-owned CPU/host-visible GPU surface。"""
        if not getattr(self._codec, "_has_decoder_surface_request", False):
            raise ToposCodecError(
                "libtopos_codec 缺少 tc_decoder_decode_surface_request 符号"
            )
        if not isinstance(request, _CDecodeRequest):
            raise ToposCodecError("request 必须由 ToposDecoder.make_request 构造")
        self._require_open()
        surface = _CDecodeSurface(
            struct_size=ctypes.sizeof(_CDecodeSurface),
            abi_version=TOPOS_CODEC_ABI_VERSION,
            memory_type=int(request.memory_type),
        )
        for p in range(TC_FRAME_MAX_PLANES):
            surface.planes[p] = views[p]
        n = len(packet) if size is None else size
        info = _CFrameOutput()
        status = int(self._codec._lib.tc_decoder_decode_surface_request(
            self._handle, packet, ctypes.c_size_t(n), ctypes.byref(request),
            ctypes.byref(surface), ctypes.byref(info),
        ))
        if status not in (TC_OK, TC_WARN_CONCEALED):
            raise ToposCodecError(
                "tc_decoder_decode_surface_request 失败 "
                f"[{status}]: {self._codec.last_error()}"
            )
        return _frame_info_from_c(info)

    def decode_surface_lease(
        self,
        packet,
        lease,
        request: Optional[_CDecodeRequest] = None,
        size: Optional[int] = None,
    ) -> ToposFrameInfo:
        """Decode synchronously into an already mapped surface lease.

        The platform owns ``lease`` and must perform ``acquire()`` and
        ``map()`` first.  This method brackets only the native write with
        ``begin_decode()/end_decode()``; the caller must then ``unmap()``,
        insert a platform fence, and ``release()``.  No pointer is retained by
        the native codec after this method returns.
        """
        if lease is None or not hasattr(lease, "begin_decode"):
            raise ToposCodecError("lease 必须实现 DecodeSurfaceLease 协议")
        memory_type = int(getattr(lease, "memory_type", -1))
        if memory_type not in (
            TC_DECODE_MEMORY_CPU,
            TC_DECODE_MEMORY_HOST_VISIBLE_GPU,
        ):
            raise ToposCodecError(f"lease memory_type 不支持: {memory_type}")
        views = lease.begin_decode()
        try:
            if request is None:
                info = self.decode_surface(
                    packet, views, memory_type=memory_type, size=size
                )
            else:
                if int(request.memory_type) != memory_type:
                    raise ToposCodecError(
                        "request.memory_type 与 surface lease 不一致"
                    )
                info = self.decode_surface_request(packet, views, request, size)
        finally:
            lease.end_decode()
        return info

    def make_views(self, arrays) -> "ctypes.Array":
        """由 numpy '<u2' 平面数组构建 plane view 数组（tight stride）。

        视图仅持有数组地址——调用方须自行保持数组存活到解码返回，
        否则 worker 写入已释放缓冲（堆损坏，远端崩溃难定位）。
        """
        views = (_CPlaneView * TC_FRAME_MAX_PLANES)()
        for p, arr in enumerate(arrays):
            views[p].struct_size = ctypes.sizeof(_CPlaneView)
            views[p].abi_version = TOPOS_CODEC_ABI_VERSION
            views[p].pixels = arr.ctypes.data
            views[p].stride = 0
        return views

    def decode_views_batch(self, packets, views_list):
        """跨帧批量解码（阶段4 单批跨帧切片队列）。

        packets 各项为 bytes（调用期间由本函数持有引用）或 (地址, 字节数)
        二元组（如 packets_into_batch 的 arena 内槽位——零拷贝直读，调用
        期间须保持 arena 存活）；views_list 为 n 组 plane view 数组
        （make_views 产物；多余槽位零值——C 侧只读 plane_count 内的槽）。
        全部帧的切片进同一线程池批次——一次唤醒/汇合消化整批，消除逐帧
        派发/汇合与帧间串行段。逐帧语义同 decode_views：坏 slice 按 §9
        conceal 交付；帧间错误隔离。返回 ToposFrameInfo 列表。
        需较新 dylib（tc_decoder_decode_batch 符号），否则抛 ToposCodecError。
        """
        if not getattr(self._codec, "_has_decoder_batch", False):
            raise ToposCodecError("libtopos_codec 缺少 tc_decoder_decode_batch 符号")
        self._require_open()
        n = len(packets)
        if n == 0:
            return []
        infos = (_CFrameOutput * n)()
        flat = (_CPlaneView * (n * TC_FRAME_MAX_PLANES))()
        for i, views in enumerate(views_list):
            for p in range(TC_FRAME_MAX_PLANES):
                flat[i * TC_FRAME_MAX_PLANES + p] = views[p]
        batch = (_CBatchPacket * n)()
        keepalive = []  # bytes 项显式持有到调用结束（循环变量重绑不保活）
        for i, pkt in enumerate(packets):
            if isinstance(pkt, tuple):
                addr, sz = pkt
                batch[i].data = ctypes.c_void_p(addr)
                batch[i].size = sz
            else:
                buf = pkt if isinstance(pkt, bytes) else bytes(pkt)
                keepalive.append(buf)
                batch[i].data = ctypes.cast(ctypes.c_char_p(buf), ctypes.c_void_p)
                batch[i].size = len(buf)
        status = int(self._codec._lib.tc_decoder_decode_batch(
            self._handle, batch, ctypes.c_uint32(n), flat, infos))
        out = []
        for i in range(n):
            info = infos[i]
            out.append(ToposFrameInfo(
                visible_width=info.visible_width,
                visible_height=info.visible_height,
                plane_count=info.plane_count,
                bit_depth=info.bit_depth,
                profile=info.profile,
                pixel_format=info.pixel_format,
                alpha_mode=info.alpha_mode,
                concealed_slices=info.concealed_slices,
                slice_count=info.slice_count,
                slice_status=tuple(info.slice_status[: info.slice_count]),
            ))
        if status not in (TC_OK, TC_WARN_CONCEALED):
            raise ToposCodecError(
                f"tc_decoder_decode_batch 失败 [{status}]: {self._codec.last_error()}"
            )
        return out

    def decode_views_batch_request(
        self,
        packets,
        views_list,
        request: _CDecodeRequest,
    ) -> list[ToposFrameInfo]:
        """按同一 request 跨帧批量解码到目标尺寸 views。"""
        if not getattr(self._codec, "_has_decoder_batch_request", False):
            raise ToposCodecError(
                "libtopos_codec 缺少 tc_decoder_decode_batch_request 符号"
            )
        if not isinstance(request, _CDecodeRequest):
            raise ToposCodecError("request 必须由 ToposDecoder.make_request 构造")
        self._require_open()
        n = len(packets)
        if n == 0:
            return []
        infos = (_CFrameOutput * n)()
        flat = (_CPlaneView * (n * TC_FRAME_MAX_PLANES))()
        for i, views in enumerate(views_list):
            for p in range(TC_FRAME_MAX_PLANES):
                flat[i * TC_FRAME_MAX_PLANES + p] = views[p]
        batch = (_CBatchPacket * n)()
        keepalive = []
        for i, pkt in enumerate(packets):
            if isinstance(pkt, tuple):
                addr, sz = pkt
                batch[i].data = ctypes.c_void_p(addr)
                batch[i].size = sz
            else:
                buf = pkt if isinstance(pkt, bytes) else bytes(pkt)
                keepalive.append(buf)
                batch[i].data = ctypes.cast(ctypes.c_char_p(buf), ctypes.c_void_p)
                batch[i].size = len(buf)
        status = int(self._codec._lib.tc_decoder_decode_batch_request(
            self._handle, batch, ctypes.c_uint32(n), ctypes.byref(request), flat, infos,
        ))
        out = [_frame_info_from_c(infos[i]) for i in range(n)]
        if status not in (TC_OK, TC_WARN_CONCEALED):
            raise ToposCodecError(
                "tc_decoder_decode_batch_request 失败 "
                f"[{status}]: {self._codec.last_error()}"
            )
        return out

    def _require_open(self) -> None:
        if getattr(self, "_handle", None) is None:
            raise ToposCodecError("ToposDecoder 已关闭")


def _repo_root() -> Path:
    # src/shared/codec/topos_binding.py → 上溯 3 级为仓库根
    return Path(__file__).resolve().parents[3]


def candidate_library_paths() -> List[Path]:
    """自动搜索 libtopos_codec 的候选路径（显式指定优先级更高，见 load_library）。

    覆盖两种布局：
    1. 仓库开发布局：``<root>/native/topos_codec/build/<cfg>/<name>``；
    2. 独立发布布局：绑定模块同级或其父级的 ``lib/`` 目录（发布包把
       dylib/so/dll 放在 ``python/topos_codec/lib/`` 即可被找到）。
    """
    paths: List[Path] = []
    here = Path(__file__).resolve().parent
    for base in (here / "lib", here.parent / "lib"):
        for name in _LIB_NAMES:
            paths.append(base / name)
    root = _repo_root()
    build_root = root / "native" / "build"
    seen: set[Path] = set()

    def append_build_dir(directory: Path) -> None:
        for name in _LIB_NAMES:
            candidate = directory / name
            if candidate not in seen:
                paths.append(candidate)
                seen.add(candidate)

    for cfg in _BUILD_CONFIGS:
        append_build_dir(build_root / cfg)

    # Windows 开发构建常按工具链命名（例如 win-release-zig4），不一定
    # 落在固定的 release/debug 目录。只扫描 codec 自己的 build 根目录，
    # 不递归到仓库其它目录，避免把测试素材或第三方 DLL 当成 codec。
    if sys.platform == "win32" and build_root.is_dir():
        for directory in sorted(build_root.iterdir(), key=lambda p: p.name.lower()):
            if directory.is_dir():
                append_build_dir(directory)
                # VS 多配置生成器把产物放在配置子目录（<dir>/Release|Debug）；
                # 单配置（Ninja/MinGW）直接在 <dir> 下——两种布局都探测
                for cfg in ("Release", "Debug"):
                    if (directory / cfg).is_dir():
                        append_build_dir(directory / cfg)
    meipass = getattr(sys, "_MEIPASS", None)
    if meipass:
        for name in _LIB_NAMES:
            paths.append(Path(meipass) / name)
    return paths


def load_library(path: Optional[str] = None) -> ctypes.CDLL:
    """加载 libtopos_codec。

    显式 path（或 TOPOS_CODEC_LIB 环境变量）时：文件必须存在，否则立即报错
    （可预测性优先，不静默回退搜索）。未指定时按 candidate_library_paths() 搜索。
    """
    explicit = path or os.environ.get("TOPOS_CODEC_LIB")
    if explicit:
        p = Path(explicit)
        if p.is_file():
            return ctypes.CDLL(str(p))
        raise ToposCodecError(f"TOPOS_CODEC_LIB/path 指定的库不存在: {p}")
    tried: List[str] = []
    failures: List[str] = []
    for cand in candidate_library_paths():
        if cand.is_file():
            try:
                return ctypes.CDLL(str(cand))
            except OSError as exc:
                # 某些 Windows 构建系统会把 import/static archive 命名成
                # .dll。继续尝试其它构建目录，而不是让第一个伪 DLL
                # 阻塞真正的共享库。
                failures.append(f"{cand}: {exc}")
        tried.append(str(cand))
    if failures:
        raise ToposCodecError(
            "找到 libtopos_codec 候选文件，但都无法加载；请重新构建 Windows DLL。"
            f"失败详情: {failures[:3]}"
        )
    raise ToposCodecError(
        "未找到 libtopos_codec；请先运行 bash native/topos_codec/run_tests.sh 构建，"
        "或将动态库放入本模块同级 lib/ 目录，或用 TOPOS_CODEC_LIB 环境变量显式指定。"
        f"已尝试: {tried[:6]}…"
    )


# ---------------- 库解析三态（T-P0-10：正确 release / 不兼容 / 不存在） ----------------

#: resolve_topos_codec_library() 的 status 取值。
LIB_RESOLUTION_OK = "ok"
LIB_RESOLUTION_INCOMPATIBLE = "incompatible"
LIB_RESOLUTION_NOT_FOUND = "not_found"


@dataclass(frozen=True)
class ToposLibraryResolution:
    """libtopos_codec 解析结果（三态 + 证据）。

    ``status``：
    - ``ok``：候选库加载且通过绑定层 ABI 校验，``library_path`` 可用；
    - ``incompatible``：存在候选但加载/ABI 校验失败（旧库、损坏、缺符号）——
      ``detail`` 携带根因；调用方不得回落为"文件损坏"；
    - ``not_found``：无任何候选文件存在。
    """

    status: str
    library_path: Optional[str] = None
    detail: Optional[str] = None


def resolve_topos_codec_library(path: Optional[str] = None) -> ToposLibraryResolution:
    """把库解析收敛为三态判定（T-P0-10 步骤 6）。

    显式 path（或 TOPOS_CODEC_LIB）：存在但加载/校验失败 → incompatible；
    不存在 → not_found。未显式指定时按候选顺序找第一个**加载并通过 ABI
    校验**的库；存在但失败的候选记录为 incompatible 证据并继续搜索——
    不再"搜索到第一个 dylib 就使用"。
    """
    explicit = path or os.environ.get("TOPOS_CODEC_LIB")
    if explicit:
        p = Path(explicit)
        if not p.is_file():
            return ToposLibraryResolution(
                status=LIB_RESOLUTION_NOT_FOUND,
                detail=f"TOPOS_CODEC_LIB/path 指定的库不存在: {p}",
            )
        try:
            ToposCodec(load_library(str(p)))
            return ToposLibraryResolution(
                status=LIB_RESOLUTION_OK, library_path=str(p))
        except (ToposCodecError, OSError, AttributeError) as exc:
            return ToposLibraryResolution(
                status=LIB_RESOLUTION_INCOMPATIBLE,
                library_path=str(p), detail=str(exc),
            )
    first_failure: Optional[str] = None
    for cand in candidate_library_paths():
        if not cand.is_file():
            continue
        try:
            ToposCodec(load_library(str(cand)))
            return ToposLibraryResolution(
                status=LIB_RESOLUTION_OK, library_path=str(cand))
        except (ToposCodecError, OSError, AttributeError) as exc:
            if first_failure is None:
                first_failure = f"{cand}: {exc}"
    if first_failure is not None:
        return ToposLibraryResolution(
            status=LIB_RESOLUTION_INCOMPATIBLE, detail=first_failure)
    return ToposLibraryResolution(
        status=LIB_RESOLUTION_NOT_FOUND,
        detail="candidate_library_paths() 无任何 libtopos_codec 候选文件",
    )


def _decode(raw: Optional[bytes]) -> str:
    return (raw or b"").decode("utf-8", errors="replace")



# ---------------- MOV 容器（阶段 5，container_spec_v1.md） ----------------

TC_IO_READ_FN = ctypes.CFUNCTYPE(
    ctypes.c_int32, ctypes.c_void_p, ctypes.c_uint64,
    ctypes.c_void_p, ctypes.c_size_t,
)
TC_IO_WRITE_FN = ctypes.CFUNCTYPE(
    ctypes.c_int32, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
)
TC_IO_SEEK_WRITE_FN = ctypes.CFUNCTYPE(
    ctypes.c_int32, ctypes.c_void_p, ctypes.c_uint64,
    ctypes.c_void_p, ctypes.c_size_t,
)


@dataclass(frozen=True)
class ToposGopFrameInfo:
    """V9 GOP context 单帧信息（observe/feed 返回）。"""

    frame_type: int
    ref_distance: int
    gop_id: int
    sample_index: int
    ref_state: int


class ToposGopContext:
    """V9 帧间微 GOP context（topos_v9_micro_gop_plan 批 5 绑定面）。

    - 序列状态机 + 参考帧事务在原生侧（§3.2/§3.8）；单实例不可重入，
      不同实例可并发；
    - feed：解码一帧并推进参考链（P 重建 = clip(ref + X' - mid)，原生侧
      完成）；``planes`` 可 None（仅推进参考/序列观察，不取像素）；
    - encode_frame：I/P 决策在原生（force_intra / 参考不 READY / P ≥ 最近
      I 自动回退 I）；返回 (packet, stats)；
    - abort：事务回滚（取消后不留参考）；reset_to_i 回到 NO_REF；
    - V9 P0 no-alpha：cfg.alpha_mode != 0 时原生 create 拒绝
      （NOT_IMPLEMENTED，§3.7 显式收缩）；
    - 旧 dylib 缺 tc_gop_context_* 符号时构造抛 ToposCodecError。
    """

    def __init__(self, codec: "ToposCodec", config: "_CFrameConfig") -> None:
        if not getattr(codec, "_has_gop_ctx", False):
            raise ToposCodecError(
                "libtopos_codec 缺少 tc_gop_context_* 符号（需重新构建 native 库）"
            )
        if int(getattr(config, "reserved", (0,))[0]) not in (10, 11):
            raise ToposCodecError(
                "ToposGopContext 需要帧间载体配置（reserved[0] 10=v9 / 11=v7r3）"
            )
        self._codec = codec
        self._cfg = config  # 持有：原生侧不拷贝调用方内存之外的对象语义
        # 编码输出缓冲 grow-only 复用（V9 复审 2026-09-13 性能项）：
        # packet_bound 对 V9 是 ~200MB 量级的保守上界——此前每次 encode_frame
        # 都 create_string_buffer 重新分配并清零，实测 2K 45~159ms、
        # 4K 215ms/帧，纯绑定开销占墙钟大头。
        self._enc_buf: Optional[ctypes.Array] = None
        handle = ctypes.c_void_p()
        status = int(codec._lib.tc_gop_context_create(
            ctypes.byref(config), ctypes.byref(handle)))
        if status != TC_OK:
            raise ToposCodecError(
                f"tc_gop_context_create 失败 [{status}]: {codec.last_error()}"
            )
        self._handle = handle

    def _require_open(self) -> None:
        if getattr(self, "_handle", None) is None:
            raise ToposCodecError("ToposGopContext 已关闭")

    def close(self) -> None:
        """释放原生 context（幂等）。"""
        if getattr(self, "_handle", None) is not None:
            self._codec._lib.tc_gop_context_close(self._handle)
            self._handle = None
        self._enc_buf = None

    def __enter__(self) -> "ToposGopContext":
        return self

    def __exit__(self, *_exc) -> None:
        self.close()

    @staticmethod
    def _make_info() -> _CGopFrameInfo:
        info = _CGopFrameInfo()
        info.struct_size = ctypes.sizeof(_CGopFrameInfo)
        info.abi_version = TOPOS_CODEC_ABI_VERSION
        return info

    @staticmethod
    def _wrap_info(info: _CGopFrameInfo) -> ToposGopFrameInfo:
        return ToposGopFrameInfo(
            frame_type=int(info.frame_type),
            ref_distance=int(info.ref_distance),
            gop_id=int(info.gop_id),
            sample_index=int(info.sample_index),
            ref_state=int(info.ref_state),
        )

    def observe(self, packet, size: Optional[int] = None) -> ToposGopFrameInfo:
        """序列观察（包扫描 + GOP 状态机；零像素重建）。"""
        self._require_open()
        n = len(packet) if size is None else size
        info = self._make_info()
        status = int(self._codec._lib.tc_gop_context_observe(
            self._handle, packet, ctypes.c_size_t(n), ctypes.byref(info)))
        if status != TC_OK:
            raise ToposCodecError(
                f"tc_gop_context_observe 失败 [{status}]: {self._codec.last_error()}"
            )
        return self._wrap_info(info)

    def feed(self, packet, size: Optional[int] = None,
             planes: Optional[List] = None) -> ToposGopFrameInfo:
        """解码一帧并推进参考链。planes 为可见域平面缓冲（C 连续 uint16
        numpy 数组列表；stride=tight）或 None（不取像素）。

        TC_ERR_REFERENCE_INVALID 等错误以 ToposCodecError 抛出（status 可
        从 str(info) 解析或比较异常消息前缀）——调用方按 §3.8 跳下一 I。
        """
        self._require_open()
        n = len(packet) if size is None else size
        views_p = ctypes.POINTER(_CPlaneView)
        views_arg = None
        keep = []
        if planes is not None:
            views = (_CPlaneView * TC_FRAME_MAX_PLANES)()
            for i, pl in enumerate(planes):
                views[i].struct_size = ctypes.sizeof(_CPlaneView)
                views[i].abi_version = TOPOS_CODEC_ABI_VERSION
                views[i].pixels = pl.ctypes.data_as(ctypes.c_void_p)
                views[i].stride = 0  # tight
                keep.append(pl)
            views_arg = ctypes.cast(views, views_p)
        info = self._make_info()
        status = int(self._codec._lib.tc_gop_context_feed(
            self._handle, packet, ctypes.c_size_t(n),
            views_arg if views_arg is not None else ctypes.cast(None, views_p),
            ctypes.byref(info)))
        if status != TC_OK:
            raise ToposCodecError(
                f"tc_gop_context_feed 失败 [{status}]: {self._codec.last_error()}"
            )
        return self._wrap_info(info)

    def encode_frame(self, planes: List, force_intra: bool = False
                     ) -> "tuple[bytes, ToposFrameStats]":
        """编码一帧（I/P 决策 + 残差合成 + 回退 + 参考事务在原生侧）。

        返回 (packet, stats)。出包缓冲起步 8MB、按 BTS 需量增长——增长
        重试即事务重放（C 契约：BTS 全量不提交，同输入重试逐字节确定）。
        """
        self._require_open()
        if not 3 <= len(planes) <= TC_FRAME_MAX_PLANES:
            raise ToposCodecError(f"planes 数量 {len(planes)} 非法（3 或 4）")
        # 出包缓冲起步 8MB（GOP 分段并行时每 ctx 常驻内存从 packet_bound
        # 的 ~268MB@4K 降到实际包长量级），不足时按 BTS 需量增长——C 契约：
        # BUFFER_TOO_SMALL 全量不提交，同输入重试逐字节确定（topos_codec.h）
        need = self._codec.packet_bound(self._cfg)
        if self._enc_buf is None:
            self._enc_buf = ctypes.create_string_buffer(min(need, 8 * 1024 * 1024))
        buf = self._enc_buf
        cin = _CFrameInput()
        cin.struct_size = ctypes.sizeof(_CFrameInput)
        cin.abi_version = TOPOS_CODEC_ABI_VERSION
        cin._keep = _fill_frame_input(cin, planes)
        need_size = ctypes.c_size_t(0)
        st = _CFrameStats()
        status = int(self._codec._lib.tc_gop_context_encode_frame(
            self._handle, ctypes.byref(cin), buf, ctypes.c_size_t(len(buf)),
            ctypes.byref(need_size), ctypes.c_int(1 if force_intra else 0),
            ctypes.byref(st)))
        if status == TC_ERR_BUFFER_TOO_SMALL and 0 < need_size.value <= need:
            self._enc_buf = ctypes.create_string_buffer(need_size.value)
            buf = self._enc_buf
            status = int(self._codec._lib.tc_gop_context_encode_frame(
                self._handle, ctypes.byref(cin), buf, ctypes.c_size_t(len(buf)),
                ctypes.byref(need_size), ctypes.c_int(1 if force_intra else 0),
                ctypes.byref(st)))
        if status != TC_OK:
            raise ToposCodecError(
                f"tc_gop_context_encode_frame 失败 [{status}]: "
                f"{self._codec.last_error()}"
            )
        # 按实际包长精确拷出（buf.raw 会先复制整个 bound 缓冲再切片——
        # 4K 下 ~268MB/帧的无效拷贝，实测 ~16ms/帧）
        packet = ctypes.string_at(buf, need_size.value)
        stats = ToposFrameStats(
            packet_size=st.packet_size,
            color_payload_bytes=st.color_payload_bytes,
            alpha_payload_bytes=st.alpha_payload_bytes,
            color_header_bytes=st.color_header_bytes,
            alpha_header_bytes=st.alpha_header_bytes,
            slice_count=st.slice_count,
            qp_base=st.qp_base,
            alpha_max_abs_error=st.alpha_max_abs_error,
        )
        return packet, stats

    def abort(self, reset_to_i: bool = False) -> None:
        """事务回滚：回到最近一次成功提交的状态；reset_to_i 额外清参考。"""
        self._require_open()
        self._codec._lib.tc_gop_context_abort(
            self._handle, ctypes.c_int(1 if reset_to_i else 0))

    def state(self) -> "tuple[int, int]":
        """(ref_state, gop_id)。"""
        self._require_open()
        st = ctypes.c_uint32(0)
        gop = ctypes.c_uint16(0)
        status = int(self._codec._lib.tc_gop_context_state(
            self._handle, ctypes.byref(st), ctypes.byref(gop)))
        if status != TC_OK:
            raise ToposCodecError(
                f"tc_gop_context_state 失败 [{status}]: {self._codec.last_error()}"
            )
        return int(st.value), int(gop.value)


class _CIo(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("ctx", ctypes.c_void_p),
        ("read", TC_IO_READ_FN),
        ("write", TC_IO_WRITE_FN),
        ("seek_write", TC_IO_SEEK_WRITE_FN),
        ("length", ctypes.c_uint64),
        ("reserved", ctypes.c_uint32 * 4),
    ]


class _CMovieConfig(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("visible_width", ctypes.c_uint16),
        ("visible_height", ctypes.c_uint16),
        ("profile", ctypes.c_uint8),
        ("pixel_format", ctypes.c_uint8),
        ("bit_depth", ctypes.c_uint8),
        ("qmatrix_id", ctypes.c_uint8),
        ("qp_base", ctypes.c_uint8),
        ("qp_delta_luma", ctypes.c_int8),
        ("qp_delta_chroma", ctypes.c_int8),
        ("alpha_mode", ctypes.c_uint8),
        ("alpha_bit_depth", ctypes.c_uint8),
        ("alpha_premultiplied", ctypes.c_uint8),
        ("color_range", ctypes.c_uint8),
        ("color_primaries", ctypes.c_uint8),
        ("color_transfer", ctypes.c_uint8),
        ("color_matrix", ctypes.c_uint8),
        ("chroma_siting", ctypes.c_uint8),
        ("sar_num", ctypes.c_uint16),
        ("sar_den", ctypes.c_uint16),
        ("timescale", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32 * 8),
    ]


class _CMovieInfo(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("visible_width", ctypes.c_uint16),
        ("visible_height", ctypes.c_uint16),
        ("profile", ctypes.c_uint8),
        ("pixel_format", ctypes.c_uint8),
        ("bit_depth", ctypes.c_uint8),
        ("qmatrix_id", ctypes.c_uint8),
        ("qp_base", ctypes.c_uint8),
        ("qp_delta_luma", ctypes.c_int8),
        ("qp_delta_chroma", ctypes.c_int8),
        ("alpha_mode", ctypes.c_uint8),
        ("alpha_bit_depth", ctypes.c_uint8),
        ("alpha_premultiplied", ctypes.c_uint8),
        ("color_range", ctypes.c_uint8),
        ("color_primaries", ctypes.c_uint8),
        ("color_transfer", ctypes.c_uint8),
        ("color_matrix", ctypes.c_uint8),
        ("chroma_siting", ctypes.c_uint8),
        ("sar_num", ctypes.c_uint16),
        ("sar_den", ctypes.c_uint16),
        ("timescale", ctypes.c_uint32),
        ("sample_count", ctypes.c_uint32),
        ("faststart", ctypes.c_uint32),
        ("index_bytes", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32 * 8),
    ]


# v1.7 电影元数据（tpcD；镜像 topos_movie_meta）。
# TC_TIER_*（镜像 topos_codec.h）：与 topos_profiles.TOPOS_TIER_IDS 同值。
TC_TIER_NONE = 0
TC_TIER_PROXY = 1
TC_TIER_LT = 2
TC_TIER_STANDARD = 3
TC_TIER_HQ = 4
TC_TIER_4444 = 5
TC_TIER_4444XQ = 6
TC_TIER_LP = 7  # v1.8 帧间档（Topos 422 LP，ADR-C056）


class _CMovieMeta(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("tier_id", ctypes.c_uint8),
        ("reserved", ctypes.c_uint8 * 3),
        ("vendor", ctypes.c_char * 16),
        ("label", ctypes.c_char * 64),
    ]


# RC1（M6）：TRAW 开发元数据（trwm 原子；载荷对 native 不透明）
TC_RAW_META_MAX = 256


class _CRawMeta(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("payload_size", ctypes.c_uint16),
        ("reserved", ctypes.c_uint16),
        ("payload", ctypes.c_uint8 * TC_RAW_META_MAX),
    ]


# R3：alpha 预算元数据（tpcB）标志位（镜像 topos_codec.h）
TC_ALPHA_BUDGET_RATIO_UNSET = 0xFFFF
TC_ALPHA_BUDGET_FLAG_OVERRUN = 0x0001
TC_ALPHA_BUDGET_FLAG_AUTHORIZED = 0x0002
TC_ALPHA_BUDGET_FLAG_ADAPTED = 0x0004
# 复验 P1-11：actual_ratio_bp 为 u16 bp（上限 65535bp=6.5535），真实比例
# 超出时置位（v1.3 定义；旧 reader 按未知位忽略）
TC_ALPHA_BUDGET_FLAG_RATIO_SATURATED = 0x0008


class _CAlphaBudgetInfo(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("target_ratio_bp", ctypes.c_uint16),
        ("actual_ratio_bp", ctypes.c_uint16),
        ("max_abs_error", ctypes.c_uint16),
        ("flags", ctypes.c_uint16),
        ("frame_count", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32 * 4),
    ]


def _budget_flags_to_int(flags: dict) -> int:
    v = 0
    if flags.get('overrun'):
        v |= TC_ALPHA_BUDGET_FLAG_OVERRUN
    if flags.get('authorized'):
        v |= TC_ALPHA_BUDGET_FLAG_AUTHORIZED
    if flags.get('adapted'):
        v |= TC_ALPHA_BUDGET_FLAG_ADAPTED
    if flags.get('ratio_saturated'):
        v |= TC_ALPHA_BUDGET_FLAG_RATIO_SATURATED
    return v


# —— v1.1 音频轨（镜像 topos_codec.h TC_AUDIO_*；声明复用 movie config
#    reserved[0..4] 槽位，全 0 = 无音轨）——
TC_AUDIO_CODEC_NONE = 0
TC_AUDIO_CODEC_LPCM = 1   # 'lpcm'（WAV 等价物，无损；PCM 字节为大端 'twos' 语义）
TC_AUDIO_CODEC_MP4A = 2   # 'mp4a'（AAC 包原样存储；编码在应用层 PyAV 完成）
TC_AUDIO_FMT_INT = 0      # v1.4：有符号整数（'twos'，formatFlags 0xE）
TC_AUDIO_FMT_FLOAT32 = 1  # v1.4：float32 大端直存（formatFlags 0xB，仅 lpcm+32bit）
TC_AUDIO_LAYOUT_STEREO = 0
TC_AUDIO_LAYOUT_5_1 = 1
TC_AUDIO_LAYOUT_7_1 = 2
TC_AUDIO_LAYOUT_MONO = 3   # v1.6：单声道 stems 轨（chan tag 0x00640001）

#: 每档声道的 chan atom 布局 tag（仅 mux 内部使用；此处用于校验）
_TC_AUDIO_LAYOUT_CHANNELS = {
    TC_AUDIO_LAYOUT_MONO: 1,
    TC_AUDIO_LAYOUT_STEREO: 2,
    TC_AUDIO_LAYOUT_5_1: 6,
    TC_AUDIO_LAYOUT_7_1: 8,
}


@dataclass(frozen=True)
class ToposAudioFormat:
    """音频轨声明（movie_config 的 audio_* 参数的结构化形态）。

    codec=NONE 时其余字段必须全为缺省（与 C 侧"防半声明"校验一致）。
    """

    codec: int = TC_AUDIO_CODEC_NONE
    sample_rate: int = 0
    channel_count: int = 0
    channel_layout: int = 0
    bits_per_sample: int = 0


@dataclass(frozen=True)
class ToposAudioTrackInfo:
    """音频轨读回信息（tc_movie_audio_info 结果）。

    无音轨文件：codec == TC_AUDIO_CODEC_NONE 且 sample_count == 0（TC_OK，
    不报错——plan §2 约定）。
    """

    codec: int
    sample_rate: int
    channel_count: int
    channel_layout: int
    bits_per_sample: int
    sample_count: int
    chunk_count: int
    priming_samples: int = 0   # v1.4：elst media_time（0 = 无 elst/旧文件）
    sample_format: int = 0     # v1.4：0=有符号整数、1=float32（lpcm 档）
    name: str = ""             # v1.6：轨名（©nam；无轨名空串）

    @property
    def has_audio(self) -> bool:
        return self.codec != TC_AUDIO_CODEC_NONE


class _CTimecodeInfo(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("start_frame_count", ctypes.c_uint32),
        ("fps", ctypes.c_uint32),
        ("drop_frame", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("hh", ctypes.c_int32),
        ("mm", ctypes.c_int32),
        ("ss", ctypes.c_int32),
        ("ff", ctypes.c_int32),
    ]


@dataclass(frozen=True)
class ToposTimecodeInfo:
    """时间码轨读回（v1.5；tc_movie_timecode 结果）。"""

    start_frame_count: int
    fps: int
    drop_frame: int
    flags: int
    hh: int
    mm: int
    ss: int
    ff: int

    def format(self) -> str:
        """HH:MM:SS;FF（DF）/ HH:MM:SS:FF（NDF）标准显示形态。"""
        sep = ";" if self.drop_frame else ":"
        return (f"{self.hh:02d}:{self.mm:02d}:{self.ss:02d}"
                f"{sep}{self.ff:02d}")


class _CAudioTrackInfo(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("codec", ctypes.c_uint32),
        ("sample_rate", ctypes.c_uint32),
        ("channel_count", ctypes.c_uint32),
        ("channel_layout", ctypes.c_uint32),
        ("bits_per_sample", ctypes.c_uint32),
        ("sample_count", ctypes.c_uint32),
        ("chunk_count", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32 * 6),
        ("name", ctypes.c_char * 64),
    ]


class _CAudioTrackConfig(ctypes.Structure):
    """tc_mux_add_audio_track 配置镜像（v1.6）。"""

    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("codec", ctypes.c_uint32),
        ("sample_rate", ctypes.c_uint32),
        ("channel_count", ctypes.c_uint32),
        ("channel_layout", ctypes.c_uint32),
        ("bits_per_sample", ctypes.c_uint32),
        ("sample_format", ctypes.c_uint32),
        ("name", ctypes.c_char_p),
        ("reserved", ctypes.c_uint32 * 4),
    ]


@dataclass(frozen=True)
class ToposAudioTrackConfig:
    """逐轨音轨声明（v1.6 M-B8；格式规则与 movie_config reserved 槽位一致）。"""

    codec: int
    sample_rate: int
    channel_count: int
    channel_layout: int
    bits_per_sample: int = 0
    sample_format: int = 0    # TC_AUDIO_FMT_*（float32 仅 lpcm+32bit）
    name: str = ""            # 轨名（©nam + hdlr；空 = 不写）


def _budget_flags_to_dict(flags: int) -> dict:
    return {
        'overrun': bool(flags & TC_ALPHA_BUDGET_FLAG_OVERRUN),
        'authorized': bool(flags & TC_ALPHA_BUDGET_FLAG_AUTHORIZED),
        'adapted': bool(flags & TC_ALPHA_BUDGET_FLAG_ADAPTED),
        'ratio_saturated': bool(flags & TC_ALPHA_BUDGET_FLAG_RATIO_SATURATED),
    }


@dataclass(frozen=True)
class ToposMovieSummary:
    width: int
    height: int
    profile: int
    pixel_format: int
    bit_depth: int
    qmatrix_id: int
    qp_base: int
    qp_delta_luma: int
    qp_delta_chroma: int
    alpha_mode: int
    alpha_bit_depth: int
    alpha_premultiplied: bool
    color_range: int
    color_primaries: int
    color_transfer: int
    color_matrix: int
    chroma_siting: int
    sar_num: int
    sar_den: int
    timescale: int
    sample_count: int
    faststart: bool
    index_bytes: int
    bitstream_major: int
    tier_id: int


def _movie_summary(info: _CMovieInfo) -> ToposMovieSummary:
    return ToposMovieSummary(
        width=info.visible_width, height=info.visible_height,
        profile=info.profile, pixel_format=info.pixel_format,
        bit_depth=info.bit_depth, qmatrix_id=info.qmatrix_id,
        qp_base=info.qp_base,
        qp_delta_luma=info.qp_delta_luma, qp_delta_chroma=info.qp_delta_chroma,
        alpha_mode=info.alpha_mode, alpha_bit_depth=info.alpha_bit_depth,
        alpha_premultiplied=bool(info.alpha_premultiplied),
        color_range=info.color_range, color_primaries=info.color_primaries,
        color_transfer=info.color_transfer, color_matrix=info.color_matrix,
        chroma_siting=info.chroma_siting,
        sar_num=info.sar_num, sar_den=info.sar_den,
        timescale=info.timescale, sample_count=info.sample_count,
        faststart=bool(info.faststart), index_bytes=info.index_bytes,
        # top-level tpcC/bitstream major is carried in the ABI-stable first
        # reserved movie-info slot; zero means an older native dylib did not
        # expose the compatibility metadata, not that the stream is V0.
        bitstream_major=int(info.reserved[0]),
        # v1.7 tpcD 档位（reserved[1]；旧 dylib 对该槽恒 0 = TC_TIER_NONE，
        # 与"文件无 tpcD"同语义，无需版本协商）
        tier_id=int(info.reserved[1]),
    )


def _file_read_cb(fd: int):
    def cb(_ctx: int, off: int, buf: int, length: int) -> int:
        try:
            data = _pread_full(fd, length, off)
            if len(data) != length:
                return -14
            ctypes.memmove(buf, data, length)
            return 0
        except OSError:
            return -14
    return cb


def _file_write_cb(fd: int):
    def cb(_ctx: int, data: int, length: int) -> int:
        try:
            chunk = ctypes.string_at(data, length)
            return 0 if os.write(fd, chunk) == length else -14
        except OSError:
            return -14
    return cb


class ToposMovieFile:
    """文件型 Topos MOV 读取器（read 回调 + O(1) 索引；支持上下文管理器）。

    线程安全：单实例持有原生 movie 句柄，实例级加锁由调用方负责
    （阶段 6 接入时经 DecodeWorker 串行化）。
    """

    def __init__(self, codec: "ToposCodec", path: str) -> None:
        self._codec = codec
        self._fd = _open_binary(path, os.O_RDONLY)
        self._length = os.fstat(self._fd).st_size
        # P1-11：优先 fd 直读（native pread，零跨语言拷贝）；旧 dylib 或
        # open_fd 失败回退 read 回调（语义一致，多一次 Python 拷贝）
        self._uses_fd_path = False
        if getattr(codec, "_has_movie_open_fd", False):
            movie = ctypes.c_void_p()
            status = int(codec._lib.tc_movie_open_fd(
                self._fd, self._length, ctypes.byref(movie)))
            if status == TC_OK:
                self._movie = movie
                self._uses_fd_path = True
                self._keep_alive = None
                self._read_lock = threading.Lock()  # 兼容属性（fd 路径不使用）
                self._io = None
                self._pkt_buf: Optional[ctypes.Array] = None
                self._pkt_cap = 0
                self._batch_buf: Optional[ctypes.Array] = None
                self._batch_cap = 0
                return
        self._keep_alive = TC_IO_READ_FN(self._read_cb)
        # Windows lseek+read 回调的实例级互斥（_HAS_PREAD 平台不参与热路径）；
        # 必须在首次 tc_movie_open 之前就位——open 期间即会触发读回调
        self._read_lock = threading.Lock()
        self._io = _CIo(
            struct_size=ctypes.sizeof(_CIo),
            abi_version=TOPOS_CODEC_ABI_VERSION,
            ctx=None,
            read=self._keep_alive,
            write=TC_IO_WRITE_FN(),
            seek_write=TC_IO_SEEK_WRITE_FN(),
            length=self._length,
        )
        movie = ctypes.c_void_p()
        status = int(codec._lib.tc_movie_open(ctypes.byref(self._io), ctypes.byref(movie)))
        if status != TC_OK:
            os.close(self._fd)
            raise ToposCodecError(
                f"tc_movie_open 失败 [{status}]: {codec.last_error()}"
            )
        self._movie = movie
        # M2：packet 复用读缓冲（grow-only；4K 帧 ~2.6MB，避免每帧分配）
        self._pkt_buf: Optional[ctypes.Array] = None
        self._pkt_cap = 0
        # 阶段4 兑现：批量聚读 arena（grow-only；2K×8 ≈ 8MB / 4K×4 ≈ 15MB）
        self._batch_buf: Optional[ctypes.Array] = None
        self._batch_cap = 0

    def _read_cb(self, _ctx: int, off: int, buf: int, length: int) -> int:
        try:
            data = _pread_full(self._fd, length, off, self._read_lock)
            if len(data) != length:
                return -14  # TC_ERR_IO
            ctypes.memmove(buf, data, length)
            return 0
        except OSError:
            return -14

    def info(self) -> ToposMovieSummary:
        ci = _CMovieInfo()
        ci.struct_size = ctypes.sizeof(_CMovieInfo)
        ci.abi_version = TOPOS_CODEC_ABI_VERSION
        status = int(self._codec._lib.tc_movie_info(self._movie, ctypes.byref(ci)))
        if status != TC_OK:
            raise ToposCodecError(f"tc_movie_info 失败 [{status}]: {self._codec.last_error()}")
        return _movie_summary(ci)

    def packet_into(self, index: int) -> "tuple[ctypes.Array, int]":
        """读取 sample packet 到实例复用缓冲（grow-only），返回 (buf, size)。

        M2：消除每帧 create_string_buffer 分配；返回缓冲与实例绑定——下一次
        调用即覆盖内容，调用方须在再次调用前完成消费（或自行物化 bytes）。
        """
        need = ctypes.c_size_t(0)
        status = int(self._codec._lib.tc_movie_packet(
            self._movie, index, None, 0, ctypes.byref(need)))
        if status == -15:  # TC_ERR_BUFFER_TOO_SMALL → 正常探测
            if self._pkt_buf is None or self._pkt_cap < need.value:
                self._pkt_buf = ctypes.create_string_buffer(need.value)
                self._pkt_cap = need.value
            status = int(self._codec._lib.tc_movie_packet(
                self._movie, index, self._pkt_buf, self._pkt_cap, None))
        if status != TC_OK:
            raise ToposCodecError(
                f"tc_movie_packet({index}) 失败 [{status}]: {self._codec.last_error()}"
            )
        return self._pkt_buf, need.value

    def packet_base_into(self, index: int) -> "tuple[ctypes.Array, int, bool]":
        """读取预览用的最小独立 packet（V7-B 只读 base byte range）。

        返回 ``(buffer, size, is_base)``。V1–V7-A 或旧 dylib 自动回退完整
        sample，``is_base`` 为 False；V7-B 返回 embedded base packet，调用
        方可把 REDUCED/AUTO_2K 请求改成 base packet 上的 FULL 解码，避免
        对原始高分辨率 packet 做完整 I/O。
        """
        if not getattr(self._codec, "_has_movie_packet_base", False):
            raise ToposCodecError("libtopos_codec 缺少 tc_movie_packet_base 符号")
        full_need = ctypes.c_size_t(0)
        status = int(self._codec._lib.tc_movie_packet(
            self._movie, index, None, 0, ctypes.byref(full_need)))
        if status != TC_ERR_BUFFER_TOO_SMALL:
            raise ToposCodecError(
                f"tc_movie_packet({index}) 探测失败 [{status}]: "
                f"{self._codec.last_error()}"
            )
        need = ctypes.c_size_t(0)
        status = int(self._codec._lib.tc_movie_packet_base(
            self._movie, index, None, 0, ctypes.byref(need)))
        if status != TC_ERR_BUFFER_TOO_SMALL:
            raise ToposCodecError(
                f"tc_movie_packet_base({index}) 探测失败 [{status}]: "
                f"{self._codec.last_error()}"
            )
        if self._pkt_buf is None or self._pkt_cap < need.value:
            self._pkt_buf = ctypes.create_string_buffer(need.value)
            self._pkt_cap = need.value
        status = int(self._codec._lib.tc_movie_packet_base(
            self._movie, index, self._pkt_buf, self._pkt_cap, None))
        if status != TC_OK:
            raise ToposCodecError(
                f"tc_movie_packet_base({index}) 失败 [{status}]: "
                f"{self._codec.last_error()}"
            )
        return self._pkt_buf, need.value, need.value < full_need.value

    def packets_base_into_batch(self, start: int, count: int):
        """批量读取预览 packet；V7-B 每个槽只包含 embedded base。"""
        if not getattr(self._codec, "_has_movie_packet_base_batch", False):
            raise ToposCodecError(
                "libtopos_codec 缺少 tc_movie_packet_base_batch 符号"
            )
        if count <= 0:
            return None, (), (), ()
        lib = self._codec._lib
        need = ctypes.c_size_t(0)
        status = int(lib.tc_movie_packet_base_batch(
            self._movie, ctypes.c_uint32(start), ctypes.c_uint32(count),
            None, 0, None, None, None, ctypes.byref(need)))
        if status != TC_ERR_BUFFER_TOO_SMALL:
            raise ToposCodecError(
                f"tc_movie_packet_base_batch({start}+{count}) 探测失败 [{status}]: "
                f"{self._codec.last_error()}"
            )
        offs = (ctypes.c_size_t * count)()
        sizes = (ctypes.c_size_t * count)()
        flags = (ctypes.c_uint8 * count)()
        if self._batch_buf is None or self._batch_cap < need.value:
            self._batch_buf = ctypes.create_string_buffer(need.value)
            self._batch_cap = need.value
        status = int(lib.tc_movie_packet_base_batch(
            self._movie, ctypes.c_uint32(start), ctypes.c_uint32(count),
            self._batch_buf, self._batch_cap, offs, sizes, flags, None))
        if status != TC_OK:
            raise ToposCodecError(
                f"tc_movie_packet_base_batch({start}+{count}) 失败 [{status}]: "
                f"{self._codec.last_error()}"
            )
        return self._batch_buf, offs, sizes, flags

    def packet(self, index: int) -> bytes:
        buf, n = self.packet_into(index)
        # M11-2c：精确长度单拷（同 encode 修复）
        return ctypes.string_at(buf, n)

    def packets_into_batch(self, start: int, count: int):
        """批量读连续 sample 区间 [start, start+count) 进实例复用 arena。

        阶段4 兑现：一次原生调用取整批——本库布局恒连续时底层合并为单次
        pread，非连续（外来交错布局）逐包读入紧凑槽位；探测仅索引运算
        无 IO。返回 (arena, offsets, sizes)：arena 为复用 ctypes 缓冲
        （下一次调用即覆盖，调用期间内容稳定），offsets/sizes 为各帧在
        arena 内的紧凑布局（ctypes c_size_t 数组）。需较新 dylib，否则
        抛 ToposCodecError（调用方回退逐包 packet()）。
        """
        if not getattr(self._codec, "_has_movie_packet_batch", False):
            raise ToposCodecError("libtopos_codec 缺少 tc_movie_packet_batch 符号")
        if count <= 0:
            return None, (), ()  # C 契约：count==0 → TC_OK 不触碰指针
        lib = self._codec._lib
        need = ctypes.c_size_t(0)
        status = int(lib.tc_movie_packet_batch(
            self._movie, ctypes.c_uint32(start), ctypes.c_uint32(count),
            None, 0, None, None, ctypes.byref(need)))
        if status != TC_ERR_BUFFER_TOO_SMALL:  # 探测必须以 BUFFER_TOO_SMALL 返回布局总量
            raise ToposCodecError(
                f"tc_movie_packet_batch({start}+{count}) 探测失败 [{status}]: "
                f"{self._codec.last_error()}"
            )
        offs = (ctypes.c_size_t * count)()
        sizes = (ctypes.c_size_t * count)()
        if self._batch_buf is None or self._batch_cap < need.value:
            self._batch_buf = ctypes.create_string_buffer(need.value)
            self._batch_cap = need.value
        status = int(lib.tc_movie_packet_batch(
            self._movie, ctypes.c_uint32(start), ctypes.c_uint32(count),
            self._batch_buf, self._batch_cap, offs, sizes, None))
        if status != TC_OK:
            raise ToposCodecError(
                f"tc_movie_packet_batch({start}+{count}) 失败 [{status}]: "
                f"{self._codec.last_error()}"
            )
        return self._batch_buf, offs, sizes

    def pts(self, index: int) -> "tuple[int, int]":
        pts = ctypes.c_uint64(0)
        dur = ctypes.c_uint32(0)
        status = int(self._codec._lib.tc_movie_packet_pts(
            self._movie, index, ctypes.byref(pts), ctypes.byref(dur)))
        if status != TC_OK:
            raise ToposCodecError(f"tc_movie_packet_pts 失败 [{status}]")
        return int(pts.value), int(dur.value)

    def is_sync(self, index: int) -> bool:
        sync = ctypes.c_uint8(0)
        status = int(self._codec._lib.tc_movie_packet_sync(
            self._movie, index, ctypes.byref(sync)))
        if status != TC_OK:
            raise ToposCodecError(f"tc_movie_packet_sync 失败 [{status}]")
        return bool(sync.value)

    def prev_sync(self, index: int) -> int:
        """≤ index 的最近同步 sample（V9 seek 定位；micro-gop 计划批 3）。

        命中同步返回自身；无 stss（默认全同步）返回自身；index 前无任何
        同步 → ToposCodecError（TC_ERR_STATE）。"""
        out = ctypes.c_uint32(0)
        status = int(self._codec._lib.tc_movie_prev_sync(
            self._movie, ctypes.c_uint32(index), ctypes.byref(out)))
        if status != TC_OK:
            raise ToposCodecError(
                f"tc_movie_prev_sync 失败 [{status}]: {self._codec.last_error()}"
            )
        return int(out.value)

    def alpha_budget(self) -> Optional[dict]:
        """读回 tpcB 预算元数据；文件未携带 → None（旧文件兼容）。"""
        ci = _CAlphaBudgetInfo(
            struct_size=ctypes.sizeof(_CAlphaBudgetInfo),
            abi_version=TOPOS_CODEC_ABI_VERSION,
        )
        status = int(self._codec._lib.tc_movie_alpha_budget(
            self._movie, ctypes.byref(ci)))
        if status == TC_ERR_STATE:
            return None
        if status != TC_OK:
            raise ToposCodecError(f"tc_movie_alpha_budget 失败 [{status}]")
        info = self.info()
        return {
            'alpha_mode': int(info.alpha_mode),
            'alpha_bit_depth': int(info.alpha_bit_depth),
            'target_ratio_bp': int(ci.target_ratio_bp),
            'actual_ratio_bp': int(ci.actual_ratio_bp),
            'max_abs_error': int(ci.max_abs_error),
            'flags': _budget_flags_to_dict(int(ci.flags)),
            'frame_count': int(ci.frame_count),
        }

    def timecode(self) -> ToposTimecodeInfo:
        """时间码轨读回（v1.5）。无 tmcd trak → ToposCodecError（STATE）。"""
        ci = _CTimecodeInfo()
        ci.struct_size = ctypes.sizeof(_CTimecodeInfo)
        ci.abi_version = TOPOS_CODEC_ABI_VERSION
        status = int(self._codec._lib.tc_movie_timecode(
            self._movie, ctypes.byref(ci)))
        if status != TC_OK:
            raise ToposCodecError(
                f"tc_movie_timecode 失败 [{status}]: {self._codec.last_error()}"
            )
        return ToposTimecodeInfo(
            start_frame_count=int(ci.start_frame_count),
            fps=int(ci.fps),
            drop_frame=int(ci.drop_frame),
            flags=int(ci.flags),
            hh=int(ci.hh),
            mm=int(ci.mm),
            ss=int(ci.ss),
            ff=int(ci.ff),
        )

    def read_raw_meta(self) -> bytes:
        """读回 TRAW 开发元数据（RC1 trwm 原子；无 → ToposCodecError）。"""
        rm = _CRawMeta()
        rm.struct_size = ctypes.sizeof(_CRawMeta)
        rm.abi_version = TOPOS_CODEC_ABI_VERSION
        status = int(self._codec._lib.tc_movie_read_raw_meta(
            self._movie, ctypes.byref(rm)))
        if status != TC_OK:
            raise ToposCodecError(
                f"tc_movie_read_raw_meta 失败 [{status}]: "
                f"{self._codec.last_error()}")
        return bytes(rm.payload[:rm.payload_size])

    def frame_timecode(self, index: int) -> str:
        """逐帧时码推导（DF 跳帧规则：每分钟跳 2、每 10 分钟整不跳）。

        返回 HH:MM:SS;FF / HH:MM:SS:FF 显示形态（plan §2.7 推导 helper）。
        """
        tc = self.timecode()
        fps = tc.fps
        df = tc.drop_frame != 0
        n = tc.start_frame_count + int(index)
        if df:
            # DF 逆换算（与 C tc_movie_timecode 同式）：每分钟跳 2（30DF）/
            # 4（60DF，SMPTE 12M：;00-;03 无效）、每 10 分钟整不跳；
            # 块内第 0 分钟 wall = cnt，其余 = cnt + 每分钟跳帧数
            dfpm = 4 if fps == 60 else 2
            fpm = fps * 60 - dfpm
            fp10 = fps * 600 - 9 * dfpm
            ten, rem = divmod(n, fp10)
            if rem < fps * 60:
                mins, cnt = 0, rem
            else:
                mins = 1 + (rem - fps * 60) // fpm
                cnt = (rem - fps * 60) % fpm
            mins += ten * 10
            frame = cnt if rem < fps * 60 else cnt + dfpm
        else:
            mins, frame = divmod(n, fps * 60)
        hh, rem2 = divmod(mins, 60)
        hh %= 24  # 24 小时回绕（flags bit3 语义）
        ss, ff = divmod(frame, fps)
        sep = ";" if df else ":"
        return (f"{hh:02d}:{rem2:02d}:{ss:02d}{sep}{ff:02d}")

    def audio_info(self) -> ToposAudioTrackInfo:
        """音频轨信息（v1.1）。无音轨：codec=NONE 且 sample_count=0（TC_OK）。"""
        ci = _CAudioTrackInfo()
        ci.struct_size = ctypes.sizeof(_CAudioTrackInfo)
        ci.abi_version = TOPOS_CODEC_ABI_VERSION
        status = int(self._codec._lib.tc_movie_audio_info(
            self._movie, ctypes.byref(ci)))
        if status != TC_OK:
            raise ToposCodecError(
                f"tc_movie_audio_info 失败 [{status}]: {self._codec.last_error()}"
            )
        return ToposAudioTrackInfo(
            codec=int(ci.codec),
            sample_rate=int(ci.sample_rate),
            channel_count=int(ci.channel_count),
            channel_layout=int(ci.channel_layout),
            bits_per_sample=int(ci.bits_per_sample),
            sample_count=int(ci.sample_count),
            chunk_count=int(ci.chunk_count),
            priming_samples=int(ci.reserved[0]),
            sample_format=int(ci.reserved[1]),
            name=ci.name.decode('utf-8', errors='replace'),
        )

    def audio_track_count(self) -> int:
        """音轨数（v1.6；无音轨 0）。"""
        n = ctypes.c_uint32(0)
        status = int(self._codec._lib.tc_movie_audio_track_count(
            self._movie, ctypes.byref(n)))
        if status != TC_OK:
            raise ToposCodecError(
                f"tc_movie_audio_track_count 失败 [{status}]: "
                f"{self._codec.last_error()}"
            )
        return int(n.value)

    def audio_info_at(self, track: int) -> ToposAudioTrackInfo:
        """按轨索引读取音轨信息（v1.6；旧 audio_info() = 轨 0 视图）。"""
        ci = _CAudioTrackInfo()
        ci.struct_size = ctypes.sizeof(_CAudioTrackInfo)
        ci.abi_version = TOPOS_CODEC_ABI_VERSION
        status = int(self._codec._lib.tc_movie_audio_info_at(
            self._movie, ctypes.c_uint32(int(track)), ctypes.byref(ci)))
        if status != TC_OK:
            raise ToposCodecError(
                f"tc_movie_audio_info_at 失败 [{status}]: "
                f"{self._codec.last_error()}"
            )
        return ToposAudioTrackInfo(
            codec=int(ci.codec),
            sample_rate=int(ci.sample_rate),
            channel_count=int(ci.channel_count),
            channel_layout=int(ci.channel_layout),
            bits_per_sample=int(ci.bits_per_sample),
            sample_count=int(ci.sample_count),
            chunk_count=int(ci.chunk_count),
            priming_samples=int(ci.reserved[0]),
            sample_format=int(ci.reserved[1]),
            name=ci.name.decode('utf-8', errors='replace'),
        )

    def audio_chunk_size(self, index: int) -> int:
        """第 ``index`` 个音频 chunk 的字节长度（v1.1；probe-only，O(1)）。

        C17（2026-09-27 检查计划）：随机读取建前缀采样索引用——探测阶段
        只查容器 sample-to-chunk 表计算精确字节数，不读 chunk 数据。
        """
        need = ctypes.c_size_t(0)
        status = int(self._codec._lib.tc_movie_read_audio(
            self._movie, int(index), 1, None, 0, ctypes.byref(need)))
        if status != TC_ERR_BUFFER_TOO_SMALL or need.value == 0:
            raise ToposCodecError(
                f"audio_chunk_size({index}) 探测失败 [{status}]: "
                f"{self._codec.last_error()}"
            )
        return int(need.value)

    def read_audio_samples_at(self, track: int, start_chunk: int = 0,
                              chunk_count: Optional[int] = None) -> bytes:
        """按轨索引连续读取音频 chunk 区间（v1.6；语义同轨 0 版）。"""
        info = self.audio_info_at(track)
        if info.chunk_count == 0:
            raise ToposCodecError("音轨无样本")
        if start_chunk >= info.chunk_count:
            raise ToposCodecError(
                f"start_chunk {start_chunk} 越界（chunk_count={info.chunk_count}）")
        count = (info.chunk_count - start_chunk) if chunk_count is None \
            else int(chunk_count)
        if count < 0 or count > info.chunk_count - start_chunk:
            raise ToposCodecError(
                f"chunk_count {chunk_count} 越界（可用 "
                f"{info.chunk_count - start_chunk}）")
        need = ctypes.c_size_t(0)
        status = int(self._codec._lib.tc_movie_read_audio_at(
            self._movie, ctypes.c_uint32(int(track)), int(start_chunk),
            int(count), None, 0, ctypes.byref(need)))
        if status == TC_OK and need.value == 0:
            return b""  # count==0 → 合法空区间
        if status != TC_ERR_BUFFER_TOO_SMALL:
            raise ToposCodecError(
                f"tc_movie_read_audio_at 探测失败 [{status}]: "
                f"{self._codec.last_error()}"
            )
        buf = ctypes.create_string_buffer(need.value)
        status = int(self._codec._lib.tc_movie_read_audio_at(
            self._movie, ctypes.c_uint32(int(track)), int(start_chunk),
            int(count), buf, need.value, None))
        if status != TC_OK:
            raise ToposCodecError(
                f"tc_movie_read_audio_at 失败 [{status}]: "
                f"{self._codec.last_error()}"
            )
        return ctypes.string_at(buf, need.value)

    def read_audio_samples(self, start_chunk: int = 0,
                           chunk_count: Optional[int] = None) -> bytes:
        """连续读取音频 chunk 区间的原始字节（v1.1）。

        lpcm = 采样帧字节流（大端）；mp4a = 包流拼接。chunk_count=None
        表示读到末尾；探测（BUFFER_TOO_SMALL + need_size）为内部实现细节。
        文件无音轨 → ToposCodecError（TC_ERR_STATE）。
        """
        info = self.audio_info()
        if info.chunk_count == 0:
            raise ToposCodecError("文件无音频轨")
        if start_chunk >= info.chunk_count:
            raise ToposCodecError(
                f"start_chunk {start_chunk} 越界（chunk_count={info.chunk_count}）")
        count = (info.chunk_count - start_chunk) if chunk_count is None \
            else int(chunk_count)
        if count < 0 or count > info.chunk_count - start_chunk:
            raise ToposCodecError(
                f"chunk_count {chunk_count} 越界（可用 "
                f"{info.chunk_count - start_chunk}）")
        need = ctypes.c_size_t(0)
        status = int(self._codec._lib.tc_movie_read_audio(
            self._movie, int(start_chunk), int(count), None, 0,
            ctypes.byref(need)))
        if status == TC_OK and need.value == 0:
            return b""  # count==0 → 合法空区间
        if status != TC_ERR_BUFFER_TOO_SMALL:
            raise ToposCodecError(
                f"tc_movie_read_audio 探测失败 [{status}]: "
                f"{self._codec.last_error()}"
            )
        buf = ctypes.create_string_buffer(need.value)
        status = int(self._codec._lib.tc_movie_read_audio(
            self._movie, int(start_chunk), int(count), buf, need.value, None))
        if status != TC_OK:
            raise ToposCodecError(
                f"tc_movie_read_audio 失败 [{status}]: {self._codec.last_error()}"
            )
        return ctypes.string_at(buf, need.value)

    def close(self) -> None:
        if getattr(self, "_movie", None) is not None:
            self._codec._lib.tc_movie_close(self._movie)
            self._movie = None
        if getattr(self, "_fd", -1) != -1:
            os.close(self._fd)
            self._fd = -1

    def __enter__(self) -> "ToposMovieFile":
        return self

    def __exit__(self, *_exc) -> None:
        self.close()


class ToposMuxFile:
    """文件型 mux（write+seek_write 回调；finish 后自动关闭）。"""

    def __init__(self, codec: "ToposCodec", path: str, config: _CMovieConfig) -> None:
        self._codec = codec
        self._fd = _open_binary(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC,
                                0o644)
        # Windows lseek+write 回调互斥（_HAS_PWRITE 平台不参与热路径）；
        # 必须在首次 tc_mux_create 之前就位
        self._wlock = threading.Lock()
        self._keep_w = TC_IO_WRITE_FN(self._write_cb)
        self._keep_sw = TC_IO_SEEK_WRITE_FN(self._seek_write_cb)
        self._io = _CIo(
            struct_size=ctypes.sizeof(_CIo),
            abi_version=TOPOS_CODEC_ABI_VERSION,
            ctx=None,
            write=self._keep_w,
            seek_write=self._keep_sw,
            length=0,
        )
        self._mux = ctypes.c_void_p()
        status = int(codec._lib.tc_mux_create(ctypes.byref(config), ctypes.byref(self._io),
                                              ctypes.byref(self._mux)))
        if status != TC_OK:
            os.close(self._fd)
            raise ToposCodecError(f"tc_mux_create 失败 [{status}]: {codec.last_error()}")
        # v1.6 追加轨索引基准：movie config reserved 槽位已声明主混音
        #（轨 0）时，首条 add_audio_track 落在 C 侧轨 1——返回值契约
        #（"返回后即可对该轨 index"，topos_codec.h）要求计数从 1 起
        self._audio_tracks = 1 if int(config.reserved[0]) != 0 else 0

    def _write_cb(self, _ctx: int, data: int, length: int) -> int:
        try:
            chunk = ctypes.string_at(data, length)
            return 0 if os.write(self._fd, chunk) == length else -14
        except OSError:
            return -14

    def _seek_write_cb(self, _ctx: int, off: int, data: int, length: int) -> int:
        try:
            chunk = ctypes.string_at(data, length)
            # Windows 走 pwrite 语义模拟（写后恢复文件指针，与顺序写共用
            # 实例锁互斥）；POSIX 直接 pwrite
            return 0 if _pwrite_full(self._fd, chunk, off,
                                     self._wlock) == length else -14
        except OSError:
            return -14

    def add_packet(self, packet: bytes, pts: int, dur: int = 1) -> None:
        status = int(self._codec._lib.tc_mux_add_packet(
            self._mux, packet, len(packet), pts, dur))
        if status != TC_OK:
            raise ToposCodecError(
                f"tc_mux_add_packet 失败 [{status}]: {self._codec.last_error()}"
            )

    def set_alpha_budget(self, info: dict) -> None:
        """记录 alpha 预算元数据（R3；finish 前调用，可选）。

        info 键：target_ratio_bp / actual_ratio_bp / max_abs_error /
        flags({'overrun','authorized','adapted'}) / frame_count。
        """
        ci = _CAlphaBudgetInfo(
            struct_size=ctypes.sizeof(_CAlphaBudgetInfo),
            abi_version=TOPOS_CODEC_ABI_VERSION,
            target_ratio_bp=int(info.get('target_ratio_bp')
                                or TC_ALPHA_BUDGET_RATIO_UNSET),
            actual_ratio_bp=int(info.get('actual_ratio_bp') or 0),
            max_abs_error=int(info.get('max_abs_error') or 0),
            flags=_budget_flags_to_int(info.get('flags') or {}),
            frame_count=int(info.get('frame_count') or 0),
        )
        status = int(self._codec._lib.tc_mux_set_alpha_budget(
            self._mux, ctypes.byref(ci)))
        if status != TC_OK:
            raise ToposCodecError(
                f"tc_mux_set_alpha_budget 失败 [{status}]: {self._codec.last_error()}"
            )

    def set_movie_meta(self, *, tier_id: int = 0, vendor: str = "",
                       label: str = "") -> None:
        """记录电影元数据（v1.7 tpcD；finish 前调用，可选，重设以最后为准）。

        tier_id ∈ TC_TIER_*（0 = 未声明档位）；vendor/label 为 UTF-8 串
        （≤15/63 字节，空 = 不写）。未调用则不写 tpcD，产物与旧版逐字节
        一致；读侧经 tc_movie_info 的 reserved[1] 回读 tier_id。
        """
        mm = _CMovieMeta()
        mm.struct_size = ctypes.sizeof(_CMovieMeta)
        mm.abi_version = TOPOS_CODEC_ABI_VERSION
        mm.tier_id = int(tier_id)
        mm.vendor = (vendor or "").encode("utf-8")
        mm.label = (label or "").encode("utf-8")
        status = int(self._codec._lib.tc_mux_set_movie_meta(
            self._mux, ctypes.byref(mm)))
        if status != TC_OK:
            raise ToposCodecError(
                f"tc_mux_set_movie_meta 失败 [{status}]: {self._codec.last_error()}"
            )

    def set_raw_meta(self, payload: bytes) -> None:
        """记录 TRAW 开发元数据（RC1 trwm 原子；finish 前调用，可选）。

        载荷不透明原样承载（v1 = topos_trwm.encode_trwm 的 40B 输出）。
        未调用则不写 trwm，产物与旧版逐字节一致。
        """
        if not payload or len(payload) > TC_RAW_META_MAX:
            raise ValueError(
                f"raw meta payload 长度域 (1..{TC_RAW_META_MAX}): "
                f"{len(payload) if payload else 0}")
        rm = _CRawMeta()
        rm.struct_size = ctypes.sizeof(_CRawMeta)
        rm.abi_version = TOPOS_CODEC_ABI_VERSION
        rm.payload_size = len(payload)
        rm.reserved = 0
        ctypes.memmove(rm.payload, payload, len(payload))
        status = int(self._codec._lib.tc_mux_set_raw_meta(
            self._mux, ctypes.byref(rm)))
        if status != TC_OK:
            raise ToposCodecError(
                f"tc_mux_set_raw_meta 失败 [{status}]: "
                f"{self._codec.last_error()}")


    def add_audio(self, data: bytes, num_samples: int) -> None:
        """追加一个音频 chunk（v1.1；finish 前可多次，与 add_packet 任意交错）。

        lpcm：len(data) 必须 == num_samples × channels × bits/8（大端 'twos'
        语义）；mp4a：一次调用 = 一个 AAC 包，须先 set_audio_asc。
        """
        status = int(self._codec._lib.tc_mux_add_audio(
            self._mux, data, len(data), int(num_samples)))
        if status != TC_OK:
            raise ToposCodecError(
                f"tc_mux_add_audio 失败 [{status}]: {self._codec.last_error()}"
            )

    def set_timecode(self, hh: int, mm: int, ss: int, ff: int, fps: int,
                     drop_frame: int = 0) -> None:
        """声明起始时间码（v1.5；finish 前至多一次）。

        fps 白名单 {24,25,30,48,50,60}（NTSC 分数帧率以标称值 + drop_frame
        表达）；DF 仅 30/60 合法，且分钟首帧标签有效（30DF 跳 ff<2、
        60DF 跳 ff<4，非 10 分钟整的分钟 ss==0 时）；负时码不支持。写入
        tmcd 恒末轨 + 视频 trak tref 引用（plan §2.7）。
        """
        status = int(self._codec._lib.tc_mux_set_timecode(
            self._mux, ctypes.c_uint32(int(hh)), ctypes.c_uint32(int(mm)),
            ctypes.c_uint32(int(ss)), ctypes.c_uint32(int(ff)),
            ctypes.c_uint32(int(fps)), ctypes.c_uint32(int(drop_frame))))
        if status != TC_OK:
            raise ToposCodecError(
                f"tc_mux_set_timecode 失败 [{status}]: {self._codec.last_error()}")

    def add_audio_track(self, cfg: ToposAudioTrackConfig) -> int:
        """逐轨声明新音轨（v1.6 M-B8）。返回轨索引（声明序，0 起）。

        格式规则与 movie_config reserved 槽位一致（fail-fast）；
        超过 16 轨 → ToposCodecError（LIMIT）。reserved 槽位已声明轨 0 时
        仍可追加（轨 id 顺延；tmcd 恒末轨不受影响）。
        """
        ccfg = _CAudioTrackConfig()
        ccfg.struct_size = ctypes.sizeof(_CAudioTrackConfig)
        ccfg.abi_version = TOPOS_CODEC_ABI_VERSION
        ccfg.codec = int(cfg.codec)
        ccfg.sample_rate = int(cfg.sample_rate)
        ccfg.channel_count = int(cfg.channel_count)
        ccfg.channel_layout = int(cfg.channel_layout)
        ccfg.bits_per_sample = int(cfg.bits_per_sample)
        ccfg.sample_format = int(cfg.sample_format)
        name_bytes = cfg.name.encode('utf-8') if cfg.name else None
        ccfg.name = name_bytes
        status = int(self._codec._lib.tc_mux_add_audio_track(
            self._mux, ctypes.byref(ccfg)))
        if status != TC_OK:
            raise ToposCodecError(
                f"tc_mux_add_audio_track 失败 [{status}]: "
                f"{self._codec.last_error()}")
        # 声明序即索引：实例计数回推（C 侧无 mux 轨数 getter）
        count = int(getattr(self, '_audio_tracks', 0))
        self._audio_tracks = count + 1
        return count

    def set_audio_track_asc(self, track: int, asc: bytes) -> None:
        """按轨声明 AAC ASC（v1.6；mp4a 轨，首个 add_audio_to 前）。"""
        status = int(self._codec._lib.tc_mux_set_audio_track_asc(
            self._mux, ctypes.c_uint32(int(track)), asc, len(asc)))
        if status != TC_OK:
            raise ToposCodecError(
                f"tc_mux_set_audio_track_asc 失败 [{status}]: "
                f"{self._codec.last_error()}")

    def set_audio_track_priming(self, track: int, samples: int) -> None:
        """按轨声明 AAC priming（v1.6；elst media_time）。"""
        status = int(self._codec._lib.tc_mux_set_audio_track_priming(
            self._mux, ctypes.c_uint32(int(track)),
            ctypes.c_uint32(int(samples))))
        if status != TC_OK:
            raise ToposCodecError(
                f"tc_mux_set_audio_track_priming 失败 [{status}]: "
                f"{self._codec.last_error()}")

    def add_audio_to(self, track: int, data: bytes, num_samples: int) -> None:
        """按轨注入音频 chunk（v1.6；lpcm 大端采样帧字节流 / mp4a 包）。"""
        status = int(self._codec._lib.tc_mux_add_audio_to(
            self._mux, ctypes.c_uint32(int(track)), data, len(data),
            ctypes.c_uint32(int(num_samples))))
        if status != TC_OK:
            raise ToposCodecError(
                f"tc_mux_add_audio_to 失败 [{status}]: "
                f"{self._codec.last_error()}")

    def set_audio_asc(self, asc: bytes) -> None:
        """声明 AAC AudioSpecificConfig（mp4a 档；首个 add_audio 前恰好一次）。

        asc 通常取 PyAV 编码器 extradata（AAC-LC 48kHz 立体声 = b'\\x12\\x10'）。
        """
        status = int(self._codec._lib.tc_mux_set_audio_asc(
            self._mux, asc, len(asc)))
        if status != TC_OK:
            raise ToposCodecError(
                f"tc_mux_set_audio_asc 失败 [{status}]: {self._codec.last_error()}"
            )

    def set_audio_priming(self, samples: int) -> None:
        """声明 AAC priming（v1.4；finish 前至多一次，仅 mp4a 档合法）。

        samples 必须是端到端校准值（ADR-C052）：写为音频 trak 的
        edts/elst media_time，解码侧据此裁剪首包使 A/V 对齐；
        lpcm 档采样精确，恒不调用。
        """
        status = int(self._codec._lib.tc_mux_set_audio_priming(
            self._mux, ctypes.c_uint32(int(samples))))
        if status != TC_OK:
            raise ToposCodecError(
                f"tc_mux_set_audio_priming 失败 [{status}]: "
                f"{self._codec.last_error()}")

    def finish(self) -> None:
        status = int(self._codec._lib.tc_mux_finish(self._mux))
        if status != TC_OK:
            raise ToposCodecError(f"tc_mux_finish 失败 [{status}]: {self._codec.last_error()}")

    def close(self) -> None:
        if getattr(self, "_mux", None) is not None:
            self._codec._lib.tc_mux_free(self._mux)
            self._mux = None
        if getattr(self, "_fd", -1) != -1:
            os.close(self._fd)
            self._fd = -1

    def __enter__(self) -> "ToposMuxFile":
        return self

    def __exit__(self, *_exc) -> None:
        self.close()


class _CCvtParams(ctypes.Structure):
    """P1-12 辅助输入转换参数（native/include/topos_codec.h topos_cvt_params）。"""

    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("format", ctypes.c_uint32),
        ("width", ctypes.c_uint32),
        ("height", ctypes.c_uint32),
        ("out_mode", ctypes.c_uint32),
        ("bit_depth", ctypes.c_uint32),
        # double 承载矩阵系数：numpy 参考的派生常数 (1-kr-kb)/2(1-kr) 以
        # float64 全精度求值——float 中转会差 1 ulp（x.5 边界 rint 翻转）
        ("kr", ctypes.c_double),
        ("kb", ctypes.c_double),
        ("full_range", ctypes.c_uint32),
        ("alpha_shift", ctypes.c_uint32),
        ("alpha_opaque", ctypes.c_uint32),
        # C 侧在 alpha_opaque 后按平台 ABI 填充对齐——ctypes 同规则
        ("src", ctypes.c_void_p),
        ("src_stride", ctypes.c_uint32),
        ("y_out", ctypes.POINTER(ctypes.c_uint16)),
        ("y_stride", ctypes.c_uint32),
        ("u_out", ctypes.POINTER(ctypes.c_uint16)),
        ("u_stride", ctypes.c_uint32),
        ("v_out", ctypes.POINTER(ctypes.c_uint16)),
        ("v_stride", ctypes.c_uint32),
        ("a_out", ctypes.POINTER(ctypes.c_uint16)),
        ("a_stride", ctypes.c_uint32),
        ("max_workers", ctypes.c_uint32),
    ]


class _CPlanar444To422Params(ctypes.Structure):
    """镜像 native topos_planar_444_to_422_params。"""

    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("width", ctypes.c_uint32),
        ("height", ctypes.c_uint32),
        ("bit_depth", ctypes.c_uint32),
        ("u_in", ctypes.c_void_p),
        ("u_in_stride", ctypes.c_size_t),
        ("v_in", ctypes.c_void_p),
        ("v_in_stride", ctypes.c_size_t),
        ("u_out", ctypes.POINTER(ctypes.c_uint16)),
        ("u_out_stride", ctypes.c_size_t),
        ("v_out", ctypes.POINTER(ctypes.c_uint16)),
        ("v_out_stride", ctypes.c_size_t),
        ("max_workers", ctypes.c_uint32),
    ]


# topos_cvt_format（packed 布局）
CVT_BGR24 = 0
CVT_RGB24 = 1
CVT_BGRA32 = 2
CVT_RGBA32 = 3
CVT_BGR48 = 4
CVT_RGB48 = 5
CVT_BGRA64 = 6
CVT_RGBA64 = 7

# topos_cvt out_mode
CVT_OUT_YUV422 = 0
CVT_OUT_YUV444 = 1
CVT_OUT_GBR = 2


class ToposCodec:
    """libtopos_codec 的类型化包装。

    线程安全：v1 ABI 全部为纯函数 + 线程局部错误详情，可多线程共享一个实例
    （context 型 API 出现后单 context 不可重入，届时另行标注）。
    编码输出缓冲（M8）为实例级复用缓冲：并发的同实例 encode/encode_sized
    由内部锁串行化（结果各自独立物化为 bytes，语义不变）。
    """

    def __init__(self, library: Optional[ctypes.CDLL] = None) -> None:
        self._lib = library if library is not None else load_library()
        try:
            self._configure_signatures()
            self._verify_abi()
        except AttributeError as exc:
            # 新增符号不触发 ABI 版本号变化，_verify_abi 拦不住符号漂移；
            # 未守卫的核心符号缺失在此转为可操作错误（否则会在视频播放
            # 深处以裸 dlsym AttributeError 形式爆出）。
            raise ToposCodecError(
                "加载的 libtopos_codec 与绑定层不匹配（缺少必需符号）。"
                "通常是 native 库早于 Python 代码构建，或发布包内 dylib 过旧；"
                "请重新构建 native 库（bash native/topos_codec/run_tests.sh）"
                f"或更新发布包。缺失符号: {exc}"
            ) from exc
        self._warn_degraded_features()
        # M8：持久编码输出缓冲（grow-only）。旧实现每帧按 packet_bound
        # 最坏上限新分配（1080p≈199MB / 4K≈797MB），仅零填充即 ~27ms/帧
        # （1080p 实测），把 M6/M7 的 native 收益整体掩盖在产品路径上。
        self._enc_buf: Optional[ctypes.Array] = None
        self._enc_buf_cap = 0
        # C4（速度计划 v2）：无状态 decode 的输出平面 scratch——per-thread
        # grow-only、各平面连续平铺（消除每帧全平面零填；decode() 的多线程
        # 并发契约由 TLS 保持；返回 bytes 仍为自有拷贝）
        self._dec_tls = threading.local()
        self._enc_lock = threading.Lock()

    # 可选符号门控 → 人类可读描述（旧库降级时一次性 warning；新增可选
    # 符号时同步登记于此）。
    _DEGRADABLE_FEATURES = (
        ("_has_capabilities", "tc_query_capabilities"),
        ("_has_scalable_status", "tc_query_scalable_status"),
        ("_has_scalable_encoder", "tc_frame_encode_scalable"),
        ("_has_decode_request", "tc_frame_decode_request"),
        ("_has_reduced_decode", "tc_frame_decode_reduced"),
        ("_has_decode_batch", "tc_frame_decode_batch"),
        ("_has_decoder_ctx", "tc_decoder_*（持久解码器族）"),
        ("_has_movie_packet_base", "tc_movie_packet_base"),
        ("_has_movie_packet_base_batch", "tc_movie_packet_base_batch"),
        ("_has_movie_packet_batch", "tc_movie_packet_batch"),
        ("_has_movie_open_fd", "tc_movie_open_fd"),
        ("_has_cvt", "tc_convert_packed_rgb"),
        ("_has_planar_cvt", "tc_convert_yuv444_to_422"),
        ("_has_encode_stage_stats", "tc_dev_encode_stats_get"),
        ("_has_decode_stage_stats", "tc_dev_decode_stats_get"),
        ("_has_symbol_hist", "tc_dev_symbol_hist_*"),
        ("_has_symbol_hist_plane",
         "tc_dev_symbol_hist_plane_get/tc_dev_symbol_pair_hist_get"),
    )

    def _warn_degraded_features(self) -> None:
        """旧库缺可选符号已静默降级；此处一次性 warning 保持可见性。"""
        missing = [
            desc for flag, desc in self._DEGRADABLE_FEATURES
            if not getattr(self, flag, True)
        ]
        if missing:
            _LOG.warning(
                "libtopos_codec 为旧版本，以下可选符号缺失、对应功能已降级"
                "（重新构建 native 库可恢复）: %s",
                ", ".join(missing),
            )

    def _encode_scratch(self, need_cap: int, input_bytes: int) -> "tuple[ctypes.Array, int]":
        """取（必要时分配）编码输出缓冲。

        初始按启发式尺寸（输入 2×，下限 32MB）——绝大多数帧远小于
        packet_bound 最坏上限；溢出（-15）时升级到全上限重试一次（见
        encode/encode_sized），此后常驻不再收缩。

        C3（速度计划 v2，2026-09-12）：早退条件改为对【启发式目标尺寸】
        判稳。旧条件 cap >= need_cap 在 4K 下（bound ≈ 797MB，启发式
        ≈ 输入 2× ≈ 94MB）永不成立 → 每帧无条件重建等大缓冲：94MB
        mmap + 触页缺页 ≈ 13ms/帧（实测 encode_frame 41.9ms vs 裸
        ctypes 27.9ms 的主体）。语义不变：容量轨迹与旧版逐值一致
        （首帧启发式 → 溢出才升全上限），仅消除稳态重建。
        """
        heuristic = min(int(need_cap), max(32 << 20, int(input_bytes) * 2))
        if self._enc_buf is not None and self._enc_buf_cap >= heuristic:
            return self._enc_buf, self._enc_buf_cap
        cap = max(self._enc_buf_cap, heuristic)
        self._enc_buf = ctypes.create_string_buffer(cap)
        self._enc_buf_cap = cap
        return self._enc_buf, cap

    # —— 基础查询 ——
    @property
    def abi_version(self) -> int:
        return int(self._lib.tc_abi_version())

    def version(self) -> ToposVersion:
        info = _CVersionInfo()
        status = int(self._lib.tc_version(ctypes.byref(info)))
        if status != TC_OK:
            raise ToposCodecError(f"tc_version 失败 [{status}]: {self.last_error()}")
        return ToposVersion(
            major=info.version_major,
            minor=info.version_minor,
            patch=info.version_patch,
            git_commit=_decode(info.git_commit),
            build_target=_decode(info.build_target),
            abi_version=info.abi_version,
            struct_size=info.struct_size,
        )

    def cpu_features(self) -> ToposCpuFeatures:
        cf = _CCpuFeatures()
        status = int(self._lib.tc_query_cpu_features(ctypes.byref(cf)))
        if status != TC_OK:
            raise ToposCodecError(
                f"tc_query_cpu_features 失败 [{status}]: {self.last_error()}"
            )
        if cf.struct_size != ctypes.sizeof(_CCpuFeatures):
            raise ToposCodecError(
                "topos_cpu_features 镜像不一致: "
                f"C sizeof={cf.struct_size}, ctypes sizeof={ctypes.sizeof(_CCpuFeatures)}"
            )
        return ToposCpuFeatures(
            flags=cf.flags, abi_version=cf.abi_version, struct_size=cf.struct_size
        )

    def capabilities(self) -> ToposCodecCapabilities:
        """Return the native public capability gate, without optimistic fallbacks."""
        if not getattr(self, "_has_capabilities", False):
            raise ToposCodecError(
                "libtopos_codec 缺少 tc_query_capabilities 符号（需重新构建 native 库）"
            )
        caps = _CCodecCapabilities()
        status = int(self._lib.tc_query_capabilities(ctypes.byref(caps)))
        if status != TC_OK:
            raise ToposCodecError(
                f"tc_query_capabilities 失败 [{status}]: {self.last_error()}"
            )
        if caps.struct_size != ctypes.sizeof(_CCodecCapabilities):
            raise ToposCodecError(
                "topos_codec_capabilities 镜像不一致: "
                f"C sizeof={caps.struct_size}, ctypes sizeof={ctypes.sizeof(_CCodecCapabilities)}"
            )
        return ToposCodecCapabilities(
            flags=int(caps.flags), max_width=int(caps.max_width),
            max_height=int(caps.max_height), auto2k_max_dim=int(caps.auto2k_max_dim),
            abi_version=int(caps.abi_version), struct_size=int(caps.struct_size),
        )

    def scalable_status(self) -> str:
        """V7-B scalable 发布状态（P0-04 四态：experimental/reference/
        eligible/default）。reference/experimental 状态下 UI 不得把 opt-in
        或 fallback 展示为 native scalable release。"""
        if not getattr(self, "_has_scalable_status", False):
            raise ToposCodecError(
                "libtopos_codec 缺少 tc_query_scalable_status 符号"
                "（需重新构建 native 库）"
            )
        value = ctypes.c_int32(TC_SCALABLE_STATUS_EXPERIMENTAL)
        status = int(self._lib.tc_query_scalable_status(ctypes.byref(value)))
        if status != TC_OK:
            raise ToposCodecError(
                f"tc_query_scalable_status 失败 [{status}]: {self.last_error()}"
            )
        name = self._lib.tc_scalable_status_name(value)
        if not name:
            raise ToposCodecError("tc_scalable_status_name 返回空指针")
        return name.decode("ascii")

    def query_support(
        self, profile: int, pixel_format: int, bit_depth: int, alpha_mode: int
    ) -> int:
        """能力协商（阶段 10）：TC_OK=可解码该组合，否则 TC_ERR_UNSUPPORTED_*。"""
        return int(
            self._lib.tc_query_support(
                ctypes.c_uint32(profile), ctypes.c_uint32(pixel_format),
                ctypes.c_uint32(bit_depth), ctypes.c_uint32(alpha_mode),
            )
        )

    def status_message(self, status: int) -> str:
        return _decode(self._lib.tc_status_message(ctypes.c_int32(status)))

    def last_error(self) -> str:
        return _decode(self._lib.tc_last_error())

    # —— R6：slice 并行线程数（进程级全局；常驻池的调度/回压旋钮） ——
    def slice_threads(self) -> int:
        """当前 codec 常驻线程池上限（默认 min(ncpu, 16)；TOPOS_SLICE_THREADS 可覆盖）。"""
        return int(self._lib.tc_dev_thread_count())

    def set_slice_threads(self, n: int) -> int:
        """设置 codec 常驻线程池上限并返回生效值（钳位 1..16；M10-6 起池上界
        从 8 提至 16，>8 仅在物理核充足的机器上有收益，默认建议不变）。

        R6 调度策略：导出等前台重负载建议满配（min(8, cpu)）；后台任务
        （渲染缓存等）建议 1 以避免与前台争抢。进程级全局设置——并发
        编码器时后设置者生效，但池上限保证了总线程数有界（结构性回压）。
        """
        v = int(n)
        if v < 1:
            v = 1
        self._lib.tc_dev_set_thread_count(ctypes.c_int32(v))
        return self.slice_threads()

    def encode_stage_stats_reset(self) -> None:
        """清空 E1 编码阶段 profile 累计；旧库无符号时静默无操作。"""
        if getattr(self, "_has_encode_stage_stats", False):
            self._lib.tc_dev_encode_stats_reset()

    def encode_stage_stats_get(self) -> ToposEncodeStageStats:
        """读取 E1 编码阶段 profile 累计。"""
        if not getattr(self, "_has_encode_stage_stats", False):
            return ToposEncodeStageStats(*(0 for _ in range(14)))
        raw = _CEncodeStageStats()
        self._lib.tc_dev_encode_stats_get(ctypes.byref(raw))
        return ToposEncodeStageStats(
            pad_ns=int(raw.pad_ns), fill_ns=int(raw.fill_ns),
            entropy_ns=int(raw.entropy_ns), crc_ns=int(raw.crc_ns),
            copy_ns=int(raw.copy_ns), asm_ns=int(raw.asm_ns),
            slices=int(raw.slices), blocks=int(raw.blocks),
            dct_ns=int(raw.dct_ns), probe_fill_ns=int(raw.probe_fill_ns),
            prep_wall_ns=int(raw.prep_wall_ns),
            probe_wall_ns=int(raw.probe_wall_ns),
            final_wall_ns=int(raw.final_wall_ns),
            check_wall_ns=int(raw.check_wall_ns),
        )

    def decode_stage_stats_reset(self) -> None:
        """清空 RD0-03 解码阶段 profile 与路径计数。"""
        if getattr(self, "_has_decode_stage_stats", False):
            self._lib.tc_dev_decode_stats_reset()

    def decode_stage_stats_get(self) -> ToposDecodeStageStats:
        """读取解码阶段计时、读/解/算/写计数及最近调用分位数。

        native 只有在 ``TOPOS_CODEC_PROFILE=1`` 时才累计这些统计；关闭
        profile 时返回全零，避免污染生产解码热路径。p50/p95/p99 是最近
        256 次 decode core 调用的墙钟样本，batch 记为一次调用。
        """
        if not getattr(self, "_has_decode_stage_stats", False):
            return ToposDecodeStageStats(*(0 for _ in range(22)))
        raw = _CDecodeStageStats()
        self._lib.tc_dev_decode_stats_get(ctypes.byref(raw))
        return ToposDecodeStageStats(
            scan_ns=int(raw.scan_ns), crc_ns=int(raw.crc_ns),
            entropy_ns=int(raw.entropy_ns), dequant_idct_ns=int(raw.dequant_idct_ns),
            output_ns=int(raw.output_ns), alloc_ns=int(raw.alloc_ns),
            blocks=int(raw.blocks), nonzero_ac=int(raw.nonzero_ac),
            dc_only_blocks=int(raw.dc_only_blocks), slices=int(raw.slices),
            packet_bytes_read=int(raw.packet_bytes_read),
            segments_parsed=int(raw.segments_parsed),
            segments_skipped=int(raw.segments_skipped),
            entropy_symbols=int(raw.entropy_symbols),
            coefficients_skipped=int(raw.coefficients_skipped),
            idct_samples=int(raw.idct_samples), upload_bytes=int(raw.upload_bytes),
            frames=int(raw.frames), wall_ns=int(raw.wall_ns),
            p50_ns=int(raw.p50_ns), p95_ns=int(raw.p95_ns), p99_ns=int(raw.p99_ns),
        )

    # —— M9：V2 canonical VLC 训练符号直方图（dev；native codec.c 同源钩子） ——
    def symbol_hist_enable(self, enable: bool) -> None:
        """开启/关闭训练符号累计（进程级；仅训练/调参工具使用）。"""
        if getattr(self, "_has_symbol_hist", False):
            self._lib.tc_dev_symbol_hist_enable(ctypes.c_int32(1 if enable else 0))

    def symbol_hist_reset(self) -> None:
        """清空训练符号累计；旧库无符号时静默无操作。"""
        if getattr(self, "_has_symbol_hist", False):
            self._lib.tc_dev_symbol_hist_reset()

    def symbol_hist_get(self) -> "tuple[list[int], list[int], list[int]]":
        """返回 (dc[29], run[64], lvl[28]) 当前累计（不重置）；旧库返回全零。"""
        dc = (ctypes.c_uint64 * 29)()
        run = (ctypes.c_uint64 * 64)()
        lvl = (ctypes.c_uint64 * 28)()
        if getattr(self, "_has_symbol_hist", False):
            self._lib.tc_dev_symbol_hist_get(dc, run, lvl)
        return list(dc), list(run), list(lvl)

    def symbol_hist_plane_get(
        self, plane: int
    ) -> "tuple[list[int], list[int], list[int]]":
        """返回指定颜色平面的 (dc[29], run[64], lvl[28]) 统计；旧库返回全零。"""
        dc = (ctypes.c_uint64 * 29)()
        run = (ctypes.c_uint64 * 64)()
        lvl = (ctypes.c_uint64 * 28)()
        if getattr(self, "_has_symbol_hist_plane", False):
            self._lib.tc_dev_symbol_hist_plane_get(
                ctypes.c_uint32(int(plane)), dc, run, lvl
            )
        return list(dc), list(run), list(lvl)

    def symbol_pair_hist_get(self, plane: int) -> list[int]:
        """返回指定颜色平面的联合 (run, level-category) 直方图；旧库返回全零。"""
        pair = (ctypes.c_uint64 * (64 * 28))()
        if getattr(self, "_has_symbol_hist_plane", False):
            self._lib.tc_dev_symbol_pair_hist_get(ctypes.c_uint32(int(plane)), pair)
        return list(pair)

    # —— 阶段 4：elementary frame 编解码 ——

    def config_validate(self, config: _CFrameConfig) -> int:
        """返回 TC_OK 或负值错误码（详情见 last_error）。"""
        return int(self._lib.tc_frame_config_validate(ctypes.byref(config)))

    def packet_bound(self, config: _CFrameConfig) -> int:
        """编码输出缓冲上界估计（保守，非承诺）。"""
        return int(self._lib.tc_frame_packet_bound(ctypes.byref(config)))

    def encode(
        self,
        config: _CFrameConfig,
        planes: List,
        stats_out: Optional[_CFrameStats] = None,
    ) -> bytes:
        """编码一帧。planes: [Y, U, V] 或 [Y, U, V, A]（uint16 LE，tight）。

        平面接受 bytes（零转换）或 C 连续小端 uint16 numpy 数组（零拷贝，
        阶段 3a——绕过 4K 每帧 ~14ms 的全平面 tobytes 拷贝）。
        输出缓冲按 packet_bound 分配；返回 frame packet 字节。
        stats_out 可传入 _CFrameStats() 实例接收统计（通常用 encode_with_stats）。
        """
        if not 3 <= len(planes) <= TC_FRAME_MAX_PLANES:
            raise ToposCodecError(f"planes 数量 {len(planes)} 非法（3 或 4）")
        need = self.packet_bound(config)
        input_bytes = sum(_plane_nbytes(p) for p in planes)
        cin = _CFrameInput()
        cin.struct_size = ctypes.sizeof(_CFrameInput)
        cin.abi_version = TOPOS_CODEC_ABI_VERSION
        cin._keep = _fill_frame_input(cin, planes)  # tight
        st = _CFrameStats()
        with self._enc_lock:
            out, cap = self._encode_scratch(need, input_bytes)
            status = int(
                self._lib.tc_frame_encode(
                    ctypes.byref(config), ctypes.byref(cin),
                    out, ctypes.c_size_t(cap),
                    ctypes.byref(st),
                )
            )
            if status == TC_ERR_BUFFER_TOO_SMALL and cap < need:
                # 启发式尺寸溢出 → 升级到 packet_bound 全上限重试一次
                self._enc_buf = ctypes.create_string_buffer(need)
                self._enc_buf_cap = need
                out, cap = self._enc_buf, need
                status = int(
                    self._lib.tc_frame_encode(
                        ctypes.byref(config), ctypes.byref(cin),
                        out, ctypes.c_size_t(cap),
                        ctypes.byref(st),
                    )
                )
            if status != TC_OK:
                raise ToposCodecError(
                    f"tc_frame_encode 失败 [{status}]: {self.last_error()}")
            # M11-2c：精确长度单拷（string_at 只拷 packet_size 字节）。
            # 此前 out.raw[:n] 先把整个 scratch（4K 422 约 50MB）完整拷成
            # bytes 再切片——纯 C 编码 15ms/帧被绑到 38ms，拷贝占大头。
            pkt = ctypes.string_at(out, st.packet_size)
        if stats_out is not None:
            ctypes.memmove(ctypes.byref(stats_out), ctypes.byref(st), ctypes.sizeof(st))
        return pkt

    def encode_scalable(
        self,
        config: _CFrameConfig,
        planes: List,
        base_max_dim: int = TC_CODEC_AUTO_2K_MAX_DIM,
        stats_out: Optional[_CFrameStats] = None,
    ) -> bytes:
        """显式 opt-in V7-B 编码，返回 base + enhancement residual packet。

        ``base_max_dim`` 为 0 时使用 2048；当前公开入口不允许超过
        ``TC_CODEC_AUTO_2K_MAX_DIM``。native 先执行一次无输出 size probe，
        再复用实例级 grow-only buffer 完成实际编码，因此调用方无需猜测
        V7-B 两层 packet 的保守容量。默认 writer ``encode()`` 不会切换到此路径。
        """
        if not getattr(self, "_has_scalable_encoder", False):
            raise ToposCodecError(
                "libtopos_codec 缺少 tc_frame_encode_scalable 符号（需重新构建 native 库）"
            )
        if not 3 <= len(planes) <= TC_FRAME_MAX_PLANES:
            raise ToposCodecError(f"planes 数量 {len(planes)} 非法（3 或 4）")
        base_dim = int(base_max_dim)
        if base_dim < 0 or base_dim > TC_CODEC_AUTO_2K_MAX_DIM:
            raise ToposCodecError(
                f"base_max_dim={base_dim} 非法（0 或 1..{TC_CODEC_AUTO_2K_MAX_DIM}）"
            )
        input_bytes = sum(_plane_nbytes(p) for p in planes)
        cin = _CFrameInput()
        cin.struct_size = ctypes.sizeof(_CFrameInput)
        cin.abi_version = TOPOS_CODEC_ABI_VERSION
        cin._keep = _fill_frame_input(cin, planes)
        probe = _CFrameStats()
        with self._enc_lock:
            status = int(
                self._lib.tc_frame_encode_scalable(
                    ctypes.byref(config), ctypes.byref(cin), ctypes.c_uint32(base_dim),
                    None, ctypes.c_size_t(0), ctypes.byref(probe),
                )
            )
            if status != TC_ERR_BUFFER_TOO_SMALL or probe.packet_size == 0:
                raise ToposCodecError(
                    f"tc_frame_encode_scalable size probe 失败 [{status}]: {self.last_error()}"
                )
            out, cap = self._encode_scratch(int(probe.packet_size), input_bytes)
            st = _CFrameStats()
            status = int(
                self._lib.tc_frame_encode_scalable(
                    ctypes.byref(config), ctypes.byref(cin), ctypes.c_uint32(base_dim),
                    out, ctypes.c_size_t(cap), ctypes.byref(st),
                )
            )
            if status == TC_ERR_BUFFER_TOO_SMALL and st.packet_size > cap:
                self._enc_buf = ctypes.create_string_buffer(st.packet_size)
                self._enc_buf_cap = st.packet_size
                out, cap = self._enc_buf, st.packet_size
                status = int(
                    self._lib.tc_frame_encode_scalable(
                        ctypes.byref(config), ctypes.byref(cin), ctypes.c_uint32(base_dim),
                        out, ctypes.c_size_t(cap), ctypes.byref(st),
                    )
                )
            if status != TC_OK:
                raise ToposCodecError(
                    f"tc_frame_encode_scalable 失败 [{status}]: {self.last_error()}"
                )
            pkt = ctypes.string_at(out, st.packet_size)
        if stats_out is not None:
            ctypes.memmove(ctypes.byref(stats_out), ctypes.byref(st), ctypes.sizeof(st))
        return pkt

    def encode_frame(self, config: _CFrameConfig, planes: List[bytes]) -> "tuple[bytes, ToposFrameStats]":
        """encode + 统计。"""
        st = _CFrameStats()
        pkt = self.encode(config, planes, stats_out=st)
        return pkt, ToposFrameStats(
            packet_size=st.packet_size,
            color_payload_bytes=st.color_payload_bytes,
            alpha_payload_bytes=st.alpha_payload_bytes,
            color_header_bytes=st.color_header_bytes,
            alpha_header_bytes=st.alpha_header_bytes,
            slice_count=st.slice_count,
            qp_base=st.qp_base,
            alpha_max_abs_error=st.alpha_max_abs_error,
        )

    def encode_sized(
        self,
        config: _CFrameConfig,
        planes: List,
        target_bytes: int,
        qp_min: int = 0,
        qp_max: int = TOPOS_QP_MAX,
    ) -> "tuple[bytes, ToposFrameStats, int]":
        """目标尺寸编码（确定性 qp 搜索）。返回 (packet, stats, qp_used)。

        平面接受 bytes 或 C 连续小端 uint16 numpy 数组（零拷贝，同 encode）。
        """
        if not 3 <= len(planes) <= TC_FRAME_MAX_PLANES:
            raise ToposCodecError(f"planes 数量 {len(planes)} 非法（3 或 4）")
        need = self.packet_bound(config)
        input_bytes = sum(_plane_nbytes(p) for p in planes)
        cin = _CFrameInput()
        cin.struct_size = ctypes.sizeof(_CFrameInput)
        cin.abi_version = TOPOS_CODEC_ABI_VERSION
        cin._keep = _fill_frame_input(cin, planes)
        st = _CFrameStats()
        qp_used = ctypes.c_uint8(0)
        with self._enc_lock:
            out, cap = self._encode_scratch(need, input_bytes)
            status = int(
                self._lib.tc_frame_encode_sized(
                    ctypes.byref(config), ctypes.byref(cin), ctypes.c_uint32(target_bytes),
                    ctypes.c_uint8(qp_min), ctypes.c_uint8(qp_max), ctypes.byref(qp_used),
                    out, ctypes.c_size_t(cap), ctypes.byref(st),
                )
            )
            if status == TC_ERR_BUFFER_TOO_SMALL and cap < need:
                self._enc_buf = ctypes.create_string_buffer(need)
                self._enc_buf_cap = need
                out, cap = self._enc_buf, need
                status = int(
                    self._lib.tc_frame_encode_sized(
                        ctypes.byref(config), ctypes.byref(cin), ctypes.c_uint32(target_bytes),
                        ctypes.c_uint8(qp_min), ctypes.c_uint8(qp_max), ctypes.byref(qp_used),
                        out, ctypes.c_size_t(cap), ctypes.byref(st),
                    )
                )
            if status != TC_OK:
                raise ToposCodecError(
                    f"tc_frame_encode_sized 失败 [{status}]: {self.last_error()}")
            stats = ToposFrameStats(
                packet_size=st.packet_size,
                color_payload_bytes=st.color_payload_bytes,
                alpha_payload_bytes=st.alpha_payload_bytes,
                color_header_bytes=st.color_header_bytes,
                alpha_header_bytes=st.alpha_header_bytes,
                slice_count=st.slice_count,
                qp_base=st.qp_base,
                alpha_max_abs_error=st.alpha_max_abs_error,
            )
            # M11-2c：精确长度单拷（同 encode；sized 探测路径每帧多次调用，
            # 全量 scratch 拷贝在此放大数倍）
            pkt = ctypes.string_at(out, st.packet_size)
        return pkt, stats, qp_used.value

    @staticmethod
    def make_decode_request(
        mode: int,
        scale: int = TC_DECODE_SCALE_FULL,
        target_size: Optional[tuple[int, int]] = None,
        quality: int = TC_DECODE_QUALITY_DEFAULT,
        memory_type: int = TC_DECODE_MEMORY_CPU,
        flags: int = 0,
    ) -> _CDecodeRequest:
        """构造可传给 request 解码 API 的稳定 ctypes 请求对象。"""
        return _make_decode_request(
            mode, scale, target_size, quality, memory_type, flags)

    def decode(self, packet: bytes) -> ToposFrame:
        """解码一帧（concealment 语义：坏 slice 填中性值，帧仍交付）。

        M2：packet 以 const 指针直传（无 create_string_buffer 复制）；查询与
        正式两次调用中查询已是纯结构解析（M1 后无 CRC），成本可忽略。
        高频路径请使用 ToposDecoder（单次调用 + 直写输出平面）。
        C4（速度计划 v2，2026-09-12）：输出平面改用实例级 grow-only
        scratch（消除每帧 47.7MB（4K444）的 ctypes 数组零填——旧实现
        在 16t 下把纯 native ~22ms 的解码拖到 ~45ms，tier 矩阵的
        decode_mt 因此系统性低估多线程扩展性）。返回 bytes 仍为自有
        拷贝（API 契约不变，仅一份全平面拷）。
        """
        info = _CFrameOutput()
        status = int(
            self._lib.tc_frame_decode(
                packet, ctypes.c_size_t(len(packet)),
                None, None, ctypes.byref(info),
            )
        )
        if status != TC_OK:
            raise ToposCodecError(f"tc_frame_decode 失败 [{status}]: {self.last_error()}")
        out_ptrs = (ctypes.POINTER(ctypes.c_uint16) * TC_FRAME_MAX_PLANES)()
        bufs: List[ctypes.Array] = []
        widths = []
        need_elems = sum(w * h for (w, h) in
                         (self.plane_geometry(info, p) for p in range(info.plane_count)))
        tls = self._dec_tls
        if getattr(tls, "scratch", None) is None or tls.elems < need_elems:
            tls.scratch = (ctypes.c_uint16 * need_elems)()
            tls.elems = need_elems
        offset_elems = 0
        for p in range(info.plane_count):
            w, h = self.plane_geometry(info, p)
            widths.append((w, h))
            # 各平面连续平铺于本线程 scratch（无重叠；免逐帧零填）
            arr = (ctypes.c_uint16 * (w * h)).from_buffer(
                tls.scratch, offset_elems * 2)
            offset_elems += w * h
            bufs.append(arr)
            out_ptrs[p] = ctypes.cast(arr, ctypes.POINTER(ctypes.c_uint16))
        status = int(
            self._lib.tc_frame_decode(
                packet, ctypes.c_size_t(len(packet)), out_ptrs, None, ctypes.byref(info),
            )
        )
        if status not in (TC_OK, TC_WARN_CONCEALED):
            raise ToposCodecError(f"tc_frame_decode 失败 [{status}]: {self.last_error()}")
        planes = [bytes(bufs[p]) for p in range(info.plane_count)]
        frame_info = ToposFrameInfo(
            visible_width=info.visible_width,
            visible_height=info.visible_height,
            plane_count=info.plane_count,
            bit_depth=info.bit_depth,
            profile=info.profile,
            pixel_format=info.pixel_format,
            alpha_mode=info.alpha_mode,
            concealed_slices=info.concealed_slices,
            slice_count=info.slice_count,
            slice_status=tuple(info.slice_status[: info.slice_count]),
        )
        return ToposFrame(planes, frame_info)

    def decode_reduced(self, packet: bytes, scale: int) -> ToposFrame:
        """固定比例低频预览解码（1/2、1/3、1/4 或 1/8）。

        该路径不生成完整源平面，且丢弃高频系数的重建；码流仍会被完整消费
        以维持当前可变长熵码流的校验语义，因此输出属于快速近似预览。
        """
        if not getattr(self, "_has_reduced_decode", False):
            raise ToposCodecError(
                "libtopos_codec 缺少 tc_frame_decode_reduced 符号（需重新构建 native 库）"
            )
        if int(scale) not in (TC_DECODE_SCALE_HALF, TC_DECODE_SCALE_THIRD,
                              TC_DECODE_SCALE_QUARTER, TC_DECODE_SCALE_EIGHTH):
            raise ToposCodecError(f"不支持的 reduced scale: {scale}")
        info = _CFrameOutput()
        status = int(self._lib.tc_frame_decode_reduced(
            packet, ctypes.c_size_t(len(packet)), ctypes.c_int32(int(scale)),
            None, None, ctypes.byref(info),
        ))
        if status != TC_OK:
            raise ToposCodecError(
                f"tc_frame_decode_reduced 查询失败 [{status}]: {self.last_error()}"
            )
        out_ptrs = (ctypes.POINTER(ctypes.c_uint16) * TC_FRAME_MAX_PLANES)()
        bufs: List[ctypes.Array] = []
        for p in range(info.plane_count):
            w, h = self.plane_geometry(info, p)
            arr = (ctypes.c_uint16 * (w * h))()
            bufs.append(arr)
            out_ptrs[p] = ctypes.cast(arr, ctypes.POINTER(ctypes.c_uint16))
        status = int(self._lib.tc_frame_decode_reduced(
            packet, ctypes.c_size_t(len(packet)), ctypes.c_int32(int(scale)),
            out_ptrs, None, ctypes.byref(info),
        ))
        if status not in (TC_OK, TC_WARN_CONCEALED):
            raise ToposCodecError(
                f"tc_frame_decode_reduced 失败 [{status}]: {self.last_error()}"
            )
        frame_info = ToposFrameInfo(
            visible_width=info.visible_width,
            visible_height=info.visible_height,
            plane_count=info.plane_count,
            bit_depth=info.bit_depth,
            profile=info.profile,
            pixel_format=info.pixel_format,
            alpha_mode=info.alpha_mode,
            concealed_slices=info.concealed_slices,
            slice_count=info.slice_count,
            slice_status=tuple(info.slice_status[: info.slice_count]),
        )
        return ToposFrame([bytes(buf) for buf in bufs], frame_info)

    def decode_request(
        self,
        packet: bytes,
        request: _CDecodeRequest,
    ) -> ToposFrame:
        """统一 request 解码，支持 AUTO_2K 与固定比例 reduced fallback。"""
        if not getattr(self, "_has_decode_request", False):
            raise ToposCodecError(
                "libtopos_codec 缺少 tc_frame_decode_request 符号（需重新构建 native 库）"
            )
        if not isinstance(request, _CDecodeRequest):
            raise ToposCodecError("request 必须由 ToposCodec.make_decode_request 构造")
        info = _CFrameOutput()
        status = int(self._lib.tc_frame_decode_request(
            packet, ctypes.c_size_t(len(packet)), ctypes.byref(request),
            None, None, ctypes.byref(info),
        ))
        if status != TC_OK:
            raise ToposCodecError(
                f"tc_frame_decode_request 查询失败 [{status}]: {self.last_error()}"
            )
        out_ptrs = (ctypes.POINTER(ctypes.c_uint16) * TC_FRAME_MAX_PLANES)()
        bufs: List[ctypes.Array] = []
        for p in range(info.plane_count):
            w, h = self.plane_geometry(info, p)
            arr = (ctypes.c_uint16 * (w * h))()
            bufs.append(arr)
            out_ptrs[p] = ctypes.cast(arr, ctypes.POINTER(ctypes.c_uint16))
        status = int(self._lib.tc_frame_decode_request(
            packet, ctypes.c_size_t(len(packet)), ctypes.byref(request),
            out_ptrs, None, ctypes.byref(info),
        ))
        if status not in (TC_OK, TC_WARN_CONCEALED):
            raise ToposCodecError(
                f"tc_frame_decode_request 失败 [{status}]: {self.last_error()}"
            )
        return ToposFrame(
            [bytes(buf) for buf in bufs],
            _frame_info_from_c(info),
        )

    def decode_batch(self, packets: List[bytes]) -> List[ToposFrame]:
        """跨帧批量解码（阶段4 单批跨帧切片队列；语义同 decode）。

        全部帧的切片进同一线程池批次——一次唤醒/汇合消化整批，消除逐帧
        派发/汇合与帧间串行段（高频连续解码应优先本入口）。逐帧语义与
        decode 一致：坏 slice 按 §9 conceal 交付；帧间错误隔离；返回值 =
        首个非 OK 帧的状态码（帧序最小）。需较新 dylib
        （tc_frame_decode_batch 符号），否则抛 ToposCodecError。
        """
        if not getattr(self, "_has_decode_batch", False):
            raise ToposCodecError("libtopos_codec 缺少 tc_frame_decode_batch 符号")
        n = len(packets)
        if n == 0:
            return []
        batch = (_CBatchPacket * n)()
        for i, pkt in enumerate(packets):
            buf = pkt if isinstance(pkt, bytes) else bytes(pkt)
            batch[i].data = ctypes.cast(ctypes.c_char_p(buf), ctypes.c_void_p)
            batch[i].size = len(buf)
        infos = (_CFrameOutput * n)()

        # 第一遍：结构解析填充几何（batch 内部完成；任一包失败即拒整批）
        status = int(self._lib.tc_frame_decode_batch(
            batch, ctypes.c_uint32(n), None, None, infos))
        if status != TC_OK:
            raise ToposCodecError(
                f"tc_frame_decode_batch 查询失败 [{status}]: {self.last_error()}"
            )
        # 输出平面：帧 × plane 扁平指针数组
        out_ptrs = (ctypes.POINTER(ctypes.c_uint16)
                    * (n * TC_FRAME_MAX_PLANES))()
        bufs: List[List[ctypes.Array]] = []
        for i in range(n):
            frame_bufs: List[ctypes.Array] = []
            for p in range(infos[i].plane_count):
                w, h = self.plane_geometry(infos[i], p)
                arr = (ctypes.c_uint16 * (w * h))()
                frame_bufs.append(arr)
                out_ptrs[i * TC_FRAME_MAX_PLANES + p] = ctypes.cast(
                    arr, ctypes.POINTER(ctypes.c_uint16))
            bufs.append(frame_bufs)
        status = int(self._lib.tc_frame_decode_batch(
            batch, ctypes.c_uint32(n), out_ptrs, None, infos))
        if status not in (TC_OK, TC_WARN_CONCEALED):
            raise ToposCodecError(
                f"tc_frame_decode_batch 失败 [{status}]: {self.last_error()}"
            )
        frames = []
        for i in range(n):
            info = infos[i]
            frame_info = ToposFrameInfo(
                visible_width=info.visible_width,
                visible_height=info.visible_height,
                plane_count=info.plane_count,
                bit_depth=info.bit_depth,
                profile=info.profile,
                pixel_format=info.pixel_format,
                alpha_mode=info.alpha_mode,
                concealed_slices=info.concealed_slices,
                slice_count=info.slice_count,
                slice_status=tuple(info.slice_status[: info.slice_count]),
            )
            frames.append(ToposFrame(
                [bytes(b) for b in bufs[i]], frame_info))
        return frames

    def plane_geometry(self, info: _CFrameOutput, plane: int) -> "tuple[int, int]":
        """plane 的 visible 尺寸（uint16 元素）。"""
        w = ctypes.c_uint32(0)
        h = ctypes.c_uint32(0)
        status = int(
            self._lib.tc_frame_plane_geometry(
                ctypes.byref(info), ctypes.c_uint32(plane), ctypes.byref(w), ctypes.byref(h)
            )
        )
        if status != TC_OK:
            raise ToposCodecError(
                f"tc_frame_plane_geometry 失败 [{status}]: {self.last_error()}"
            )
        return w.value, h.value

    @property
    def has_cvt(self) -> bool:
        """P1-12 融合输入转换符号是否可用（旧 dylib → False，调用方回退）。"""
        return self._has_cvt

    def convert_packed_rgb(
        self,
        src,
        width: int,
        height: int,
        *,
        fmt: int = CVT_BGR24,
        out_mode: int = CVT_OUT_YUV422,
        bit_depth: int = 10,
        kr: float = 0.2126,
        kb: float = 0.0722,
        full_range: bool = False,
        alpha_shift: int = 0,
        alpha_opaque: bool = False,
        src_stride: int = 0,
        max_workers: int = 1,
        planes_out: "Optional[list]" = None,
    ) -> "list":
        """P1-12：packed RGB(A) → ['<u2' numpy 平面]。

        数学与 ToposVideoEncoder 的 numpy 参考实现**逐位一致**（float32 同序 +
        rint 半到偶 + 整型 box；native TU 以 -ffp-contract=off 编译）。
        src 为 HxWxN uint8/uint16（小端）C 连续数组；输出平面数 = 3 + has_alpha，
        4:2:2 时 U/V 形状 (h, ceil(w/2))。旧 dylib 缺符号 → ToposCodecError，
        调用方应先查 has_cvt。max_workers>1 用内部线程池行分片（输出逐位一致）。
        planes_out 非空时复用其缓冲（形状/dtype 须匹配）——编码器逐帧零分配。
        """
        import numpy as np  # 延迟导入：仅本 API 需要

        if src is None or not src.flags.c_contiguous:
            raise ToposCodecError("convert_packed_rgb: src 须为 C 连续数组")
        if src.dtype == np.uint8:
            expect_fmt = (CVT_BGR24, CVT_RGB24, CVT_BGRA32, CVT_RGBA32)
        elif src.dtype == np.uint16:
            expect_fmt = (CVT_BGR48, CVT_RGB48, CVT_BGRA64, CVT_RGBA64)
        else:
            raise ToposCodecError(
                f"convert_packed_rgb: dtype {src.dtype} 不受支持（u8/u16）")
        if fmt not in expect_fmt:
            raise ToposCodecError(
                f"convert_packed_rgb: fmt {fmt} 与 dtype {src.dtype} 不匹配")
        if not self._has_cvt:
            raise ToposCodecError("convert_packed_rgb: dylib 缺 tc_convert_packed_rgb")

        nch = 4 if fmt in (CVT_BGRA32, CVT_RGBA32, CVT_BGRA64, CVT_RGBA64) else 3
        if src.ndim != 2 or src.shape[1] != width * nch:
            raise ToposCodecError(
                f"convert_packed_rgb: src 形状 {src.shape} ≠ (h, w×{nch})；"
                f"请传行主 view（避免 HxWxN 拷贝）")
        if src.shape[0] != height:
            raise ToposCodecError(
                f"convert_packed_rgb: src 高 {src.shape[0]} ≠ height {height}")

        has_alpha = alpha_opaque or nch == 4
        cw = (width + 1) // 2 if out_mode == CVT_OUT_YUV422 else width
        shapes = [(height, width), (height, cw), (height, cw)]
        if has_alpha:
            shapes.append((height, width))
        if planes_out is not None:
            if len(planes_out) != len(shapes) or any(
                p.dtype != np.dtype('<u2') or p.shape != s or not p.flags.c_contiguous
                for p, s in zip(planes_out, shapes)
            ):
                raise ToposCodecError(
                    "convert_packed_rgb: planes_out 形状/dtype 与当前几何不匹配")
            y, u, v = planes_out[0], planes_out[1], planes_out[2]
            a = planes_out[3] if has_alpha else None
        else:
            y = np.empty((height, width), dtype="<u2")
            u = np.empty((height, cw), dtype="<u2")
            v = np.empty((height, cw), dtype="<u2")
            a = np.empty((height, width), dtype="<u2") if has_alpha else None

        cp = _CCvtParams()
        cp.struct_size = ctypes.sizeof(_CCvtParams)
        cp.format = fmt
        cp.width = width
        cp.height = height
        cp.out_mode = out_mode
        cp.bit_depth = bit_depth
        cp.kr = kr
        cp.kb = kb
        cp.full_range = 1 if full_range else 0
        cp.alpha_shift = alpha_shift
        cp.alpha_opaque = 1 if alpha_opaque else 0
        cp.src = src.ctypes.data_as(ctypes.c_void_p)
        cp.src_stride = src_stride if src_stride else (width * nch * src.itemsize)
        cp.y_out = y.ctypes.data_as(ctypes.POINTER(ctypes.c_uint16))
        cp.y_stride = width
        cp.u_out = u.ctypes.data_as(ctypes.POINTER(ctypes.c_uint16))
        cp.v_out = v.ctypes.data_as(ctypes.POINTER(ctypes.c_uint16))
        if a is not None:
            cp.a_out = a.ctypes.data_as(ctypes.POINTER(ctypes.c_uint16))
            cp.a_stride = width
        cp.max_workers = max_workers
        status = int(self._lib.tc_convert_packed_rgb(ctypes.byref(cp)))
        if status != TC_OK:
            raise ToposCodecError(
                f"tc_convert_packed_rgb 失败 [{status}]: {self.last_error()}")
        planes = [y, u, v]
        if a is not None:
            planes.append(a)
        return planes

    def convert_yuv444_to_422(
        self,
        u_in,
        v_in,
        *,
        bit_depth: int = 16,
        max_workers: int = 1,
        planes_out: "Optional[list]" = None,
    ) -> "list":
        """把 uint16 4:4:4 U/V 平面逐位转换为 4:2:2。

        该辅助 API 不改变 Y 平面；它只把已量化色度相邻样本按
        ``(a+b+1)>>1`` 合并，和时间线直通路径的 numpy 参考实现一致。
        旧 native 库缺符号时由调用方回退 Python 实现。
        """
        import numpy as np  # 延迟导入：仅平面转换需要

        if not getattr(self, "_has_planar_cvt", False):
            raise ToposCodecError(
                "convert_yuv444_to_422: dylib 缺 tc_convert_yuv444_to_422")
        u = np.asarray(u_in)
        v = np.asarray(v_in)
        if (u.ndim != 2 or v.ndim != 2 or u.shape != v.shape
                or u.dtype != np.dtype("<u2")
                or v.dtype != np.dtype("<u2")):
            raise ToposCodecError(
                "convert_yuv444_to_422: 输入须为形状相同的二维 uint16 平面")
        height, width = (int(u.shape[0]), int(u.shape[1]))
        if width <= 0 or height <= 0:
            raise ToposCodecError("convert_yuv444_to_422: 输入平面不可为空")
        if int(bit_depth) not in (10, 12, 16):
            raise ToposCodecError(
                f"convert_yuv444_to_422: bit_depth {bit_depth!r} 不支持")
        chroma_width = (width + 1) // 2
        shape = (height, chroma_width)
        if planes_out is None:
            u_out = np.empty(shape, dtype="<u2")
            v_out = np.empty(shape, dtype="<u2")
        else:
            if (len(planes_out) != 2
                    or any(p.dtype != np.dtype("<u2")
                           or p.shape != shape
                           or not p.flags.c_contiguous
                           for p in planes_out)):
                raise ToposCodecError(
                    "convert_yuv444_to_422: planes_out 形状/dtype 不匹配")
            u_out, v_out = planes_out
        if (not u.flags.c_contiguous or not v.flags.c_contiguous
                or not u_out.flags.c_contiguous
                or not v_out.flags.c_contiguous):
            raise ToposCodecError(
                "convert_yuv444_to_422: 输入/输出平面须为 C 连续数组")
        params = _CPlanar444To422Params()
        params.struct_size = ctypes.sizeof(_CPlanar444To422Params)
        params.width = width
        params.height = height
        params.bit_depth = int(bit_depth)
        params.u_in = u.ctypes.data_as(ctypes.c_void_p)
        params.u_in_stride = u.strides[0] // u.itemsize
        params.v_in = v.ctypes.data_as(ctypes.c_void_p)
        params.v_in_stride = v.strides[0] // v.itemsize
        params.u_out = u_out.ctypes.data_as(
            ctypes.POINTER(ctypes.c_uint16))
        params.u_out_stride = u_out.strides[0] // u_out.itemsize
        params.v_out = v_out.ctypes.data_as(
            ctypes.POINTER(ctypes.c_uint16))
        params.v_out_stride = v_out.strides[0] // v_out.itemsize
        params.max_workers = max(1, min(64, int(max_workers)))
        status = int(self._lib.tc_convert_yuv444_to_422(
            ctypes.byref(params)))
        if status != TC_OK:
            raise ToposCodecError(
                f"convert_yuv444_to_422 失败 [{status}]: {self.last_error()}")
        return [u_out, v_out]

    # —— 内部 ——
    def _configure_signatures(self) -> None:
        lib = self._lib
        lib.tc_abi_version.restype = ctypes.c_int32
        lib.tc_abi_version.argtypes = []
        lib.tc_version.restype = ctypes.c_int32
        lib.tc_version.argtypes = [ctypes.POINTER(_CVersionInfo)]
        lib.tc_query_cpu_features.restype = ctypes.c_int32
        lib.tc_query_cpu_features.argtypes = [ctypes.POINTER(_CCpuFeatures)]
        self._has_capabilities = hasattr(lib, "tc_query_capabilities")
        if self._has_capabilities:
            lib.tc_query_capabilities.restype = ctypes.c_int32
            lib.tc_query_capabilities.argtypes = [ctypes.POINTER(_CCodecCapabilities)]
        # P0-04：V7-B scalable 发布四态查询（老库无此符号时按缺失处理）
        self._has_scalable_status = hasattr(lib, "tc_query_scalable_status")
        if self._has_scalable_status:
            lib.tc_query_scalable_status.restype = ctypes.c_int32
            lib.tc_query_scalable_status.argtypes = [
                ctypes.POINTER(ctypes.c_int32),
            ]
            lib.tc_scalable_status_name.restype = ctypes.c_char_p
            lib.tc_scalable_status_name.argtypes = [ctypes.c_int32]
        lib.tc_query_support.restype = ctypes.c_int32
        lib.tc_query_support.argtypes = [
            ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32,
        ]
        lib.tc_status_message.restype = ctypes.c_char_p
        lib.tc_status_message.argtypes = [ctypes.c_int32]
        lib.tc_last_error.restype = ctypes.c_char_p
        lib.tc_last_error.argtypes = []
        # R6：常驻线程池调度旋钮（进程级；详见 set_slice_threads）
        lib.tc_dev_set_thread_count.restype = None
        lib.tc_dev_set_thread_count.argtypes = [ctypes.c_int32]
        lib.tc_dev_thread_count.restype = ctypes.c_int32
        lib.tc_dev_thread_count.argtypes = []
        # E1：编码阶段画像（profile 开启时累计，关闭时保持零）。
        self._has_encode_stage_stats = hasattr(lib, "tc_dev_encode_stats_get")
        if self._has_encode_stage_stats:
            lib.tc_dev_encode_stats_reset.restype = None
            lib.tc_dev_encode_stats_reset.argtypes = []
            lib.tc_dev_encode_stats_get.restype = None
            lib.tc_dev_encode_stats_get.argtypes = [
                ctypes.POINTER(_CEncodeStageStats),
            ]
        # RD0-03：解码 profile/路径计数（旧 dylib 无符号时保持 False）。
        self._has_decode_stage_stats = hasattr(lib, "tc_dev_decode_stats_get")
        if self._has_decode_stage_stats:
            lib.tc_dev_decode_stats_reset.restype = None
            lib.tc_dev_decode_stats_reset.argtypes = []
            lib.tc_dev_decode_stats_get.restype = None
            lib.tc_dev_decode_stats_get.argtypes = [
                ctypes.POINTER(_CDecodeStageStats),
            ]
        # M9：训练符号直方图（dev；详见 ToposCodec.symbol_hist_*）。
        # 旧 dylib 缺符号时静默降级（同 _has_encode_stage_stats 口径）——
        # 绑定期抛错会让旧库连视频都打不开。
        u64_p = ctypes.POINTER(ctypes.c_uint64)
        self._has_symbol_hist = hasattr(lib, "tc_dev_symbol_hist_enable")
        if self._has_symbol_hist:
            lib.tc_dev_symbol_hist_enable.restype = None
            lib.tc_dev_symbol_hist_enable.argtypes = [ctypes.c_int32]
            lib.tc_dev_symbol_hist_reset.restype = None
            lib.tc_dev_symbol_hist_reset.argtypes = []
            lib.tc_dev_symbol_hist_get.restype = None
            lib.tc_dev_symbol_hist_get.argtypes = [u64_p, u64_p, u64_p]
        self._has_symbol_hist_plane = hasattr(lib, "tc_dev_symbol_hist_plane_get")
        if self._has_symbol_hist_plane:
            lib.tc_dev_symbol_hist_plane_get.restype = None
            lib.tc_dev_symbol_hist_plane_get.argtypes = [ctypes.c_uint32, u64_p, u64_p, u64_p]
            lib.tc_dev_symbol_pair_hist_get.restype = None
            lib.tc_dev_symbol_pair_hist_get.argtypes = [ctypes.c_uint32, u64_p]

        cfg_p = ctypes.POINTER(_CFrameConfig)
        in_p = ctypes.POINTER(_CFrameInput)
        u16_p = ctypes.POINTER(ctypes.c_uint16)
        lib.tc_frame_config_validate.restype = ctypes.c_int32
        lib.tc_frame_config_validate.argtypes = [cfg_p]
        lib.tc_frame_packet_bound.restype = ctypes.c_size_t
        lib.tc_frame_packet_bound.argtypes = [cfg_p]
        lib.tc_frame_encode.restype = ctypes.c_int32
        lib.tc_frame_encode.argtypes = [
            cfg_p, in_p, ctypes.c_void_p, ctypes.c_size_t,
            ctypes.POINTER(_CFrameStats),
        ]
        self._has_scalable_encoder = hasattr(lib, "tc_frame_encode_scalable")
        if self._has_scalable_encoder:
            lib.tc_frame_encode_scalable.restype = ctypes.c_int32
            lib.tc_frame_encode_scalable.argtypes = [
                cfg_p, in_p, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_size_t,
                ctypes.POINTER(_CFrameStats),
            ]
        lib.tc_frame_encode_sized.restype = ctypes.c_int32
        lib.tc_frame_encode_sized.argtypes = [
            cfg_p, in_p, ctypes.c_uint32, ctypes.c_uint8, ctypes.c_uint8,
            ctypes.POINTER(ctypes.c_uint8), ctypes.c_void_p, ctypes.c_size_t,
            ctypes.POINTER(_CFrameStats),
        ]
        lib.tc_frame_decode.restype = ctypes.c_int32
        lib.tc_frame_decode.argtypes = [
            ctypes.c_void_p, ctypes.c_size_t,
            ctypes.POINTER(u16_p), ctypes.POINTER(ctypes.c_size_t),
            ctypes.POINTER(_CFrameOutput),
        ]
        self._has_decode_request = hasattr(lib, "tc_frame_decode_request")
        if self._has_decode_request:
            lib.tc_frame_decode_request.restype = ctypes.c_int32
            lib.tc_frame_decode_request.argtypes = [
                ctypes.c_void_p, ctypes.c_size_t,
                ctypes.POINTER(_CDecodeRequest),
                ctypes.POINTER(u16_p), ctypes.POINTER(ctypes.c_size_t),
                ctypes.POINTER(_CFrameOutput),
            ]
        self._has_reduced_decode = hasattr(lib, "tc_frame_decode_reduced")
        if self._has_reduced_decode:
            lib.tc_frame_decode_reduced.restype = ctypes.c_int32
            lib.tc_frame_decode_reduced.argtypes = [
                ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int32,
                ctypes.POINTER(u16_p), ctypes.POINTER(ctypes.c_size_t),
                ctypes.POINTER(_CFrameOutput),
            ]
            lib.tc_decode_scale_dimensions.restype = ctypes.c_int32
            lib.tc_decode_scale_dimensions.argtypes = [
                ctypes.c_uint32, ctypes.c_uint32, ctypes.c_int32,
                ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_uint32),
            ]
        # 阶段4：跨帧批量解码（旧 dylib 无符号时保持 False，调用方走单帧路径）
        self._has_decode_batch = hasattr(lib, "tc_frame_decode_batch")
        if self._has_decode_batch:
            lib.tc_frame_decode_batch.restype = ctypes.c_int32
            lib.tc_frame_decode_batch.argtypes = [
                ctypes.POINTER(_CBatchPacket), ctypes.c_uint32,
                ctypes.POINTER(u16_p), ctypes.POINTER(ctypes.c_size_t),
                ctypes.POINTER(_CFrameOutput),
            ]
        lib.tc_frame_plane_geometry.restype = ctypes.c_int32
        lib.tc_frame_plane_geometry.argtypes = [
            ctypes.POINTER(_CFrameOutput), ctypes.c_uint32,
            ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_uint32),
        ]
        # M2：持久 decoder context（旧 dylib 无这些符号时保持 None，走旧路径）
        if hasattr(lib, "tc_decoder_create"):
            views_p = ctypes.POINTER(_CPlaneView)
            lib.tc_decoder_create.restype = ctypes.c_int32
            lib.tc_decoder_create.argtypes = [
                ctypes.POINTER(_CDecoderConfig), ctypes.POINTER(ctypes.c_void_p),
            ]
            lib.tc_decoder_destroy.restype = None
            lib.tc_decoder_destroy.argtypes = [ctypes.c_void_p]
            if hasattr(lib, "tc_decoder_set_max_slice_workers"):
                lib.tc_decoder_set_max_slice_workers.restype = ctypes.c_int32
                lib.tc_decoder_set_max_slice_workers.argtypes = [
                    ctypes.c_void_p, ctypes.c_uint32,
                ]
            else:
                # Context worker rebalancing is optional for pre-D3 libraries;
                # callers detect this capability before acquiring a lease.
                self._has_decoder_worker_budget = False
            self._has_decoder_worker_budget = hasattr(
                lib, "tc_decoder_set_max_slice_workers")
            lib.tc_decoder_prepare.restype = ctypes.c_int32
            lib.tc_decoder_prepare.argtypes = [
                ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                ctypes.POINTER(_CFrameOutput),
            ]
            lib.tc_decoder_decode.restype = ctypes.c_int32
            lib.tc_decoder_decode.argtypes = [
                ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                views_p, ctypes.POINTER(_CFrameOutput),
            ]
            self._has_decoder_request = hasattr(lib, "tc_decoder_decode_request")
            if self._has_decoder_request:
                lib.tc_decoder_prepare_request.restype = ctypes.c_int32
                lib.tc_decoder_prepare_request.argtypes = [
                    ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                    ctypes.POINTER(_CDecodeRequest), ctypes.POINTER(_CFrameOutput),
                ]
                lib.tc_decoder_decode_request.restype = ctypes.c_int32
                lib.tc_decoder_decode_request.argtypes = [
                    ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                    ctypes.POINTER(_CDecodeRequest), views_p,
                    ctypes.POINTER(_CFrameOutput),
                ]
            self._has_decoder_surface_request = hasattr(
                lib, "tc_decoder_decode_surface_request")
            if self._has_decoder_surface_request:
                lib.tc_decoder_decode_surface_request.restype = ctypes.c_int32
                lib.tc_decoder_decode_surface_request.argtypes = [
                    ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                    ctypes.POINTER(_CDecodeRequest),
                    ctypes.POINTER(_CDecodeSurface), ctypes.POINTER(_CFrameOutput),
                ]
            if hasattr(lib, "tc_decoder_decode_surface"):
                lib.tc_decoder_decode_surface.restype = ctypes.c_int32
                lib.tc_decoder_decode_surface.argtypes = [
                    ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                    ctypes.POINTER(_CDecodeSurface), ctypes.POINTER(_CFrameOutput),
                ]
                self._has_decoder_surface = True
            else:
                self._has_decoder_surface = False
            if hasattr(lib, "tc_decoder_decode_batch"):
                lib.tc_decoder_decode_batch.restype = ctypes.c_int32
                lib.tc_decoder_decode_batch.argtypes = [
                    ctypes.c_void_p, ctypes.POINTER(_CBatchPacket),
                    ctypes.c_uint32, views_p, ctypes.POINTER(_CFrameOutput),
                ]
                self._has_decoder_batch = True
            else:
                self._has_decoder_batch = False
            self._has_decoder_batch_request = hasattr(
                lib, "tc_decoder_decode_batch_request")
            if self._has_decoder_batch_request:
                lib.tc_decoder_decode_batch_request.restype = ctypes.c_int32
                lib.tc_decoder_decode_batch_request.argtypes = [
                    ctypes.c_void_p, ctypes.POINTER(_CBatchPacket), ctypes.c_uint32,
                    ctypes.POINTER(_CDecodeRequest), views_p,
                    ctypes.POINTER(_CFrameOutput),
                ]
            self._has_decoder_ctx = True
        else:
            self._has_decoder_ctx = False
            self._has_decoder_batch = False
            self._has_decoder_request = False
            self._has_decoder_surface_request = False
            self._has_decoder_batch_request = False
        # V9 GOP context（micro-gop 计划批 5 绑定面；旧 dylib 缺符号 = 无能力）
        self._has_gop_ctx = hasattr(lib, "tc_gop_context_create")
        if self._has_gop_ctx:
            gi_p = ctypes.POINTER(_CGopFrameInfo)
            lib.tc_gop_context_create.restype = ctypes.c_int32
            lib.tc_gop_context_create.argtypes = [
                ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p),
            ]
            lib.tc_gop_context_close.restype = None
            lib.tc_gop_context_close.argtypes = [ctypes.c_void_p]
            lib.tc_gop_context_observe.restype = ctypes.c_int32
            lib.tc_gop_context_observe.argtypes = [
                ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, gi_p,
            ]
            lib.tc_gop_context_feed.restype = ctypes.c_int32
            lib.tc_gop_context_feed.argtypes = [
                ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                views_p, gi_p,
            ]
            lib.tc_gop_context_encode_frame.restype = ctypes.c_int32
            lib.tc_gop_context_encode_frame.argtypes = [
                ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
                ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t),
                ctypes.c_int, ctypes.POINTER(_CFrameStats),
            ]
            lib.tc_gop_context_abort.restype = None
            lib.tc_gop_context_abort.argtypes = [ctypes.c_void_p, ctypes.c_int]
            lib.tc_gop_context_state.restype = ctypes.c_int32
            lib.tc_gop_context_state.argtypes = [
                ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32),
                ctypes.POINTER(ctypes.c_uint16),
            ]
        mv = ctypes.c_void_p
        lib.tc_mux_create.restype = ctypes.c_int32
        lib.tc_mux_create.argtypes = [
            ctypes.POINTER(_CMovieConfig), ctypes.POINTER(_CIo),
            ctypes.POINTER(mv),
        ]
        lib.tc_mux_add_packet.restype = ctypes.c_int32
        lib.tc_mux_add_packet.argtypes = [
            mv, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint64, ctypes.c_uint32,
        ]
        lib.tc_mux_finish.restype = ctypes.c_int32
        lib.tc_mux_finish.argtypes = [mv]
        lib.tc_mux_free.restype = None
        lib.tc_mux_free.argtypes = [mv]
        lib.tc_mux_set_alpha_budget.restype = ctypes.c_int32
        lib.tc_mux_set_alpha_budget.argtypes = [mv, ctypes.c_void_p]
        lib.tc_movie_alpha_budget.restype = ctypes.c_int32
        lib.tc_movie_alpha_budget.argtypes = [mv, ctypes.c_void_p]
        # v1.1 音频轨（新 dylib 唯一基线：缺符号直接 AttributeError → fail-fast，
        # 不做旧库降级——绑定层与 native 同仓同版）
        lib.tc_mux_add_audio.restype = ctypes.c_int32
        lib.tc_mux_add_audio.argtypes = [
            mv, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32,
        ]
        lib.tc_mux_set_audio_asc.restype = ctypes.c_int32
        lib.tc_mux_set_audio_asc.argtypes = [mv, ctypes.c_void_p, ctypes.c_size_t]
        lib.tc_mux_set_audio_priming.restype = ctypes.c_int32
        lib.tc_mux_set_audio_priming.argtypes = [mv, ctypes.c_uint32]
        lib.tc_mux_set_timecode.restype = ctypes.c_int32
        lib.tc_mux_set_timecode.argtypes = [
            mv, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32,
            ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32,
        ]
        lib.tc_movie_timecode.restype = ctypes.c_int32
        lib.tc_movie_timecode.argtypes = [mv, ctypes.c_void_p]
        # v1.6 多音轨（M-B8）；mux 句柄与其余 tc_mux_* 同用 mv void_p 别名
        lib.tc_mux_add_audio_track.restype = ctypes.c_int32
        lib.tc_mux_add_audio_track.argtypes = [mv, ctypes.POINTER(_CAudioTrackConfig)]
        lib.tc_mux_set_audio_track_asc.restype = ctypes.c_int32
        lib.tc_mux_set_audio_track_asc.argtypes = [mv, ctypes.c_uint32,
                                                   ctypes.c_char_p,
                                                   ctypes.c_size_t]
        lib.tc_mux_set_audio_track_priming.restype = ctypes.c_int32
        lib.tc_mux_set_audio_track_priming.argtypes = [mv, ctypes.c_uint32,
                                                       ctypes.c_uint32]
        lib.tc_mux_add_audio_to.restype = ctypes.c_int32
        lib.tc_mux_add_audio_to.argtypes = [mv, ctypes.c_uint32,
                                            ctypes.c_char_p, ctypes.c_size_t,
                                            ctypes.c_uint32]
        lib.tc_movie_audio_track_count.restype = ctypes.c_int32
        lib.tc_movie_audio_track_count.argtypes = [mv, ctypes.POINTER(ctypes.c_uint32)]
        lib.tc_movie_audio_info_at.restype = ctypes.c_int32
        lib.tc_movie_audio_info_at.argtypes = [mv, ctypes.c_uint32,
                                               ctypes.POINTER(_CAudioTrackInfo)]
        lib.tc_movie_read_audio_at.restype = ctypes.c_int32
        lib.tc_movie_read_audio_at.argtypes = [mv, ctypes.c_uint32,
                                               ctypes.c_uint64, ctypes.c_uint32,
                                               ctypes.c_char_p, ctypes.c_size_t,
                                               ctypes.POINTER(ctypes.c_size_t)]
        lib.tc_movie_audio_info.restype = ctypes.c_int32
        lib.tc_movie_audio_info.argtypes = [mv, ctypes.POINTER(_CAudioTrackInfo)]
        lib.tc_movie_read_audio.restype = ctypes.c_int32
        lib.tc_movie_read_audio.argtypes = [
            mv, ctypes.c_uint64, ctypes.c_uint32, ctypes.c_void_p,
            ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t),
        ]
        lib.tc_movie_open.restype = ctypes.c_int32
        lib.tc_movie_open.argtypes = [ctypes.POINTER(_CIo), ctypes.POINTER(mv)]
        lib.tc_movie_info.restype = ctypes.c_int32
        lib.tc_movie_info.argtypes = [mv, ctypes.POINTER(_CMovieInfo)]
        lib.tc_movie_packet.restype = ctypes.c_int32
        lib.tc_movie_packet.argtypes = [
            mv, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_size_t),
        ]
        self._has_movie_packet_base = hasattr(lib, "tc_movie_packet_base")
        if self._has_movie_packet_base:
            lib.tc_movie_packet_base.restype = ctypes.c_int32
            lib.tc_movie_packet_base.argtypes = [
                mv, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_size_t,
                ctypes.POINTER(ctypes.c_size_t),
            ]
        self._has_movie_packet_base_batch = hasattr(
            lib, "tc_movie_packet_base_batch")
        if self._has_movie_packet_base_batch:
            lib.tc_movie_packet_base_batch.restype = ctypes.c_int32
            lib.tc_movie_packet_base_batch.argtypes = [
                mv, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p,
                ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t),
                ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_uint8),
                ctypes.POINTER(ctypes.c_size_t),
            ]
        # 阶段4 兑现：批量聚读 arena（旧 dylib 缺符号 → 源侧回退逐包读取）
        self._has_movie_packet_batch = hasattr(lib, "tc_movie_packet_batch")
        if self._has_movie_packet_batch:
            lib.tc_movie_packet_batch.restype = ctypes.c_int32
            lib.tc_movie_packet_batch.argtypes = [
                mv, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p,
                ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t),
                ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_size_t),
            ]
        # P1-11：fd 直读（native pread 绕过宿主回调；旧 dylib 缺符号 → 回退）
        self._has_movie_open_fd = hasattr(lib, "tc_movie_open_fd")
        if self._has_movie_open_fd:
            lib.tc_movie_open_fd.restype = ctypes.c_int32
            lib.tc_movie_open_fd.argtypes = [
                ctypes.c_int, ctypes.c_uint64, ctypes.POINTER(mv),
            ]
        lib.tc_movie_packet_pts.restype = ctypes.c_int32
        lib.tc_movie_packet_pts.argtypes = [
            mv, ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint64),
            ctypes.POINTER(ctypes.c_uint32),
        ]
        self._has_movie_prev_sync = hasattr(lib, "tc_movie_prev_sync")
        if self._has_movie_prev_sync:
            lib.tc_movie_prev_sync.restype = ctypes.c_int32
            lib.tc_movie_prev_sync.argtypes = [
                ctypes.c_void_p, ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32),
            ]
        lib.tc_movie_packet_sync.restype = ctypes.c_int32
        lib.tc_movie_packet_sync.argtypes = [
            mv, ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint8),
        ]
        lib.tc_movie_close.restype = None
        lib.tc_movie_close.argtypes = [mv]
        lib.tc_movie_faststart.restype = ctypes.c_int32
        lib.tc_movie_faststart.argtypes = [ctypes.POINTER(_CIo), ctypes.POINTER(_CIo)]
        # P1-12：融合输入转换（旧 dylib 缺符号 → 编码器回退 numpy 路径）
        self._has_cvt = hasattr(lib, "tc_convert_packed_rgb")
        if self._has_cvt:
            lib.tc_convert_packed_rgb.restype = ctypes.c_int32
            lib.tc_convert_packed_rgb.argtypes = [
                ctypes.POINTER(_CCvtParams),
            ]
        self._has_planar_cvt = hasattr(lib, "tc_convert_yuv444_to_422")
        if self._has_planar_cvt:
            lib.tc_convert_yuv444_to_422.restype = ctypes.c_int32
            lib.tc_convert_yuv444_to_422.argtypes = [
                ctypes.POINTER(_CPlanar444To422Params),
            ]

    # —— MOV 容器（阶段 5）——
    def movie_config(
        self,
        width: int,
        height: int,
        qp: int = 24,
        profile: int = 3,
        qmatrix: int = 1,
        alpha_mode: int = 0,
        alpha_bit_depth: int = 16,
        alpha_premultiplied: bool = False,
        color_range: int = 1,
        color_primaries: int = 1,
        color_transfer: int = 1,
        color_matrix: int = 1,
        chroma_siting: int = 0,
        sar_num: int = 1,
        sar_den: int = 1,
        timescale: int = 24000,
        bit_depth: int = 10,
        pixel_format: int = 0,
        audio_codec: int = TC_AUDIO_CODEC_NONE,
        audio_sample_rate: int = 0,
        audio_channels: int = 0,
        audio_layout: int = 0,
        audio_bits_per_sample: int = 0,
        audio_sample_format: int = TC_AUDIO_FMT_INT,
    ) -> _CMovieConfig:
        """R4.1–R4.4 + v1.6（TRAW 批 1）：YUV 4:2:2|4:4:4|GBR|CFA 10/12-bit；
        profile 3=Standard（默认）、5=Pro444（4:4:4 10/12）、6=Extreme
        （4:4:4 12-bit）、7=TRAW（CFA 12-bit，qm0/LOG0/matrix0 契约）；
        pixel_format 0=4:2:2、1=4:4:4、2=GBR、3=CFA。

        v1.1 音频轨（可选）：audio_codec=NONE（默认）时不声明音轨；
        LPCM 档 bits 16/24/32（PCM 字节须为大端 'twos' 语义）、MP4A 档
        bits 须为 0（位深由码流自描述），且须随后 tc_mux_set_audio_asc。
        声明写入 movie config 的 reserved 槽位（sizeof 稳定，零 ABI 变更）。
        """
        cfg = _CMovieConfig()
        cfg.struct_size = ctypes.sizeof(_CMovieConfig)
        cfg.abi_version = TOPOS_CODEC_ABI_VERSION
        cfg.visible_width = width
        cfg.visible_height = height
        # 复验 P1-07：非法 capability 参数显式抛错——此前未知 profile→3、
        # 未知 pixel format→0、非 10/12-bit→10 的静默降级会隐藏调用方 bug
        #（最小复现 11：(999,999,999) 被对拍成 (3,0,10)）。
        if profile not in (3, 5, 6, 7):
            raise ToposCodecError(
                f"movie_config.profile {profile!r} 非法（合法 3=Standard/"
                f"5=Pro444/6=Extreme/7=TRAW；R4.4/v1.6 激活）")
        if pixel_format not in (0, 1, 2, 3):
            raise ToposCodecError(
                f"movie_config.pixel_format {pixel_format!r} 非法"
                f"（0=YUV422/1=YUV444/2=GBR/3=CFA）")
        if bit_depth not in (10, 12, 16):
            raise ToposCodecError(
                f"movie_config.bit_depth {bit_depth!r} 非法（枚举域 "
                f"10/12/16；16 = 批 4 内核加宽解锁）")
        if alpha_mode not in (0, 1, 2):
            raise ToposCodecError(
                f"movie_config.alpha_mode {alpha_mode!r} 非法（0=无/1=无损/"
                f"2=受限近似）")
        if alpha_mode and alpha_bit_depth not in (8, 10, 12, 16):
            raise ToposCodecError(
                f"movie_config.alpha_bit_depth {alpha_bit_depth!r} 非法"
                f"（alpha 开启时须 8/10/12/16）")
        cfg.profile = profile
        cfg.pixel_format = pixel_format
        cfg.bit_depth = bit_depth
        cfg.qmatrix_id = qmatrix
        cfg.qp_base = qp
        cfg.alpha_mode = alpha_mode
        cfg.alpha_bit_depth = alpha_bit_depth if alpha_mode else 0
        cfg.alpha_premultiplied = int(alpha_premultiplied)
        cfg.color_range = color_range
        cfg.color_primaries = color_primaries
        cfg.color_transfer = color_transfer
        cfg.color_matrix = color_matrix
        cfg.chroma_siting = chroma_siting
        cfg.sar_num = sar_num
        cfg.sar_den = sar_den
        cfg.timescale = timescale
        if audio_codec != TC_AUDIO_CODEC_NONE:
            if audio_codec not in (TC_AUDIO_CODEC_LPCM, TC_AUDIO_CODEC_MP4A):
                raise ToposCodecError(
                    f"movie_config.audio_codec {audio_codec!r} 非法"
                    f"（0=无/1=lpcm/2=mp4a）")
            # v1.4：全家族采样率（对齐 Apple 交付规范）
            if audio_sample_rate not in (44100, 48000, 88200, 96000,
                                         176400, 192000):
                raise ToposCodecError(
                    f"movie_config.audio_sample_rate {audio_sample_rate!r} 非法"
                    f"（44100/48000/88200/96000/176400/192000）")
            if _TC_AUDIO_LAYOUT_CHANNELS.get(audio_layout) != audio_channels:
                raise ToposCodecError(
                    f"movie_config 音频声道/布局不匹配 "
                    f"channels={audio_channels!r} layout={audio_layout!r}"
                    f"（2↔stereo 6↔5.1 8↔7.1）")
            if audio_codec == TC_AUDIO_CODEC_LPCM:
                if audio_bits_per_sample not in (16, 24, 32):
                    raise ToposCodecError(
                        f"movie_config lpcm 位深 {audio_bits_per_sample!r} 非法"
                        f"（16/24/32）")
            elif audio_bits_per_sample not in (0, 16):
                raise ToposCodecError(
                    f"movie_config mp4a 不接受位深声明 {audio_bits_per_sample!r}"
                    f"（0 或 16）")
            # v1.4：float32 仅 lpcm+32bit 合法
            if audio_sample_format not in (TC_AUDIO_FMT_INT, TC_AUDIO_FMT_FLOAT32):
                raise ToposCodecError(
                    f"movie_config.audio_sample_format {audio_sample_format!r} 非法")
            if (audio_sample_format == TC_AUDIO_FMT_FLOAT32
                    and not (audio_codec == TC_AUDIO_CODEC_LPCM
                             and audio_bits_per_sample == 32)):
                raise ToposCodecError(
                    "movie_config float32 仅 lpcm+32bit 合法")
        elif (audio_sample_rate or audio_channels or audio_layout
              or audio_bits_per_sample
              or audio_sample_format != TC_AUDIO_FMT_INT):
            raise ToposCodecError(
                "movie_config 音频 codec=NONE 时其余 audio_* 参数必须为缺省"
                "（防半声明）")
        cfg.reserved[0] = audio_codec
        cfg.reserved[1] = audio_sample_rate
        cfg.reserved[2] = audio_channels
        cfg.reserved[3] = audio_layout
        cfg.reserved[4] = (audio_bits_per_sample
                           if audio_codec == TC_AUDIO_CODEC_LPCM else 0)
        cfg.reserved[5] = (audio_sample_format
                           if audio_codec == TC_AUDIO_CODEC_LPCM
                           and audio_sample_format == TC_AUDIO_FMT_FLOAT32
                           else 0)
        return cfg

    def faststart_file(self, src_path: str, dst_path: str) -> None:
        """FastStart 后处理（标准布局 → ftyp+moov+mdat）。"""
        src_fd = _open_binary(src_path, os.O_RDONLY)
        dst_fd = _open_binary(dst_path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC,
                              0o644)
        try:
            keep_r = TC_IO_READ_FN(_file_read_cb(src_fd))
            keep_w = TC_IO_WRITE_FN(_file_write_cb(dst_fd))
            src_io = _CIo(
                struct_size=ctypes.sizeof(_CIo),
                abi_version=TOPOS_CODEC_ABI_VERSION,
                read=keep_r, length=os.fstat(src_fd).st_size,
            )
            dst_io = _CIo(
                struct_size=ctypes.sizeof(_CIo),
                abi_version=TOPOS_CODEC_ABI_VERSION,
                write=keep_w,
            )
            status = int(self._lib.tc_movie_faststart(ctypes.byref(src_io), ctypes.byref(dst_io)))
            if status != TC_OK:
                raise ToposCodecError(f"tc_movie_faststart 失败 [{status}]: {self.last_error()}")
        finally:
            os.close(src_fd)
            os.close(dst_fd)

    def _verify_abi(self) -> None:
        abi = self.abi_version
        if abi != TOPOS_CODEC_ABI_VERSION:
            raise ToposCodecError(
                f"ABI 版本不一致: lib={abi}, 绑定层={TOPOS_CODEC_ABI_VERSION}"
            )
        info = _CVersionInfo()
        status = int(self._lib.tc_version(ctypes.byref(info)))
        if status != TC_OK:
            raise ToposCodecError(f"tc_version 失败 [{status}]: {self.last_error()}")
        if info.struct_size != ctypes.sizeof(_CVersionInfo):
            raise ToposCodecError(
                "topos_version_info 镜像不一致（R-18 守卫）: "
                f"C sizeof={info.struct_size}, ctypes sizeof={ctypes.sizeof(_CVersionInfo)}"
            )


# —— M10-4：顺序播放双帧流水（帧间并行） ——
# 动机：单帧 slice 并行有粒度上限（1080p 有效核 ~4.5-6.3），顺序解码时剩余
# 核闲置。本 API 以 workers 路条带化流水同时解多帧，共享进程级 slice 线程池，
# 按序产出。每路独立 ToposMovieFile 句柄（独立 fd/读锁/packet 缓冲）与独立
# 输出平面缓冲，无共享可变状态；tc_frame_decode 每调用栈上自持帧状态，
# 可多线程并发提交（pool 侧按 batch 交错调度）。

import queue as _queue


class ParallelSequentialDecoder:
    """顺序解码双帧流水：workers 路条带化并发，按序产出 (index, planes)。

    planes 为 List[numpy.ndarray]（uint16，1D 紧凑视图，形状 (h, w) 由调用方
    reshape）。用法：
        with ParallelSequentialDecoder(path, workers=2, threads=8) as dec:
            for index, planes in dec:
                ...
    每路独立 ToposMovieFile 句柄与输出缓冲；线程池为进程级共享（threads
    是池上限，两帧同飞时由池按 batch 交错调度）。
    """

    def __init__(self, path: str, workers: int = 2, threads: Optional[int] = None,
                 codec: Optional["ToposCodec"] = None) -> None:
        import numpy as np  # 延迟导入：仅本 API 需要 numpy
        self._np = np
        self._path = str(path)
        if workers < 1:
            raise ValueError("workers 必须 ≥ 1")
        self._workers = int(workers)
        self._codec = codec if codec is not None else ToposCodec()
        self._owns_codec = codec is None
        self._threads = int(threads) if threads is not None else None
        if threads is not None:
            self._codec.set_slice_threads(int(threads))
        self._total: Optional[int] = None

    def __enter__(self) -> "ParallelSequentialDecoder":
        # 预探测帧数（一次轻量 open/info/close）
        probe = ToposMovieFile(self._codec, self._path)
        try:
            self._total = int(probe.info().sample_count)
        finally:
            probe.close()
        return self

    def __exit__(self, *_exc) -> None:
        # ToposCodec 为进程级常驻（无 close）；各 worker 内打开的
        # ToposMovieFile 句柄已由 worker 函数 finally 自行释放
        pass

    def __iter__(self):
        if self._total is None:
            raise ToposCodecError("需先进入 with 上下文（探测帧数）")
        np = self._np
        workers = min(self._workers, self._total) if self._total else 1
        total = self._total
        PREFETCH = 2  # 每路在飞令牌数（含正在解码的 1 帧）

        stop = object()

        def worker(wid: int, in_q: "_queue.Queue", out_q: "_queue.Queue") -> None:
            movie = ToposMovieFile(self._codec, self._path)
            # M10-4：每路独立 decoder context + 半池预算（向上取整）——tpool
            # 按 id_base 把并发批次落到不相交 worker 区间，真正帧间并行
            # threads//2 + 1：A 批取 worker [1, b]，B 批错开后取 [b+2, T]——
            # 恰好覆盖全部 worker（budget=(T+1)//2 会让 T/2 号 worker 两批都落空）
            budget = ((self._threads // 2) + 1) if (self._threads and workers > 1) else 0
            dec = ToposDecoder(self._codec, max_slice_workers=budget)
            lease = None
            if getattr(self._codec, "_has_decoder_worker_budget", False):
                from .decode_worker_budget import DECODE_WORKER_BUDGET

                lease = DECODE_WORKER_BUDGET.acquire(
                    f"parallel:{id(self)}:{wid}", requested=budget,
                    role="export", capacity=(self._threads or self._codec.slice_threads()),
                    apply=dec.set_max_slice_workers,
                )
            planes_buf: List = []
            views = None
            try:
                for idx in range(wid, total, workers):
                    tok = in_q.get()  # 流控令牌（等待主线程消费放行）
                    if tok is stop:
                        return
                    buf, size = movie.packet_into(idx)
                    if views is None:  # 首帧探测几何并分配输出平面
                        info = dec.prepare(buf, size)
                        planes_buf = []
                        for p in range(int(info.plane_count)):
                            pw, ph = self._codec.plane_geometry(info, p)
                            planes_buf.append(np.empty(
                                (ph, pw), dtype="<u2"))
                        views = dec.make_views(planes_buf)
                    dec.decode_views(buf, views, len(planes_buf), size)
                    planes = [pl.copy() for pl in planes_buf]  # 复用前必须物化
                    out_q.put((idx, planes))
                out_q.put((None, None))  # 本路完成
            except BaseException as e:  # 异常传给主线程
                out_q.put((None, e))
            finally:
                dec.close()
                if lease is not None:
                    lease.release()
                movie.close()

        in_qs: List["_queue.Queue"] = [_queue.Queue() for _ in range(workers)]
        out_q: "_queue.Queue" = _queue.Queue()
        threads = []
        for wid in range(workers):
            t = threading.Thread(target=worker, args=(wid, in_qs[wid], out_q),
                                 daemon=True)
            t.start()
            threads.append(t)

        # 初始放行：每路首帧
        for wid in range(workers):
            if wid < total:
                in_qs[wid].put(True)

        results: "dict[int, list]" = {}
        next_idx = 0
        finished = 0
        try:
            while finished < workers:
                idx, payload = out_q.get()
                if isinstance(payload, BaseException):
                    raise payload
                if idx is None:  # worker 退出
                    finished += 1
                    continue
                results[idx] = payload
                # 按序产出；产出后向产生该帧的路放行下一帧令牌
                while next_idx in results:
                    planes = results.pop(next_idx)
                    yield next_idx, planes
                    owner = next_idx % workers
                    nxt = next_idx + workers
                    if nxt < total:
                        in_qs[owner].put(True)
                    next_idx += 1
        finally:
            for q in in_qs:
                try:
                    q.put_nowait(stop)
                except Exception:
                    pass
            for t in threads:
                t.join(timeout=2.0)
