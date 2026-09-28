"""Topos Video Codec — Python 绑定与编码器封装（开源发布包）。

动态库定位：TOPOS_CODEC_LIB 环境变量 → 本包 lib/ 目录 → 仓库构建目录。
"""
from .topos_binding import (  # noqa: F401
    TOPOS_CODEC_ABI_VERSION,
    ToposCodec,
    ToposCodecError,
    ToposCpuFeatures,
    ToposVersion,
    load_library,
)
from .topos_profiles import (  # noqa: F401
    TOPOS_PROFILE_TIERS,
    available_topos_tiers,
    default_topos_tier_id,
    get_topos_tier,
    get_topos_tier_or_none,
    tier_pix_fmt_choices,
    topos_capability_summary,
)
from .color_metadata_config import ColorMetadataConfig  # noqa: F401
from .topos_rate_control import ToposRateFeedback  # noqa: F401
from .slice_threads_policy import SLICE_THREADS_POLICY  # noqa: F401
from .topos_encoder import ToposVideoEncoder, parse_topos_pix_fmt  # noqa: F401
from .topos_image_binding import ToposImageCodec  # noqa: F401

__all__ = [
    "TOPOS_CODEC_ABI_VERSION", "ToposCodec", "ToposCodecError",
    "ToposCpuFeatures", "ToposVersion", "load_library",
    "TOPOS_PROFILE_TIERS", "available_topos_tiers", "default_topos_tier_id",
    "get_topos_tier", "get_topos_tier_or_none", "tier_pix_fmt_choices",
    "topos_capability_summary", "ColorMetadataConfig", "ToposRateFeedback",
    "SLICE_THREADS_POLICY", "ToposVideoEncoder", "parse_topos_pix_fmt",
    "ToposImageCodec",
]
