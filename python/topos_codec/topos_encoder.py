"""ToposVideoEncoder — 自研 Topos Video Codec 的应用层编码器（阶段 8）。

duck-type 对齐 ``FFmpegVideoEncoder``（同构造签名与生命周期），可被
``TimelineExporter._create_encoder`` / ``RenderCacheEncoder`` 直接分派：

- ``__init__(output_path, config: FFmpegEncoderConfig)``（不做 IO）
- ``open()`` / ``encode_frame(DecodedFrame)`` / ``close()``
- ``add_audio_stream`` / ``write_audio_samples``（v1.1 音频轨，
  container_spec_v1.3 附录 A：PCM 直存 / AAC 应用层编码；图片序列模式
  无容器音轨，显式拒绝）
- ``get_progress()`` / ``frame_count`` / ``is_open`` / ``output_path`` / ``config``

设计决策（ADR-C008）：

1. **临时文件 + 原子替换**：始终写 ``<output>.topos-tmp``，close() 成功
   （≥1 帧 + finish）才 ``os.replace`` 到目标路径；任何失败/``abort()``
   （用户取消）删除临时文件——**取消不会留下被误识别为成功的文件**。
2. **无静默回退**：native 库缺失/档位不可用/色彩标签未知均抛带明确诊断
   的 ``RuntimeError``，绝不回退到其它 codec。
3. **双输入形态**：
   - planar ``yuv422p10le`` / ``topos_yuva422p10a*``（Topos 源直通）：
     满刻度码值平面免转换再编码（bit-preserving 输入级），Alpha 直通；
   - packed BGR24/BGR48[BG]（时间线合成）：numpy 正向矩阵 RGB→YUV
     （BT.709/BT.601/BT.2020nc，limited/full 精确位移，4:2:2 box 子采样）。
   其它 planar 布局（420/444）v1 不支持——显式报错而非偷偷重采样。
4. **码控**：档位（capability schema）定标 bits-per-pixel → 帧级确定性
   qp 搜索（``encode_sized``）；``config.crf``（0–63）提供固定 QP 覆盖。
5. **R1 数据正确性（2026-08-30 审计）**：
   - packed 通道映射显式化——四通道禁止整轴 ``[..., ::-1]``（BGRA 会变
     ARGB，P0-2）；仅接受 rgb*/bgr* 3/4 通道布局；
   - ``_map_color_codes`` 的 range 仅接受 limited/full（空=未指定按
     limited），未知值显式失败（P1-4）；
   - planar 输入严格校验：格式白名单 / PlaneInfo 位深 / 紧排列 stride /
     码值域（P1-1，12-bit 数据不得以 10-bit 头静默编码）；
   - ``alpha_premultiplied`` 整段固定语义（显式配置，默认 straight＝应用
     内合成约定），帧自带标志不一致时显式报错（P0-3）；
   - alpha 位深从 ``pix_fmt`` 解析（a16→mode1 无损），不硬编码 12；
   - alpha 标度对齐 spec §8.6：码流内为 16-bit 满刻度容器，应用层平面
     语义是声明位深满刻度——编码 ``<<(16−N)``、解码侧 topos_source
     ``>>(16−N)``（修复 a8 近二值 / a12 仅 6% 不透明度的静默标度错）。
6. **R3 Alpha 预算闭环（2026-08-30 审计 §4 R3 / P1-3）**：
   - 逐帧消费 native stats（此前直接丢弃），比例 = alpha_payload /
     color_payload（计划 §2.2），文件级累计 + 跨帧最大误差记入 tpcB；
   - mode2 + **隐式位深**（裸 pix_fmt + has_alpha＝质量旋钮）时首帧探测
     12→10→8 直到满足档位目标比例（探测复用生产编码路径，纯函数确定）；
     tpcC/mux 容器级位深一致性 ⇒ 文件级定深（ADR-C014）；
   - **显式位深声明**（pix_fmt 带 a{N} 后缀＝格式契约，如 planar 直通/
     代理链）不降档——超限只记录；mode1（a16 无损）任何情况绝不降质；
   - 超硬上限三态策略（``alpha_budget_policy``）：record=记录交付（默认，
     警告 + tpcB overrun 标志）/ error=结构化诊断报错 / continue=授权交付
     （authorized 标志）；
   - close() 经 ``tc_mux_set_alpha_budget`` 写 tpcB（container_spec v1.1），
     解码侧 topos_source 读回 ``extra['topos_alpha_budget']``。
"""
from __future__ import annotations

import contextlib
import json
import logging
import math
import os
import queue
import re
import shutil
import tempfile
import threading
import time
from typing import Any, Dict, Iterator, List, Optional, Tuple

try:  # POSIX 目录锁；Windows 降级为无锁提交（见 _sequence_commit_lock）
    import fcntl
    _HAS_FCNTL = True
except ImportError:  # pragma: no cover - 平台分支
    fcntl = None
    _HAS_FCNTL = False

#: 同进程内正在执行提交的暂存目录（N01：同 pid 时区分"活提交"与
#: "本进程内中断/回滚未完成的事务"——后者允许被同进程的下一次导出恢复）。
_SEQ_COMMIT_IN_FLIGHT: "set[str]" = set()
_SEQ_COMMIT_IN_FLIGHT_LOCK = threading.Lock()

import numpy as np

from .topos_binding import (
    TC_AUDIO_CODEC_LPCM,
    TC_AUDIO_CODEC_MP4A,
    TC_AUDIO_FMT_FLOAT32,
    TC_AUDIO_FMT_INT,
    TC_AUDIO_LAYOUT_5_1,
    TC_AUDIO_LAYOUT_7_1,
    TC_AUDIO_LAYOUT_MONO,
    TC_AUDIO_LAYOUT_STEREO,
    CVT_BGR24,
    CVT_BGR48,
    CVT_BGRA32,
    CVT_BGRA64,
    CVT_RGB24,
    CVT_RGB48,
    CVT_RGBA32,
    CVT_RGBA64,
    TOPOS_MATRIX_CODES,
    TOPOS_PRIMARIES_CODES,
    TOPOS_QP_MAX,
    TOPOS_TRANSFER_CODES,
    ToposAudioTrackConfig,
    ToposCodecError,
)
from .slice_threads_policy import SLICE_THREADS_POLICY
from .topos_rate_control import ToposRateFeedback
from .topos_profiles import (
    TOPOS_DEFAULT_ALPHA_BIT_DEPTH,
    TOPOS_DEFAULT_ALPHA_MODE,
    available_topos_tiers,
    default_topos_tier_id,
    get_topos_tier,
    get_topos_image_tier_or_none,
    get_topos_tier_or_none,
    topos_tier_numeric_id,
)

logger = logging.getLogger(__name__)

# H.273 对齐码表（bitstream_spec §A.7；与 topos_source 解码侧逆向映射同源）
_PRIMARIES_CODES = TOPOS_PRIMARIES_CODES
_TRANSFER_CODES = TOPOS_TRANSFER_CODES
_MATRIX_CODES = TOPOS_MATRIX_CODES

# 正向 RGB→YUV 权重（Kr, Kb）；U 分母 2(1−Kb)，V 分母 2(1−Kr)
_YUV_FORWARD = {
    'bt709': (0.2126, 0.0722),
    'smpte170m': (0.299, 0.114),
    'bt601': (0.299, 0.114),
    'bt470bg': (0.299, 0.114),
    'bt2020': (0.2627, 0.0593),
    'bt2020nc': (0.2627, 0.0593),
}

_SUBSAMPLING_W = 2  # 4:2:2 水平减半，色度全高
_DEFAULT_BIT_DEPTH = 10

# v1 码流接受的 planar 输入（R1：白名单取代 '422 in fmt' 宽松匹配；
# R4.1：v1.2 加入 12-bit 4:2:2；R4.2：v1.3 加入 4:4:4；R4.3：v1.4 加入 GBR；
# 批 4 阶段 2：加入 16-bit（内核加宽，与解析器 _parse_topos_pix_fmt 同步）
_PLANAR_FMT_3 = ('yuv422p10le', 'yuv422p12le', 'yuv422p16le',
                 'yuv444p10le', 'yuv444p12le', 'yuv444p16le',
                 'gbrp10le', 'gbrp12le', 'gbrp16le')
_PLANAR_FMT_4 = tuple(
    f'topos_yuva{cf}p{bd}a{d}'
    for cf in ('422', '444') for bd in (10, 12, 16) for d in (8, 10, 12, 16)
) + tuple(
    f'topos_gbrap{bd}a{d}'
    for bd in (10, 12, 16) for d in (8, 10, 12, 16)
)

# R4.2/R4.3：pix_fmt 色度结构 → pf 枚举（yuv422p → 0；yuv444p → 1（v1.3）；
# gbr[p/ap] → 2（v1.4：G,B,R 平面序 + matrix=0 identity 契约））
_CHROMA_FMT_RE = re.compile(r'yuva?(422|444)p')
_GBR_FMT_RE = re.compile(r'gbra?p\d')

# pix_fmt 尾部 alpha 位深（topos_yuva422p10a{8,10,12,16}）
_ALPHA_DEPTH_RE = re.compile(r'a(\d+)$')


# ---- 单一 pix_fmt 解析器（复验 P0-01：构造器与 planar 输入共用同一张表；
# 域外名字显式失败，不再默认 422/10 或换头静默编码） ----

def _parse_topos_pix_fmt(name: str) -> Optional[Tuple[str, int, int, Optional[int]]]:
    """Topos 编码域 pix_fmt → (kind, pf_enum, bit_depth, alpha_depth)。

    kind: 'planar' | 'packed-bgr' | 'packed-rgb'；pf_enum: 0=4:2:2、1=4:4:4、
    2=GBR（packed 无子采样概念，pf_enum 按通道序给 0/2 占位，仅供诊断）。
    未知格式返回 None——调用方必须显式拒绝（复验 P0-01 最小复现 11：
    'totally_invalid' 曾被静默猜成 422/10-bit）。
    """
    s = str(name or '').strip().lower()
    # 批 4：16-bit planar（视频/图片 16-bit 档，profile 3/5 合法；profile 6
    # 仍 12-bit-only 由 codec 白名单兜底）
    m = re.fullmatch(r'(yuv422|yuv444|gbr)p(10|12|16)le', s)
    if m:
        pf = {'yuv422': 0, 'yuv444': 1, 'gbr': 2}[m.group(1)]
        return ('planar', pf, int(m.group(2)), None)
    m = re.fullmatch(r'topos_yuva(422|444)p(10|12|16)a(8|10|12|16)', s)
    if m:
        pf = 0 if m.group(1) == '422' else 1
        return ('planar', pf, int(m.group(2)), int(m.group(3)))
    m = re.fullmatch(r'topos_gbrap(10|12|16)a(8|10|12|16)', s)
    if m:
        return ('planar', 2, int(m.group(1)), int(m.group(2)))
    # HALF 样本域（spec §15，2026-09-19）：gbrph16le / topos_gbraph16a16 ——
    # float 输入经冻结映射直通 half 码值（无 [0,1] 契约/OETF 假设）；
    # alpha 仅 a16（mode1 无损，alpha 平面同为 half 域）。'h' 标记样本域，
    # 与整数 gbrp16le 区分（构造器按名字二分 _half_float）。
    if s == 'gbrph16le':
        return ('planar', 2, 16, None)
    # M3-R1：TRAW CFA 输入域（4 相位平面 R/Gr/Gb/B，各 W/2×H/2；
    # image_profile 3 / codec profile 7——与 image_cli --tier raw* 同域）
    m = re.fullmatch(r'topos_cfa(12|16)le', s)
    if m:
        return ('planar', 3, int(m.group(1)), None)
    if s == 'topos_gbraph16a16':
        return ('planar', 2, 16, 16)
    if s in ('bgr24', 'bgr48le', 'bgr48be'):
        return ('packed-bgr', 0, 8 if s == 'bgr24' else 16, None)
    if s in ('bgra', 'bgra64le', 'bgra64be'):
        return ('packed-bgr', 0, 8 if s == 'bgra' else 16, None)
    if s in ('rgb24', 'rgb48le', 'rgb48be'):
        return ('packed-rgb', 2, 8 if s == 'rgb24' else 16, None)
    if s in ('rgba', 'rgba64le', 'rgba64be'):
        return ('packed-rgb', 2, 8 if s == 'rgba' else 16, None)
    return None

# R3：超硬上限交付策略（审计任务 3）
_BUDGET_POLICIES = ('record', 'error', 'continue')

#: v1.4 AAC priming（ADR-C052）：AAC-LC 流固有序曲延迟 = 1024 采样
#: （编码器首包自报 pts=-1024）。FFT 级校准实测（2026-09-20，PyAV 17.0.1
#: + ffmpeg 8.1）：无 elst 端到端延迟恰为 1024；elst media_time=1024 时
#: 解码残差精确为 0。本常量仅在编码运行时未捕获到 priming_enc 时的兜底；
#: 正常路径用当次编码实测值（编码器后端如 aac_at 自报 2112 则随流自适应）。
#: 校准单测以 FFT 互相关 |lag| ≤ 1 采样钉死，漂移先红。
TOPOS_AAC_E2E_PRIMING_SAMPLES = 1024

#: v1.5 tmcd 标称 fps 白名单（container_spec §A.5；C 层同规则）。
_TC_NOMINAL_FPS = (24, 25, 30, 48, 50, 60)


def _parse_start_timecode(
        tc_str: str, config: Any) -> Optional[Tuple[int, int, int, int, int, int]]:
    """sequence 起始时码字符串 → (hh, mm, ss, ff, fps, df)；不可写返回 None。

    `HH:MM:SS:FF` = NDF、`HH:MM:SS;FF` = DF；fps 取导出配置帧率四舍五入
    到标称白名单（23.976→24、29.97→30、59.94→60；偏差 >2% 视为不在域内）。
    分量校验交给 C 层（fail-fast 拒绝 → 调用方降级为不写）。
    """
    tc_str = tc_str.strip()
    if ';' in tc_str:
        df = 1
        prefix, ff_s = tc_str.rsplit(';', 1)
        parts = prefix.split(':')
    else:
        df = 0
        if ':' not in tc_str:
            return None
        parts = tc_str.split(':')
        ff_s = parts[-1]
        parts = parts[:-1]
    if len(parts) != 3:
        return None
    try:
        hh, mm, ss, ff = int(parts[0]), int(parts[1]), int(parts[2]), int(ff_s)
    except ValueError:
        return None
    fpsv = getattr(config, 'fps', None)
    num = int(getattr(fpsv, 'num', getattr(fpsv, 'numerator', 24)) or 24)
    den = int(getattr(fpsv, 'den', getattr(fpsv, 'denominator', 1)) or 1)
    if num <= 0 or den <= 0:
        return None
    nominal = int(round(num / den))
    if nominal not in _TC_NOMINAL_FPS:
        return None
    if abs((num / den) / nominal - 1.0) > 0.02:
        return None
    return hh, mm, ss, ff, nominal, df



#: R8/S-2：公开别名——deliver 侧（video_export_job）与外部工具经此入口
#: 取同一张解析表，不再跨模块引用私有名。
parse_topos_pix_fmt = _parse_topos_pix_fmt

def _map_color_codes(color_metadata: Any) -> Tuple[int, int, int, int]:
    """ColorMetadataConfig → (primaries, transfer, matrix, range) 码值。

    未知标签抛 ValueError（R-13 哲学：不许默认值掩盖）。
    """
    primaries = str(getattr(color_metadata, 'primaries', '') or '').lower()
    transfer = str(getattr(color_metadata, 'transfer', '') or '').lower()
    matrix = str(getattr(color_metadata, 'matrix', '') or '').lower()
    rng = str(getattr(color_metadata, 'range', '') or '').lower()

    if primaries not in _PRIMARIES_CODES:
        raise ValueError(
            f"Topos encoder: unsupported color primaries {primaries!r} "
            f"(supported: {sorted(_PRIMARIES_CODES)})"
        )
    if transfer not in _TRANSFER_CODES:
        raise ValueError(
            f"Topos encoder: unsupported color transfer {transfer!r} "
            f"(supported: {sorted(_TRANSFER_CODES)})"
        )
    if matrix not in _MATRIX_CODES:
        raise ValueError(
            f"Topos encoder: unsupported color matrix {matrix!r} "
            f"(supported: {sorted(_MATRIX_CODES)})"
        )
    # R1（审计 P1-4）：range 仅接受 limited/full；'' = 未指定（按 limited），
    # 其余未知值（含 from_metadata 产生的 'unknown'）显式失败——不许默认值掩盖
    if rng in ('', 'limited'):
        range_code = 0
    elif rng == 'full':
        range_code = 1
    else:
        raise ValueError(
            f"Topos encoder: unsupported color range {rng!r} "
            f"(supported: limited/full；空=未指定按 limited)"
        )
    return (
        _PRIMARIES_CODES[primaries],
        _TRANSFER_CODES[transfer],
        _MATRIX_CODES[matrix],
        range_code,
    )


def _rgb_to_gbrp(
    rgb: np.ndarray,
    bit_depth: int = _DEFAULT_BIT_DEPTH,
) -> Tuple[np.ndarray, np.ndarray, np.ndarray]:
    """float32 RGB [0,1] (H,W,3) → GBR planar 满刻度平面（R4.3）。

    - 平面序 = FFmpeg gbrp 约定（G, B, R），全幅全宽，值域 0..2^bd−1；
    - 无矩阵/无 range 折算（identity 语义），通道直通量化。
    """
    scale = float((1 << bit_depth) - 1)

    def _q(ch: np.ndarray) -> np.ndarray:
        return np.clip(np.rint(ch * scale), 0, scale).astype('<u2')

    return _q(rgb[..., 1]), _q(rgb[..., 2]), _q(rgb[..., 0])


def _rgb_to_gbrp_half(
    rgb: np.ndarray,
) -> Tuple[np.ndarray, np.ndarray, np.ndarray]:
    """float RGB（任意值域：负值/HDR>1.0 均合法）→ GBR half 码值平面。

    HALF 样本域（spec §15）：float32 先舍入到 float16 位模式，再经冻结
    单调映射转 u16 码值——无裁剪、无标定、无 OETF 假设（合成数据的
    线性域直通）。映射实现与 native tc_image_half_to_codes 逐位一致
    （topos_image_binding.half_bits_to_codes）。
    """
    return (_half_plane(rgb[..., 1]), _half_plane(rgb[..., 2]),
            _half_plane(rgb[..., 0]))


def _half_plane(ch: np.ndarray) -> np.ndarray:
    """float 通道 → half 码值平面（颜色/alpha 通用；spec §15 冻结映射）。

    非 float 输入显式拒绝（样本域契约——整数输入应走 gbrp16le）。
    """
    from .topos_image_binding import half_bits_to_codes

    arr = np.asarray(ch)
    if not np.issubdtype(arr.dtype, np.floating):
        raise RuntimeError(
            f"Topos encoder: HALF 样本域只接受 float 输入，得到 "
            f"{arr.dtype}——整数输入请用 gbrp16le"
        )
    f16 = np.ascontiguousarray(arr, dtype=np.float16)
    return half_bits_to_codes(f16.view('<u2'))


def _rgb_to_yuv_planar(
    rgb: np.ndarray,
    matrix_name: str,
    full_range: bool,
    bit_depth: int = _DEFAULT_BIT_DEPTH,
    chroma_full: bool = False,
) -> Tuple[np.ndarray, np.ndarray, np.ndarray]:
    """float32 RGB [0,1] (H,W,3) → N-bit 满刻度 YUV 平面（R4.1：10/12；
    R4.2：chroma_full=True 为 4:4:4 全宽色度，否则 4:2:2）。

    - limited 跨度按 2^(n-8) 精确位移（10-bit Y [64,940] / 12-bit Y [256,3760]），
      与 FFmpegVideoEncoder._convert_rgb_to_yuv_bt2020 同一套数学；
    - 4:2:2 色度 = 水平 box 平均（奇宽边缘补列），色度全高；
      4:4:4（R4.2）不做下采样。
    """
    kr, kb = _YUV_FORWARD[matrix_name]
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    y = kr * r + (1.0 - kr - kb) * g + kb * b
    u = (b - y) / (2.0 * (1.0 - kb))
    v = (r - y) / (2.0 * (1.0 - kr))

    scale = float((1 << bit_depth) - 1)
    if full_range:
        y_q = y * scale
        c_scale = scale
        c_off = 0.0
    else:
        shift = bit_depth - 8
        y_q = y * (219 << shift) + float(16 << shift)
        c_scale = float(224 << shift)
        c_off = float(16 << shift)
    u_q = (u + 0.5) * c_scale + c_off
    v_q = (v + 0.5) * c_scale + c_off

    y_p = np.clip(np.rint(y_q), 0, scale).astype('<u2')
    u_p = np.clip(np.rint(u_q), 0, scale).astype('<u2')
    v_p = np.clip(np.rint(v_q), 0, scale).astype('<u2')
    if chroma_full:
        return y_p, u_p, v_p
    return y_p, _box_subsample_h(u_p), _box_subsample_h(v_p)


def _box_subsample_h(plane: np.ndarray) -> np.ndarray:
    """4:2:2 水平 box 子采样：ceil(w/2) 输出，奇宽边缘补列。"""
    h, w = plane.shape
    cw = (w + 1) // 2
    if w < 2 * cw:
        plane = np.pad(plane, ((0, 0), (0, 2 * cw - w)), mode='edge')
    return np.rint(
        plane.reshape(h, cw, 2).mean(axis=2)
    ).astype(plane.dtype)


def _default_slice_rows(config: Any) -> int:
    """Choose the native slice-row fan-out for a TOPOS video config.

    The 4K 4:4:4/alpha path is entropy-bound and was measured on Windows
    with four block rows per worker about five percent faster than the
    historical eight-row setting.  Keep the conservative eight-row default
    for 4:2:2 and smaller frames: that is the broad, previously validated
    path and avoids changing unrelated exports.

    This is only a scheduling hint.  It does not change the bitstream,
    quality, pixel format, or rate-control parameters.
    """
    try:
        width = int(getattr(config, "width", 0) or 0)
        height = int(getattr(config, "height", 0) or 0)
    except (TypeError, ValueError):
        width = height = 0
    if width < 3840 or height < 2160:
        return 8

    profile = str(getattr(config, "profile", "") or "").strip().lower()
    pixel_format = str(getattr(config, "pix_fmt", "") or "").strip().lower()
    is_444_family = (
        profile in {"4444", "4444xq"}
        or "yuva444" in pixel_format
        or pixel_format.startswith("yuv444")
        or "gbrap" in pixel_format
    )
    return 4 if is_444_family else 8


class ToposVideoEncoder:
    """Topos Video Codec 编码器（duck-type FFmpegVideoEncoder）。"""

    #: 容器能力显式契约（导出管线据此跳过/接线音频渲染，P1-02）。
    #: v1.1 音频轨（container_spec_v1.3 附录 A / ADR-C051）：mov 容器档
    #: 携带单音轨；图片序列模式无音轨（add_audio_stream 显式拒绝）。
    supports_audio = True

    def __init__(self, output_path: str, config: Any,
                 slice_threads: Optional[int] = None,
                 slice_rows: Optional[int] = None,
                 container_mode: str = "mov",
                 start_timecode: str = "",
                 raw_meta_payload: Optional[bytes] = None) -> None:
        self._output_path = str(output_path)
        self._config = config
        self._container_mode = str(container_mode or "mov").strip().lower()
        if self._container_mode not in ("mov", "image_sequence"):
            raise ValueError(
                f"Topos encoder: unsupported container_mode={container_mode!r}"
            )
        # v1.5 tmcd 起始时码（plan §2.7；sequence format_settings 透传）。
        # 有则写、无则不写（零字节变化）；mov 容器 + 帧率在标称白名单内
        # 才落轨，解析/校验失败降级为不写（时码是元数据，不阻塞交付）。
        self._start_timecode = str(start_timecode or "").strip()
        # RC1：TRAW 开发元数据（trwm 原子；RAW→RAW 直出时透传源 payload）
        self._raw_meta_payload = (
            bytes(raw_meta_payload) if raw_meta_payload else None)
        self._image_sequence_paths: list[str] = []
        # C01：序列帧先落独立暂存目录，close 提交时才进输出目录——
        # 取消/失败只清暂存，绝不触碰用户已有文件。
        # N02：目录按实例唯一（mkdtemp），同目标并发导出互不覆盖暂存。
        self._image_sequence_tmp_dir: "str | None" = None
        # N01：回滚未完成时置位——唯一备份所在的暂存目录禁止清理，
        # 留待下次对同一目标导出时按事务日志自动恢复。
        self._image_sequence_keep_tmp = False
        self._is_open = False
        self._frame_count = 0
        self._last_pts = 0
        # GOP 分段并行状态（ADR-C050；ctor ip2 块按配置覆盖）
        self._gop_seg = False         # 分段并行开关（仅 ip2 + gop_len>0）
        self._gop_len = 0             # 段长（帧）；0 = 单 GOP 全片
        self._gop_workers = 2
        self._seg_buf: list = []      # 当前段缓冲（已准备 planes）
        self._seg_futs: list = []     # 在途段 future FIFO（有序回填）
        self._seg_next = 0            # 已派发段数
        self._pool = None             # 惰性 ThreadPoolExecutor
        # R6：codec 常驻线程池上限（进程级）。None = 不干预（库默认
        # min(4, cpu)）；open 时设置、close/abort 时恢复前一值。
        # 导出（前台重负载）建议 min(16, cpu)（= native TC_SLICE_MAX_THREADS
        # 上限；4K 实测 8→16 线程 +39%），后台任务建议 1。
        self._slice_threads = slice_threads
        # M10-3 phase1：带高（块行）。16 曾是默认（单帧并行任务 1080p
        # 15→27、4K 27→51）。T-P1-11 编码阶段矩阵（2026-09-18，
        # bench_out/ai_upgrade/T-P1-11_stage_matrix：4 档位 × 2K/4K ×
        # 线程 1/4/8/16 × rows 4/8/16/32，48 帧+8 warmup）定位 entropy
        # 为第一大阶段（worker CPU 52–57%），rows=8 在全线程轴占优且
        # 无质量代价：t16 下 2K standard 编码 121.8→170.3 fps（+40%）、
        # 4K 50.4→53.0（+5%）；同档输出解码 204.8→294.5 fps（+44%）；
        # bytes/frame +≤1%，roundtrip 平面均值 Δ=0。0 = 库默认 32；
        # 4K 4:4:4/alpha 在 Windows 实机复测后采用 rows=4；其他规格仍为
        # rows=8。显式 slice_rows 继续拥有最高优先级，None 表示自适应。
        self._slice_rows = (
            _default_slice_rows(config)
            if slice_rows is None else int(slice_rows)
        )
        self._threads_registered = False
        # 对标达芬奇路线 阶段 1：帧间反馈单遍码控（档位目标码率 → 长期均值
        # 贴合，单帧可浮动）。首帧 sized 搜索定标，此后单遍出流（~1.8x）。
        # P4（2026-09-21，画质对齐计划 §5 P4）：短窗 VBV + 切换检测 +
        # 借贷封顶（ToposRateFeedback）+ 复杂度前瞻重锚（_should_reanchor）。
        self._rc: Optional[ToposRateFeedback] = None
        self._cplx_prev: Optional[float] = None    # P4 上一帧复杂度估计
        self._last_reanchor: int = -16             # 冷却起始于"已过"
        # 复验 P1-17：进程级登记表键（id(self) 保证多实例互不踩踏）
        self._policy_key = f"topos-enc-{id(self):x}"
        # 转换/编码流水线（与 FFmpegVideoEncoder 同契约，2026-09-17 接入
        # 并实测后**默认停用**）：实现正确性已钉死（sync/pipe×{1,3} 输出
        # 逐字节一致），但 M1 1080P standard packed BGR48 实测流水线比同步
        # 慢 ~25%（61.9/65.9 vs 87.4 fps）——Topos 的 native 融合转换
        #（行分片）与编码共用进程级线程池，"重叠"只是同池工作交错 +
        # 线程交接/每帧新分配开销，不产生新容量（FFmpeg 路径的收益来自
        # swscale 与 libx264 分立线程池，前提在此不成立）。保持同步语义；
        # TOPOS_ENCODER_PIPELINE=1 显式启用（4K/分立资源形态待验证）。
        self._pipelined = (
            os.environ.get("TOPOS_ENCODER_PIPELINE") == "1"
            and bool(getattr(config, 'pipelined_conversion', False)))
        self._convert_workers = max(
            1, int(getattr(config, 'convert_workers', 1) or 1))
        self._convert_queue: Optional["queue.Queue"] = None
        self._converted_queue: Optional["queue.Queue"] = None
        self._convert_threads: list = []
        self._converted_map: Optional[dict] = None
        self._convert_cond: Optional["threading.Condition"] = None
        self._next_submit_seq = 0
        self._next_encode_seq = 0

        # 档位：profile 名（'proxy'/'lt'/'standard'/'hq'），缺省 Standard。
        tier = get_topos_tier_or_none(getattr(config, 'profile', None))
        if tier is None:
            tier_id = str(getattr(config, 'profile', '') or '').strip().lower()
            if tier_id and tier_id not in ('standard',):
                # 显式给了档位名但不认识——报错而不是静默用默认
                get_topos_tier(tier_id)  # 抛 KeyError 带全部已知档位
            tier = get_topos_tier(default_topos_tier_id())
        self._tier = tier

        # 固定 QP 覆盖（与 codec qp 语义一致，0–95；qp≥64 为 v1.5 域）
        crf = getattr(config, 'crf', None)
        self._qp_override: Optional[int] = None
        if crf is not None:
            crf_i = int(crf)
            if not 0 <= crf_i <= TOPOS_QP_MAX:
                raise ValueError(
                    f"Topos encoder: crf/qp {crf_i} 超出 codec 范围 0-{TOPOS_QP_MAX}"
                )
            self._qp_override = crf_i

        self._has_alpha = bool(getattr(config, 'has_alpha', False))
        # T1.5 起默认取 FFmpegEncoderConfig.entropy_mode（ADR-C038 起
        # 'rans2'）；'v1' 显式回退（reserved[0]=0，位流与历史逐字节一致）。
        # 取值域在构造期拒绝，不延后到编码期。
        # V 代际收纳（2026-09-13）：写面收缩为 {v1, v2, rans2}—— retired
        # 值显式 ValueError，禁止静默回落（退役 ≠ 回退，em 编号永久封存）。
        em = str(getattr(config, 'entropy_mode', 'rans2') or 'rans2').strip().lower()
        _retired_em = {
            'intra': 'V3 (cfg em=3)',
            'acpair': 'V4 (cfg em=4)',
            'acpair_table': 'V5 (cfg em=5)',
            'intra-range': 'V6 (cfg em=6)',
            'rans': 'V7-R (cfg em=7)',
        }
        if em in _retired_em:
            raise ValueError(
                f"Topos encoder: entropy_mode {em!r} 已退役（{_retired_em[em]}，"
                "V 代际收纳 2026-09-13）：请改用 'rans2'（产品默认）或 'v2'"
                "（RDO 依赖）；退役代际仅保留解码（写端拒绝），见"
                " docs/codec/topos_consolidation_audit_2026-09-13.md"
            )
        if em not in ('v1', 'v2', 'rans2'):
            raise ValueError(
                f"Topos encoder: entropy_mode {em!r} 不在 v1|v2|rans2"
            )
        self._entropy_v2 = (em == 'v2')
        # V7-R2（ADR-C036）：rANS + order-1 上下文扩展（lvl|位置/前 lvl 桶 +
        # dc|前块桶，per-slice 信令）——真实素材同画质较 rans 再省 1.2~2.8%
        # payload（qp20-84 全段），解码 ~1×。ADR-C038 起为产品默认
        self._rans2 = (em == 'rans2')
        # V2.x：逐带 AQ（色度专属）——默认关（切片粒度实测天花板 ≈422
        # 2.5% 码率，见 video_encoder.aq_mode 注释）；'on' 显式选用
        #（reserved[1]=1，位流零格式变更，解码侧无需感知）。
        aqm = str(getattr(config, 'aq_mode', 'off') or 'off').strip().lower()
        if aqm not in ('on', 'off'):
            raise ValueError(
                f"Topos encoder: aq_mode {aqm!r} 不在 on|off"
            )
        self._aq_on = (aqm == 'on')
        # V2.x：逐系数 level RDO 精修（须 V2 熵——码率模型基于 VLC
        # 符号位；V1 流自动不精修）。默认关；'on' 选用（reserved[2]=1，
        # 码流零格式变更）。同码率 422 +0.15 / 444 +0.4~0.6 dB luma。
        rdm = str(getattr(config, 'rdo_mode', 'off') or 'off').strip().lower()
        if rdm not in ('on', 'off'):
            raise ValueError(
                f"Topos encoder: rdo_mode {rdm!r} 不在 on|off"
            )
        if rdm == 'on' and not self._entropy_v2:
            raise ValueError(
                "Topos encoder: rdo_mode='on' 须 entropy_mode='v2'"
                "（RDO 码率模型基于 V2 VLC 符号位）"
            )
        self._rdo_on = (rdm == 'on')
        # V9（topos_v9_micro_gop_plan 批 5；ADR-C047）→ ADR-C048 载体更换：
        # gop='ip2' 写端切至 **V7-R3**（cfg em=11，major 7 em 8）——V7 band
        # 并行机器（编码实时友好）+ order-1 同 qp 体积优于 V8 机器。
        # V9（em=10）写端保留为实验路径（绑定 ToposGopContext 直用）。
        # 帧间反馈码控不适用（P/I 决策与残差合成在原生侧）。'intra'
        # （默认）保持现行全 I。IP-2 no-alpha（§3.7 显式收缩，禁静默降级）。
        gop = str(getattr(config, 'gop', 'intra') or 'intra').strip().lower()
        if gop not in ('intra', 'ip2'):
            raise ValueError(
                f"Topos encoder: gop {gop!r} 不在 intra|ip2"
            )
        self._gop = gop
        # v1.8（ADR-C056）：lp 档即帧间档——tier 决定载体（gop='ip2'），
        # 调用方无需重复声明；档位是产品语义，gop 只是实现开关（显式传
        # gop='intra' 与 lp 组合同样落 ip2，不是静默降级——lp 不存在
        # 帧内形态）。
        if self._tier.tier_id == 'lp':
            self._gop = 'ip2'
        # M4-R7（D3）：raw 视频档 = 帧内 RAW——与 lp 对偶的"档位决定载体"：
        # raw 不存在帧间形态，显式 gop='ip2' 与 raw 组合同样落 intra（档位
        # 是产品语义，不是静默降级）。CFA 域显式收缩（构造期给调用方可读
        # 错误，native 交叉规则兜底）：AQ/RDO/VLC(v2) 未在 CFA 域验收——
        # 载体走 V7-R2（rans2，bd16 宽域熵冻结）或 V1（bd12 显式回退）。
        if self._tier.tier_id == 'raw':
            self._gop = 'intra'
            if self._has_alpha:
                raise ValueError(
                    "Topos encoder: raw 档（TRAW CFA）与 alpha 互斥"
                    "（no-alpha capability，spec §3.2）")
            if self._aq_on:
                raise ValueError(
                    "Topos encoder: raw 档（TRAW CFA）不支持 aq_mode='on'"
                    "（AQ 是 V2 熵时代特性，CFA 域未验收）")
            if self._rdo_on:
                raise ValueError(
                    "Topos encoder: raw 档（TRAW CFA）不支持 rdo_mode='on'"
                    "（RDO 须 V2 熵；CFA 域走 V1/V7 载体）")
            if self._entropy_v2:
                raise ValueError(
                    "Topos encoder: raw 档（TRAW CFA）entropy_mode='v2'"
                    " 不受支持——CFA 域仅 V7-R2（rans2，默认）/V1（bd12）"
                    " 载体")
            if self._qp_override is None:
                raise ValueError(
                    "Topos encoder: raw 档 qp 由位深×比率锚表驱动（与图片"
                    " 线 12 档共享，如 raw12-4→qp59 / raw16-4→qp72）——"
                    "请以 crf 传入所选比率锚，不设默认（qp20 近无损语义"
                    " 不适用 RAW 比率标签）")
        if self._gop == 'ip2':
            if self._container_mode == 'image_sequence':
                raise ValueError(
                    "Topos encoder: 帧间档（gop='ip2'，含 lp）不支持图片序列"
                    "容器——P 帧包不可独立解码，违背 .toos 每帧自包含契约"
                )
            if self._has_alpha:
                raise ValueError(
                    "Topos encoder: gop='ip2'（V9 P0）不支持 alpha"
                    "（no-alpha capability，计划 §3.7）"
                )
            # V9 复审 2026-09-14：AQ/RDO 与 ip2 显式互斥（禁静默携带
            # reserved[1]/[2] 进 V9 写路径——m7 F-cache 写端不是 V8/V9
            # 机器；C 侧另有确定性降级闸，构造期先给调用方可读错误）。
            if self._aq_on:
                raise ValueError(
                    "Topos encoder: gop='ip2'（V9 P0 固定锚 qp）不支持"
                    " aq_mode='on'（AQ 是 V2 熵时代特性）"
                )
            if self._rdo_on:
                raise ValueError(
                    "Topos encoder: gop='ip2'（V9 P0 固定锚 qp）不支持"
                    " rdo_mode='on'（RDO 须 V2 熵）"
                )
            self._entropy_v2 = False
            self._rans2 = False
            # GOP 分段并行（ADR-C050，2026-09-14）：gop_size>0 → 按 gop_size
            # 帧分段，段间线程池并行（每段独立 GOP context，段首强制 I）——
            # 流格式零变更：mux 链校验天然支持多 GOP（每段 I 重开链，段
            # gop_id 恒 1，解码端 I 接受任意 id）。gop_size 未显式设置时
            # ≤2K 档默认开（fps×2 帧 ≈2s GOP；band 机器在 4K 已近饱和，
            # 分段收益低于在途帧内存 workers×gop_len×帧字节，默认关）；
            # =0 显式单 GOP（现行为，逐字节不变）。
            gs = getattr(config, 'gop_size', None)
            if gs is None:
                fpsv = getattr(config, 'fps', None)
                fnum = int(getattr(fpsv, 'num', getattr(fpsv, 'numerator', 24)) or 24)
                fden = int(getattr(fpsv, 'den', getattr(fpsv, 'denominator', 1)) or 1)
                two_sec = max(2, int(round(2.0 * fnum / max(1, fden))))
                area = int(self._config.width) * int(self._config.height)
                gs = two_sec if area <= 2560 * 1440 else 0
            gs = int(gs)
            if gs < 0:
                raise ValueError(
                    f"Topos encoder: gop_size {gs} 非法（0=单 GOP 全片 / "
                    "≥1=分段帧数）")
            self._gop_len = gs
            gw_raw = getattr(config, 'gop_workers', 2)
            gw = 2 if gw_raw is None else int(gw_raw)
            if gw < 1:
                raise ValueError(
                    f"Topos encoder: gop_workers {gw} 非法（≥1）")
            self._gop_workers = gw
            self._gop_seg = (self._gop_len > 0)
        # P1 预算接口（2026-09-21，画质对齐计划 §5 P1）：三级预算优先级
        # max_file_bytes > max_video_bytes > bitrate > 档位 target_bpp。
        # 预算换算逐帧目标 + 码控总预算模式（ToposRateFeedback 预算自适应）
        # + close() 硬上限裁决；要求码控路径（crf/lp/ip2 显式互斥）。
        self._max_video_bytes = getattr(config, 'max_video_bytes', None)
        self._max_file_bytes = getattr(config, 'max_file_bytes', None)
        self._budget_frames = getattr(config, 'budget_frames', None)
        self._bitrate_bps = getattr(config, 'bitrate', None)
        for _fname, _val in (('max_video_bytes', self._max_video_bytes),
                             ('max_file_bytes', self._max_file_bytes),
                             ('budget_frames', self._budget_frames)):
            if _val is not None and int(_val) <= 0:
                raise ValueError(
                    f"Topos encoder: {_fname} 必须为正整数（得到 {_val}）")
        _has_budget = (self._max_video_bytes is not None
                       or self._max_file_bytes is not None)
        if _has_budget and self._budget_frames is None:
            raise ValueError(
                "Topos encoder: max_video_bytes/max_file_bytes 须同时给出"
                " budget_frames（预算换算总帧数；FFmpegEncoderConfig 已校验，"
                "SimpleNamespace 直构路径由此兜底）")
        if self._bitrate_bps is not None and int(self._bitrate_bps) <= 0:
            raise ValueError(
                f"Topos encoder: bitrate 必须为正 bps（得到 {self._bitrate_bps}）")
        if (_has_budget or self._bitrate_bps is not None):
            if self._qp_override is not None:
                raise ValueError(
                    "Topos encoder: 预算/码率覆盖与 crf（固定 qp）互斥——"
                    "预算模式走 sized 搜索 + 反馈码控，固定 qp 无法兑现硬上限")
            if self._gop == 'ip2':
                raise ValueError(
                    "Topos encoder: 预算/码率覆盖与 gop='ip2'（固定锚 qp，"
                    "无码控）互斥（ADR-C047/C056 收缩域）")
            if self._container_mode == "image_sequence":
                raise ValueError(
                    "Topos encoder: 预算/码率覆盖不支持 image_sequence 容器"
                    "（N 个独立 .toos 文件无单一字节上限语义）")
        self._video_budget_resolved: Optional[int] = None  # 惰性（音轨声明先于首帧）
        # R1（审计 P0-3）：premultiplied 语义整段固定——显式配置，默认
        # straight（应用内合成约定）；与帧自带标志不一致时 encode_frame
        # 显式报错，不静默改写文件语义。
        self._alpha_premult = bool(getattr(config, 'alpha_premultiplied', False))
        # R1（审计 P1-1 同类）：alpha 位深从 pix_fmt 解析
        # （topos_yuva422p10a{8,10,12,16}），不得硬编码 12——a16 源必须
        # mode1 无损，a8 源不得被当 12-bit 压成近全透明。
        self._alpha_bit_depth = 0
        self._alpha_mode = 0
        if self._has_alpha:
            m = _ALPHA_DEPTH_RE.search(
                str(getattr(config, 'pix_fmt', '') or ''))
            depth = int(m.group(1)) if m else TOPOS_DEFAULT_ALPHA_BIT_DEPTH
            if depth not in (8, 10, 12, 16):
                raise ValueError(
                    f"Topos encoder: pix_fmt 声明的 alpha 位深 {depth} 不在 "
                    f"8/10/12/16（pix_fmt={getattr(config, 'pix_fmt', None)!r}）"
                )
            self._alpha_bit_depth = depth
            self._alpha_mode = 1 if depth == 16 else TOPOS_DEFAULT_ALPHA_MODE
        # spec §8.6：alpha 平面在码流中为 **16-bit 满刻度容器**，alpha_bit_depth
        # 只是顶层 N-bit 预量化精度——应用层平面语义是"声明位深满刻度"，
        # 编码时须左移 s=16−N（解码侧 topos_source 对应右移）。
        self._alpha_shift = 16 - self._alpha_bit_depth if self._has_alpha else 0
        # R4.1：颜色位深从 pix_fmt 主位深解析（yuv422p{10,12}le）；
        # 12-bit 走 v1.2 枚举（帧头 minor=1），qp 等效偏移由 native 承担。
        # 复验 P0-01：pix_fmt 必须经单一解析表显式匹配——域外名字
        # （如 'totally_invalid'）不再默认 422/10-bit，直接拒绝。
        pix_fmt_name = str(getattr(config, 'pix_fmt', '') or '')
        parsed_fmt = _parse_topos_pix_fmt(pix_fmt_name)
        if parsed_fmt is None:
            raise ValueError(
                f"Topos encoder: pix_fmt {pix_fmt_name!r} 不在编码域"
                f"（合法：yuv422/444/gbr p10/p12/p16le、topos_yuva*/topos_gbrap* "
                f"planar、bgr24/bgra/bgr48le/bgra64le 及 rgb 对偶 packed）——"
                f"域外格式显式拒绝，不猜测默认格式（复验 P0-01）"
            )
        self._stream_kind = parsed_fmt[0]
        # 流位深 = 码流枚举深度（10/12）；packed 输入的 8/16 位深是
        # **容器位深**（帧数据语义），流深度维持默认 10（与历史行为一致）
        self._stream_bit_depth = (
            parsed_fmt[2] if parsed_fmt[0] == 'planar' else _DEFAULT_BIT_DEPTH)
        if self._stream_bit_depth not in (10, 12, 16):
            raise ValueError(
                f"Topos encoder: pix_fmt 主位深 {self._stream_bit_depth} 不在 "
                f"10/12/16（v1.2/v1.7 枚举域；批 4 解锁 16-bit 视频/图片档）"
            )
        self._color_full_scale = (1 << self._stream_bit_depth) - 1
        # R4.2/R4.3：色度结构（yuv444p → pf=1（v1.3，minor=2）；gbrp/gbrap
        # → pf=2（v1.4，minor=3，G,B,R 平面序 + matrix=0））；pf≠0 几何/转换
        # 按全宽（无下采样）。packed 名字无子采样语义 → pf=0（BGR→YUV422）。
        self._stream_pixel_format = parsed_fmt[1] if parsed_fmt[0] == 'planar' else 0
        # R4.4：档位格式交叉（与 native frame_header 同规则前置拦截——
        # Pro444 须 4:4:4；Extreme 须 4:4:4 + 12-bit）
        native_profile = int(getattr(self._tier, 'native_profile', 3))
        if native_profile in (5, 6) and self._stream_pixel_format == 0:
            raise ValueError(
                f"Topos encoder: 档位 {self._tier.tier_id!r}（profile "
                f"{native_profile}）要求 4:4:4 像素格式——当前 pix_fmt "
                f"{pix_fmt_name!r} 为 4:2:2，请选择 yuv444p/gbrp 系输出"
            )
        if native_profile == 6 and self._stream_bit_depth != 12:
            raise ValueError(
                f"Topos encoder: 档位 '4444xq'（profile 6）要求 12-bit——"
                f"当前 pix_fmt {pix_fmt_name!r} 主位深 "
                f"{self._stream_bit_depth}，请选择 *p12le 输出"
            )
        if native_profile == 7:
            # M4-R7：TRAW 交叉规则（native 同规则前置拦截）——profile 7 ↔
            # pf=3 CFA 相位平面 + 12/16-bit；bd≥16 须宽域熵 rans2。
            if self._stream_pixel_format != 3:
                raise ValueError(
                    f"Topos encoder: raw 档（profile 7）要求 CFA 相位平面"
                    f" 格式 topos_cfa12le/topos_cfa16le——当前 pix_fmt "
                    f"{pix_fmt_name!r}（RAW→RAW 直出语义，拒绝普通 "
                    f"planar/packed 换头编码）"
                )
            if self._stream_bit_depth not in (12, 16):
                raise ValueError(
                    f"Topos encoder: raw 档（profile 7）位深域 12/16-bit——"
                    f"当前 {self._stream_bit_depth}-bit"
                )
            if self._stream_bit_depth >= 13 and not self._rans2:
                raise ValueError(
                    "Topos encoder: raw 档 16-bit（bd≥13）须宽域熵 "
                    "entropy_mode='rans2'（native 交叉规则：V1/V2 解码核心"
                    "是 12-bit 冻结验收域）"
                )
        self._native_profile = native_profile
        # HALF 样本域（spec §15，2026-09-19）：gbrph16le / topos_gbraph16a16。
        # 码流组合冻结为 pf=2 + bd=16 + codec profile 5（image_profile 4 的
        # IDSC 交叉规则）——档位必须 4444；alpha 只允许 mode1（a16 名字），
        # mode2 近似的 8/10/12 码值域语义对 half 域无定义。
        self._half_float = pix_fmt_name in ('gbrph16le', 'topos_gbraph16a16')
        if self._half_float:
            if native_profile != 5:
                raise ValueError(
                    f"Topos encoder: HALF 样本域（{pix_fmt_name!r}）要求 "
                    f"4444 档（codec profile 5）——当前档位 "
                    f"{self._tier.tier_id!r} 为 profile {native_profile}，"
                    f"请选择 4444 档导出"
                )
            if self._stream_pixel_format != 2 or self._stream_bit_depth != 16:
                raise ValueError(
                    f"Topos encoder: HALF 样本域要求 GBR 4:4:4 + 16-bit 容器"
                    f"（解析结果 pf={self._stream_pixel_format} "
                    f"bd={self._stream_bit_depth}）"
                )
            if self._has_alpha and self._alpha_mode != 1:
                raise ValueError(
                    "Topos encoder: HALF alpha 仅支持 mode1 无损（a16）——"
                    "请使用 topos_gbraph16a16 或关闭 Alpha 输出"
                )
        # R3：平面预量化深度（= 请求深度，固定）与码流深度（探测后可降档）
        # 分离——探测在"预量化@请求深度 + native 顶层再量化@候选深度"口径
        # 上测量，自适应后保持同口径（预量化不提前有损，最高精度进 native）。
        self._alpha_stream_depth = self._alpha_bit_depth
        # R3：位深来源二分——pix_fmt 显式 a{N} 后缀 = 格式契约（planar 直通/
        # 代理链，超预算不降档）；裸 pix_fmt + has_alpha = 质量旋钮（允许
        # mode2 首帧探测 12→10→8 自适应）。
        self._alpha_depth_explicit = self._has_alpha and bool(
            _ALPHA_DEPTH_RE.search(str(getattr(config, 'pix_fmt', '') or '')))
        # R3：超硬上限策略（record=记录交付[默认] / error=报错 / continue=授权）
        policy = str(getattr(config, 'alpha_budget_policy', 'record')
                     or 'record').strip().lower()
        if policy not in _BUDGET_POLICIES:
            raise ValueError(
                f"Topos encoder: alpha_budget_policy {policy!r} 不在 "
                f"{_BUDGET_POLICIES}（record=记录交付 / error=报错 / "
                f"continue=授权交付）"
            )
        self._budget_policy = policy
        self._budget = None                # open() 时初始化（has_alpha 才有）
        self._budget_overrun_warned = False
        self._audio_warned = False
        # v1.1 音频轨状态（add_audio_stream 声明 → 随 _create_mux 写入容器
        # movie config；write_audio_samples 在 close 前喂入 PCM/AAC 包）
        self._audio_declaration: "dict | None" = None
        # v1.6 追加音轨（M-B8 stems；声明序 = 容器轨 1..N，轨 0 = 主混音）
        self._stem_declarations: "list[dict]" = []
        self._aac_encoder: "object | None" = None
        self._float_range_warned = False   # R2：浮点信号域越界一次性警告
        self._opaque_alpha_warned = False
        # P1-12：native 融合输入转换（open() 探测符号；不可用回退 numpy 路径）
        self._cvt_available = False
        self._cvt_failed_warned = False
        self._cvt_bufs = None             # (key, planes) 逐帧复用，零分配稳态
        # P1-12：码值域全平面扫描的抽样计数（首帧 + 每 256 帧全检）
        self._frames_domain_checked = 0
        self._drop_alpha_warned = False
        self._codec = None
        self._mux = None
        self._mux_close_error: Optional[str] = None  # P1-03：abort 路径可查询
        self._frame_cfg = None
        self._timescale = 24000
        self._dur = 1000
        self._temp_path = self._output_path + '.topos-tmp'

    # —— 属性（对齐 FFmpegVideoEncoder） ——

    @property
    def output_path(self) -> str:
        return self._output_path

    @property
    def config(self) -> Any:
        return self._config

    @property
    def frame_count(self) -> int:
        return self._frame_count

    @property
    def is_open(self) -> bool:
        return self._is_open

    @property
    def tier_id(self) -> str:
        return self._tier.tier_id

    # —— 生命周期 ——

    def open(self) -> None:
        if self._is_open:
            return

        if not self._tier.available:
            raise RuntimeError(
                f"Topos encoder: 档位 {self._tier.label} 当前不可用——"
                f"{self._tier.unavailable_reason}（可用档位："
                f"{[t.label for t in available_topos_tiers()]}）"
            )

        try:
            from .topos_binding import (
                TOPOS_CODEC_ABI_VERSION,
                ToposCodec,
                ToposCodecError,
                ToposMuxFile,
            )
            import ctypes
        except ImportError as e:
            raise RuntimeError(
                f"Topos encoder: 编码绑定不可用（{e}）——"
                f"native 库未构建时拒绝导出，不回退其它 codec；"
                f"构建方式见 native/topos_codec/README.md"
            ) from e

        if not self._tier.available:  # pragma: no cover - 上面已拦截
            raise RuntimeError(self._tier.unavailable_reason)

        try:
            self._binding = {
                'ctypes': ctypes,
                'abi': TOPOS_CODEC_ABI_VERSION,
                'ToposCodecError': ToposCodecError,
                'ToposMuxFile': ToposMuxFile,
            }
            self._codec = ToposCodec()
            # P1-12：符号探测（旧 dylib 无 tc_convert_packed_rgb → 回退 numpy；
            # TOPOS_ENCODER_DISABLE_FUSED_CVT=1 逃逸舱口：强制 numpy 参考路径）
            self._cvt_available = (
                bool(getattr(self._codec, "has_cvt", False))
                and os.environ.get("TOPOS_ENCODER_DISABLE_FUSED_CVT") != "1"
            )
        except Exception as e:
            raise RuntimeError(
                f"Topos encoder: 加载 libtopos_codec 失败（{e}）——"
                f"不回退其它 codec；先运行 bash native/topos_codec/run_tests.sh"
            ) from e

        width = int(self._config.width)
        height = int(self._config.height)
        if width <= 0 or height <= 0:
            raise ValueError(f"Topos encoder: 非法分辨率 {width}x{height}")

        # fps → timescale/dur（码流 pts 单位；Rational 自动约分还原 fps）
        fps = getattr(self._config, 'fps', None)
        num = int(getattr(fps, 'num', getattr(fps, 'numerator', 24)) or 24)
        den = int(getattr(fps, 'den', getattr(fps, 'denominator', 1)) or 1)
        if num <= 0 or den <= 0:
            raise ValueError(f"Topos encoder: 非法帧率 {num}/{den}")
        self._timescale, self._dur = num, den

        # 色彩码（未知标签显式失败）
        color_metadata = getattr(self._config, 'color_metadata', None)
        if color_metadata is None:
            from .color_metadata_config import ColorMetadataConfig
            color_metadata = ColorMetadataConfig.sdr()
        try:
            primaries, transfer, matrix, range_code = _map_color_codes(color_metadata)
        except ValueError as e:
            if self._native_profile == 7:
                # M4-R7：TRAW 契约冻结全部色彩码（下方 profile-7 块：transfer
                # 冻结对 + matrix=0 + full + primaries 占位）——调用方色彩
                # 标签不进入码流，未知标签（RAW 源时间线常见）不阻断导出。
                primaries, transfer, matrix, range_code = 1, 1, 0, 1
            else:
                raise RuntimeError(str(e)) from e

        # v1.8：qp 三态——显式 qp 覆盖 > 档位固定锚（lp=72，ADR-C050）>
        # 帧内默认 20（码控档的 sized 搜索会覆盖 qp_base，此值仅是锚）。
        if self._qp_override is not None:
            qp = self._qp_override
        elif self._tier.anchor_qp is not None:
            qp = int(self._tier.anchor_qp)
        else:
            qp = 20
        # R3：预算状态（比例常量来自档位声明；累计在 _account_budget）
        self._budget = None
        if self._has_alpha:
            self._budget = {
                'target': float(self._tier.alpha_budget_ratio),
                'cap': float(self._tier.alpha_hard_cap),
                'total_alpha': 0, 'total_color': 0,
                'max_err': 0, 'frames': 0,
                'overrun': False, 'authorized': False, 'adapted': False,
            }
        self._cfg_fields = dict(qp=qp, range_code=range_code,
                                primaries=primaries, transfer=transfer,
                                matrix=matrix)
        if self._half_float:
            # HALF 样本域传递特性冻结 linear（spec §15）——half 值即样本
            # 本身（线性合成数据约定），色彩元数据的 transfer 不改写数值
            # 解释，但标签必须诚实：统一写 linear(8)，与 TRAW 16-bit 归档
            # 档（transfer 冻结对）同一模式。range 同冻结 full（native 对
            # HALF 无 range 约束；limited 标签会误导消费端做 16-235 缩放，
            # 2026-09-21 复查补）。
            self._cfg_fields['transfer'] = 8
            self._cfg_fields['range_code'] = 1
        if self._native_profile == 7:
            # M3-R1：TRAW 契约（spec §3.2 冻结对）——12-bit→LOG0(20)、
            # 16-bit→linear(8)；相位平面 matrix=0 identity、full range、
            # primaries 占位 1（真实色彩语义由应用层 RAW 参数/debayer 承
            # 载，D6：不在共享域私造 camera-native 枚举）。
            self._cfg_fields['transfer'] = (
                20 if self._stream_bit_depth == 12 else 8)
            self._cfg_fields['matrix'] = 0
            self._cfg_fields['range_code'] = 1
            self._cfg_fields['primaries'] = 1

        # R3：mode2 + 隐式位深（质量旋钮）→ 延迟建 mux：首帧预算探测
        # （12→10→8）定深后再建（mux/tpcC 容器级位深一致性要求文件级定深）。
        self._lazy_mux = (self._has_alpha and self._alpha_mode == 2
                          and not self._alpha_depth_explicit)
        try:
            movie_cfg, fc = self._build_configs(self._alpha_stream_depth)
            self._frame_cfg = fc
            self._movie_cfg = movie_cfg
            if not self._lazy_mux and self._container_mode == "mov":
                self._create_mux()
        except RuntimeError:
            raise
        except Exception as e:
            self._cleanup_temp()
            raise IOError(f"Topos encoder: 创建输出失败: {e}") from e

        self._is_open = True
        self._frame_count = 0
        self._last_pts = 0
        logger.info(
            "Topos encoder: open %s (tier=%s qp=%s sized=%s alpha=%s %dx%d@%d/%d)",
            self._output_path, self._tier.tier_id,
            self._qp_override if self._qp_override is not None else 'rate',
            self._tier.is_rate_controlled and self._qp_override is None,
            self._has_alpha, width, height, self._timescale, self._dur,
        )

    #: id=4「边缘均衡」禁用集（宽度 ≥ 3840 时）。2026-09-21 A/B 裁决
    #: （bench_out/edge_jag/variants/，487 帧全量 ×2K/4K）：
    #:   2K proxy/standard/hq 边缘 +0.66/+0.54/+0.29 dB，4K proxy/lt
    #:   +1.01/+0.78 dB——粗量化域全胜；4K standard −0.50 dB、4K hq
    #:   −0.20 dB（Y≈52–60 dB 近透明域，flat 的低频供给无盈余可换）。
    #:   netproxy 为最粗域（同源 qp 单调性），恒启用。
    _EDGE_BALANCED_4K_SUPPRESS = frozenset({"standard", "hq"})

    def _effective_qmatrix_id(self) -> int:
        """帧配置量化矩阵（P5.1 档位矩阵；图片档位导出在子类覆写）。

        id=4「边缘均衡」（2026-09-21 锯齿战役）合法域 = 帧头校验同款：
        YUV 4:2:2 + 10-bit + profile 3。档位声明仅描述主域：hq 等档可被
        用户切到 4:4:4/12-bit，细量化大画幅工作点（4K standard/hq）实测
        负收益——域外或禁用集命中时回退 flat(0)（位流最老字段值，任何
        解码器可读）。"""
        qm = int(self._tier.qmatrix_id)
        if qm == 4:
            if not (
                self._stream_pixel_format == 0
                and self._stream_bit_depth == 10
                and self._native_profile == 3
            ):
                return 0
            if (int(self._config.width) >= 3840
                    and self._tier.tier_id in self._EDGE_BALANCED_4K_SUPPRESS):
                return 0
        return qm

    # V7-R4/R5 输出特性位（reserved[5] → fh->flags bit2/bit3）
    _FEATURE_DEBLOCK = 1  # 输出去块滤波（P6；8×8 网格，qp 自适应强度）
    _FEATURE_QPT2 = 2     # 细化 qp 表（qp≥64 每 6 qp 翻倍；仅 4K standard/hq）

    def _output_features(self) -> int:
        """输出特性位掩码（锯齿战役 P6，2026-09-21）。

        DEBLOCK：全部 4:2:2 10-bit 视频档开启——滤波强度随 qp 自适应，高 qp 粗档
        （锯齿主诉区）强；**近透明档被 α floor=3 收到最小**（qp ≤ 68 时 α 恒 3、
        clip=1——强度推导只在 qp ≥ 70 生效，实测见
        docs/image/toos_deblock_verify_2026-09-22.md §1.1）。真实边缘由
        m1/m2 < beta 保护（deblock.c spec）。QPT2 细化表：仅 4K
        standard/hq（A/B 实测 +0.53 dB，2K/4K proxy 域负收益不启用；
        锯齿诊断 §5.1）。alpha / GBR / CFA / 12-bit 域外一律 0（native
        域校验兜底）。帧间档由调用方屏蔽（V9 载体不携带）。"""
        if not (self._stream_pixel_format == 0
                and self._stream_bit_depth == 10
                and self._native_profile == 3):
            return 0
        feats = self._FEATURE_DEBLOCK
        if self._tier.tier_id in ("standard", "hq") \
                and int(self._config.width) >= 3840:
            feats |= self._FEATURE_QPT2
        return feats

    def _effective_sar(self) -> tuple:
        """帧配置 SAR（默认 1:1；图片 RAW 档对齐 CLI = 未指定 0:0）。"""
        return (1, 1)

    def _build_configs(self, alpha_bit_depth: int) -> tuple:
        """按指定位深构建 (movie_config, frame_config)（色彩/qp 字段同源）。"""
        f = self._cfg_fields
        # R4.3：GBR（pf=2）契约——matrix=0 identity、无色度采样位置、
        # 满刻度直通（color_range 标 full）；YUV 输出沿用配置标签
        # M3-R1：pf=3（CFA 相位平面）与 GBR 同旁路——无 YUV 矩阵语义、
        # 满刻度直通（native validate 同规则）
        is_gbr = self._stream_pixel_format in (2, 3)
        matrix_code = 0 if is_gbr else f['matrix']
        range_code = 1 if is_gbr else f['range_code']
        movie_cfg = self._codec.movie_config(
            int(self._config.width), int(self._config.height),
            qp=f['qp'], qmatrix=self._effective_qmatrix_id(),  # P5.1 档位矩阵
            profile=self._native_profile,   # R4.4：tier → 帧头/tpcC profile
            alpha_mode=self._alpha_mode,
            alpha_bit_depth=alpha_bit_depth if self._has_alpha else 0,
            alpha_premultiplied=self._alpha_premult,
            color_range=range_code,
            color_primaries=f['primaries'],
            color_transfer=f['transfer'],
            color_matrix=matrix_code,
            timescale=self._timescale,
            bit_depth=self._stream_bit_depth,
            pixel_format=self._stream_pixel_format,
            **self._movie_config_audio_kwargs(),
        )
        ctypes = self._binding['ctypes']
        from .topos_binding import _CFrameConfig
        fc = _CFrameConfig()
        fc.struct_size = ctypes.sizeof(_CFrameConfig)
        fc.abi_version = self._binding['abi']
        fc.visible_width = int(self._config.width)
        fc.visible_height = int(self._config.height)
        fc.qp_base = f['qp']
        # P2 码率效率战役：444 profile 色度 qp 偏移（tier 声明；422 家族=0）
        fc.qp_delta_chroma = int(getattr(self._tier, 'chroma_qp_offset', 0) or 0)
        fc.qmatrix_id = self._effective_qmatrix_id()   # P5.1（ADR-C033）
        fc.profile = self._native_profile
        fc.pixel_format = self._stream_pixel_format  # R4.2/3：1=4:4:4，2=GBR
        fc.bit_depth = self._stream_bit_depth
        fc.alpha_mode = self._alpha_mode
        fc.alpha_bit_depth = alpha_bit_depth if self._has_alpha else 0
        fc.alpha_premultiplied = int(self._alpha_premult)
        fc.color_range = range_code
        fc.color_primaries = f['primaries']
        fc.color_transfer = f['transfer']
        fc.color_matrix = matrix_code
        fc.sar_num, fc.sar_den = self._effective_sar()
        fc.slice_rows = self._slice_rows
        # T1.5 转正 → V 代际收纳（2026-09-13）：写面三态 {rans2=8 默认,
        # v2=1（RDO 宿主）, v1=0（显式回退，位流与历史逐字节一致）}；
        # 退役 em 2..7 在构造期已拒绝（解码侧码流自描述，旧文件永远可读）。
        fc.reserved[0] = (11 if self._gop == 'ip2' else
                          (8 if self._rans2 else
                           (1 if self._entropy_v2 else 0)))
        # V2.x AQ：色度专属逐带偏移（编码器内部决策，qp_delta_biased 逐
        # slice 入流——位流零格式变更，任何历史解码器可读）
        fc.reserved[1] = 1 if self._aq_on else 0
        # V2.x RDO：逐系数 level 精修（纯编码端决策，合法 level 集即
        # 合法码流——零格式变更；仅 V2 熵生效）
        fc.reserved[2] = 1 if self._rdo_on else 0
        # V7-R4/R5 输出特性（锯齿战役 P6，2026-09-21）：仅 intra 产品路径
        # （reserved[0]=8，V7-R2）；位流侧 fh->flags bit2/bit3 选载，旧解码
        # 器对保留 flags 位非零干净拒绝。帧间 ip2（em=11）不携带。
        fc.reserved[5] = self._output_features() if fc.reserved[0] == 8 else 0
        return movie_cfg, fc

    def set_raw_meta(self, payload: bytes) -> None:
        """RC1：写入 TRAW 开发元数据（trwm 原子；open 后、close 前调用）。

        供 RAW→RAW 直出透传源 as-shot 基线；重复写入以最后一次为准。
        """
        if self._mux is None:
            raise RuntimeError(
                "Topos encoder: set_raw_meta 须在 open() 之后调用")
        self._mux.set_raw_meta(payload)

    def _create_mux(self) -> None:
        """以当前码流 alpha 位深创建 mux（临时文件此刻才落盘）。

        C09：GOP context 建 → mux 建 → meta/timecode/stems/trwm 声明，
        任一阶段失败统一关闭 mux/GOP、清 temp，原异常保留——open 失败后
        不残留半初始化句柄或 ``.topos-tmp``（清理幂等，可重复进入）。
        """
        movie_cfg, fc = self._build_configs(self._alpha_stream_depth)
        self._frame_cfg = fc
        self._movie_cfg = movie_cfg
        try:
            if self._gop == 'ip2':
                # V9：GOP context 持有序列状态 + 参考帧（I/P 决策、残差合成、
                # I 回退、事务提交全在原生侧；abort/close 即丢弃未提交事务）。
                # 分段并行模式（ADR-C050）：不建长驻 ctx——每段在 worker 内
                # 惰性建独立 ctx（段首强制 I 重开链，gop_id 恒 1）。
                if not self._gop_seg:
                    from .topos_binding import ToposGopContext
                    self._gop_ctx = ToposGopContext(self._codec, fc)
            parent = os.path.dirname(os.path.abspath(self._temp_path))
            os.makedirs(parent, exist_ok=True)
            self._mux = self._binding['ToposMuxFile'](
                self._codec, self._temp_path, movie_cfg)
            self._apply_movie_meta()
            if self._raw_meta_payload:
                # RC1：trwm 原子（RAW as-shot 基线随流走；声明在 tpcD 之后，
                # 旧 reader 按未知子原子跳过）
                self._mux.set_raw_meta(self._raw_meta_payload)
            self._apply_start_timecode()
            # v1.6：追加音轨（stems）声明随每次 mux 创建重放（探测/重建路径
            # 换新 mux 对象后追加轨必须重新声明）
            self._apply_stem_tracks()
        except BaseException:
            gop_ctx = getattr(self, '_gop_ctx', None)
            if gop_ctx is not None:
                try:
                    gop_ctx.close()
                except Exception as e:
                    logger.warning(
                        "Topos encoder: open 失败清理 GOP context 异常: %s", e)
                self._gop_ctx = None
            self._force_close_mux()
            self._cleanup_temp()
            raise

    def _apply_movie_meta(self) -> None:
        """v1.7 tpcD：档位 + 厂商标识落盘（探测侧恢复精确档位显示）。"""
        if self._mux is None:
            return
        self._mux.set_movie_meta(
            tier_id=topos_tier_numeric_id(self._tier.tier_id),
            vendor='TOPOS',
            label=self._tier.label,
        )

    def _apply_stem_tracks(self) -> None:
        """把已声明的 stems 轨写到当前 mux（v1.6；无声明 = 空操作）。"""
        if not self._stem_declarations or self._mux is None:
            return
        for d in self._stem_declarations:
            self._mux.add_audio_track(ToposAudioTrackConfig(
                codec=d['codec'],
                sample_rate=d['sample_rate'],
                channel_count=d['channels'],
                channel_layout=d['layout'],
                bits_per_sample=d['bits'],
                sample_format=d.get('sample_format', TC_AUDIO_FMT_INT),
                name=d['name'],
            ))

    def _parse_audio_stream_args(self, codec: str, sample_rate: int,
                                 channels: int) -> dict:
        """宿主音频 codec 名/采样率/声道 → 容器声明字段（fail-fast 校验）。

        add_audio_stream 与 add_audio_track_stream 共用；声道白名单含
        1（v1.6 mono stems 轨）。
        """
        codec_name = str(codec).strip().lower()
        container_codec = self._AUDIO_CODEC_MAP.get(codec_name)
        if container_codec is None:
            raise RuntimeError(
                f"Topos encoder: 音频 codec {codec!r} 不被 topos 容器支持"
                f"（可选：aac / pcm_s16le / pcm_s24le / pcm_s32le / pcm_f32le）")
        # v1.4：全家族采样率（44.1→192k，对齐 Apple 交付规范）
        if sample_rate not in (44100, 48000, 88200, 96000, 176400, 192000):
            raise RuntimeError(
                f"Topos encoder: 音频采样率 {sample_rate} 不支持"
                f"（44100/48000/88200/96000/176400/192000）")
        layout = self._AUDIO_LAYOUT_BY_CHANNELS.get(int(channels))
        if layout is None:
            raise RuntimeError(
                f"Topos encoder: 音频声道数 {channels} 不支持（1/2/6/8）")
        bits = 0
        sample_format = TC_AUDIO_FMT_INT
        if container_codec == TC_AUDIO_CODEC_LPCM:
            if codec_name in self._AUDIO_FLOAT:
                bits = 32
                sample_format = TC_AUDIO_FMT_FLOAT32
            else:
                bits = self._AUDIO_BITS.get(
                    int(codec_name.replace('pcm_s', '').replace('le', '')))
                if bits is None:
                    raise RuntimeError(
                        f"Topos encoder: PCM 位深无法从 {codec!r} 解析")
        return {
            'codec': container_codec,
            'sample_rate': int(sample_rate),
            'channels': int(channels),
            'layout': layout,
            'bits': bits,
            'sample_format': sample_format,
            'source_name': codec_name,
        }

    def _apply_start_timecode(self) -> None:
        """v1.5：sequence 起始时码 → tmcd 轨（mov 容器；失败降级不写）。"""
        if not self._start_timecode or self._container_mode != "mov" \
                or self._mux is None:
            return
        parsed = _parse_start_timecode(self._start_timecode, self._config)
        if parsed is None:
            logger.warning(
                "Topos encoder: 起始时码 %r 未写入（帧率不在标称白名单内"
                "或格式非法；导出不中断）", self._start_timecode)
            return
        hh, mm, ss, ff, fps, df = parsed
        try:
            self._mux.set_timecode(hh, mm, ss, ff, fps, int(df))
        except ToposCodecError as e:
            logger.warning(
                "Topos encoder: 起始时码 %r 被 C 层校验拒绝，不写 tmcd"
                "（%s）", self._start_timecode, e)

    def encode_frame(self, frame: Any) -> None:
        if not self._is_open:
            raise RuntimeError("Topos encoder not open. Call open() first.")

        try:
            if self._pipelined:
                # 流水线：帧提交转换队列后即返回；已完成帧在后续调用/
                # close drain 中保序编码（契约同 FFmpegVideoEncoder——
                # 转换错误延迟至 drain 抛出）
                self._pipeline_submit(frame)
                return
            planes = self._prepare_planes(frame)
            self._encode_prepared(planes)
        except Exception as e:
            logger.error("Topos encoder: encode error at frame %d: %s",
                         self._frame_count, e)
            raise RuntimeError(f"Topos encode error: {e}") from e

    def _encode_prepared(self, planes: list) -> None:
        """编码一帧已转换平面（只允许主调用线程执行，避免并发 mux）。"""
        try:
            if self._gop_seg and self._container_mode == "mov" \
                    and self._mux is not None:
                # GOP 分段并行（ADR-C050）：帧缓冲进当前段，段满派发；
                # 包在段完成时有序回写 mux（_seg_reap_one）。预算记账/
                # pts 推进都在主线程回收点，顺序与串行一致。
                self._seg_submit(planes)
                self._frame_count += 1
                if self._frame_count % 100 == 0:
                    logger.debug("Topos encoder: %d frames encoded",
                                 self._frame_count)
                return
            if self._container_mode == "image_sequence":
                if self._lazy_mux:
                    raise RuntimeError(
                        "Topos image sequence currently requires explicit alpha depth"
                    )
                pkt, stats = self._encode_packet(planes)
                self._account_budget(stats)
                self._write_image_packet(pkt)
                self._frame_count += 1
                if self._frame_count % 100 == 0:
                    logger.debug("Topos encoder: %d frames encoded", self._frame_count)
                return
            elif self._mux is None:
                # R3：首帧预算探测（mode2 自适应路径）→ 定深后建 mux
                pkt, stats = self._probe_budget_and_open_mux(planes)
            else:
                pkt, stats = self._encode_packet(planes)
            self._account_budget(stats)
            pts = self._last_pts
            self._mux.add_packet(pkt, pts=pts, dur=self._dur)
            self._last_pts += self._dur
            self._frame_count += 1
            if self._frame_count % 100 == 0:
                logger.debug("Topos encoder: %d frames encoded", self._frame_count)
        except Exception as e:
            logger.error("Topos encoder: encode error at frame %d: %s",
                         self._frame_count, e)
            raise RuntimeError(f"Topos encode error: {e}") from e

    # ---- 转换/编码流水线（契约与实现模式对齐 FFmpegVideoEncoder） ----

    def _topos_is_zero_conversion(self, frame: Any) -> bool:
        """planar 直通帧判定：pixel_format 与导出 pix_fmt 一致的 yuv 系
        planar 帧——转换层为校验+紧化拷贝（近零成本），走内联路径免
        两次线程交接 + 重排开销。任何不确定（字段缺失/packed）→ False。
        """
        if frame is None or getattr(frame, 'data', None) is not None:
            return False
        planes = getattr(frame, 'planes', None)
        if not planes:
            return False
        source_format = str(getattr(frame, 'pixel_format', '') or '').lower()
        target_format = str(getattr(self._config, 'pix_fmt', '') or '').lower()
        return bool(
            source_format
            and source_format == target_format
            and source_format.startswith('yuv')
        )

    def _pipeline_start_workers(self) -> None:
        """惰性启动转换线程（全直通帧时永不启动，保序天然成立）。"""
        if self._convert_threads:
            return
        self._convert_queue = queue.Queue(maxsize=self._convert_workers + 1)
        if self._convert_workers == 1:
            self._converted_queue = queue.Queue()
            target = self._pipeline_convert_worker
        else:
            self._converted_map = {}
            self._convert_cond = threading.Condition()
            target = self._pipeline_convert_worker_multi
        self._convert_threads = [
            threading.Thread(
                target=target,
                name=f"ToposVideoEncoder-Convert-{i}",
                daemon=True,
            )
            for i in range(self._convert_workers)
        ]
        for t in self._convert_threads:
            t.start()

    def _pipeline_submit(self, frame: Any) -> None:
        """提交一帧：直通帧内联；否则入转换队列并编码已就绪帧。"""
        if not self._convert_threads and self._topos_is_zero_conversion(frame):
            self._encode_prepared(self._prepare_planes(frame))
            return
        self._pipeline_start_workers()
        seq = self._next_submit_seq
        self._next_submit_seq += 1
        # put 满时阻塞 = 背压（in-flight 转换不超过 workers+1 帧）
        self._convert_queue.put((frame, seq))
        if self._convert_workers == 1:
            self._pipeline_drain_fifo(block=False)
        else:
            self._pipeline_encode_next(block=False)

    def _pipeline_convert_worker(self) -> None:
        """后台转换线程（单线程 FIFO 保序）：DecodedFrame → codec 平面。

        packed→planar 走 native 融合转换（释放 GIL），可与主线程的
        codec 调用（ctypes 释放 GIL）真并行。
        """
        while True:
            item = self._convert_queue.get()
            if item is None:
                return
            frame, seq = item
            try:
                entry = (seq, self._prepare_planes(frame), None)
            except BaseException as e:  # noqa: BLE001 转换失败必须传递
                entry = (seq, None, e)
            self._converted_queue.put(entry)

    def _pipeline_convert_worker_multi(self) -> None:
        """并行转换线程：完成序与提交序无关，主线程按 seq 重排后编码。"""
        while True:
            item = self._convert_queue.get()
            if item is None:
                return
            frame, seq = item
            try:
                entry = (self._prepare_planes(frame), None)
            except BaseException as e:  # noqa: BLE001
                entry = (None, e)
            with self._convert_cond:
                self._converted_map[seq] = entry
                self._convert_cond.notify_all()

    def _pipeline_drain_fifo(self, block: bool) -> None:
        """把转换完成的帧保序编码（按提交计数收尾，不依赖队列 EOF）。

        工人退出后 converted_queue 不再增长——block 模式下等待超时即
        转换丢失，显式报错而非静默丢帧。
        """
        while self._next_encode_seq < self._next_submit_seq:
            try:
                seq, planes, error = (
                    self._converted_queue.get(timeout=60) if block
                    else self._converted_queue.get_nowait())
            except queue.Empty:
                if block:
                    raise RuntimeError(
                        "Topos encoder: 帧转换超时/丢失 "
                        f"(seq {self._next_encode_seq}/"
                        f"{self._next_submit_seq})")
                return
            if error is not None:
                logger.error("Topos encoder: 帧转换失败: %s", error)
                raise RuntimeError(
                    f"Topos 帧转换错误: {error}") from error
            if seq != self._next_encode_seq:  # FIFO 语义防御断言
                raise RuntimeError(
                    f"Topos encoder: 转换序错乱（期望 "
                    f"{self._next_encode_seq}，得到 {seq}）")
            self._encode_prepared(planes)
            self._next_encode_seq += 1

    def _pipeline_encode_next(self, block: bool) -> None:
        """按 seq 序编码下一张已转换完成的帧（多转换线程重排语义）。"""
        with self._convert_cond:
            ready = self._convert_cond.wait_for(
                lambda: self._next_encode_seq in self._converted_map,
                timeout=60.0 if block else 0.0)
            if not ready:
                if block:
                    raise RuntimeError(
                        "Topos encoder: 帧转换超时 "
                        f"(seq {self._next_encode_seq})")
                return
            entry = self._converted_map.pop(self._next_encode_seq)
        planes, error = entry
        if error is not None:
            logger.error("Topos encoder: 帧转换失败: %s", error)
            raise RuntimeError(f"Topos 帧转换错误: {error}") from error
        self._encode_prepared(planes)
        self._next_encode_seq += 1

    def _pipeline_drain(self) -> None:
        """close 路径：停工人 → 全量保序排空（错误在此抛出）。"""
        if not self._convert_threads:
            return
        # 阻塞 put 哨兵：有界队列满时等待工人消费——工人对 converted
        # 队列（无界）的 put 永不阻塞，无死锁环；put_nowait 会丢哨兵
        #（队列满瞬间），工人将永堵在 get 上
        for _ in self._convert_threads:
            self._convert_queue.put(None)
        for t in self._convert_threads:
            t.join(timeout=10.0)
        self._convert_threads = []
        if self._convert_workers == 1:
            self._pipeline_drain_fifo(block=True)
        else:
            while self._next_encode_seq < self._next_submit_seq:
                self._pipeline_encode_next(block=True)

    def _pipeline_discard(self) -> None:
        """abort 路径：尽力停工人、丢弃在途帧（无输出残留）。"""
        if self._convert_threads:
            for _ in self._convert_threads:
                try:
                    self._convert_queue.put_nowait(None)
                except queue.Full:
                    pass
            for t in self._convert_threads:
                t.join(timeout=2.0)
            self._convert_threads = []
        if self._convert_queue is not None:
            try:
                while True:
                    self._convert_queue.get_nowait()
            except queue.Empty:
                pass
        self._converted_queue = None
        self._converted_map = None
        self._convert_cond = None

    def close(self) -> None:
        """定稿：预算元数据 → finish → 原子替换。0 帧 / 失败 → 删临时文件并抛错。"""
        if not self._is_open:
            return
        self._restore_slice_threads()  # R6：先恢复进程级线程数（后续不再编码）
        try:
            # 流水线排空（先于 0 帧检查——在途帧此刻才真正编码/计数；
            # 放 try 内：转换错误走既有定稿失败清理路径，不泄漏临时文件）
            self._pipeline_drain()
            if self._frame_count == 0:
                raise RuntimeError(
                    "Topos encoder: 没有任何帧被编码——拒绝产出空文件"
                )
            if self._gop_seg and self._container_mode == "mov":
                # 分段并行：先有序回收全部在途段（mux 写完）再定稿
                self._seg_drain()
            if self._container_mode == "image_sequence":
                if self._frame_count == 0:
                    raise RuntimeError("Topos image sequence: no frames written")
                # C01：全部帧已落暂存目录，此处一次性提交到输出目录。
                # 提交失败先回滚（暂存内保留被替换原文件的硬链接备份），
                # 再走下方定稿失败清理路径。
                self._commit_image_sequence()
            else:
                # P1 预算硬裁决：视频流累计字节超上限 = 定稿失败（不静默
                # 交付超预算文件；qp 顶格仍超支时调用方须放宽预算）。
                rc = self._rc
                if rc is not None and rc.over_budget:
                    raise RuntimeError(
                        f"Topos encoder: 视频预算超限——累计 {rc.total} B > "
                        f"硬上限 {rc.budget} B（{self._frame_count} 帧，"
                        f"qp 已顶格 {rc.qp}；请提高 max_video_bytes/"
                        f"max_file_bytes 或降低分辨率/帧数）")
                self._write_budget_metadata()
                self._mux.finish()
        except Exception as e:
            self._seg_discard()
            if self._container_mode == "mov":
                self._force_close_mux()
            self._cleanup_temp()
            self._is_open = False
            raise RuntimeError(f"Topos encoder: 定稿失败（输出未生成）: {e}") from e
        finally:
            # C13：GOP context 释放覆盖 close() 全部退出路径（finish 失败/
            # mux close 失败/replace 失败/成功）——close 失败仅告警不掩盖
            # 主错误；置 None 后成功路径尾部与 abort 的重复 close 均为 no-op。
            gop_ctx = getattr(self, '_gop_ctx', None)
            if gop_ctx is not None:
                try:
                    gop_ctx.close()
                except Exception as e:
                    logger.warning(
                        "Topos encoder: close 释放 GOP context 异常: %s", e)
                self._gop_ctx = None
        # 复验 P1-03：mux close 失败 = 定稿不完整（fd/资源释放未确认），
        # 不得发布输出文件——删除临时文件并抛错，让调用方知道 close 失败。
        if self._container_mode == "mov":
            try:
                self._mux.close()
            except Exception as e:
                self._mux = None
                self._cleanup_temp()
                self._is_open = False
                raise RuntimeError(
                    f"Topos encoder: mux close 失败（输出未发布，临时文件已删除；"
                    f"native 资源释放状态未确认）: {e}"
                ) from e
            self._mux = None   # R1：定稿后不残留已关闭 mux 引用（abort 幂等依赖）
            try:
                os.replace(self._temp_path, self._output_path)
            except OSError as e:
                self._cleanup_temp()
                self._is_open = False
                raise RuntimeError(
                    f"Topos encoder: 原子替换失败（输出未生成）: {e}"
                ) from e
        # C13：GOP context 已在 finally 统一释放（上方），此处不再重复
        self._is_open = False
        # P4 遥测：QP 轨迹/切换帧/借贷摘要（码控路径）
        if self._rc is not None:
            t = self._rc.telemetry()
            logger.info(
                "Topos encoder: 码控遥测 qp∈[%s,%s] 切换帧=%s 借贷余=%dB "
                "最大帧=%dB（%d 帧）",
                t['qp_min'], t['qp_max'], t['cut_frames'], t['debt_bytes'],
                t['bytes_max_frame'], t['frames_recorded'])
        logger.info("Topos encoder: closed, %d frames → %s",
                    self._frame_count, self._output_path)

    def abort(self) -> None:
        """取消路径：丢弃未定稿数据并删除临时文件（无输出残留）。"""
        self._restore_slice_threads()  # R6
        self._pipeline_discard()       # 停转换工人、丢弃在途帧
        self._seg_discard()
        gop_ctx = getattr(self, '_gop_ctx', None)
        if gop_ctx is not None:
            gop_ctx.close()
            self._gop_ctx = None
        if self._mux is not None:
            self._force_close_mux()
        if self._container_mode == "image_sequence":
            # C01：帧只存在于暂存目录，整目录移除即可；输出目录（用户
            # 原有序列）从未被触碰，无需也不允许逐路径 unlink。
            self._image_sequence_paths.clear()
            self._remove_image_sequence_tmp_dir()
        self._cleanup_temp()
        self._is_open = False
        logger.info("Topos encoder: aborted (%d frames discarded, no output)",
                    self._frame_count)

    def _write_image_packet(self, packet: bytes) -> None:
        """Write one persistent-context packet as an independent ``.toos``.

        C01：帧先写入独立暂存目录（输出目录的同级、按实例唯一命名），
        close 时由 ``_commit_image_sequence`` 一次性提交——写帧/取消阶段
        输出目录不被创建也不被改写，用户已有序列逐字节安全。
        N01/N02：暂存目录创建即落事务日志（state=writing）；同目标的
        其它实例暂存目录绝不触碰，死亡实例的中断事务在创建时自动恢复。
        """
        if self._container_mode != "image_sequence":
            raise RuntimeError("_write_image_packet requires image_sequence mode")
        from .topos_image_binding import get_image_codec

        if self._image_sequence_tmp_dir is None:
            self._image_sequence_tmp_dir = self._create_image_sequence_staging()
        frame_no = self._frame_count + 1
        path = os.path.join(self._image_sequence_tmp_dir,
                            f"frame_{frame_no:04d}.toos")
        # 批 5（计划点名）：16→profile2 黑洞改显式报错 + TRAW 映射。
        # 映射表与 docs/image/capability_manifest.json profiles 一一对应；
        # 不可映射组合显式抛错（此前 bd16 会静默误标 profile2/XQ）。
        # 批 4 阶段 2（复查解锁）：TRAW 16-bit linear 归档（native
        # TC_IMG_PROFILE_RAW 已接受 pf3+bd16+profile7）。
        if self._half_float:
            # HALF 样本域（spec §15）：image_profile 4（GBR444 float16；
            # pf2/bd16/profile5 组合已由构造器校验冻结）
            image_profile = 4
        elif self._native_profile == 7:
            if self._stream_bit_depth not in (12, 16):
                raise RuntimeError(
                    f"TRAW image sequence requires 12/16-bit stream "
                    f"(got {self._stream_bit_depth}-bit)")
            image_profile = 3  # Image RAW（CFA/profile 7；12=LOG0/16=linear）
        elif self._native_profile == 3:
            if self._stream_bit_depth not in (10, 12):
                raise RuntimeError(
                    f"Image Preview（YUV 4:2:2 序列）仅 10/12-bit "
                    f"（got {self._stream_bit_depth}-bit；native IDSC 组合域）")
            image_profile = 0  # Image Preview（YUV 4:2:2）
        elif self._stream_bit_depth == 10:
            image_profile = 1  # Image HQ（GBR 4:4:4 10-bit）
        elif self._stream_bit_depth == 12:
            image_profile = 2  # Image XQ（GBR 4:4:4 12-bit）
        else:
            raise RuntimeError(
                f"no image_profile mapping for codec profile "
                f"{self._native_profile} at {self._stream_bit_depth}-bit")
        try:
            # v1.7 TMET：档位 + 厂商标识落盘。M2-F10：图片档位导出
            # （image tier 注册表路径）按 N-4/N-5/N-6 落 tier id + 展示名
            # （'toos High 10bit' / 'toos Ultra 16bit float' /
            # 'toos RAW 12bit 4:1'，444 家族不入阶梯仅 vendor）；旧视频档
            # 路径（image_profile 0/1/2 无 image tier）保持视频档位体系。
            from .topos_image_binding import (
                TC_IMG_CHUNK_ICCP,
            )
            from .topos_meta import (
                TOPOS_IMAGE_META_CHUNK_TYPE,
                encode_topos_meta,
            )
            if self._image_tier is not None:
                meta_payload = encode_topos_meta(
                    tier_id=int(self._image_tier.tm_tier_id),
                    vendor='TOPOS',
                    label=self._image_tier.file_label,
                )
            elif image_profile in (0, 1, 2):
                meta_payload = encode_topos_meta(
                    tier_id=topos_tier_numeric_id(self._tier.tier_id),
                    vendor='TOPOS',
                    label=self._tier.label,
                )
            else:
                meta_payload = encode_topos_meta(vendor='TOPOS')
            # M3-H2：源 ICC profile 随流（ICCP chunk + iccp_ref=1 权威位）。
            # 导入→.toos 再导出全程 profile 保留；CICP 数值码并存，矛盾由
            # native writer 按 spec §7 硬错。
            icc_bytes = getattr(self._config, 'icc_profile', None)
            extra = [(TOPOS_IMAGE_META_CHUNK_TYPE, meta_payload)]
            iccp_ref = False
            if icc_bytes:
                extra.append((TC_IMG_CHUNK_ICCP, bytes(icc_bytes)))
                iccp_ref = True
            # The sequence is a batch export: avoid an fsync for every frame;
            # atomic rename still prevents partial files from being published.
            get_image_codec().write_packet(
                path, packet, image_profile, durable=False,
                extra_chunks=extra, iccp_ref=iccp_ref,
            )
        except Exception:
            # A failed frame must not leave a partially published sequence.
            try:
                os.unlink(path)
            except OSError:
                pass
            raise
        self._image_sequence_paths.append(path)

    def _remove_image_sequence_tmp_dir(self) -> None:
        """幂等清空暂存目录——里面只有本次导出的产物（含提交备份）。

        N01：回滚未完成（``_image_sequence_keep_tmp``）时目录里保有
        唯一旧帧备份，禁止清理——保留给恢复流程，直到确认全部还原。
        """
        tmp_dir = self._image_sequence_tmp_dir
        self._image_sequence_tmp_dir = None
        if tmp_dir is None:
            return
        if self._image_sequence_keep_tmp:
            logger.warning(
                "Topos encoder: 序列回滚未完成——唯一备份保留于 %s"
                "（下次对同一目标导出时自动尝试恢复）", tmp_dir)
            return
        shutil.rmtree(tmp_dir, ignore_errors=True)

    # —— N01/N02：序列暂存目录 + 提交事务（journal） ————————

    #: 暂存目录名标记（<output><mark>.<mkdtemp 随机>，同级、同文件系统）。
    _SEQ_TMP_MARK = ".topos-seq-tmp"
    #: 暂存目录内提交备份子目录名。
    _SEQ_BACKUP_DIR = ".backup"
    #: 暂存目录内事务日志文件名（原子写：tmp + fsync + replace）。
    _SEQ_JOURNAL_NAME = ".commit-journal.json"

    def _image_sequence_output(self) -> str:
        return self._output_path.rstrip(os.sep)

    def _create_image_sequence_staging(self) -> str:
        """创建按实例唯一的暂存目录，并先恢复同目标的中断事务。

        N02：mkdtemp 保证并发导出不共享目录——一方取消只删自己的目录，
        另一方的帧不受影响。N01：建目录即写 journal(state=writing)，使
        得进程中断后恢复流程能区分"活实例"与"可恢复的死实例"。
        """
        output = self._image_sequence_output()
        parent = os.path.dirname(output) or "."
        base = os.path.basename(output)
        self._recover_stale_image_staging(parent, base)
        tmp_dir = tempfile.mkdtemp(prefix=f"{base}{self._SEQ_TMP_MARK}.",
                                   dir=parent)
        self._write_sequence_journal(tmp_dir, state="writing")
        return tmp_dir

    def _sequence_journal_path(self, tmp_dir: str) -> str:
        return os.path.join(tmp_dir, self._SEQ_JOURNAL_NAME)

    def _write_sequence_journal(self, tmp_dir: str, **fields: Any) -> None:
        """原子写事务日志（tmp + fsync + replace）——中断后可据此恢复。"""
        journal: Dict[str, Any] = {
            "version": 1,
            "output": self._image_sequence_output(),
            "pid": os.getpid(),
            "time": time.time(),
        }
        journal.update(fields)
        path = self._sequence_journal_path(tmp_dir)
        tmp = path + ".tmp"
        try:
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump(journal, f)
                f.flush()
                os.fsync(f.fileno())
            os.replace(tmp, path)
        except OSError as e:  # 日志属恢复辅助，失败不阻断主流程
            logger.warning("Topos encoder: 写序列事务日志失败: %s", e)
            try:
                os.unlink(tmp)
            except OSError:
                pass

    @staticmethod
    def _read_sequence_journal(tmp_dir: str) -> Optional[Dict[str, Any]]:
        path = os.path.join(tmp_dir, ToposVideoEncoder._SEQ_JOURNAL_NAME)
        try:
            with open(path, "r", encoding="utf-8") as f:
                data = json.load(f)
            return data if isinstance(data, dict) else None
        except (OSError, ValueError):
            return None

    @staticmethod
    def _staging_owner_alive(journal: Optional[Dict[str, Any]],
                             tmp_dir: str) -> bool:
        """journal 记录的宿主是否仍在使用该暂存目录（无 journal 视为死亡）。

        同进程：state=writing（仍在写帧，含其它线程/实例）或提交正在
        执行（登记表命中）才算活——回滚未完成/异常中断的残留允许被
        同进程的下一次导出立即恢复，不必等进程退出。
        """
        if not journal:
            return False
        pid = journal.get("pid")
        if not isinstance(pid, int) or pid <= 0:
            return False
        if pid == os.getpid():
            if journal.get("state") == "writing":
                return True
            with _SEQ_COMMIT_IN_FLIGHT_LOCK:
                return tmp_dir in _SEQ_COMMIT_IN_FLIGHT
        try:
            os.kill(pid, 0)
            return True
        except ProcessLookupError:
            return False
        except OSError:  # EPERM 等：进程存在但属他人
            return True

    def _recover_stale_image_staging(self, parent: str, base: str) -> None:
        """恢复同目标的陈旧暂存目录（宿主进程已死的中断事务）。

        - writing / 无日志：帧从未发布 → 直接清理；
        - prepared / rollback_incomplete：按日志还原备份、摘除新增帧，
          输出序列收敛回提交前状态；还原失败保留目录待人工/下次处理；
        - committed / rolled_back：输出已是终态 → 清理即可。

        输出目录不存在时无"混合版本"可收敛，直接清理（旧序列已被用户
        删除，备份无从谈起）。
        """
        prefix = f"{base}{self._SEQ_TMP_MARK}."
        try:
            names = sorted(os.listdir(parent))
        except OSError:
            return
        for name in names:
            if not name.startswith(prefix):
                continue
            stale = os.path.join(parent, name)
            if not os.path.isdir(stale):
                continue
            journal = self._read_sequence_journal(stale)
            if self._staging_owner_alive(journal, stale):
                continue
            self._recover_one_stale_staging(stale, journal)

    def _recover_one_stale_staging(
            self, stale: str, journal: Optional[Dict[str, Any]]) -> None:
        state = (journal or {}).get("state")
        output = (journal or {}).get("output")
        frames = (journal or {}).get("frames") or []
        if state not in ("prepared", "rollback_incomplete") \
                or not isinstance(output, str) \
                or not os.path.isdir(output):
            shutil.rmtree(stale, ignore_errors=True)
            return
        backup_dir = os.path.join(stale, self._SEQ_BACKUP_DIR)
        unrestored: List[str] = []
        for entry in frames:
            if not isinstance(entry, dict):
                continue
            name = entry.get("name")
            if not isinstance(name, str):
                continue
            dst = os.path.join(output, name)
            if entry.get("existed"):
                bak = os.path.join(backup_dir, name)
                if os.path.lexists(bak):
                    try:
                        os.replace(bak, dst)
                    except OSError:
                        unrestored.append(name)
                elif os.path.lexists(dst):
                    # 备份已被先前一次部分恢复消耗，dst 即旧内容
                    pass
                else:
                    unrestored.append(name)
            else:
                try:
                    os.unlink(dst)
                except OSError:
                    pass
        if unrestored:
            logger.error(
                "Topos encoder: 中断事务自动恢复未完成——帧 %s 的旧内容"
                "仍在备份 %s，目录保留待下次恢复", unrestored, backup_dir)
            return
        if journal.get("created_output"):
            # 与 _rollback_image_sequence 对齐：本次新建的输出目录在恢复
            # 后为空则移除，不留伪成功空目录。
            try:
                if not any(os.scandir(output)):
                    os.rmdir(output)
            except OSError:
                pass
        shutil.rmtree(stale, ignore_errors=True)
        logger.info(
            "Topos encoder: 已恢复同目标中断事务（输出收敛回提交前状态，"
            "暂存目录 %s 已清理）", stale)

    @contextlib.contextmanager
    def _sequence_commit_lock(self, output: str) -> Iterator[None]:
        """同目标提交互斥：输出目录 fd 上的 flock（不产生额外文件）。

        只在提交期间持有——写帧/取消阶段互不干扰（N02 完成标准：两个
        实例同目标交错写/取消/定稿互不删除对方数据）。flock 不可用的
        平台降级为无锁（帧级 os.replace 本身原子，串行化是集合边界）。
        """
        os.makedirs(output, exist_ok=True)
        fd: Optional[int] = None
        locked = False
        if _HAS_FCNTL:
            try:
                fd = os.open(output, os.O_RDONLY)
                fcntl.flock(fd, fcntl.LOCK_EX)
                locked = True
            except OSError as e:
                logger.warning(
                    "Topos encoder: 序列提交锁不可用，降级为无锁提交: %s", e)
        try:
            yield
        finally:
            if locked and fd is not None:
                try:
                    fcntl.flock(fd, fcntl.LOCK_UN)
                except OSError:
                    pass
            if fd is not None:
                try:
                    os.close(fd)
                except OSError:
                    pass

    @staticmethod
    def _fsync_best_effort(path: str) -> None:
        try:
            fd = os.open(path, os.O_RDONLY)
            try:
                os.fsync(fd)
            finally:
                os.close(fd)
        except OSError:
            pass

    def _commit_image_sequence(self) -> None:
        """把暂存帧提交进输出目录；覆盖策略 = 同名替换、其余不动。

        N01 两阶段事务（journal 驱动，进程中断后可自动恢复）：

        1. 备份：将被替换的每个输出文件建同 inode 硬链接备份（不可用
           时内容级 copy）；日志落 ``prepared``（含逐帧 existed 与
           created_output）——此后任意中断，下次导出按日志整体回滚。
        2. 发布：逐帧 ``os.replace``（单帧原子；集合边界由提交锁串行
           化同目标并发提交）。全部完成后日志落 ``committed``。

        失败路径：先回滚（还原备份、摘除新增帧、本次新建的输出目录
        回滚后为空则一并移除，不留伪成功空目录）。回滚未完成时保留
        唯一备份目录并抛出带恢复位置的错误；成功回滚才清暂存目录。

        读者可见边界：提交是逐帧发布，中断窗口内目录可能新旧混合；
        journal 保证下次导出自动收敛回旧版本——读者不应把窗口内的
        混合集当作完整序列。
        """
        tmp_dir = self._image_sequence_tmp_dir
        if tmp_dir is None or not self._image_sequence_paths:
            return
        output = self._image_sequence_output()
        existed_before = os.path.lexists(output)
        backup_dir = os.path.join(tmp_dir, self._SEQ_BACKUP_DIR)
        frames: List[Dict[str, Any]] = []
        with _SEQ_COMMIT_IN_FLIGHT_LOCK:
            _SEQ_COMMIT_IN_FLIGHT.add(tmp_dir)
        try:
            with self._sequence_commit_lock(output):
                os.makedirs(backup_dir, exist_ok=True)
                # 1) 备份 + 记账
                for src in self._image_sequence_paths:
                    name = os.path.basename(src)
                    dst = os.path.join(output, name)
                    existed = os.path.lexists(dst)
                    if existed:
                        bak = os.path.join(backup_dir, name)
                        if os.path.lexists(bak):
                            os.unlink(bak)
                        try:
                            os.link(dst, bak, follow_symlinks=False)
                        except OSError:
                            # 硬链接不可用（exotic fs）→ 内容级备份保字节
                            shutil.copy2(dst, bak, follow_symlinks=False)
                    frames.append({"name": name, "existed": existed})
                # 2) prepared：此后中断可被恢复流程整体回滚
                self._write_sequence_journal(
                    tmp_dir, state="prepared", frames=frames,
                    created_output=not existed_before)
                # 3) 暂存帧落盘（发布前持久化，journal 才有意义）
                for src in self._image_sequence_paths:
                    self._fsync_best_effort(src)
                # 4) 逐帧发布
                for src in self._image_sequence_paths:
                    name = os.path.basename(src)
                    os.replace(src, os.path.join(output, name))
                # 5) committed：输出为终态，备份可弃
                self._write_sequence_journal(tmp_dir, state="committed")
            self._fsync_best_effort(output)
            # 成功：备份随暂存目录一并丢弃
            self._remove_image_sequence_tmp_dir()
        except BaseException as e:
            unrestored = self._rollback_image_sequence(
                tmp_dir, output, frames, created_output=not existed_before)
            if unrestored:
                self._image_sequence_keep_tmp = True
                try:
                    self._write_sequence_journal(
                        tmp_dir, state="rollback_incomplete", frames=frames,
                        created_output=not existed_before)
                except Exception:
                    pass
                raise RuntimeError(
                    f"序列提交失败且回滚未完成：旧帧 {unrestored} 暂未能"
                    f"还原，唯一备份保留于 {backup_dir}（下次对同一目标"
                    f"导出时自动尝试恢复）") from e
            try:
                self._write_sequence_journal(
                    tmp_dir, state="rolled_back", frames=frames,
                    created_output=not existed_before)
            except Exception:
                pass
            raise
        finally:
            with _SEQ_COMMIT_IN_FLIGHT_LOCK:
                _SEQ_COMMIT_IN_FLIGHT.discard(tmp_dir)

    def _rollback_image_sequence(
            self, tmp_dir: str, output: str, frames: List[Dict[str, Any]],
            created_output: bool) -> List[str]:
        """回滚一次未完成的提交；返回未能还原旧内容的帧名列表。

        备份还原逐帧 best-effort：单帧失败不放弃其余帧。本次新建的
        输出目录在回滚后为空时移除——失败不留下伪成功的空输出目录。
        """
        backup_dir = os.path.join(tmp_dir, self._SEQ_BACKUP_DIR)
        unrestored: List[str] = []
        for entry in frames:
            name = entry.get("name", "")
            dst = os.path.join(output, name)
            if entry.get("existed"):
                bak = os.path.join(backup_dir, name)
                if os.path.lexists(bak):
                    try:
                        os.replace(bak, dst)
                    except OSError as err:
                        logger.error(
                            "Topos encoder: 提交回滚失败，备份保留于 %s"
                            "（%s）", bak, err)
                        unrestored.append(name)
                elif not os.path.lexists(dst):
                    unrestored.append(name)
            else:
                try:
                    os.unlink(dst)
                except OSError:
                    pass
        if created_output and not unrestored:
            try:
                if not any(os.scandir(output)):
                    os.rmdir(output)
            except OSError:
                pass
        return unrestored
    # —— 音频（v1.1 音频轨；container_spec_v1.1，plan M-A6）——

    #: 宿主音频 codec 名 → 容器音频格式（ToposAudioFormat 字段）。
    #: PCM 输入按小端惯例命名（'pcm_s16le'），容器内统一大端 'twos' 语义，
    #: 写入时由 write_audio_samples 转换。
    _AUDIO_CODEC_MAP = {
        'aac': TC_AUDIO_CODEC_MP4A,
        'pcm_s16': TC_AUDIO_CODEC_LPCM,
        'pcm_s16le': TC_AUDIO_CODEC_LPCM,
        'pcm_s24': TC_AUDIO_CODEC_LPCM,
        'pcm_s24le': TC_AUDIO_CODEC_LPCM,
        'pcm_s32': TC_AUDIO_CODEC_LPCM,    # v1.4：补齐悬空能力（容器可存
        'pcm_s32le': TC_AUDIO_CODEC_LPCM,  # 可读但此前无写入口）
        'pcm_f32': TC_AUDIO_CODEC_LPCM,    # v1.4：float32 母带直通
        'pcm_f32le': TC_AUDIO_CODEC_LPCM,  #（无 clip 无量化，NaN/Inf 拒绝）
    }
    _AUDIO_BITS = {16: 16, 24: 24, 32: 32}
    _AUDIO_FLOAT = {'pcm_f32', 'pcm_f32le'}
    _AUDIO_LAYOUT_BY_CHANNELS = {1: TC_AUDIO_LAYOUT_MONO,   # v1.6 mono stems
                                 2: TC_AUDIO_LAYOUT_STEREO,
                                 6: TC_AUDIO_LAYOUT_5_1,
                                 8: TC_AUDIO_LAYOUT_7_1}

    def _movie_config_audio_kwargs(self) -> dict:
        """音频声明 → movie_config kwargs（未声明 = 全缺省，行为不变）。"""
        d = self._audio_declaration
        if not d:
            return {}
        return {
            'audio_codec': d['codec'],
            'audio_sample_rate': d['sample_rate'],
            'audio_channels': d['channels'],
            'audio_layout': d['layout'],
            'audio_bits_per_sample': d['bits'],
            'audio_sample_format': d.get('sample_format', TC_AUDIO_FMT_INT),
        }

    def add_audio_stream(self, codec: str = "aac", sample_rate: int = 48000,
                         channels: int = 2, bitrate: Optional[int] = None) -> None:
        """声明音频轨（v1.1）。必须在首帧编码前调用——mux 尚未写帧时
        以带音频的 movie config 重建（临时文件此刻仍为空，重建零代价）。

        AAC 档的 AudioSpecificConfig 在首个包产出时提取，延迟到
        write_audio_samples 注入（容器 API 要求先于首个 add_audio）。
        """
        if self._container_mode != "mov":
            raise RuntimeError(
                "Topos encoder: image sequence 不支持音频轨")
        if self._gop_seg:
            # 分段并行（ADR-C050）每段独立 mux——音频注入待追加轨工具落地
            # （共存路线已定稿：ADR-C053，段外独立 pass + moov 重写追加）
            raise RuntimeError(
                "Topos encoder: 分段并行模式暂不支持音频轨"
                "（共存路线见 ADR-C053；当前请关闭 V9 分段或改用无音频导出）")
        # C12：生命周期检查前置——先拒绝再变更状态。历史先覆盖
        # _audio_declaration 再因已有视频帧抛错，随后 write_audio_samples
        # 按新格式写入旧 mux（字节数/格式错乱）。
        if self._is_open and self._frame_count > 0:
            raise RuntimeError(
                "Topos encoder: 音频声明必须在首帧编码前（add_audio_stream）")
        self._audio_declaration = dict(
            self._parse_audio_stream_args(codec, int(sample_rate), int(channels)),
            bitrate=int(bitrate) if bitrate else None,
        )
        if not self._is_open:
            return  # open 时 _create_mux 将带上音频声明
        if self._mux is not None:
            # open 已建无音频 mux：0 帧时重建（临时文件清空，零代价）
            self._force_close_mux()
            self._mux = None
        self._create_mux()
        if not self._audio_warned:
            self._audio_warned = True

    def add_audio_track_stream(self, name: str, codec: str = "pcm_s24le",
                               sample_rate: int = 48000, channels: int = 2,
                               bitrate: Optional[int] = None) -> int:
        """声明追加音轨（v1.6 M-B8 stems 角色轨）。返回追加轨索引（1 起，
        供 write_audio_track_samples 使用；0 恒为主混音轨）。

        约束与 add_audio_stream 同源：必须在首帧编码前；主混音轨
        （add_audio_stream）须先声明——轨 0 是旧 reader 的兼容视图，
        stems 语义为"主混音 + 每角色离散轨"。校验失败 fail-fast 抛出
        （不静默丢轨——广播交付少一轨属于静默数据丢失）。
        """
        if self._container_mode != "mov":
            raise RuntimeError(
                "Topos encoder: image sequence 不支持音频轨")
        if self._gop_seg:
            raise RuntimeError(
                "Topos encoder: 分段并行模式暂不支持音频轨"
                "（共存路线见 ADR-C053）")
        if not self._audio_declaration:
            raise RuntimeError(
                "Topos encoder: 追加音轨前须先 add_audio_stream 声明主混音轨"
                "（轨 0 = 兼容视图）")
        track_name = str(name or "").strip()
        if not track_name:
            raise RuntimeError("Topos encoder: 追加音轨名不能为空"
                               "（容器 ©nam 轨名承载角色标签）")
        if len(track_name.encode('utf-8')) > 63:
            raise RuntimeError(
                f"Topos encoder: 追加音轨名 {track_name!r} 超 63 字节"
                "（容器轨名上限）")
        d = self._parse_audio_stream_args(codec, int(sample_rate), int(channels))
        d['bitrate'] = int(bitrate) if bitrate else None
        d['name'] = track_name
        if self._is_open and self._frame_count > 0:
            raise RuntimeError(
                "Topos encoder: 追加音轨声明必须在首帧编码前")
        self._stem_declarations.append(d)
        if self._is_open and self._mux is not None:
            # mux 已建（0 帧）：直接追加声明；尚未建（懒建/探测前）时
            # 由 _create_mux 统一重放
            self._mux.add_audio_track(ToposAudioTrackConfig(
                codec=d['codec'],
                sample_rate=d['sample_rate'],
                channel_count=d['channels'],
                channel_layout=d['layout'],
                bits_per_sample=d['bits'],
                sample_format=d['sample_format'],
                name=d['name'],
            ))
        return len(self._stem_declarations)

    def _aac_encode(self, samples: np.ndarray, sample_rate: int,
                    bitrate: Optional[int], channels: int,
                    ) -> "tuple[list[bytes], bytes, int]":
        """(channels, N) float32 → (AAC 包序列, ASC, priming)（M-A3；
        v1.6 起按轨通用，轨 0/stems 共用）。"""
        try:
            import av
        except ImportError as e:
            raise RuntimeError(
                "Topos encoder: AAC 音频编码需要 PyAV（av）依赖——"
                "请安装或改用 PCM 音频") from e
        import numpy as _np

        enc = av.CodecContext.create('aac', 'w')
        enc.sample_rate = int(sample_rate)
        enc.layout = 'mono' if channels == 1 else (
            'stereo' if channels == 2 else (
                '5.1' if channels == 6 else '7.1'))
        enc.format = 'fltp'
        if bitrate:
            enc.bit_rate = int(bitrate) * 1000
        packets: "list[bytes]" = []
        asc = b""
        total = samples.shape[1]
        step = 1024
        # 顺序 pts 喂帧：编码器首包 pts = -priming_enc——流固有序曲延迟的
        # 运行时实测（编码器后端各异：native 1024 / aac_at 2112），直接作为
        # elst media_time（ADR-C052 校准结论：原始 e2e 延迟恰等于它）
        frame_pts = 0
        priming_enc = 0
        for off in range(0, total, step):
            block = _np.ascontiguousarray(samples[:, off:off + step],
                                          dtype=_np.float32)
            if block.shape[1] < step:
                pad = _np.zeros((block.shape[0], step - block.shape[1]),
                                dtype=_np.float32)
                block = _np.concatenate([block, pad], axis=1)
            frame = av.AudioFrame.from_ndarray(
                block, format='fltp', layout=enc.layout)
            frame.sample_rate = int(sample_rate)
            frame.pts = frame_pts
            frame_pts += step
            for pkt in enc.encode(frame):
                if priming_enc == 0 and pkt.pts is not None and int(pkt.pts) < 0:
                    priming_enc = -int(pkt.pts)
                if not asc:
                    ed = getattr(enc, 'extradata', None)
                    asc = bytes(ed) if ed else b'\x12\x10'
                packets.append(bytes(pkt))
        for pkt in enc.encode(None):
            # flush 期同样提取首包 pts：短音频（单帧）首包只在 flush 产出，
            # 漏掉会静默退到兜底常量（aac_at 后端 2112 ≠ native 1024，elst 写错）
            if priming_enc == 0 and pkt.pts is not None and int(pkt.pts) < 0:
                priming_enc = -int(pkt.pts)
            if not asc:
                ed = getattr(enc, 'extradata', None)
                asc = bytes(ed) if ed else b'\x12\x10'
            packets.append(bytes(pkt))
        if not packets or not asc:
            raise RuntimeError("Topos encoder: AAC 编码未产出包/ASC")
        if priming_enc == 0:
            priming_enc = TOPOS_AAC_E2E_PRIMING_SAMPLES
            logger.debug("Topos encoder: aac 首包 pts 未捕获，priming 用兜底常量 %d",
                         priming_enc)
        logger.debug("Topos encoder: aac priming=%d（写 elst media_time）",
                     priming_enc)
        return packets, asc, priming_enc

    def _mux_add_track_audio(self, track: int, data: bytes,
                             num_samples: int) -> None:
        """按轨注入 chunk：轨 0 走 v1.1 API，追加轨走 v1.6 API。"""
        if track == 0:
            self._mux.add_audio(data, num_samples)
        else:
            self._mux.add_audio_to(track, data, num_samples)

    def _mux_set_track_asc(self, track: int, asc: bytes) -> None:
        if track == 0:
            self._mux.set_audio_asc(asc)
        else:
            self._mux.set_audio_track_asc(track, asc)

    def _mux_set_track_priming(self, track: int, samples: int) -> None:
        if track == 0:
            self._mux.set_audio_priming(samples)
        else:
            self._mux.set_audio_track_priming(track, samples)

    def _write_track_pcm(self, track: int, d: dict, samples: np.ndarray,
                         sample_rate: int) -> None:
        """声明 d 的 PCM → 容器轨（mp4a 编码 / float32 直存 / lpcm 量化）。

        write_audio_samples（轨 0）与 write_audio_track_samples（stems）
        共用；语义与 M-A6/v1.4 逐字节一致。
        """
        import numpy as _np

        if self._mux is None:
            raise RuntimeError("Topos encoder: mux 未就绪，无法写入音频")
        arr = _np.asarray(samples)
        if arr.ndim != 2 or arr.shape[0] != d['channels']:
            raise RuntimeError(
                f"Topos encoder: 音频形状 {arr.shape} 与声明声道数 "
                f"{d['channels']} 不符")
        if int(sample_rate) != d['sample_rate']:
            raise RuntimeError(
                f"Topos encoder: 音频采样率 {sample_rate} 与声明 "
                f"{d['sample_rate']} 不符")
        if d['codec'] == TC_AUDIO_CODEC_MP4A:
            packets, asc, priming = self._aac_encode(
                arr, sample_rate, d.get('bitrate'), d['channels'])
            self._mux_set_track_asc(track, asc)
            for pkt in packets:
                self._mux_add_track_audio(track, pkt, 1024)
            # v1.4：全部包注入后声明 priming（C 校验要求 < 已写总采样数）；
            # elst 使解码侧裁剪流固有序曲，端到端零残差。lpcm 恒不调用。
            # 短音频例外：总采样 ≤ priming（如单帧导出只有 1 个 AAC 包）
            # 时 elst 语义不可表达（media_time 须 < 轨总采样数），跳过
            # priming 声明保住导出——极端短内容的同步偏差无实际意义。
            total_written = len(packets) * 1024
            if priming >= total_written:
                logger.warning(
                    "Topos encoder: AAC 轨 %d 总采样 %d ≤ priming %d，"
                    "跳过 elst（内容全部落在编码器延迟窗内）",
                    track, total_written, priming)
            else:
                self._mux_set_track_priming(track, priming)
            logger.info("Topos encoder: AAC 音频已注入（轨 %d，%d 包，"
                        "priming=%d%s）", track, len(packets), priming,
                        "（跳过 elst）" if priming >= total_written else "")
            return
        # v1.4 float32：母带浮点直通（无 clip 无量化，NaN/Inf fail-fast
        # 拒绝——导出失败而非写出损坏文件）；'>f4' 大端直存
        if d.get('sample_format') == TC_AUDIO_FMT_FLOAT32:
            f = arr.T.astype(np.float32, copy=False)
            if not np.all(np.isfinite(f)):
                bad = int(np.size(f) - np.count_nonzero(np.isfinite(f)))
                raise RuntimeError(
                    f"Topos encoder: float32 音频（轨 {track}）含 {bad} 个"
                    " NaN/Inf 采样——拒绝写出损坏文件（母带浮点直通档）")
            be = f.astype('>f4')
            chunk = 1024
            n_total = be.shape[0]
            for off in range(0, n_total, chunk):
                blk = np.ascontiguousarray(be[off:off + chunk])
                self._mux_add_track_audio(track, blk.tobytes(),
                                          int(blk.shape[0]))
            logger.info("Topos encoder: float32 PCM 已直存（轨 %d，%d 采样帧，"
                        "零量化）", track, n_total)
            return
        # lpcm：float32 (ch, N) → int → 大端交错字节，按 1024 帧分块。
        # 24-bit 取大端 int32 的字节 1..3（[0, hi, mid, lo] → hi mid lo），
        # 必须先转 '>i4' 再切片——小端直切会得到 [mid, hi, 0] 的错序字节。
        bits = d['bits']
        x = _np.clip(arr.T, -1.0, 1.0)  # (N, ch)
        if bits == 16:
            q = _np.rint(x * 32767.0).astype('<i2')
            be = q.view('<u2').astype('>u2')  # 字节序翻转 → 'twos'
            raw = be.tobytes()
            frame_bytes = 2 * d['channels']
        elif bits == 24:  # int32 高位 3 字节有效（'twos' 大端）
            # float64 量化（float32 乘满刻度会有 ±1 LSB 舍入漂移）；
            # arr.T 是 F 连续，astype(order='K') 会保留——view 成 1 字节
            # 要求末轴连续，必须先 ascontiguousarray（否则 ValueError）
            q = _np.rint(x.astype(_np.float64) * 8388607.0)
            raw = _np.ascontiguousarray(q).astype('>i4').view(
                '>u1').reshape(-1, 4)[:, 1:4].tobytes()
            frame_bytes = 3 * d['channels']
        else:  # 32-bit 整数（v1.4 pcm_s32le：clip+rint→i32 全宽直存）
            # float64 必需：float32 下 2147483647.0 舍入到 2^31，cast 越界
            q = _np.rint(x.astype(_np.float64) * 2147483647.0)
            raw = q.astype('>i4').tobytes()
            frame_bytes = 4 * d['channels']
        chunk_frames = 1024
        bytes_per_chunk = chunk_frames * frame_bytes
        total_bytes = len(raw)
        written_frames = 0
        for off in range(0, total_bytes, bytes_per_chunk):
            chunk = raw[off:off + bytes_per_chunk]
            frames = len(chunk) // frame_bytes
            if frames == 0:
                break
            self._mux_add_track_audio(track, chunk, frames)
            written_frames += frames
        logger.info("Topos encoder: PCM 音频已注入（轨 %d，%d 采样帧 / "
                    "%d 声道 / %d-bit）", track, written_frames,
                    d['channels'], bits)

    def write_audio_samples(self, samples: np.ndarray, sample_rate: int = 48000) -> None:
        """写入音频母带 PCM（v1.1 轨 0；close/finish 前调用）。

        samples: (channels, total_samples) float32（_render_audio_master 口径）。
        lpcm：转 int 并按大端 'twos' 语义按 1024 采样帧分块注入；
        aac：PyAV 编码为包 + esds ASC 后逐包注入（native 只存包）。
        """
        d = self._audio_declaration
        if not d:
            return  # 未声明音频轨：与 FFmpeg 编码器静默跳过语义一致
        if samples is None or getattr(samples, 'size', 0) == 0:
            return
        self._write_track_pcm(0, d, samples, sample_rate)

    def write_audio_track_samples(self, track: int, samples: np.ndarray,
                                  sample_rate: int = 48000) -> None:
        """写入追加轨 PCM（v1.6 M-B8 stems；track = add_audio_track_stream
        返回索引，≥1）。格式/采样率与该轨声明不符 fail-fast。"""
        if track <= 0 or track > len(self._stem_declarations):
            raise RuntimeError(
                f"Topos encoder: 追加轨索引 {track} 越界"
                f"（已声明 {len(self._stem_declarations)} 条 stems 轨；"
                "轨 0 走 write_audio_samples）")
        if samples is None or getattr(samples, 'size', 0) == 0:
            return
        self._write_track_pcm(track, self._stem_declarations[track - 1],
                              samples, sample_rate)

    # —— 进度 ——

    def get_progress(self) -> float:
        return float(self._frame_count)

    def get_progress_percentage(self, total_frames: int) -> float:
        if total_frames <= 0:
            return 0.0
        return min(1.0, float(self._frame_count) / float(total_frames))

    # —— 内部 ——

    def _should_full_domain_check(self) -> bool:
        """P1-12：码值域全平面扫描调度——首帧全检、此后每 256 帧抽检；
        TOPOS_ENCODER_FULL_CHECK=1（调试/仲裁）强制逐帧全检。"""
        if os.environ.get('TOPOS_ENCODER_FULL_CHECK', '') == '1':
            return True
        due = self._frames_domain_checked % 256 == 0
        self._frames_domain_checked += 1
        return due

    def _apply_slice_threads_once(self) -> None:
        """首次编码前登记线程数偏好（复验 P1-17：协作式登记表取代
        嵌套保存/恢复——后者的交错序列会互相覆盖，恢复到非原始值）。
        P1-05：登记角色为 'export'——与并发播放解码共存时由策略记
        QoS warning（进程级单值在 V2.1 per-context 前无法分域）。"""
        if self._slice_threads is not None and not self._threads_registered:
            self._threads_registered = True
            SLICE_THREADS_POLICY.register(self._policy_key, self._slice_threads,
                                          role='export')

    def _restore_slice_threads(self) -> None:
        """close/abort：注销偏好（登记集变化时由策略重算生效值）。"""
        if self._threads_registered:
            self._threads_registered = False
            SLICE_THREADS_POLICY.unregister(self._policy_key)

    def _clone_frame_cfg(self, fc: Any) -> Any:
        """ctypes 帧配置深拷贝（memmove；守卫路径改 reserved 后重编用）。"""
        ctypes = self._binding['ctypes']
        from .topos_binding import _CFrameConfig
        f2 = _CFrameConfig()
        ctypes.memmove(ctypes.byref(f2), ctypes.byref(fc),
                       ctypes.sizeof(_CFrameConfig))
        return f2

    def _seg_submit(self, planes: list) -> None:
        """分段并行（ADR-C050）：帧进当前段缓冲，段满派发。

        在途段数 ≤ gop_workers（超界先有序回收最老段——同槽位串行 +
        在途帧内存上界 workers×gop_len×帧字节）；worker 线程做 ctypes
        原生调用（释放 GIL），与调用方的帧准备天然重叠。
        """
        self._apply_slice_threads_once()
        self._seg_buf.append(planes)
        if len(self._seg_buf) >= self._gop_len:
            self._seg_dispatch()

    def _seg_dispatch(self) -> None:
        if self._pool is None:
            from concurrent.futures import ThreadPoolExecutor
            self._pool = ThreadPoolExecutor(
                max_workers=self._gop_workers, thread_name_prefix='topos-gop')
        seg = self._seg_buf
        self._seg_buf = []
        self._seg_next += 1
        while len(self._seg_futs) >= self._gop_workers:
            self._seg_reap_one()
        self._seg_futs.append(self._pool.submit(self._seg_encode, seg))

    def _seg_encode(self, seg: list):
        """worker：独立 GOP context 编码一段（段首强制 I 重开链）。

        每段新 ctx → 段 gop_id 恒 1、包字节只依赖段内帧——worker 数与
        派发时序不改变任何字节（确定性，测试钉死 1 vs 4 worker 逐字节）。
        """
        from .topos_binding import ToposGopContext
        ctx = ToposGopContext(self._codec, self._frame_cfg)
        try:
            out = []
            for i, planes in enumerate(seg):
                pkt, st = ctx.encode_frame(planes, force_intra=(i == 0))
                out.append((pkt, st))
            return out
        finally:
            ctx.close()

    def _seg_reap_one(self) -> None:
        """有序回收最老段：mux/pts/预算记账都在主线程，顺序=段序=帧序。"""
        fut = self._seg_futs.pop(0)
        for pkt, st in fut.result():
            self._account_budget(st)
            self._mux.add_packet(pkt, pts=self._last_pts, dur=self._dur)
            self._last_pts += self._dur

    def _seg_drain(self) -> None:
        """close 路径：尾段（不足 gop_len）派发 + 全部回收 + 池关闭。"""
        if self._seg_buf:
            self._seg_dispatch()
        while self._seg_futs:
            self._seg_reap_one()
        self._seg_discard()

    def _seg_discard(self) -> None:
        """abort/失败路径：丢弃在途段与缓冲（无输出残留）。"""
        if self._pool is not None:
            self._pool.shutdown(wait=False)
            self._pool = None
        self._seg_futs.clear()
        self._seg_buf = []

    def _container_reserve_bytes(self) -> int:
        """MOV 容器开销预留（P0 语料实测 ≈29.6 B/帧 + 微量固定；取
        64 B/帧 + 4KB 裕量，追加 stems 轨每轨 1KB——预留宁可偏大，
        最终由 close() 硬裁决兜底）。"""
        return (4096 + 64 * int(self._budget_frames or 0)
                + 1024 * len(self._stem_declarations))

    def _audio_reserve_bytes(self) -> int:
        """音轨字节预留（按 budget_frames 换算时长；PCM 精确、AAC 估算）。"""
        d = self._audio_declaration
        if not d:
            return 0
        dur_s = int(self._budget_frames or 0) * self._dur / float(self._timescale)
        samples = int(math.ceil(d['sample_rate'] * dur_s))
        if d['codec'] == TC_AUDIO_CODEC_LPCM:
            # lpcm/float32：bits 字段即每采样字节数 ×8（16/24/32）
            return samples * d['channels'] * max(1, int(d['bits']) // 8)
        # AAC：声明码率（未声明按 192kbps 惯例）+ ASC/elst/对齐余量
        kbps = int(d.get('bitrate') or 192)
        return int(kbps * 1000 / 8.0 * dur_s) + 16384

    def _resolve_video_budget(self) -> Optional[int]:
        """三级预算 → 视频流总字节硬上限（None = 无预算，走档位/bitrate）。

        max_file_bytes 先扣音轨 + 容器预留；再与 max_video_bytes 取紧。
        惰性求值一次（首帧编码时音轨声明已齐——add_audio_stream 契约
        保证先于首帧）。
        """
        if self._video_budget_resolved is not None or (
                self._max_video_bytes is None and self._max_file_bytes is None):
            return self._video_budget_resolved
        budget: Optional[int] = None
        if self._max_file_bytes is not None:
            reserve = self._audio_reserve_bytes() + self._container_reserve_bytes()
            budget = int(self._max_file_bytes) - reserve
        if self._max_video_bytes is not None:
            budget = (int(self._max_video_bytes) if budget is None
                      else min(budget, int(self._max_video_bytes)))
        if budget is not None and budget <= 0:
            raise RuntimeError(
                f"Topos encoder: 文件预算不足——max_file_bytes="
                f"{self._max_file_bytes} 扣除音频 {self._audio_reserve_bytes()} B"
                f" + 容器预留 {self._container_reserve_bytes()} B 后无视频预算；"
                f"请提高上限或缩短内容（budget_frames="
                f"{self._budget_frames}）")
        self._video_budget_resolved = budget
        return budget

    def _frame_target_bytes(self) -> Optional[int]:
        """逐帧目标字节（P1 优先级：预算 > bitrate > 档位 bpp；None=未定标）。"""
        budget = self._resolve_video_budget()
        if budget is not None:
            return max(1, budget // int(self._budget_frames))
        if self._bitrate_bps is not None:
            per_frame_bits = float(self._bitrate_bps) * self._dur / float(self._timescale)
            return max(1, int(per_frame_bits / 8.0))
        return self._tier.frame_target_bytes(
            int(self._config.width), int(self._config.height))

    def _encode_packet(self, planes: list, frame_cfg: Any = None) -> Tuple[bytes, Any]:
        """编码一帧 → (packet, stats)。

        R3：stats（颜色/alpha 分载荷 + alpha 最大误差）不再丢弃——预算
        闭环（探测/记账/元数据）依赖它们（审计 P1-3）。
        """
        fc = frame_cfg if frame_cfg is not None else self._frame_cfg
        self._apply_slice_threads_once()
        if self._gop == 'ip2':
            # V9（micro-gop 计划批 5）：I/P 决策 + 残差合成 + 回退 + 参考
            # 事务全在原生 GOP context；锚 qp = fc.qp_base（P0 固定 qp，
            # sized/反馈码控不适用——ADR-C047 范围）。
            if frame_cfg is not None:
                raise RuntimeError(
                    "Topos encoder: V9 gop 路径不支持 alpha 预算探测")
            pkt, st = self._gop_ctx.encode_frame(planes)
            return pkt, st
        if self._qp_override is not None:
            return self._codec.encode_frame(fc, planes)
        target = self._frame_target_bytes()
        if target is None:
            return self._codec.encode_frame(fc, planes)
        # 对标达芬奇路线 阶段 1：帧间反馈单遍码控。首帧（及探测路径之外的
        # 生产首帧）sized 搜索定标 qp*（确定性唯一），此后单遍固定 qp 出流，
        # 以帧字节反馈更新下一帧 qp——长期均值贴目标，单帧可浮动。
        # frame_cfg 非空 = alpha 预算探测（_probe_budget_and_open_mux），
        # 不推进反馈状态（探测编码不计入码控序列）。
        if frame_cfg is not None:
            pkt, stats, _qp_used = self._codec.encode_sized(
                fc, planes, target, qp_min=0, qp_max=TOPOS_QP_MAX)
            return pkt, stats
        rc = self._rc
        if rc is None or not rc.active:
            pkt, stats, qp_used = self._codec.encode_sized(
                fc, planes, target, qp_min=0, qp_max=TOPOS_QP_MAX)
            # V2.x AQ qp 顶格守卫：搜索落到 qp95 且超目标时关 AQ 重编同 qp
            #（AQ 在 qp95 只会更胖——+偏移被钳 95、−偏移细化忙带；地板档
            # 低码率 tier 不因 AQ 加重 qp 顶格超支。确定性判定，
            # qp 不变 ⇒ 仅去掉色度带偏移）。
            if (self._aq_on and int(qp_used) >= TOPOS_QP_MAX
                    and len(pkt) > target):
                fcg = self._clone_frame_cfg(fc)
                fcg.reserved[1] = 0
                pkt2, stats2, _ = self._codec.encode_sized(
                    fcg, planes, target, qp_min=TOPOS_QP_MAX, qp_max=TOPOS_QP_MAX)
                if len(pkt2) < len(pkt):
                    pkt, stats = pkt2, stats2
            if rc is None:
                budget = self._resolve_video_budget()
                rc = ToposRateFeedback(
                    target, qp_max=TOPOS_QP_MAX,
                    total_budget_bytes=budget,
                    total_frames=(int(self._budget_frames)
                                  if budget is not None else None))
                self._rc = rc
            rc.seed(qp_used, len(pkt))
            return pkt, stats
        fc.qp_base = rc.qp
        # P4 帧级码控：复杂度前瞻（本帧编码前的空间活动度比）→ 预测复杂度
        # 突变（或 rc 切换检测 reseed_hint）时对本帧做 sized 搜索即刻重锚，
        # 复杂帧首帧即拿到正确 qp（不被平均码率饿死/撑死）。冷却 8 帧
        # 防闪烁内容反复重锚（确定性：活动度是平面的纯函数）。
        if self._should_reanchor(planes, rc):
            pkt, stats, qp_used = self._codec.encode_sized(
                fc, planes, int(rc.target), qp_min=0, qp_max=TOPOS_QP_MAX)
            rc.anchored(qp_used, len(pkt))
            return pkt, stats
        pkt, stats = self._codec.encode_frame(fc, planes)
        if self._aq_on and int(rc.qp) >= TOPOS_QP_MAX and len(pkt) > target:
            fcg = self._clone_frame_cfg(fc)
            fcg.reserved[1] = 0
            pkt2, stats2 = self._codec.encode_frame(fcg, planes)
            if len(pkt2) < len(pkt):
                pkt, stats = pkt2, stats2
        rc.note(len(pkt))
        return pkt, stats

    #: P4 复杂度前瞻：活动度比阈值（与 rc.cut_factor 同值语义）与重锚冷却。
    _CPLX_RATIO_CUT = 1.8
    _REANCHOR_COOLDOWN = 8

    def _complexity_estimate(self, planes: list) -> float:
        """帧内复杂度前瞻（编码前）：Y/G 平面采样梯度的均值。

        每 16 行 × 每 4 列采样（1080p ~33k 样本，远低于编码成本），
        确定性（纯 numpy 读取路径）。
        """
        y = np.frombuffer(planes[0], dtype='<u2')
        h = int(self._config.height)
        w = len(y) // h
        if w < 2:
            return 1.0
        sample = y.reshape(h, w)[::16, ::4].astype(np.int32)
        dx = float(np.abs(np.diff(sample, axis=1)).mean())
        return dx + 1e-6

    def _should_reanchor(self, planes: list, rc: Any) -> bool:
        """P4 重锚判定：rc 切换检测信号，或本帧复杂度前瞻比超阈值。"""
        cplx = self._complexity_estimate(planes)
        prev = self._cplx_prev
        self._cplx_prev = cplx
        if self._frame_count - self._last_reanchor < self._REANCHOR_COOLDOWN:
            return False
        ratio_cut = prev is not None and (
            cplx > prev * self._CPLX_RATIO_CUT
            or cplx < prev / self._CPLX_RATIO_CUT)
        if rc.reseed_hint or ratio_cut:
            self._last_reanchor = self._frame_count
            return True
        return False

    def rate_control_telemetry(self) -> Optional[dict]:
        """P4 遥测：QP/实际字节/切换帧/借贷（未定标或固定 qp → None）。"""
        if self._rc is None:
            return None
        t = self._rc.telemetry()
        t.pop('per_frame', None)   # 摘要（逐帧明细经 _rc 直取）
        return t

    def _probe_budget_and_open_mux(self, planes: list) -> Tuple[bytes, Any]:
        """R3 首帧预算探测：mode2 位深 12→10→8 依次试编码，取首个满足档位
        目标比例的深度；全部超目标则取下限（超硬上限由逐帧策略处理）。

        - 探测复用生产编码路径（qp 覆盖 / sized 同口径），alpha 平面保持
          16-bit 容器语义不变，仅变 ``fc.alpha_bit_depth`` 交由 native 顶层
          预量化（spec §8.6）——决策是输入的纯函数（spec §11.1 确定性）；
        - mux/tpcC 容器级位深一致性 ⇒ 文件级定深（ADR-C014 C-113）：
          后续帧沿用选定深度，不逐帧切换。
        """
        assert self._budget is not None
        ctypes = self._binding['ctypes']
        from .topos_binding import _CFrameConfig
        target_ratio = self._budget['target']
        candidates = [d for d in (12, 10, 8) if d <= self._alpha_bit_depth]
        pkt, stats, chosen_depth = None, None, candidates[-1]
        ratio = float('inf')
        for depth in candidates:
            fc = _CFrameConfig()
            ctypes.memmove(ctypes.byref(fc), ctypes.byref(self._frame_cfg),
                           ctypes.sizeof(_CFrameConfig))
            fc.alpha_bit_depth = depth
            # T1.5：探测固定 V1 口径（reserved[0]=0）——深度决策钉住历史
            # 行为，不随熵模式漂移（V2 下 color/alpha 负载比例改变会诱发
            # a12→a8 的画质回归）；首帧随后以生产配置重编码。AQ/RDO
            # 同理钉住（reserved[1/2]=0）：深度决策不随实验开关漂移。
            # reserved[5]（P6 输出特性）必须一并清零：该槽仅在 em=8 下合法
            # （native 域校验按 reserved[0] 分支），残留非零会让整个 sized
            # 探测被拒——422 10-bit 目标体积导出曾因此在首帧直接报错。
            fc.reserved[0] = 0
            fc.reserved[1] = 0
            fc.reserved[2] = 0
            fc.reserved[5] = 0
            pkt, stats = self._encode_packet(planes, frame_cfg=fc)
            ratio = int(stats.alpha_payload_bytes) / max(
                1, int(stats.color_payload_bytes))
            chosen_depth = depth
            if ratio <= target_ratio:
                break
        if chosen_depth != self._alpha_stream_depth:
            logger.info(
                "Topos encoder: alpha 预算自适应 a%d → a%d（首帧比例 %.3f > "
                "目标 %.2f，tier=%s；平面预量化保持 a%d，native 顶层再量化）",
                self._alpha_stream_depth, chosen_depth, ratio, target_ratio,
                self._tier.tier_id, self._alpha_bit_depth)
            self._alpha_stream_depth = chosen_depth
            self._budget['adapted'] = True
        self._create_mux()
        # 生产口径（默认 V2）重编码首帧——探测包（V1）只用于决策，不入流。
        pkt, stats = self._encode_packet(planes)
        return pkt, stats

    def _account_budget(self, stats: Any) -> None:
        """逐帧累计预算统计 + 超硬上限策略（审计任务 1/3/4）。"""
        b = self._budget
        if b is None or stats is None:
            return
        alpha_b = int(stats.alpha_payload_bytes)
        color_b = int(stats.color_payload_bytes)
        b['total_alpha'] += alpha_b
        b['total_color'] += color_b
        b['max_err'] = max(b['max_err'], int(stats.alpha_max_abs_error))
        b['frames'] += 1
        frame_ratio = alpha_b / max(1, color_b)
        cumulative = b['total_alpha'] / max(1, b['total_color'])
        if (frame_ratio > b['cap'] or cumulative > b['cap']) and not b['overrun']:
            self._handle_budget_overrun(frame_ratio, cumulative)

    def _handle_budget_overrun(self, frame_ratio: float, cumulative: float) -> None:
        """超硬上限：error=报错 / continue=授权标志 / record=警告+记录（默认）。

        mode1（a16 无损）任何策略下都不降质——文案显式说明（spec §11.3）。
        """
        b = self._budget
        b['overrun'] = True
        if self._budget_policy == 'continue':
            b['authorized'] = True
        if self._alpha_mode == 1:
            msg = (
                f"无损 Alpha（mode1）超预算硬上限——绝不自动降质："
                f"alpha/color={frame_ratio:.3f}（累计 {cumulative:.3f}）"
                f"cap={b['cap']:.2f} tier={self._tier.tier_id}"
            )
        else:
            msg = (
                f"Alpha 预算超硬上限：alpha/color={frame_ratio:.3f}"
                f"（累计 {cumulative:.3f}）cap={b['cap']:.2f} "
                f"target={b['target']:.2f} alpha_bit_depth=a{self._alpha_stream_depth}"
                f"（mode2 已降至下限仍超）tier={self._tier.tier_id}"
            )
        if self._budget_policy == 'error':
            raise RuntimeError(
                msg + "（alpha_budget_policy=error 拒绝交付；可改 "
                      "record=记录交付 / continue=授权交付）")
        if not self._budget_overrun_warned:
            self._budget_overrun_warned = True
            logger.warning(
                "%s——按 policy=%s 交付，超限与最大误差将记入 tpcB 元数据",
                msg, self._budget_policy)

    def _write_budget_metadata(self) -> None:
        """close() 时把目标/实际比例、模式、最大误差写入 tpcB（R3 任务 5）。"""
        b = self._budget
        if b is None or self._mux is None or b['frames'] == 0:
            return
        actual = b['total_alpha'] / max(1, b['total_color'])
        actual_bp = int(round(actual * 10000.0))
        # 复验 P1-11：真实比例超出 u16 bp 域时置饱和标志（此前静默钳到
        # 65535，读者无法区分 6.5535 与 65.5）
        saturated = actual_bp > 65535
        self._mux.set_alpha_budget({
            'target_ratio_bp': int(round(b['target'] * 10000.0)),
            # alpha/color 比例可 >1；u16 域饱和（65535bp = ≥6.5535）
            'actual_ratio_bp': min(65535, actual_bp),
            'max_abs_error': b['max_err'],
            'flags': {'overrun': b['overrun'], 'authorized': b['authorized'],
                      'adapted': b['adapted'],
                      'ratio_saturated': saturated},
            'frame_count': b['frames'],
        })
        logger.info(
            "Topos encoder: alpha 预算 tier=%s mode=%d a%d 目标=%.2f 实际=%.2f "
            "max_err=%d frames=%d%s", self._tier.tier_id, self._alpha_mode,
            self._alpha_stream_depth, b['target'], actual, b['max_err'],
            b['frames'],
            f"（超硬上限，policy={self._budget_policy} 交付）" if b['overrun'] else "",
        )

    def _prepare_planes(self, frame: Any) -> list:
        """DecodedFrame → codec 平面字节列表 [(Y, U, V[, A]) 的 tobytes]。"""
        pixel_format = str(getattr(frame, 'pixel_format', '') or '').lower()
        frame_planes = getattr(frame, 'planes', None) or ()

        # —— planar 直通（Topos 源 / 中间片再编码）：免转换 ——
        if getattr(frame, 'is_planar', False) or frame_planes:
            return self._prepare_planar(pixel_format, frame_planes, frame)

        # —— packed（时间线合成 BGR24/BGR48[LE]，或 RGBA 变体） ——
        if self._stream_pixel_format == 3:
            raise RuntimeError(
                "Topos encoder: RAW 档（topos_cfa* 流）要求 CFA 相位平面"
                "源（RAW→RAW 直出）——packed RGB 合成输入不支持编码为 "
                "RAW（debayer 不可逆）；请选择整数/浮点档或保持 RAW 源"
                "直出路径")
        return self._prepare_packed(pixel_format, frame)

    def _cvt_native_fmt(self, data_dtype: Any, pixel_format: str, nch: int) -> Optional[int]:
        """packed 布局 → topos_binding CVT_* 常量；不支持/未知 → None。"""
        fmt = str(pixel_format)
        if nch == 3:
            if fmt.startswith('bgr'):
                return CVT_BGR48 if data_dtype == np.uint16 else CVT_BGR24
            if fmt.startswith('rgb'):
                return CVT_RGB48 if data_dtype == np.uint16 else CVT_RGB24
            return None
        if fmt.startswith('bgr'):
            return CVT_BGRA64 if data_dtype == np.uint16 else CVT_BGRA32
        if fmt.startswith('rgb'):
            return CVT_RGBA64 if data_dtype == np.uint16 else CVT_RGBA32
        return None

    def _try_native_packed(self, data: Any, pixel_format: str,
                           h: int, w: int) -> Optional[list]:
        """P1-12：u8/u16 packed 帧走 native 融合转换（tc_convert_packed_rgb）。

        返回平面列表；不可用（旧 dylib）/不支持布局/运行失败 → None（调用方
        继续 numpy 参考路径；运行失败只发生一次并留 warning）。逐位一致契约
        见 color_convert.c 头注释——本方法不做任何数学。
        """
        if not self._cvt_available or self._codec is None:
            return None
        dtype = data.dtype
        if dtype not in (np.uint8, np.uint16):
            return None
        if not data.flags.c_contiguous:
            return None  # 非连续切片：numpy 参考路径天然支持，不告警不禁用
        nch = int(data.shape[2])
        fmt = self._cvt_native_fmt(dtype, str(pixel_format), nch)
        if fmt is None:
            return None

        # alpha 语义告警与 numpy 路径逐一对应（文案同源）
        if self._has_alpha and nch == 3 and not self._opaque_alpha_warned:
            self._opaque_alpha_warned = True
            logger.warning(
                "Topos encoder: 已选择 Alpha 输出但输入为 3 通道，"
                "Alpha 以不透明写入"
            )
        elif not self._has_alpha and nch == 4 and not self._drop_alpha_warned:
            self._drop_alpha_warned = True
            logger.warning(
                "Topos encoder: packed 输入含第 4 通道但未选择 Alpha 输出"
                "（非 Alpha 档），Alpha 通道被丢弃——如需保留请在导出设置"
                "勾选 Alpha"
            )

        out_mode = int(self._stream_pixel_format)  # 0=422（packed 恒此）1=444 2=GBR
        if out_mode == 2:
            kr, kb, full = 0.0, 0.0, False
        else:
            color_metadata = getattr(self._config, 'color_metadata', None)
            matrix_name = str(getattr(color_metadata, 'matrix', 'bt709') or 'bt709').lower()
            if matrix_name not in _YUV_FORWARD:
                matrix_name = 'bt709'
            kr, kb = _YUV_FORWARD[matrix_name]
            full = str(getattr(color_metadata, 'range', 'limited') or 'limited').lower() == 'full'

        key = (w, h, str(dtype), nch, out_mode, self._stream_bit_depth,
               self._has_alpha, kr, kb, full)
        planes = None
        # 缓冲复用仅限同步编码路径（prepare → encode 同帧内完成）。分段
        # 并行（ADR-C050）与转换流水线（pipelined_conversion）都会缓冲
        # planes 待后续编码——复用缓冲会被下一帧转换覆写（撕裂输入，
        # 实测运行间非确定），故按帧新分配。
        if (self._cvt_bufs is not None and self._cvt_bufs[0] == key
                and not self._gop_seg and not self._pipelined):
            planes = self._cvt_bufs[1]
        # 转换 worker 数与编码池同配（cvt 与编码逐帧交替占用同一常驻池，
        # 2026-09-04 4K 实测 8→16 worker 转换 21.5→13.3 ms/帧）。未显式
        # 配置时跟库默认池规模一致（min(4, cpu)）。
        cvt_workers = (self._slice_threads if self._slice_threads is not None
                       else min(4, max(1, os.cpu_count() or 1)))
        try:
            planes = self._codec.convert_packed_rgb(
                data.reshape(h, w * nch), w, h,
                fmt=fmt, out_mode=out_mode, bit_depth=self._stream_bit_depth,
                kr=kr, kb=kb, full_range=full,
                alpha_shift=self._alpha_shift,
                alpha_opaque=(self._has_alpha and nch == 3),
                max_workers=cvt_workers,
                planes_out=planes,
            )
        except Exception as exc:
            if not self._cvt_failed_warned:
                self._cvt_failed_warned = True
                logger.warning(
                    "Topos encoder: native 融合转换失败（%s）——本流回退 numpy "
                    "参考路径", exc)
            self._cvt_available = False
            return None
        if not self._gop_seg and not self._pipelined:
            self._cvt_bufs = (key, planes)  # 分段/流水线模式不复用（见上）
        return planes

    def _prepare_planar(self, pixel_format: str, frame_planes: tuple, frame: Any) -> list:
        """planar 直通（R1 严格校验：格式白名单/位深/几何/stride/码值域）。

        v1 码流域 = YUV 4:2:2/4:4:4/GBR 10/12/16-bit（v1.2/v1.3 枚举扩展
        R4.1/R4.2 + 批 4 内核加宽 16-bit）——域外输入显式拒绝（审计
        P1-1），不得换头静默编码；码值超出声明位深满刻度同样拒绝（典型
        成因：12-bit 数据误标 10-bit 格式名）。
        """
        h = int(getattr(frame, 'height', 0) or frame_planes[0].shape[0])
        w = int(getattr(frame, 'width', 0) or frame_planes[0].shape[1])

        # —— M3-R1：TRAW CFA（pf=3）专用直通 ——
        # 4 相位平面 R/Gr/Gb/B，各 ceil(W/2)×ceil(H/2)，uint16 满刻度；
        # 无 alpha（TRAW 与 alpha 互斥）；帧 width/height = 全幅尺寸。
        if self._stream_pixel_format == 3:
            if pixel_format not in ('topos_cfa12le', 'topos_cfa16le'):
                raise RuntimeError(
                    f"Topos encoder: CFA 流要求帧 pixel_format 为 "
                    f"topos_cfa12le/16le，得到 {pixel_format!r}（拒绝换头）")
            if len(frame_planes) != 4:
                raise RuntimeError(
                    f"Topos encoder: CFA 输入须为 4 相位平面 R/Gr/Gb/B，"
                    f"得到 {len(frame_planes)} 平面")
            ph, pw = (h + 1) // 2, (w + 1) // 2
            for i, p in enumerate(frame_planes):
                if p.dtype != np.uint16:
                    raise RuntimeError(
                        f"Topos encoder: CFA 相位平面 {i} 要求 uint16，"
                        f"得到 {p.dtype}")
                if (p.shape[0], p.shape[1]) != (ph, pw):
                    raise RuntimeError(
                        f"Topos encoder: CFA 相位平面 {i} 几何 {p.shape} "
                        f"≠ 期望 ({ph}, {pw})（ceil(W/2)×ceil(H/2)）")
                if not p.flags['C_CONTIGUOUS']:
                    logger.warning(
                        "Topos encoder: CFA 平面 %d 非连续（显式紧化拷贝）", i)
            # 2026-09-21 复查（P1 域校验对齐）：CFA 码值不得越流位深满刻度
            # （16-bit 数据喂 12-bit 流此前绕过 planar 路径的同款域检查，
            # 由 native 侧隐性截断）
            full_scale = (1 << self._stream_bit_depth) - 1
            for i, p in enumerate(frame_planes):
                p_max = int(p.max(initial=0))
                if p_max > full_scale:
                    raise RuntimeError(
                        f"Topos encoder: CFA 相位平面 {i} 码值越界 "
                        f"(max={p_max} > {full_scale}，"
                        f"{self._stream_bit_depth}-bit 流)——位深错配请对齐 "
                        f"pix_fmt（topos_cfa{self._stream_bit_depth}le）")
            frame_planes = tuple(np.ascontiguousarray(q) for q in frame_planes)
            if self._has_alpha:
                raise RuntimeError(
                    "Topos encoder: RAW 输出不接受 Alpha 平面（TRAW 与 "
                    "alpha 互斥）")
            return [q.tobytes() for q in frame_planes]

        # R4.2/R4.3：pf≠0（4:4:4/GBR）第 2/3 平面全宽；4:2:2 保持 ceil(w/2)
        cw = w if self._stream_pixel_format != 0 else (w + 1) // 2

        allowed = _PLANAR_FMT_3 if len(frame_planes) < 4 else _PLANAR_FMT_4
        if pixel_format not in allowed:
            raise RuntimeError(
                f"Topos encoder: planar 输入 {pixel_format!r} 不在 v1 允许清单"
                f"（{'/'.join(allowed)}）——v1 码流域 = YUV 4:2:2/4:4:4/GBR "
                f"10/12/16-bit（R4.1–R4.3 + 批 4），其余格式拒绝而非换头"
                f"静默编码；请经 RGB 合成路径导出"
            )
        # 复验 P0-01（最小复现 2）：帧 pixel_format 必须与导出 pix_fmt 的
        # 色度结构/位深逐项一致——config=yuv444p10le 而 frame=gbrp10le 时，
        # 白名单并集会放行，G/B/R 平面被静默标成 Y/U/V。GBR↔YUV、422↔444
        # 一律显式拒绝。
        parsed_frame = _parse_topos_pix_fmt(pixel_format)
        if parsed_frame is None or parsed_frame[0] != 'planar':
            raise RuntimeError(
                f"Topos encoder: planar 输入 {pixel_format!r} 无法解析为"
                f"已知 planar 格式（解析表见 _parse_topos_pix_fmt）"
            )
        if (parsed_frame[1] != self._stream_pixel_format
                or parsed_frame[2] != self._stream_bit_depth):
            cf = {0: '4:2:2', 1: '4:4:4', 2: 'GBR 4:4:4'}
            raise RuntimeError(
                f"Topos encoder: planar 输入 {pixel_format!r} 与导出 pix_fmt "
                f"{str(getattr(self._config, 'pix_fmt', ''))!r} 错配——"
                f"帧为 {cf[parsed_frame[1]]}/{parsed_frame[2]}-bit，流为 "
                f"{cf[self._stream_pixel_format]}/{self._stream_bit_depth}-bit；"
                f"平面语义不同（YUV↔GBR/422↔444），拒绝换头静默编码，"
                f"请对齐导出 pix_fmt 与源格式（复验 P0-01）"
            )
        if len(frame_planes) < 3:
            raise RuntimeError(
                f"Topos encoder: planar 输入至少需要 Y/U/V 三平面，"
                f"得到 {len(frame_planes)}"
            )
        for i, p in enumerate(frame_planes[:3]):
            if p.dtype != np.uint16:
                raise RuntimeError(
                    f"Topos encoder: planar 直通要求 uint16 满刻度平面，"
                    f"平面 {i} dtype={p.dtype}（请走 packed 路径）"
                )
        # 复验 P1-02：Alpha 平面严格 dtype/连续性校验（float alpha 此前会被
        # astype('<u4') 静默截断）
        if len(frame_planes) >= 4 and frame_planes[3].dtype != np.uint16:
            raise RuntimeError(
                f"Topos encoder: planar Alpha 平面要求 uint16 满刻度，"
                f"得到 dtype={frame_planes[3].dtype}——浮点/宽整型 alpha 会被"
                f"静默截断，显式拒绝（复验 P1-02）"
            )
        # 非连续 stride：显式紧化并记录（R1 契约"拒绝或明确转换"——
        # tobytes() 的隐式紧化不留下任何痕迹）
        for i, p in enumerate(frame_planes):
            if not p.flags['C_CONTIGUOUS']:
                logger.warning(
                    "Topos encoder: planar 平面 %d 非连续（显式紧化拷贝）", i)
                frame_planes = tuple(
                    np.ascontiguousarray(q) for q in frame_planes)
                break
        y, u, v = frame_planes[0], frame_planes[1], frame_planes[2]
        if y.shape != (h, w) or u.shape != (h, cw) or v.shape != (h, cw):
            cf = {0: '4:2:2', 1: '4:4:4', 2: 'GBR 4:4:4'}[self._stream_pixel_format]
            raise RuntimeError(
                f"Topos encoder: planar 几何不符 {cf}——p0{y.shape} "
                f"p1{u.shape} p2{v.shape}，期望 ({h},{w})/({h},{cw})/({h},{cw})"
            )

        # PlaneInfo 位深/stride 交叉核验（声明与数据/配置必须一致）
        plane_infos = getattr(frame, 'plane_infos', None)
        if plane_infos:
            for i, pi in enumerate(plane_infos[:len(frame_planes)]):
                if i < 3 and int(pi.bit_depth) != self._stream_bit_depth:
                    raise RuntimeError(
                        f"Topos encoder: planar 平面 {i} 声明位深 "
                        f"{int(pi.bit_depth)} != {self._stream_bit_depth}"
                        f"（颜色平面位深须与导出 pix_fmt 一致；v1.2 域 10/12）"
                    )
                # 复验 P1-02：Alpha 位深独立校验（颜色满刻度 ≠ alpha 满刻度）
                if i == 3 and self._has_alpha and \
                        int(pi.bit_depth) != self._alpha_bit_depth:
                    raise RuntimeError(
                        f"Topos encoder: Alpha 平面声明位深 {int(pi.bit_depth)}"
                        f" != 配置 a{self._alpha_bit_depth}（alpha 位深独立于"
                        f"颜色位深，spec §8.6）"
                    )
                w_exp = w if i in (0, 3) else cw
                if int(pi.stride) != w_exp * 2:
                    raise RuntimeError(
                        f"Topos encoder: planar 平面 {i} stride "
                        f"{int(pi.stride)} != 紧排列 {w_exp * 2}"
                        f"（含 padding 的平面须先紧化）"
                    )
        elif len(frame_planes) >= 4 and self._has_alpha:
            # 复验 P1-02：缺 PlaneInfo 时不再跳过 alpha 位深核验——回落到
            # 帧 pixel_format 的 a{N} 后缀（无后缀 = 帧声明不完整，拒绝）
            m_a = _ALPHA_DEPTH_RE.search(pixel_format)
            if m_a is None:
                raise RuntimeError(
                    f"Topos encoder: 4 平面输入缺 PlaneInfo 且 pixel_format "
                    f"{pixel_format!r} 无 a{{N}} 位深后缀——alpha 位深无法"
                    f"核验，拒绝（复验 P1-02）"
                )

        has_alpha_plane = len(frame_planes) >= 4
        if self._has_alpha:
            if has_alpha_plane:
                m = _ALPHA_DEPTH_RE.search(pixel_format)
                in_depth = int(m.group(1)) if m else self._alpha_bit_depth
                if in_depth != self._alpha_bit_depth:
                    raise RuntimeError(
                        f"Topos encoder: alpha 位深不符——输入 a{in_depth} vs "
                        f"配置 pix_fmt a{self._alpha_bit_depth}（整段导出语义"
                        f"必须一致；请将导出 pix_fmt 对齐源格式）"
                    )
            # premultiplied 一致性（P0-3）：帧自带标志必须与整段配置一致
            frame_premult = bool(
                (getattr(frame, 'extra', None) or {}).get(
                    'topos_alpha_premultiplied', False))
            if frame_premult != self._alpha_premult:
                raise RuntimeError(
                    f"Topos encoder: alpha_premultiplied 不一致——配置为 "
                    f"{self._alpha_premult}，帧 "
                    f"{int(getattr(frame, 'frame_index', 0))} 为 "
                    f"{frame_premult}（整段导出必须固定语义；请在导出配置"
                    f"显式设置 alpha_premultiplied 与源一致）"
                )

        # 码值域：颜色平面 ≤ 位深满刻度。P1-12：全平面 max 扫描改为
        # 「首帧全检 + 每 256 帧抽样」——可信 PlaneInfo 的 planar 直通上
        # 这是唯一每帧整帧扫描（4K 3 平面 ≈ 12 MB 归约 × 每帧）；
        # TOPOS_ENCODER_FULL_CHECK=1 强制逐帧全检（调试/仲裁）。
        do_full_domain_check = self._should_full_domain_check()
        if do_full_domain_check:
            for i, p in enumerate(frame_planes[:3]):
                pmax = int(p.max())
                if pmax > self._color_full_scale:
                    raise RuntimeError(
                        f"Topos encoder: planar 平面 {i} 码值 {pmax} 超出 "
                        f"{self._stream_bit_depth}-bit 满刻度 "
                        f"{self._color_full_scale}（输入位深与导出配置不符——拒绝"
                        f"而非静默截断；请核对源格式声明）"
                    )

        # 阶段 3a：零拷贝——平面已验证 C 连续 uint16，直接传数组（绑定层取
        # 指针）；tobytes() 全平面拷贝 4K 实测 ~14ms/帧
        planes = [np.ascontiguousarray(y),
                  np.ascontiguousarray(u),
                  np.ascontiguousarray(v)]

        if self._has_alpha:
            if has_alpha_plane:
                a = frame_planes[3]
                if a.ndim != 2 or a.shape[:2] != (h, w):
                    raise RuntimeError(
                        f"Topos encoder: Alpha 平面几何 {a.shape} != ({h}, {w})"
                    )
                a_limit = (1 << self._alpha_bit_depth) - 1
                if do_full_domain_check:
                    amax = int(a.max())
                    if amax > a_limit:
                        raise RuntimeError(
                            f"Topos encoder: alpha 平面码值 {amax} 超出 "
                            f"a{self._alpha_bit_depth} 满刻度 {a_limit}"
                            f"（输入位深与导出配置不符）"
                        )
                # 16-bit 容器标度（spec §8.6）：N-bit 满刻度 → <<(16−N)。
                # 同规格 a16 直通时无需再做 32→16 位移拷贝；4444/Alpha
                # 时间线直通会频繁命中该分支，保持源池平面零拷贝。
                if (self._alpha_shift == 0
                        and a.dtype == np.dtype('<u2')
                        and a.flags['C_CONTIGUOUS']):
                    planes.append(a)
                else:
                    a16 = (a.astype('<u4') << self._alpha_shift).astype('<u2')
                    planes.append(np.ascontiguousarray(a16))
            else:
                if not self._opaque_alpha_warned:
                    self._opaque_alpha_warned = True
                    logger.warning(
                        "Topos encoder: 已选择 Alpha 输出但输入无第 4 平面，"
                        "该帧 Alpha 以不透明写入"
                    )
                planes.append(np.full(
                    (h, w), ((1 << self._alpha_bit_depth) - 1) << self._alpha_shift,
                    dtype='<u2'))
        elif has_alpha_plane and not self._drop_alpha_warned:
            self._drop_alpha_warned = True
            logger.warning(
                "Topos encoder: 输入含 Alpha 平面但未选择 Alpha 输出（非 Alpha "
                "档），Alpha 平面被丢弃——如需保留请在导出设置勾选 Alpha"
            )
        return planes

    def _prepare_packed(self, pixel_format: str, frame: Any) -> list:
        """packed 输入（R1 修复审计 P0-2：显式通道映射）。

        四通道禁止整轴 ``[..., ::-1]``——BGRA 翻转会变 ARGB，颜色与 Alpha
        同时错；仅接受 rgb*/bgr* 前缀的 3/4 通道布局，其余显式拒绝。
        ``alpha_premultiplied`` 为配置级元数据（整段语义），packed 帧不携带
        逐帧标志、不与配置冲突。
        """
        data = getattr(frame, 'data', None)
        if data is None:
            raise RuntimeError("Topos encoder: 帧既无平面也无 packed 数据")
        h = int(getattr(frame, 'height', 0) or data.shape[0])
        w = int(getattr(frame, 'width', 0) or data.shape[1])

        if data.ndim != 3 or data.shape[2] not in (3, 4):
            raise RuntimeError(
                f"Topos encoder: packed 输入须为 HxWx3/4，得到 {data.shape}"
            )
        # HALF 样本域：只接受 float 输入（spec §15——half 值即样本本身，
        # 无满刻度标定语义）。整数输入静默归一化到 [0,1] 会重释样本域，
        # 显式拒绝（整数源请走 gbrp16le 整数档）。
        if self._half_float and not np.issubdtype(data.dtype, np.floating):
            raise RuntimeError(
                f"Topos encoder: HALF 样本域只接受 float 输入，得到 "
                f"{data.dtype}——整数输入请用 gbrp16le"
            )
        # 产品 float-* 四档是有损预设；Inf/NaN 经量化可能跨类别，不能
        # 静默把 HDR 特殊值变成有限亮度。显式 HALF/固定 QP 路径仍按既有
        # 契约开放给需要特殊值的调用方。
        image_tier = getattr(self, '_image_tier', None)
        if (self._half_float and image_tier is not None
                and image_tier.family == 'float'
                and not np.isfinite(data).all()):
            raise RuntimeError(
                "Topos encoder: float-* 有损图片档不接受 Inf/NaN；"
                "请先清理特殊值，或使用显式 HALF/固定 QP 工作流并验证往返"
            )
        # —— P1-12：u8/u16 packed → native 融合转换（逐位一致于下方 numpy
        #    参考；float 输入与不支持布局继续走原路径）——
        native = self._try_native_packed(data, pixel_format, h, w)
        if native is not None:
            return native

        if np.issubdtype(data.dtype, np.floating):
            if self._half_float:
                # HALF 样本域（spec §15）：无 [0,1] 契约——负值与 HDR>1.0
                # 是合法线性域样本，不裁剪、不告警（区别于整数档的
                # 信号域契约）
                rgbf = data.astype(np.float32)
                if rgbf.ndim == 3 and rgbf.shape[-1] == 4:
                    pass  # alpha 同为 half 域浮点，不做 [0,1] 钳制
            else:
                # R2（审计 P1-2）：浮点输入契约 = **信号域值**（OETF 已由
                # 调用方完成）——下界裁负、上界不预裁：>1.0 高光在码值级
                # 量化天花板可见地裁剪并显式警告，不做编码器内部无条件
                # [0,1] 静默裁剪。
                rgbf = np.clip(data.astype(np.float32), 0.0, None)
                if not self._float_range_warned and rgbf.ndim == 3:
                    mx = float(np.max(rgbf[..., :3]))
                    if mx > 1.0 + 1e-6:
                        self._float_range_warned = True
                        logger.warning(
                            "Topos encoder: 浮点输入存在 >1.0 值（max=%.4f）——"
                            "浮点契约为信号域 [0,1]（OETF 须由调用方完成，场景"
                            "线性值请先经显式色彩变换）；超出部分将在码值级量化"
                            "裁剪", mx
                        )
                if rgbf.ndim == 3 and rgbf.shape[-1] == 4:
                    rgbf[..., 3] = np.clip(rgbf[..., 3], 0.0, 1.0)  # alpha 恒 [0,1]
        else:
            max_val = float(np.iinfo(data.dtype).max)
            rgbf = data.astype(np.float32) / max_val

        # 通道映射（显式，P0-2）
        fmt = pixel_format
        nch = data.shape[2]
        if nch == 4:
            if fmt.startswith('bgr'):
                rgbf = rgbf[..., [2, 1, 0, 3]]   # BGRA → RGBA
            elif not fmt.startswith('rgb'):
                raise RuntimeError(
                    f"Topos encoder: packed 输入格式 {fmt!r} 不受支持"
                    f"（4 通道须 rgba*/bgra*；argb/abgr 须先重排）"
                )
        else:
            if fmt.startswith('bgr'):
                rgbf = rgbf[..., ::-1]           # BGR → RGB
            elif not fmt.startswith('rgb'):
                raise RuntimeError(
                    f"Topos encoder: packed 输入格式 {fmt!r} 不受支持"
                    f"（3 通道须 rgb*/bgr*）"
                )

        if self._stream_pixel_format == 2:
            if self._half_float:
                # HALF 样本域：float 直通 half 码值（无标定/裁剪）
                y, u, v = _rgb_to_gbrp_half(rgbf[..., :3])
            else:
                # R4.3：GBR 输出——通道直通（G,B,R 序、满刻度、无矩阵折算）
                y, u, v = _rgb_to_gbrp(rgbf[..., :3], self._stream_bit_depth)
        else:
            color_metadata = getattr(self._config, 'color_metadata', None)
            matrix_name = str(getattr(color_metadata, 'matrix', 'bt709') or 'bt709').lower()
            if matrix_name not in _YUV_FORWARD:
                matrix_name = 'bt709'
            rng = str(getattr(color_metadata, 'range', 'limited') or 'limited').lower()
            y, u, v = _rgb_to_yuv_planar(
                rgbf[..., :3], matrix_name, rng == 'full',
                bit_depth=self._stream_bit_depth,
                chroma_full=(self._stream_pixel_format == 1))

        planes = [np.ascontiguousarray(y), np.ascontiguousarray(u),
                  np.ascontiguousarray(v)]
        if self._has_alpha:
            if self._half_float:
                # HALF：alpha 同为 half 域浮点（mode1 无损；负值/HDR 合法，
                # 无 [0,1] 钳制与满刻度量化）
                if data.shape[2] == 4:
                    a = _half_plane(rgbf[..., 3])
                else:
                    if not self._opaque_alpha_warned:
                        self._opaque_alpha_warned = True
                        logger.warning(
                            "Topos encoder: 已选择 Alpha 输出但输入为 3 通道，"
                            "Alpha 以不透明写入（half 1.0）"
                        )
                    a = np.full((h, w), 0xBC00, dtype='<u2')  # half 1.0 码值
                planes.append(np.ascontiguousarray(a))
            else:
                a_nmax = (1 << self._alpha_bit_depth) - 1
                if data.shape[2] == 4:
                    # N-bit 满刻度量化后左移进 16-bit 容器（spec §8.6）
                    a = np.clip(np.rint(rgbf[..., 3] * a_nmax), 0,
                                a_nmax).astype('<u4')
                    a = (a << self._alpha_shift).astype('<u2')
                else:
                    if not self._opaque_alpha_warned:
                        self._opaque_alpha_warned = True
                        logger.warning(
                            "Topos encoder: 已选择 Alpha 输出但输入为 3 通道，"
                            "Alpha 以不透明写入"
                        )
                    a = np.full((h, w), a_nmax << self._alpha_shift, dtype='<u2')
                planes.append(np.ascontiguousarray(a))
        elif data.shape[2] == 4 and not self._drop_alpha_warned:
            self._drop_alpha_warned = True
            logger.warning(
                "Topos encoder: packed 输入含第 4 通道但未选择 Alpha 输出"
                "（非 Alpha 档），Alpha 通道被丢弃——如需保留请在导出设置"
                "勾选 Alpha"
            )
        return planes

    def _force_close_mux(self) -> None:
        """abort/失败清理路径：尽力关闭并显式记录资源释放未确认（P1-03）。"""
        if self._mux is not None:
            try:
                self._mux.close()
            except Exception as e:
                # abort 已是丢弃路径：不再抛出掩盖原始错误，但把"释放未确认"
                # 记为 error 级 + 可查询标志，不再静默吞掉
                self._mux_close_error = str(e)
                logger.error(
                    "Topos encoder: 关闭未定稿 mux 失败——native mux/fd 释放"
                    "状态未确认（%s）", e)
            self._mux = None

    def _cleanup_temp(self) -> None:
        try:
            if os.path.exists(self._temp_path):
                os.remove(self._temp_path)
        except OSError as e:
            logger.warning("Topos encoder: 清理临时文件失败 %s: %s",
                           self._temp_path, e)
        # C01：序列暂存目录同属本次产物，所有失败路径一并清掉
        self._remove_image_sequence_tmp_dir()


class ToposImageSequenceEncoder(ToposVideoEncoder):
    """Persistent Topos encoder that publishes one independent ``.toos`` per frame.

    The frame kernel, native codec instance, rate feedback, slice pool and
    grow-only buffers are exactly the video path. Only the final sink differs:
    each packet is wrapped in its own TPIM envelope, so every output remains
    independently seekable and decodable.
    """

    # 图片档位 family → 承载视频档（仅取 native_profile/格式域协商；
    # qp/矩阵/展示名由图片档位注册表权威）
    _IMAGE_TIER_VIDEO_TIER = {
        'int422': 'standard',   # profile 3
        'int444': '4444',       # profile 5
        'float': '4444',        # profile 5（HALF 冻结组合）
        'raw': 'hq',            # native_profile 3 协商占位——构造后改写 7
    }

    def __init__(self, output_dir: str, config: Any,
                 slice_threads: Optional[int] = None,
                 slice_rows: int = 16) -> None:
        # M2-F8：``profile`` 命中图片档位注册表（'422-high' / 'float-ultra' /
        # 'raw12-4'…）→ 按注册表 coerce pix_fmt/qp（与 CLI --tier 同锚）；
        # 未命中（None/视频档名/空）保持既有视频档路径，历史调用零漂移。
        self._image_tier = get_topos_image_tier_or_none(
            getattr(config, 'profile', None))
        if self._image_tier is not None:
            config = self._coerce_config_for_image_tier(config)
        super().__init__(
            output_dir, config,
            slice_threads=slice_threads,
            slice_rows=slice_rows,
            container_mode="image_sequence",
        )
        if self._image_tier is not None and self._image_tier.family == 'raw':
            # R1（M3）：TRAW 帧头 profile 7（pf3 CFA；视频档协商域无 7）
            self._native_profile = 7

    def _coerce_config_for_image_tier(self, config: Any) -> Any:
        """图片档位 → 编码配置（pix_fmt/qp/档位名；单一注册表取值）。"""
        import dataclasses

        tier = self._image_tier
        if tier.family == 'raw' and bool(getattr(config, 'has_alpha', False)):
            raise ValueError(
                f"Topos encoder: RAW 档 {tier.tier_id!r} 不支持 Alpha"
                f"（TRAW 与 alpha 互斥，native 编码域契约）——请关闭 Alpha 输出")
        video_tier = self._IMAGE_TIER_VIDEO_TIER[tier.family]
        if not hasattr(config, '_replace') and dataclasses.is_dataclass(config):
            updates = dict(
                pix_fmt=tier.pix_fmt,
                profile=video_tier,
                crf=int(tier.qp),   # 图片档位 qp 即质量锚（CQ 语义）
            )
            return dataclasses.replace(config, **updates)
        # dataclass 之外（duck-type 配置）显式失败，不静默半改
        raise TypeError(
            f"ToposImageSequenceEncoder: image tier 配置须为 dataclass，"
            f"得到 {type(config)!r}")

    def _effective_qmatrix_id(self) -> int:
        # F8：图片档位矩阵（422-low qm3 / medium·high qm1 / ultra·444·
        # float·raw qm0）优先于承载视频档矩阵。
        if self._image_tier is not None:
            return int(self._image_tier.qmatrix)
        return super()._effective_qmatrix_id()

    def _effective_sar(self) -> tuple:
        # M3-R1：TRAW 档 SAR 未指定（0/0，与 image_cli 同锚——档位产物
        # 逐位一致验收含 IDSC）；其余图片档保持 1:1。
        if self._image_tier is not None and self._image_tier.family == 'raw':
            return (0, 0)
        return super()._effective_sar()
        # 2026-09-12：TPIM 信封解封内层 V1..V8（image_container.c 域校验
        # 扩展，spec §14 内层版本独立演进）——图片序列不再覆写回 v2，
        # 跟随视频熵档（默认 rans2，ADR-C038）原生入 .toos：同画质码率
        # −7~12%、解码 −11~27%（tools/toos_entropy_2026-09-12.json）。
