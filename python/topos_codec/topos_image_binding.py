"""Topos Image (.toos/TPIM) ctypes 绑定（Topos Image 阶段 5）。

镜像 native/topos_codec/include/topos_image.h 的公共 ABI：
 probe / validate / decode / decode_preview / query_decode_buffer / read_metadata /
query_capabilities / write。

约定（与 topos_binding.py 一致）：
- 不编译 CPython 扩展，标准库 ctypes；
- 库加载复用 topos_binding.load_library（TOPOS_CODEC_LIB 环境变量优先）；
- 所有 C 结构保持 struct_size/abi_version 首两字段，全 0 初始化即合法最小；
- 解码输出直接写入 caller-provided numpy 平面（零拷贝，无中间复制）。

错误模型：C 返回码非 OK 时抛 ToposImageError（附 tc_image_status_message
与 tc_last_error 详情）；警告码 TC_WARN_CONCEALED 由调用方按需放行。
"""
from __future__ import annotations

import ctypes
import logging
import os
import threading
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional, Tuple

import numpy as np

from .topos_binding import (
    _CIo,
    TC_IO_READ_FN,
    TC_IO_WRITE_FN,
    TC_IO_SEEK_WRITE_FN,
    load_library,
)
from .topos_meta import (
    TOPOS_IMAGE_META_CHUNK_TYPE,
    ToposMetaError,
    decode_topos_meta,
)

logger = logging.getLogger(__name__)

TOPOS_IMAGE_ABI_VERSION = 1
TOPOS_CODEC_ABI_VERSION = 2

# —— image 层错误码（镜像 topos_image.h，值冻结） ——
TC_OK = 0
TC_WARN_CONCEALED = 1
TC_IMG_ERR_BAD_MAGIC = -100
TC_IMG_ERR_BAD_PREAMBLE = -101
TC_IMG_ERR_BAD_DIRECTORY = -102
TC_IMG_ERR_UNKNOWN_CRITICAL = -103
TC_IMG_ERR_CHUNK_CONFLICT = -104
TC_IMG_ERR_METADATA_CONFLICT = -105
TC_IMG_ERR_LIMIT = -106
TC_IMG_ERR_IO_WRITE_FAILED = -107

TC_FRAME_MAX_PLANES = 4

# chunk FourCC
TC_IMG_CHUNK_IDSC = 0x49445343
TC_IMG_CHUNK_PIXL = 0x5049584C
TC_IMG_CHUNK_ICCP = 0x49434350
# RC1（M6）：TRAW 开发元数据（as-shot WB/EI/black level/cfa_layout）
TC_IMG_CHUNK_TRWM = 0x5452574D
TC_IMG_CHUNK_OCIO = 0x4F43494F
TC_IMG_CHUNK_XMP = 0x584D5020
TC_IMG_CHUNK_EXIF = 0x45584946
TC_IMG_CHUNK_THMB = 0x54484D42
TC_IMG_CHUNK_HASH = 0x48415348

# chunk_flags 位（镜像 topos_image.h）
TC_IMG_CHUNK_FLAG_CRITICAL = 0x1
TC_IMG_CHUNK_FLAG_OPTIONAL = 0x2
TC_IMG_CHUNK_FLAG_PRESERVE = 0x4   # 可编辑 round-trip 字节级保留
TC_IMG_CHUNK_FLAG_DROPPABLE = 0x8  # 如 THMB；允许写入端丢弃

TC_IMG_VALIDATE_DEEP = 1

# —— HALF 样本域（spec §15，2026-09-19 冻结） ——
TC_IMG_SAMPLE_KIND_UINT = 0
TC_IMG_SAMPLE_KIND_HALF = 1
TC_IMG_PROFILE_PREVIEW = 0
TC_IMG_PROFILE_HQ = 1
TC_IMG_PROFILE_XQ = 2
TC_IMG_PROFILE_RAW = 3
TC_IMG_PROFILE_HALF_FLOAT = 4

# 冻结映射（与 native half_map.h / tc_image_half_to_codes 逐位一致）：
#   code = (h & 0x8000) ? 0x8000 - (h & 0x7FFF) : 0x8000 + (h & 0x7FFF)
# 有限值严格单调（对称对数域）；±0 归一为码 0x8000；逆映射对不可达码 0
# 饱和到 0xFFFF（-NaN）。全部有限 half 与码值双射 → qp≤10（Q=1）无损。
_HALF_MID = np.uint32(0x8000)
_HALF_MAG = np.uint32(0x7FFF)


# G9：u16→u16 纯函数 → 65536 项 LUT 惰性建表 + gather。相对逐元素
# np.where 链（每平面 2–3 份全帧 uint32 临时，8K 单平面 ~1.5GB 内存
# 流量），gather 只有一次查表读；映射为纯函数，LUT 与逐位实现严格等价
# （建表即原实现跑满定义域）。
_HALF_TO_CODE_LUT: "np.ndarray | None" = None
_CODE_TO_HALF_LUT: "np.ndarray | None" = None


def _half_to_code_lut() -> np.ndarray:
    global _HALF_TO_CODE_LUT
    if _HALF_TO_CODE_LUT is None:
        bits = np.arange(65536, dtype=np.uint32)
        m = bits & _HALF_MAG
        neg = (bits & np.uint32(0x8000)) != 0
        _HALF_TO_CODE_LUT = np.where(
            neg, _HALF_MID - m, _HALF_MID + m).astype(np.uint16)
    return _HALF_TO_CODE_LUT


def _code_to_half_lut() -> np.ndarray:
    global _CODE_TO_HALF_LUT
    if _CODE_TO_HALF_LUT is None:
        codes = np.arange(65536, dtype=np.uint32)
        pos = codes >= _HALF_MID
        m = np.where(pos, codes - _HALF_MID,
                     np.minimum(_HALF_MID - codes, _HALF_MAG))
        _CODE_TO_HALF_LUT = np.where(
            pos, m, np.uint32(0x8000) | m).astype(np.uint16)
    return _CODE_TO_HALF_LUT


def half_bits_to_codes(half_bits: np.ndarray) -> np.ndarray:
    """half 位模式（u16 视图）→ 码值平面（u16；等价 native 批量映射）。

    输入为 ``float16`` 数组的 ``view('<u2')`` 或等价 u16 位模式；输出可直接
    送 ``tc_frame_encode``（bd=16）。不修改输入。
    """
    h = np.ascontiguousarray(half_bits, dtype=np.uint16)
    return _half_to_code_lut()[h]


def codes_to_half_bits(codes: np.ndarray) -> np.ndarray:
    """码值平面（u16）→ half 位模式（u16；``view(np.float16)`` 得 float 值）。

    逆映射；码 0（前向不可达，损坏流）饱和到 0xFFFF（-NaN），与 native 一致。
    """
    c = np.ascontiguousarray(codes, dtype=np.uint16)
    return _code_to_half_lut()[c]


def float16_to_half_codes(plane: np.ndarray) -> np.ndarray:
    """float16/float32 平面 → 码值平面（float32 先舍入到 float16 位模式）。"""
    p = np.asarray(plane)
    if p.dtype == np.float16:
        return half_bits_to_codes(p.view(np.uint16))
    return half_bits_to_codes(p.astype(np.float16).view(np.uint16))


def half_codes_to_float16(codes: np.ndarray) -> np.ndarray:
    """码值平面 → float16 平面（新缓冲；逆映射后 view 为 float16）。"""
    return codes_to_half_bits(codes).view(np.float16)


class ToposImageError(RuntimeError):
    """image 层调用失败（含状态码与 last_error 详情）。"""

    def __init__(self, status: int, detail: str = "") -> None:
        self.status = status
        msg = f"topos_image 调用失败 status={status}"
        if detail:
            msg += f": {detail}"
        super().__init__(msg)


# ---------------- C 结构镜像 ----------------

class _CImagePreamble(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("file_version_major", ctypes.c_uint16),
        ("file_version_minor", ctypes.c_uint16),
        ("flags", ctypes.c_uint32),
        ("file_size", ctypes.c_uint64),
        ("directory_offset", ctypes.c_uint64),
        ("directory_entry_size", ctypes.c_uint32),
        ("directory_count", ctypes.c_uint32),
        ("primary_image_index", ctypes.c_uint32),
        ("compatibility_flags", ctypes.c_uint32),
        ("header_crc32", ctypes.c_uint32),
        ("directory_crc32", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32 * 4),
    ]


class _CImageIdsc(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("idsc_version_major", ctypes.c_uint16),
        ("idsc_version_minor", ctypes.c_uint16),
        ("flags", ctypes.c_uint32),
        ("display_x_min", ctypes.c_int32),
        ("display_y_min", ctypes.c_int32),
        ("display_x_max", ctypes.c_int32),
        ("display_y_max", ctypes.c_int32),
        ("data_x_min", ctypes.c_int32),
        ("data_y_min", ctypes.c_int32),
        ("data_x_max", ctypes.c_int32),
        ("data_y_max", ctypes.c_int32),
        ("orientation", ctypes.c_uint32),
        ("pixel_aspect_num", ctypes.c_uint32),
        ("pixel_aspect_den", ctypes.c_uint32),
        ("channel_model", ctypes.c_uint8),
        ("channel_count", ctypes.c_uint8),
        ("sample_kind", ctypes.c_uint8),
        ("valid_bit_depth", ctypes.c_uint8),
        ("container_bit_depth", ctypes.c_uint8),
        ("storage_layout", ctypes.c_uint8),
        ("subsampling", ctypes.c_uint8),
        ("chroma_siting", ctypes.c_uint8),
        ("alpha_presence", ctypes.c_uint8),
        ("alpha_mode", ctypes.c_uint8),
        ("alpha_bit_depth", ctypes.c_uint8),
        ("alpha_max_abs_err", ctypes.c_uint8),
        ("codec_id", ctypes.c_uint8),
        ("payload_major", ctypes.c_uint8),
        ("payload_minor", ctypes.c_uint8),
        ("image_profile", ctypes.c_uint8),
        ("codec_profile", ctypes.c_uint8),
        ("pixel_format", ctypes.c_uint8),
        ("color_primaries", ctypes.c_uint8),
        ("color_transfer", ctypes.c_uint8),
        ("color_matrix", ctypes.c_uint8),
        ("color_range", ctypes.c_uint8),
        ("iccp_ref", ctypes.c_uint8),
        ("ocio_ref", ctypes.c_uint8),
        ("payload_header_crc32", ctypes.c_uint32),
    ]


class _CImageInfo(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("preamble", _CImagePreamble),
        ("idsc", _CImageIdsc),
        ("pixl_offset", ctypes.c_uint64),
        ("pixl_size", ctypes.c_uint64),
        ("visible_width", ctypes.c_uint16),
        ("visible_height", ctypes.c_uint16),
        ("plane_count", ctypes.c_uint8),
        ("has_alpha", ctypes.c_uint8),
        ("bit_depth", ctypes.c_uint8),
        ("profile", ctypes.c_uint8),
        ("pixel_format", ctypes.c_uint8),
        ("alpha_mode", ctypes.c_uint8),
        ("alpha_bit_depth", ctypes.c_uint8),
        ("alpha_premultiplied", ctypes.c_uint8),
        ("reserved", ctypes.c_uint32 * 8),
    ]


class _CPlaneView(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("pixels", ctypes.POINTER(ctypes.c_uint16)),
        ("stride", ctypes.c_size_t),
    ]


class _CFrameOutput(ctypes.Structure):
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
        ("slice_status", ctypes.c_uint8 * 512),
        ("reserved", ctypes.c_uint32 * 8),
    ]


class _CImageCapabilities(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("file_version_major", ctypes.c_uint32),
        ("file_version_minor", ctypes.c_uint32),
        ("max_coded_dim", ctypes.c_uint32),
        ("max_pixl_bytes", ctypes.c_uint64),
        ("max_chunks", ctypes.c_uint32),
        ("profiles_mask", ctypes.c_uint32),
        ("metadata_mask", ctypes.c_uint32),
        ("bits", ctypes.c_uint32),  # tile_roi:1 half_float:1 multi_image:1 reserved:29
        ("reserved", ctypes.c_uint32 * 6),
    ]


class _CImageChunkIn(ctypes.Structure):
    """Mirror of ``topos_image_chunk_in`` (optional chunks are unused here)."""

    _fields_ = [
        ("chunk_type", ctypes.c_uint32),
        ("chunk_flags", ctypes.c_uint32),
        ("data", ctypes.c_void_p),
        ("size", ctypes.c_size_t),
    ]


class _CImageWriteParams(ctypes.Structure):
    """Mirror of ``topos_image_write_params``."""

    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("idsc", _CImageIdsc),
        ("pixl_data", ctypes.c_void_p),
        ("pixl_size", ctypes.c_size_t),
        ("extra_chunks", ctypes.POINTER(_CImageChunkIn)),
        ("extra_count", ctypes.c_uint32),
        ("compatibility_flags", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32 * 8),
    ]


@dataclass
class ToposImageInfo:
    """probe 结果的 Python 视图（host 字段子集）。"""

    path: str
    visible_width: int
    visible_height: int
    plane_count: int
    has_alpha: bool
    bit_depth: int
    pixel_format: int
    profile: int
    alpha_mode: int
    alpha_bit_depth: int
    alpha_premultiplied: bool
    image_profile: int
    payload_version: Tuple[int, int]
    color_range: int
    color_primaries: int
    color_transfer: int
    color_matrix: int
    pixel_aspect: Tuple[int, int]
    orientation: int
    pixl_size: int
    sample_kind: int = TC_IMG_SAMPLE_KIND_UINT
    iccp_ref: bool = False
    ocio_ref: bool = False
    raw: dict = field(default_factory=dict)


@dataclass
class ToposImageCapabilities:
    file_version_major: int
    file_version_minor: int
    profiles_mask: int
    metadata_mask: int
    max_coded_dim: int
    max_pixl_bytes: int
    max_chunks: int
    tile_roi: bool
    half_float: bool
    multi_image: bool


def _decode(raw: Optional[bytes]) -> str:
    return (raw or b"").decode("utf-8", errors="replace")


class ToposImageCodec:
    """image 层 C ABI 的 ctypes 封装（进程级共享实例见 get_image_codec）。"""

    def __init__(self, lib_path: Optional[str] = None) -> None:
        self._lib = load_library(lib_path)
        lib = self._lib
        c_ptr = ctypes.POINTER(ctypes.c_uint16)
        lib.tc_image_status_message.restype = ctypes.c_char_p
        lib.tc_image_status_message.argtypes = [ctypes.c_int32]
        lib.tc_image_probe.restype = ctypes.c_int32
        lib.tc_image_probe.argtypes = [ctypes.c_void_p, ctypes.POINTER(_CImageInfo)]
        lib.tc_image_validate.restype = ctypes.c_int32
        lib.tc_image_validate.argtypes = [
            ctypes.c_void_p, ctypes.c_uint32, ctypes.POINTER(_CImageInfo)]
        lib.tc_image_decode.restype = ctypes.c_int32
        lib.tc_image_decode.argtypes = [
            ctypes.c_void_p, ctypes.POINTER(_CPlaneView),
            ctypes.POINTER(_CFrameOutput)]
        self._has_native_preview = hasattr(lib, "tc_image_decode_preview")
        if self._has_native_preview:
            lib.tc_image_decode_preview.restype = ctypes.c_int32
            lib.tc_image_decode_preview.argtypes = [
                ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32,
                ctypes.POINTER(_CPlaneView), ctypes.POINTER(_CFrameOutput)]
        lib.tc_image_query_decode_buffer.restype = ctypes.c_int32
        lib.tc_image_query_decode_buffer.argtypes = [
            ctypes.POINTER(_CImageInfo), ctypes.c_uint32,
            ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_uint32)]
        lib.tc_image_read_metadata.restype = ctypes.c_int32
        lib.tc_image_read_metadata.argtypes = [
            ctypes.c_void_p, ctypes.POINTER(_CImageInfo), ctypes.c_uint32,
            ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
        lib.tc_image_query_capabilities.restype = ctypes.c_int32
        lib.tc_image_query_capabilities.argtypes = [
            ctypes.POINTER(_CImageCapabilities)]
        lib.tc_image_derive_idsc.restype = ctypes.c_int32
        lib.tc_image_derive_idsc.argtypes = [
            ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint8,
            ctypes.POINTER(_CImageIdsc)]
        lib.tc_image_write.restype = ctypes.c_int32
        lib.tc_image_write.argtypes = [
            ctypes.POINTER(_CImageWriteParams), ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_uint64)]
        # HALF 映射批量 API（spec §15；旧库无此符号 → numpy 实现仍可用）
        self._has_native_half_map = hasattr(lib, "tc_image_half_to_codes")
        if self._has_native_half_map:
            _u16p = ctypes.POINTER(ctypes.c_uint16)
            lib.tc_image_half_to_codes.restype = ctypes.c_int32
            lib.tc_image_half_to_codes.argtypes = [_u16p, ctypes.c_size_t, _u16p]
            lib.tc_image_codes_to_half.restype = ctypes.c_int32
            lib.tc_image_codes_to_half.argtypes = [_u16p, ctypes.c_size_t, _u16p]
        lib.tc_last_error.restype = ctypes.c_char_p
        lib.tc_last_error.argtypes = []
        self._view_ptr = ctypes.POINTER(_CPlaneView)
        self._u16_ptr = c_ptr

    # -- 基础 --

    def status_message(self, status: int) -> str:
        return _decode(self._lib.tc_image_status_message(ctypes.c_int32(status)))

    def _detail(self, status: int) -> str:
        return f"{self.status_message(status)} ({_decode(self._lib.tc_last_error())})"

    # -- 探测 / 校验 --

    @staticmethod
    def _with_io_detail(rc: int, detail: str, io: "_CFileIo") -> str:
        """C08：回调产生的 Python 侧 I/O 错误（native last_error 为空）时，
        把 _CFileIo.last_error 拼进诊断信息。"""
        io_err = getattr(io, "last_error", None)
        if io_err:
            return f"{detail}（io: {io_err}）"
        return detail

    def probe(self, path: str) -> ToposImageInfo:
        info = _CImageInfo()
        io = _CFileIo(path)
        try:
            rc = self._lib.tc_image_probe(ctypes.cast(io.byref(), ctypes.c_void_p),
                                          ctypes.byref(info))
        finally:
            io.close()
        if rc != TC_OK:
            raise ToposImageError(
                rc, self._with_io_detail(rc, self._detail(rc), io))
        return self._to_info(path, info)

    def validate(self, path: str, deep: bool = False) -> ToposImageInfo:
        info = _CImageInfo()
        io = _CFileIo(path)
        try:
            rc = self._lib.tc_image_validate(
                ctypes.cast(io.byref(), ctypes.c_void_p),
                ctypes.c_uint32(TC_IMG_VALIDATE_DEEP if deep else 0),
                ctypes.byref(info))
        finally:
            io.close()
        if rc != TC_OK:
            raise ToposImageError(
                rc, self._with_io_detail(rc, self._detail(rc), io))
        return self._to_info(path, info)

    @staticmethod
    def _to_info(path: str, info: _CImageInfo) -> ToposImageInfo:
        return ToposImageInfo(
            path=path,
            visible_width=int(info.visible_width),
            visible_height=int(info.visible_height),
            plane_count=int(info.plane_count),
            has_alpha=bool(info.has_alpha),
            bit_depth=int(info.bit_depth),
            pixel_format=int(info.pixel_format),
            profile=int(info.profile),
            alpha_mode=int(info.alpha_mode),
            alpha_bit_depth=int(info.alpha_bit_depth),
            alpha_premultiplied=bool(info.alpha_premultiplied),
            image_profile=int(info.idsc.image_profile),
            payload_version=(int(info.idsc.payload_major),
                             int(info.idsc.payload_minor)),
            color_range=int(info.idsc.color_range),
            color_primaries=int(info.idsc.color_primaries),
            color_transfer=int(info.idsc.color_transfer),
            color_matrix=int(info.idsc.color_matrix),
            pixel_aspect=(int(info.idsc.pixel_aspect_num),
                          int(info.idsc.pixel_aspect_den)),
            orientation=int(info.idsc.orientation),
            pixl_size=int(info.pixl_size),
            sample_kind=int(info.idsc.sample_kind),
            iccp_ref=bool(info.idsc.iccp_ref),
            ocio_ref=bool(info.idsc.ocio_ref),
        )

    # -- 解码 --

    def decode(self, path: str, target_size: Optional[tuple[int, int]] = None
               ) -> Tuple[List[np.ndarray], ToposImageInfo, dict]:
        """解码为 planar uint16 numpy 平面（零拷贝直接写入 numpy 缓冲）。

        ``target_size`` 是预览/缩略图的最大包围盒，不改变宽高比，也不会放大
        小图。新 native 库使用 ``tc_image_decode_preview`` 在 packet 解码阶段
        直接重建目标采样网格；旧库继续走完整解码后的 Python fallback，以保持
        部署兼容。native target-size 路径仍需顺序读取并校验所有 slice，且
        V3/V6/alpha 预测保留源状态，因此收益取决于缩小比例和编码 profile。
        返回 (planes, info, frame_out_dict)；plane 序见 spec §6.1（GBR=G,B,R）。
        """
        requested_size = self._normalise_target_size(target_size)
        info_c = _CImageInfo()
        io = _CFileIo(path)
        try:
            rc = self._lib.tc_image_probe(ctypes.cast(io.byref(), ctypes.c_void_p),
                                          ctypes.byref(info_c))
            if rc != TC_OK:
                raise ToposImageError(
                    rc, self._with_io_detail(rc, self._detail(rc), io))

            plane_count = int(info_c.plane_count)
            native_preview = False
            native_preview_mode = None
            preview_width = int(info_c.visible_width)
            preview_height = int(info_c.visible_height)
            if requested_size is not None:
                preview_width, preview_height = self._preview_dimensions(
                    int(info_c.visible_width), int(info_c.visible_height), requested_size,
                )
                native_preview = self._has_native_preview and (
                    (preview_width, preview_height) !=
                    (int(info_c.visible_width), int(info_c.visible_height))
                )
                if native_preview:
                    native_preview_mode = self._fixed_preview_scale(
                        int(info_c.visible_width), int(info_c.visible_height),
                        preview_width, preview_height,
                    )
            views = (_CPlaneView * TC_FRAME_MAX_PLANES)()
            arrays: List[np.ndarray] = []
            for p in range(TC_FRAME_MAX_PLANES):
                views[p].struct_size = ctypes.sizeof(_CPlaneView)
                # topos_plane_view 是 codec 域结构（topos_codec.h）——abi 必须
                # 填 TOPOS_CODEC_ABI_VERSION；曾误填 image 域版本（=1）
                views[p].abi_version = TOPOS_CODEC_ABI_VERSION
            for p in range(plane_count):
                if native_preview:
                    ph_value = preview_height
                    pw_value = preview_width
                    if int(info_c.pixel_format) == 0 and p in (1, 2):
                        pw_value = max(1, (preview_width + 1) // 2)
                    elif int(info_c.pixel_format) == 3:
                        # pf=3 CFA：4 个相位平面各 ceil(target/2)（native
                        # tc_frame_plane_geometry / dec_frame_build_jobs 的
                        # pf=3 缩放几何）——按整帧分配会让 3/4 缓冲残留
                        # 零值且帧尺寸推错 2×
                        ph_value = max(1, (preview_height + 1) // 2)
                        pw_value = max(1, (preview_width + 1) // 2)
                else:
                    pw = ctypes.c_uint32(0)
                    ph = ctypes.c_uint32(0)
                    rc = self._lib.tc_image_query_decode_buffer(
                        ctypes.byref(info_c), ctypes.c_uint32(p),
                        ctypes.byref(pw), ctypes.byref(ph))
                    if rc != TC_OK:
                        raise ToposImageError(rc, self._detail(rc))
                    pw_value, ph_value = int(pw.value), int(ph.value)
                arr = np.zeros((ph_value, pw_value), dtype=np.uint16)
                arrays.append(arr)
                views[p].pixels = arr.ctypes.data_as(ctypes.POINTER(ctypes.c_uint16))
                views[p].stride = 0
            out = _CFrameOutput()
            out.struct_size = ctypes.sizeof(_CFrameOutput)
            out.abi_version = TOPOS_CODEC_ABI_VERSION
            if native_preview:
                rc = self._lib.tc_image_decode_preview(
                    ctypes.cast(io.byref(), ctypes.c_void_p),
                    ctypes.c_uint32(preview_width), ctypes.c_uint32(preview_height),
                    ctypes.cast(views, self._view_ptr), ctypes.byref(out))
            else:
                rc = self._lib.tc_image_decode(
                    ctypes.cast(io.byref(), ctypes.c_void_p),
                    ctypes.cast(views, self._view_ptr), ctypes.byref(out))
        finally:
            io.close()
        if rc not in (TC_OK, TC_WARN_CONCEALED):
            raise ToposImageError(rc, self._detail(rc))
        info = self._to_info(path, info_c)
        if requested_size is not None and not native_preview:
            arrays, preview_width, preview_height = self._resize_for_preview(
                arrays, info, requested_size,
            )
            info.visible_width = preview_width
            info.visible_height = preview_height
        elif native_preview:
            info.visible_width = preview_width
            info.visible_height = preview_height
        return arrays, info, {
            "concealed_slices": int(out.concealed_slices),
            "slice_count": int(out.slice_count),
            "decode_size": (info.visible_width, info.visible_height),
            "source_size": (int(info_c.visible_width), int(info_c.visible_height)),
            "target_size_applied": requested_size is not None,
            "target_size_native": native_preview,
            "decode_mode": native_preview_mode or (
                "full" if requested_size is None else "python_resize"
            ),
        }

    @staticmethod
    def _normalise_target_size(
        target_size: Optional[tuple[int, int]],
    ) -> Optional[tuple[int, int]]:
        if target_size is None:
            return None
        if not isinstance(target_size, (tuple, list)) or len(target_size) != 2:
            raise ValueError("target_size must be a (width, height) pair")
        width, height = target_size
        if isinstance(width, bool) or isinstance(height, bool):
            raise ValueError("target_size dimensions must be positive integers")
        if int(width) != width or int(height) != height:
            raise ValueError("target_size dimensions must be positive integers")
        width, height = int(width), int(height)
        if width <= 0 or height <= 0:
            raise ValueError("target_size dimensions must be positive integers")
        return width, height

    @staticmethod
    def _resize_plane_nearest(plane: np.ndarray, width: int, height: int) -> np.ndarray:
        """Deterministically reduce one uint16 plane without a new dependency."""
        if plane.shape == (height, width):
            return plane
        y = np.floor(np.arange(height, dtype=np.float64)
                     * plane.shape[0] / height).astype(np.intp)
        x = np.floor(np.arange(width, dtype=np.float64)
                     * plane.shape[1] / width).astype(np.intp)
        return np.ascontiguousarray(plane[y[:, None], x[None, :]])

    @staticmethod
    def _preview_dimensions(
        source_width: int, source_height: int, target_size: tuple[int, int],
    ) -> tuple[int, int]:
        scale = min(
            1.0,
            target_size[0] / max(source_width, 1),
            target_size[1] / max(source_height, 1),
        )
        return (
            max(1, int(source_width * scale + 0.5)),
            max(1, int(source_height * scale + 0.5)),
        )

    @staticmethod
    def _fixed_preview_scale(
        source_width: int,
        source_height: int,
        target_width: int,
        target_height: int,
    ) -> str:
        """返回与 codec request 对齐的固定比例诊断标签。"""
        for label, divisor in (
            ("reduced_1_2", 2),
            ("reduced_1_3", 3),
            ("reduced_1_4", 4),
            ("reduced_1_8", 8),
        ):
            if ((source_width + divisor - 1) // divisor == target_width and
                    (source_height + divisor - 1) // divisor == target_height):
                return label
        return "scaled"

    @classmethod
    def _resize_for_preview(
        cls,
        planes: List[np.ndarray],
        info: ToposImageInfo,
        target_size: tuple[int, int],
    ) -> tuple[List[np.ndarray], int, int]:
        source_width = int(info.visible_width)
        source_height = int(info.visible_height)
        width, height = cls._preview_dimensions(source_width, source_height, target_size)
        if width == source_width and height == source_height:
            return planes, width, height

        resized: List[np.ndarray] = []
        for index, plane in enumerate(planes):
            if int(info.pixel_format) == 0 and index in (1, 2):
                plane_width = max(1, (width + 1) // 2)
            elif int(info.pixel_format) == 3:
                # pf=3 CFA：相位平面缩到 ceil(target/2)，不放大到整帧
                plane_width = max(1, (width + 1) // 2)
                plane_height = max(1, (height + 1) // 2)
                resized.append(cls._resize_plane_nearest(
                    np.asarray(plane, dtype=np.uint16), plane_width, plane_height,
                ))
                continue
            else:
                plane_width = width
            plane_height = height
            resized.append(cls._resize_plane_nearest(
                np.asarray(plane, dtype=np.uint16), plane_width, plane_height,
            ))
        return resized, width, height

    def decode_preview(
        self, path: str, target_size: tuple[int, int],
    ) -> Tuple[List[np.ndarray], ToposImageInfo, dict]:
        """明确表达预览用途的 target-size decode 入口。"""
        return self.decode(path, target_size=target_size)

    def decode_float(
        self, path: str, target_size: Optional[tuple[int, int]] = None,
    ) -> Tuple[List[np.ndarray], ToposImageInfo, dict]:
        """解码 HALF 样本域（sample_kind=1，spec §15）文件为 float16 平面。

        仅接受 image_profile 4 / sample_kind=HALF 的文件（UINT 文件请用
        decode()——样本域混用是显式错误，不做隐式转换）。返回
        (planes_f2, info, meta)：planes 为 float16 数组（G,B,R 序，alpha 为
        plane 3，同样为 half 域浮点）；qp≤10（Q=1）编码的文件逐位无损。
        """
        arrays, info, meta = self.decode(path, target_size=target_size)
        if info.sample_kind != TC_IMG_SAMPLE_KIND_HALF:
            raise ToposImageError(
                -1, f"decode_float: sample_kind={info.sample_kind} != HALF "
                    f"({path}；UINT 文件请用 decode())")
        planes = [half_codes_to_float16(a) for a in arrays]
        return planes, info, meta

    def write_packet(self, path: str, packet: bytes, image_profile: int,
                     *, atomic: bool = True, durable: bool = True,
                     extra_chunks: Optional[List[Tuple[int, bytes]]] = None,
                     iccp_ref: bool = False) -> int:
        """Wrap one already encoded TPIC packet in a ``.toos`` image file.

        The frame packet is produced by the persistent video encoder context;
        this method only performs the small TPIM envelope write.  It exists so
        image-sequence export does not have to spawn the ``toos`` CLI once per
        frame.  The native writer remains the single source of truth for IDSC
        validation and envelope layout.

        extra_chunks：追加 optional chunk（(FourCC int, 载荷) 列表；上限与
        内容校验由 native writer 强制）。用于容器元数据 TMET（topos_meta）。
        """
        if not packet:
            raise ToposImageError(-1, "write_packet: empty TPIC packet")
        target = str(path)
        tmp = target + ".toos-tmp" if atomic else target
        parent = os.path.dirname(os.path.abspath(target))
        os.makedirs(parent, exist_ok=True)
        fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
        write_failed = False

        def _write_cb(_ctx, data, length):
            nonlocal write_failed
            try:
                raw = ctypes.string_at(data, length)
                if os.write(fd, raw) != length:
                    write_failed = True
                    return -14  # TC_ERR_IO
                return TC_OK
            except OSError:
                write_failed = True
                return -14

        def _seek_write_cb(_ctx, offset, data, length):
            nonlocal write_failed
            try:
                raw = ctypes.string_at(data, length)
                if hasattr(os, "pwrite"):
                    n = os.pwrite(fd, raw, offset)
                else:  # pragma: no cover - POSIX/macOS path is pwrite
                    pos = os.lseek(fd, 0, os.SEEK_CUR)
                    os.lseek(fd, offset, os.SEEK_SET)
                    n = os.write(fd, raw)
                    os.lseek(fd, pos, os.SEEK_SET)
                if n != length:
                    write_failed = True
                    return -14
                return TC_OK
            except OSError:
                write_failed = True
                return -14

        keep_write = TC_IO_WRITE_FN(_write_cb)
        keep_seek_write = TC_IO_SEEK_WRITE_FN(_seek_write_cb)
        io = _CIo(
            struct_size=ctypes.sizeof(_CIo),
            abi_version=TOPOS_CODEC_ABI_VERSION,
            ctx=None,
            read=TC_IO_READ_FN(),
            write=keep_write,
            seek_write=keep_seek_write,
            length=0,
        )
        packet_buf = ctypes.create_string_buffer(packet)
        idsc = _CImageIdsc()
        try:
            rc = int(self._lib.tc_image_derive_idsc(
                ctypes.cast(packet_buf, ctypes.c_void_p), len(packet),
                ctypes.c_uint8(int(image_profile)), ctypes.byref(idsc)))
        except Exception:
            os.close(fd)
            try:
                os.unlink(tmp)
            except OSError:
                pass
            raise
        if rc != TC_OK:
            os.close(fd)
            try:
                os.unlink(tmp)
            except OSError:
                pass
            raise ToposImageError(rc, self._detail(rc))
        # M3-H2：ICCP 权威位（0/1）——调用方携带 ICCP chunk 时置位；native
        # writer 校验 ICC 与 IDSC 通道模型一致性（非 'RGB ' profile 拒绝）。
        if iccp_ref:
            idsc.iccp_ref = 1

        params = _CImageWriteParams()
        params.struct_size = ctypes.sizeof(_CImageWriteParams)
        params.abi_version = TOPOS_IMAGE_ABI_VERSION
        params.idsc = idsc
        params.pixl_data = ctypes.cast(packet_buf, ctypes.c_void_p)
        params.pixl_size = len(packet)
        params.extra_chunks = ctypes.POINTER(_CImageChunkIn)()
        params.extra_count = 0
        params.compatibility_flags = 0
        # extra chunk 缓冲必须保活到 tc_image_write 返回（c_void_p 不持有）
        chunk_bufs: List[ctypes.Array] = []
        if extra_chunks:
            chunk_array = (_CImageChunkIn * len(extra_chunks))()
            for i, (chunk_type, chunk_data) in enumerate(extra_chunks):
                buf = ctypes.create_string_buffer(chunk_data)
                chunk_bufs.append(buf)
                chunk_array[i].chunk_type = int(chunk_type)
                chunk_array[i].chunk_flags = (TC_IMG_CHUNK_FLAG_OPTIONAL
                                              | TC_IMG_CHUNK_FLAG_PRESERVE)
                chunk_array[i].data = ctypes.cast(buf, ctypes.c_void_p)
                chunk_array[i].size = len(chunk_data)
            params.extra_chunks = ctypes.cast(chunk_array,
                                              ctypes.POINTER(_CImageChunkIn))
            params.extra_count = len(extra_chunks)
        out_size = ctypes.c_uint64(0)
        try:
            rc = int(self._lib.tc_image_write(
                ctypes.byref(params), ctypes.byref(io), ctypes.byref(out_size)))
            if durable and rc == TC_OK:
                os.fsync(fd)
        except OSError:
            write_failed = True
            rc = -14  # TC_ERR_IO
        except Exception:
            write_failed = True
            rc = -14  # TC_ERR_IO
        finally:
            os.close(fd)
        if rc != TC_OK or write_failed:
            try:
                os.unlink(tmp)
            except OSError:
                pass
            raise ToposImageError(
                rc if rc != TC_OK else TC_IMG_ERR_IO_WRITE_FAILED,
                self._detail(rc if rc != TC_OK else TC_IMG_ERR_IO_WRITE_FAILED),
            )
        if atomic:
            os.replace(tmp, target)
        return int(out_size.value)

    def read_metadata(self, path: str, chunk_type: int) -> bytes:
        info = _CImageInfo()
        io = _CFileIo(path)
        try:
            rc = self._lib.tc_image_probe(ctypes.cast(io.byref(), ctypes.c_void_p),
                                          ctypes.byref(info))
            if rc != TC_OK:
                raise ToposImageError(
                    rc, self._with_io_detail(rc, self._detail(rc), io))
            size = ctypes.c_size_t(0)
            rc = self._lib.tc_image_read_metadata(
                ctypes.cast(io.byref(), ctypes.c_void_p), ctypes.byref(info),
                ctypes.c_uint32(chunk_type), None, 0, ctypes.byref(size))
            if rc != TC_OK:
                raise ToposImageError(rc, self._detail(rc))
            buf = (ctypes.c_uint8 * size.value)()
            rc = self._lib.tc_image_read_metadata(
                ctypes.cast(io.byref(), ctypes.c_void_p), ctypes.byref(info),
                ctypes.c_uint32(chunk_type), buf, size.value, ctypes.byref(size))
        finally:
            io.close()
        if rc != TC_OK:
            raise ToposImageError(rc, self._detail(rc))
        return bytes(buf)

    def read_export_meta(self, path: str) -> Optional[dict]:
        """读 TMET 容器元数据（v1.7：档位/厂商；见 topos_meta）。

        chunk 缺失（TC_ERR_INVALID_ARGUMENT）或载荷损坏 → None——容器元
        数据只服务展示，任何失败一律降级，不阻塞媒体打开。
        """
        try:
            payload = self.read_metadata(path, TOPOS_IMAGE_META_CHUNK_TYPE)
        except ToposImageError as exc:
            logger.debug("TMET 读取降级（%s）: %s", path, exc)
            return None
        try:
            return decode_topos_meta(payload)
        except ToposMetaError as exc:
            logger.debug("TMET 解码降级（%s）: %s", path, exc)
            return None

    def capabilities(self) -> ToposImageCapabilities:
        caps = _CImageCapabilities()
        rc = self._lib.tc_image_query_capabilities(ctypes.byref(caps))
        if rc != TC_OK:
            raise ToposImageError(rc, self._detail(rc))
        bits = int(caps.bits)
        return ToposImageCapabilities(
            file_version_major=int(caps.file_version_major),
            file_version_minor=int(caps.file_version_minor),
            profiles_mask=int(caps.profiles_mask),
            metadata_mask=int(caps.metadata_mask),
            max_coded_dim=int(caps.max_coded_dim),
            max_pixl_bytes=int(caps.max_pixl_bytes),
            max_chunks=int(caps.max_chunks),
            tile_roi=bool(bits & 1),
            half_float=bool(bits & 2),
            multi_image=bool(bits & 4),
        )

    def library_identity(self) -> tuple:
        """解码器构建身份（库路径 stat 指纹；缓存 key 组成之一）。

        库重构建后指纹变化，旧构建的解码缓存不与新构建混用（计划书
        阶段 6：缓存 key 须含 codec/file version——格式版本静态，构建
        指纹随重编译变化，是实际有效的失效维度）。stat 失败退化为路径名。
        """
        name = str(getattr(self._lib, "_name", "unknown"))
        try:
            st = os.stat(name)
            return (name, st.st_size, st.st_mtime_ns)
        except OSError:
            return (name,)


class _CFileIo:
    """基于文件句柄的 topos_io（read 回调；生命周期内保持句柄打开）。

    CFUNCTYPE 闭包持有 self（回调期间对象存活）；close 后不得再使用。
    """

    def __init__(self, path: str) -> None:
        self._f = open(path, "rb")
        self._length = os.fstat(self._f.fileno()).st_size
        self._io = _CIo()
        self._io.struct_size = ctypes.sizeof(_CIo)
        self._io.abi_version = TOPOS_CODEC_ABI_VERSION
        self._io.ctx = None
        self._io.read = TC_IO_READ_FN(self._read_impl)
        self._io.length = self._length
        self.last_error: Optional[str] = None

    def byref(self):
        return ctypes.byref(self._io)

    def _read_impl(self, _ctx, off: int, buf, n: int) -> int:
        # C08（2026-09-27 检查计划）：回调边界必须捕获异常——句柄已关闭
        # （ValueError）/磁盘/权限错误（OSError）逃逸时 ctypes 打印
        # "Exception ignored on calling ctypes callback function" 并按返回
        # 值 0（TC_OK）继续，缓冲未填充被当成成功读。捕获后返回
        # TC_ERR_IO，详情留在 last_error 供上层拼入诊断。
        try:
            f = self._f
            f.seek(off)
            data = f.read(n)
        except (OSError, ValueError) as exc:
            self.last_error = f"read@{off}: {type(exc).__name__}: {exc}"
            return -14  # TC_ERR_IO
        if len(data) != n:
            self.last_error = f"short read@{off}: {len(data)}/{n}"
            return -14  # TC_ERR_IO
        ctypes.memmove(buf, data, n)
        return 0

    def close(self) -> None:
        self._f.close()


_CODEC: Optional[ToposImageCodec] = None
_CODEC_ERR: Optional[str] = None
_CODEC_LOCK = threading.Lock()


def get_image_codec(lib_path: Optional[str] = None) -> ToposImageCodec:
    """进程级共享 ToposImageCodec（互斥懒加载；失败原因缓存）。

    并发首调不再双重构造（后写胜出）；失败缓存保留（与既有语义一致，
    进程内不自动重试——环境修复后需重启或显式传 lib_path 重试）。
    """
    global _CODEC, _CODEC_ERR
    if _CODEC is not None:
        return _CODEC
    with _CODEC_LOCK:
        if _CODEC is not None:
            return _CODEC
        if _CODEC_ERR is not None and lib_path is None:
            raise ToposImageError(-1, _CODEC_ERR)
        try:
            _CODEC = ToposImageCodec(lib_path)
            return _CODEC
        except Exception as exc:  # noqa: BLE001 —— 缓存失败原因供后续调用方诊断
            _CODEC_ERR = str(exc)
            raise


def check_image_codec_available(lib_path: Optional[str] = None) -> bool:
    try:
        get_image_codec(lib_path)
        return True
    except Exception:  # noqa: BLE001
        return False
