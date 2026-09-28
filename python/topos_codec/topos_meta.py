"""Topos 容器元数据载荷（tpcD atom / TMET chunk 共用）编码与解码。

container_spec v1.7 在 MOV 的 stsd 内新增可选子 atom ``tpcD``；.toos 图片
容器同期新增 optional chunk ``TMET``——两者携带同一份自描述载荷（MOV 侧
magic 'TPCD' 即载荷首 4 字节；TMET chunk 的 FourCC 在目录级，载荷内部仍以
'TPCD' 开头，跨容器同构、单一真相）。v1.8 起 tier_id 域扩展到 7
（TC_TIER_LP，帧间档，ADR-C056）：

    0  4B  magic 'TPCD'
    4  2B  version = 1 (u16be)
    6  1B  tier_id     0=未声明；1..7 = TC_TIER_*（topos_profiles.TOPOS_TIER_IDS）
    7  1B  reserved = 0
    8  2B  vendor_len (u16be, ≤15)
   10  2B  label_len  (u16be, ≤63)
   12  N   vendor UTF-8（如 "TOPOS"）
  12+M  M  label UTF-8（如 "Topos 422 HQ"）
  12+N+M 4B crc32 (IEEE, 覆盖前缀)

C 侧 demux（mov.c parse_stsd）按同一布局严格校验；本模块的解码镜像其全部
规则（magic/version/保留域/tier 域/串长一致性/CRC），任何违反抛
``ToposMetaError``——容器元数据只做展示，校验失败一律降级为"无元数据"，
绝不阻塞媒体打开。
"""
from __future__ import annotations

import struct
import zlib
from typing import Any, Dict

META_MAGIC = b"TPCD"
META_VERSION = 1
VENDOR_MAX = 15
LABEL_MAX = 63
TIER_ID_MAX = 8  # TC_TIER_RAW（M4-R5 视频RAW；v1.9 tpcD 域 0..8；与 mov.c 写/读两侧上限同源）

# .toos 图片容器的同名元数据 chunk（optional | preserve；目录级 FourCC）
TOPOS_IMAGE_META_CHUNK = b"TMET"
# 字节序 int 视图（ctypes c_uint32 / read_metadata chunk_type 参数用）
TOPOS_IMAGE_META_CHUNK_TYPE = int.from_bytes(TOPOS_IMAGE_META_CHUNK, "big")

# —— 图片档位（.toos TMET 专用 tier_id 语义；与 MOV tpcD 的 TC_TIER_*
# 视频档位命名空间相互独立，按容器解释）——质量阶梯 Low/Medium/High/
# Ultra（ADR-I011 整数 422 预设 + v1.8 FLOAT 阶梯 ADR-C057 共用同一套
# tier id；浮点域由 IDSC sample_kind=HALF 自描述，不入 tier id——未来
# FLOAT32 同词表换位深即可）。
# 位深（10/12/16-bit）不入 tier_id——IDSC 已记录，label 落盘时带位深后缀。
TOPOS_IMAGE_TIER_IDS: Dict[str, int] = {
    "low": 1,
    "medium": 2,
    "high": 3,
    "ultra": 4,
}
_TOPOS_IMAGE_QUALITY_BY_ID: Dict[int, str] = {v: k.capitalize()
                                              for k, v in TOPOS_IMAGE_TIER_IDS.items()}


def topos_image_tier_label(tier_id: int, bit_depth: int,
                           floating: bool = False) -> Optional[str]:
    """图片 tier id + 位深 → 展示名；未知 → None。

    命名口径（产品定名）：toos Low/Medium/High/Ultra + '{bit_depth}bit'
    后缀；浮点样本域（sample_kind=HALF，v1.8 产品命名 ADR-C057）追加
    ' float' → 'toos Low 16bit float'。444/RAW 家族不入此表（tier_id=0，
    展示回退 image_profile 类名）。
    """
    quality = _TOPOS_IMAGE_QUALITY_BY_ID.get(int(tier_id))
    if quality is None:
        return None
    suffix = " float" if floating else ""
    return f"toos {quality} {int(bit_depth)}bit{suffix}"


_FIXED_HEAD = struct.Struct(">4sHBBHH")
_FIXED_LEN = _FIXED_HEAD.size  # 12
_CRC_LEN = 4
_MIN_PAYLOAD = _FIXED_LEN + _CRC_LEN


class ToposMetaError(ValueError):
    """载荷违反容器元数据布局（与 C 侧 MALFORMED/CHECKSUM 同语义）。"""


def encode_topos_meta(tier_id: int = 0, vendor: str = "", label: str = "") -> bytes:
    """tier_id/vendor/label → 载荷字节（与 native build_tpcd 逐字节一致）。

    tier_id 越界或串超长显式抛错（fail-fast，不静默截断——档位名写错比
    写失败更糟）。
    """
    if not 0 <= int(tier_id) <= TIER_ID_MAX:
        raise ToposMetaError(f"tier_id 越界（0..{TIER_ID_MAX}）: {tier_id!r}")
    vendor_bytes = (vendor or "").encode("utf-8")
    label_bytes = (label or "").encode("utf-8")
    if len(vendor_bytes) > VENDOR_MAX:
        raise ToposMetaError(f"vendor 超 {VENDOR_MAX} 字节: {vendor!r}")
    if len(label_bytes) > LABEL_MAX:
        raise ToposMetaError(f"label 超 {LABEL_MAX} 字节: {label!r}")
    head = _FIXED_HEAD.pack(META_MAGIC, META_VERSION, int(tier_id), 0,
                            len(vendor_bytes), len(label_bytes))
    body = head + vendor_bytes + label_bytes
    return body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)


def decode_topos_meta(payload: bytes) -> Dict[str, Any]:
    """载荷字节 → {'tier_id', 'vendor', 'label'}（严格校验，镜像 C 解析）。"""
    if len(payload) < _MIN_PAYLOAD or len(payload) > _FIXED_LEN + VENDOR_MAX + LABEL_MAX + _CRC_LEN:
        raise ToposMetaError(f"载荷长度越界: {len(payload)}")
    magic, version, tier_id, reserved, vendor_len, label_len = _FIXED_HEAD.unpack_from(payload, 0)
    if magic != META_MAGIC:
        raise ToposMetaError(f"magic 不符: {magic!r}")
    if version != META_VERSION:
        raise ToposMetaError(f"version 不符: {version}")
    if reserved != 0:
        raise ToposMetaError("reserved 域非 0")
    if tier_id > TIER_ID_MAX:
        raise ToposMetaError(f"tier_id 越界: {tier_id}")
    if _FIXED_LEN + vendor_len + label_len + _CRC_LEN != len(payload):
        raise ToposMetaError("串域长度与载荷长度不一致")
    stored_crc = struct.unpack_from(">I", payload, len(payload) - _CRC_LEN)[0]
    if zlib.crc32(payload[:-_CRC_LEN]) & 0xFFFFFFFF != stored_crc:
        raise ToposMetaError("CRC 不符")
    vendor_start = _FIXED_LEN
    label_start = vendor_start + vendor_len
    return {
        "tier_id": int(tier_id),
        "vendor": payload[vendor_start:label_start].decode("utf-8"),
        "label": payload[label_start:label_start + label_len].decode("utf-8"),
    }
