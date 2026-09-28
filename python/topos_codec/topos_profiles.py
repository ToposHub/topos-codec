"""Topos Video Codec 质量预设与能力声明（R0 冻结，2026-08-30）。

v1.1（2026-09-19）：能力声明 `audio` 由 False 升级为结构化档
（supported/codecs/layouts，container_spec_v1.1 附录 A；ADR-C051）——
音频轨仅时间线导出携带，渲染缓存/代理纯视频的 ADR-C008 语义不变。

v1.8（2026-09-21，ADR-C056）：**Topos 422 LP** 产品化——首个非帧内档
（帧间微 GOP zero-motion IP-2，码流载体 V7-R3，ADR-C047/C048）。显式
收缩：no-alpha、无 AQ/RDO、无码控（固定锚 qp=72）。帧内六档语义不变。

v1.9（2026-09-21）两项变更：**422 家族 ProRes 容量重标定**（画质对齐
计划 §5 P2）——proxy/lt/standard/hq 的 ``target_bpp`` 由"等画质定标"
（ADR-C043 链）改为"ProRes 容量上限下的默认目标"——同母版实测 ProRes
bpp 取紧端 × 0.98 × 0.95（proxy 2.22×/lt 1.44×/standard 1.41×/hq 1.24×）。
旧 Proxy 低码率档保留为新 **netproxy** 档（网络代理，非 ProRes 对标；
容器 tier id 复用 1，label 区分）。444 家族维持 ADR-C042/C043 定标
（实测已占 ProRes 81–84%，无重标定必要）。导出需精确体积控制时用 P1
预算接口（``max_file_bytes``/``max_video_bytes``，本表只是静态默认）。
**图片编码档位注册表**（产品化计划 M0/E3，``TOPOS_IMAGE_TIERS``）——
全 22 档（整数 422/444 六档 + 浮点四档 ADR-C057 + RAW 十二档 TRAW
批 0~5）的 display name ↔ tier id ↔ TMET label ↔ CLI ``--tier`` id
单一映射（命名规则总纲 N-7 单一注册表，
``docs/float_toos_and_raw_productization_plan_2026-09-21.md`` §2.1）；
应用侧档位选择面（图像序列/Quick Export/TMET 写入）一律从这里取名，
不再各持硬编码字串表。

位于 ``src/shared/codec/``（中立层）：media（TPIC 代理生成）与 export
（ToposVideoEncoder / UI 档位展示）共同消费，避免跨层反向依赖。

**命名与范围（审计 P1-5 纠偏 + R4.4 档位激活）**：当前交付为 **Topos
Codec V1 Preview（10/12-bit 4:2:2/4:4:4/GBR 帧内）**。Proxy/LT/Standard/HQ
四档是同一 native 码流 profile（profile=3）之上的**质量预设**——区别只在
目标码率（bpp 定标）与 Alpha 策略参数，不是四个独立 bitstream profile；
Topos 4444（profile=5，4:4:4 10/12-bit）与 Topos 4444 XQ（profile=6，
4:4:4 12-bit）——命名对齐 ProRes 4444/4444 XQ（ADR-C023 原名 Pro444/Extreme）
是 R4.4 起激活的**独立 bitstream profile**（帧头 profile 字节区分，
格式交叉规则由 native 强制）。

档位是**显式可查询的能力声明**，不允许隐式参数猜测（计划 §V2.1 Profile：
"必须是明确可查询的 profile capability"）。每个档位声明：

- 像素格式与位深（v1.4 域：YUV 4:2:2/4:4:4 + GBR，10/12-bit；
  profile 3/5/6，R4.1–R4.4 扩展）；
- 目标 bits-per-pixel（P5 起按 ProRes 实测档位体积对齐定标——真实素材
  2K/4K `prores_ks` 同名 profile 实测 bpp 的均值，ADR-C032；供
  ``tc_frame_encode_sized`` 帧级确定性 qp 搜索，是调参目标非格式定义）；
- Alpha 预算比例与硬上限（计划 §2.2；自动执行闭环属整改 R3）；
- ``available``：当前 native 编码器是否可实现该档位——R4.4 起六档全部
  可用（4444/4444XQ 档依赖 v1.2–v1.4 枚举扩展，已交付）；
- ``native_profile``：帧头/tpcC profile 字节（Proxy/LT/Standard/HQ=3、
  4444=5、4444XQ=6；ADR-C023 原名 Pro444/Extreme），encoder 按 tier 取值写入。

QP 覆盖：调用方可用固定 QP 替代档位目标码率（FFmpegEncoderConfig.crf，
0–95——v1.5 qp 域，数值语义与 codec qp 一致）。
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import Dict, List, Optional, Tuple

# 对外统一命名（R0：spec / capability query / UI / SDK / 计划表同一答案）
TOPOS_CODEC_LABEL = "Topos Video Codec V1 Preview（Intra 4:2:2/4:4:4/GBR 10/12/16-bit）"


@dataclass(frozen=True)
class ToposProfileTier:
    """一档 Topos 编码能力的完整声明。"""

    tier_id: str                 # 'proxy' | 'lt' | 'standard' | 'hq' | '4444' |
                                 # '4444xq' | 'lp'（v1.8 帧间档）
    label: str                   # 展示名（UI/i18n 回退用）
    native_profile: int          # 帧头 profile 字节（R4.4：3=Standard、5=4444、6=4444XQ；
                                 # Proxy/LT/HQ 是 profile3 之上的质量预设）
    pixel_format: str            # 目标像素格式（声明性，展示用）
    bit_depths: Tuple[int, ...]  # 声明位深
    # R7：机器可读格式基名（'yuv422p'/'yuv444p'/'gbrp'）——pix_fmt 选择域 =
    # format_bases × bit_depths（tier_pix_fmt_choices）；与 pixel_format
    # 声明串同源维护（capability manifest 交叉钉死）
    format_bases: Tuple[str, ...]
    # 目标 bits-per-pixel：ref_mid_Mb_s * 1e6 / (1920*1080*25) / 8 → 每像素比特。
    # None = 未定标（不用于 rate 模式）。
    target_bpp: Optional[float]
    # 1080p25 等效参考数据率区间（MiB/s）。P5 起为 ProRes 实测档位体积
    # 在两段真实素材上的落点区间（min,max）；早期为计划 §2.1 纸面区间。
    reference_mb_per_s: Tuple[float, float]
    alpha_budget_ratio: float    # 计划 §2.2 Alpha 目标占颜色主码流比例
    alpha_hard_cap: float        # Alpha 硬上限（比例）
    # P2 码率效率战役（2026-09-03）：444 profile 色度 qp 偏移（slice 级
    # qp_delta_chroma）。实测（raw_2K444p10，16 帧）：offset=4 时 ΔY=+0.00 dB
    # 总码率 −18~19%（U −1.5 dB，≥60 dB 无感知区间）。422 家族保持 0
    # （色度半分辨率，同 qp 预算占用与 ProRes 相当，无此结构性劣势）。
    chroma_qp_offset: int
    # P5.1 矩阵选择（ADR-C033）：422 视频档切换 flat（id=0）——真实素材
    # 同码率 PSNR 比 Standard（id=1）高 0.2~1.6 dB（V2 熵实测，档位越高
    # 增益越大）；qm=0 为最老字段值，任何旧解码器可读，零格式风险。
    # 444 家族暂保持 Standard（无 4444 真实素材锚点，flat 未验证）。
    qmatrix_id: int
    available: bool              # 当前 native codec 能否实现
    unavailable_reason: str      # available=False 时的明确原因（UI/预检展示）
    use_case: str                # 主要用途（计划 §2.1）
    # v1.8（ADR-C056）：是否可携带 Alpha（计划 §2.2）。LP 帧间档显式
    # no-alpha（topos_v9_micro_gop_plan §3.7；编码器 gop='ip2' 同规则
    # fail-fast）——能力声明不猜测，UI 据此隐藏 Alpha 控件。
    alpha_supported: bool = True
    # v1.8（ADR-C056）：固定锚 qp（调用方无 qp 覆盖时的档位默认质量锚）。
    # None = 档位走目标码率码控（target_bpp 定标）；LP 帧间档无 sized/
    # 反馈码控（P/I 决策与残差合成在原生侧，ADR-C047 范围），质量 =
    # 固定锚 qp（72 = ADR-C050 分段并行基准锚，体积对齐 LT 档带）。
    anchor_qp: Optional[int] = None

    @property
    def is_rate_controlled(self) -> bool:
        """档位是否有定标的目标码率（帧级 sized 搜索可用）。"""
        return self.target_bpp is not None

    def frame_target_bytes(self, width: int, height: int) -> Optional[int]:
        """按分辨率换算单帧目标字节数（None = 档位未定标）。"""
        if self.target_bpp is None:
            return None
        return max(1, int(round(self.target_bpp * width * height / 8.0)))


def _bpp_from_mb_s(mid_mb_s: float) -> float:
    """1080p25 参考数据率（MB/s）中值 → bits-per-pixel。"""
    bits_per_frame = mid_mb_s * 1024.0 * 1024.0 * 8.0
    return bits_per_frame / (1920.0 * 1080.0 * 25.0)


# 六档定义（顺序 = 计划 §2.1 表序）。reference 区间取中值定标 bpp。
TOPOS_PROFILE_TIERS: Dict[str, ToposProfileTier] = {
    t.tier_id: t
    for t in (
        ToposProfileTier(
            # P2 重标定（2026-09-21）：旧 Proxy 低码率档原样保留——远程
            # 预览/低带宽流转码用途（体积 ≈ ProRes 同名档 42–48%）。
            # **非 ProRes 对标档**：不做画质对标验收（计划 §5 P2 语义
            # 拆分）。容器 tier id 复用 TC_TIER_PROXY(1)——tpcD tier 域
            # native 校验上限 7，新增 id 需容器规范变更且旧读端会把
            # >7 的 tpcD 判 MALFORMED；档位身份由 tpcD label 串
            # （"Topos 422 Net Proxy"）与产品档位表区分，零格式风险。
            tier_id="netproxy",
            native_profile=3,
            label="Topos 422 Net Proxy",
            pixel_format="yuv422p10", format_bases=("yuv422p",),
            bit_depths=(10,),
            # 旧链 ADR-C032→C043 等画质定标值（见 git 历史/proxy 注释）
            target_bpp=0.29108,
            reference_mb_per_s=(1.7, 2.3),
            alpha_budget_ratio=0.20, alpha_hard_cap=0.25,
            # 2026-09-21 边缘均衡矩阵战役：422 视频档 flat(0)→id=4
            # （锯齿诊断 docs/topos_edge_jag_diagnosis_2026-09-21.md：
            #   同尺寸边缘 +0.5~0.7 dB、全帧 +0.1~0.3、色度持平、速度
            #   持平；A/B 见 bench_out/edge_jag/variants/）。仅限
            #   YUV422 10-bit profile3 域，编码器域外自动回退 flat。
            chroma_qp_offset=0, qmatrix_id=4,
            available=True, unavailable_reason="",
            use_case="低带宽远程预览、网络流转码（非 ProRes 对标档）",
        ),
        ToposProfileTier(
            tier_id="proxy",
            native_profile=3,
            label="Topos 422 Proxy",
            pixel_format="yuv422p10", format_bases=("yuv422p",),
            bit_depths=(10,),
            # P2 ProRes 容量重标定（2026-09-21，画质对齐计划 §5 P2）：
            # 旧链 ADR-C032→C043 等画质定标 0.29108 实测仅占同档 ProRes
            # 体积 42–48%，低码率 8×8 块效应显著（P0 语料 block_index
            # 3.95/2.94 vs 其他档 1.0–1.6）。新值 = 同母版 ProRes 实测
            # bpp（2K 0.71376 / 4K 0.69262）取紧端 × 0.98 上限 × 0.95
            # 内容波动余量 → 语料落点 90.9%/93.2% of ProRes。旧低码率
            # 档保留为 netproxy（网络代理，非 ProRes 对标档）。
            target_bpp=0.64483,
            reference_mb_per_s=(1.7, 2.3),
            alpha_budget_ratio=0.20, alpha_hard_cap=0.25,
            chroma_qp_offset=0, qmatrix_id=4,   # 2026-09-21 边缘均衡（见 netproxy 注释；
                                                # P5.1 flat → id=4，域外回退 flat）
            available=True, unavailable_reason="",
            use_case="离线代理、远程剪辑",
        ),
        ToposProfileTier(
            tier_id="lt",
            native_profile=3,
            label="Topos 422 LT",
            pixel_format="yuv422p10", format_bases=("yuv422p",),
            bit_depths=(10,),
            # P2 ProRes 容量重标定（2026-09-21）：同母版 ProRes 实测
            # bpp 2K 1.54992 / 4K 1.45257 取紧端 × 0.98 × 0.95；
            # 旧链 ADR-C032→C043 0.94075 → 语料落点 87.6%/93.2%
            target_bpp=1.35234,
            reference_mb_per_s=(5.6, 8.5),
            alpha_budget_ratio=0.20, alpha_hard_cap=0.25,
            chroma_qp_offset=0, qmatrix_id=4,   # 2026-09-21 边缘均衡（见 netproxy 注释；
                                                # P5.1 flat → id=4，域外回退 flat）
            available=True, unavailable_reason="",
            use_case="轻量中间缓存、粗剪",
        ),
        ToposProfileTier(
            tier_id="standard",
            native_profile=3,
            label="Topos 422",
            pixel_format="yuv422p10", format_bases=("yuv422p",),
            bit_depths=(10,),
            # P2 ProRes 容量重标定（2026-09-21）：同母版 ProRes 实测
            # bpp 2K 2.31364 / 4K 2.26830 取紧端 × 0.98 × 0.95；
            # 旧链 ADR-C032→C043 1.50296 → 语料落点 91.4%/93.1%
            target_bpp=2.11179,
            reference_mb_per_s=(8.1, 12.2),
            alpha_budget_ratio=0.25, alpha_hard_cap=0.30,
            chroma_qp_offset=0, qmatrix_id=4,   # 2026-09-21 边缘均衡（见 netproxy 注释；
                                                # P5.1 flat → id=4，域外回退 flat）
            available=True, unavailable_reason="",
            use_case="常规剪辑和渲染缓存",
        ),
        ToposProfileTier(
            tier_id="hq",
            native_profile=3,
            label="Topos 422 HQ",
            pixel_format="yuv422p/444p/gbrp 10/12/16", format_bases=("yuv422p", "yuv444p", "gbrp"),
            bit_depths=(10, 12, 16),  # 批 4：16-bit 视频；整数 .toos 图片另有 profile 域
            # R4.1–R4.3：v1.2 12-bit + v1.3 4:4:4 + v1.4 GBR 枚举
            # P2 ProRes 容量重标定（2026-09-21）：同母版 ProRes 实测
            # bpp 2K 3.50464 / 4K 3.38427 取紧端 × 0.98 × 0.95；
            # 旧链 ADR-C032→C043 2.54956 → 语料落点 90.0%/93.1%
            target_bpp=3.15075,
            reference_mb_per_s=(13.7, 19.2),
            alpha_budget_ratio=0.25, alpha_hard_cap=0.30,
            chroma_qp_offset=0, qmatrix_id=4,   # 2026-09-21 边缘均衡（见 netproxy 注释；
                                                # P5.1 flat → id=4，域外回退 flat）
            available=True, unavailable_reason="",
            use_case="调色、中间母版",
        ),
        ToposProfileTier(
            tier_id="4444",
            native_profile=5,
            label="Topos 4444",
            pixel_format="yuv444p/gbrp 10/12/16", format_bases=("yuv444p", "gbrp"),
            bit_depths=(10, 12, 16),  # 批 4：16-bit（profile 5；4444xq/profile6 仍 12-bit-only）
            # P5 码率对齐（ADR-C042，2026-09-11）：prores_ks 4444 实测
            # bpp 2K 5.6114/5.6123、4K 5.5520/5.5645（10/12bit 轴）均值
            # 5.58505——ADR-C032 422 家族同口径补齐 444 档；旧 P4 值
            # 4.11063 无 4444 素材锚点。Apple 自家 4444 全片实占 5.22
            # （prores_ks 口径热 ~7%，tier 定标沿用 prores_ks）
            # ADR-C043 等画质重定标：444 家族领先最大（+5.2~+7.3 dB）
            # → 等画质比最差格 0.727（4K10）×1.05；旧 5.58505（同码率
            # 定标，注释见 git 历史/ADR-C042）
            target_bpp=4.26335,
            reference_mb_per_s=(30.0, 44.0),
            alpha_budget_ratio=0.25, alpha_hard_cap=0.30,
            # ADR-C042：flat 在真实 444 素材 6 个对齐码率点全胜 Standard
            # （合成 +0.30~+3.25 dB）；色度偏移归零（dc0 合成 6 点全胜
            # dc4 +0.03~+1.75 dB，444 工作流语义=chroma 保真；422 家族
            # tier 本就全 0，P2 的 +4 系随 Standard 矩阵选定）
            chroma_qp_offset=0, qmatrix_id=0,   # flat（ADR-C042 终版）
            available=True, unavailable_reason="",  # R4.4 激活（v1.4 包络内）
            use_case="VFX、动态图形、合成",
        ),
        ToposProfileTier(
            tier_id="4444xq",
            native_profile=6,
            label="Topos 4444 XQ",
            pixel_format="yuv444p12/gbrp12", format_bases=("yuv444p", "gbrp"),
            bit_depths=(12,),
            # P5 码率对齐（ADR-C042，2026-09-11）：prores_ks 4444xq 实测
            # bpp 2K 8.3462、4K 7.6016（12bit 轴）均值 7.97390；旧 P4 值
            # 6.76296 系 qp63 顶格逐帧钉扎（v1.5 qp 域 0..95 后该天花板
            # 已不构成约束）。Apple 自家 XQ 全片实占 7.80（口径差 ~2%）
            # ADR-C043 等画质重定标：等画质比最差格 0.751（4K）×1.05；
            # 旧 7.97390（同码率定标，注释见 git 历史/ADR-C042）
            target_bpp=6.28784,
            reference_mb_per_s=(45.0, 68.0),
            alpha_budget_ratio=0.30, alpha_hard_cap=0.30,
            # ADR-C042：flat + 色度偏移归零（同 4444；4444xq 档位落点
            # flat 对 Standard +1.42~+2.35 dB，matched@xq +1.16~+3.25 dB）
            chroma_qp_offset=0, qmatrix_id=0,   # flat（ADR-C042 终版）
            available=True, unavailable_reason="",  # R4.4 激活（v1.4 包络内）
            use_case="高宽容度 HDR 和多代制作",
        ),
        ToposProfileTier(
            # v1.8 产品化（ADR-C056）：唯一非帧内档。帧间微 GOP（zero-motion
            # IP-2，P 只参考同 GOP 紧邻上一帧），码流载体 V7-R3（major 7,
            # em 8；ADR-C048）——帧头 profile 仍是 3（4:2:2 10-bit），tier
            # 与帧间性正交：帧间性由包头自描述，tier 只描述质量档位语义
            # （容器 tpcD tier_id=7）。显式收缩（编码器/导出链 fail-fast，
            # 禁静默降级）：no-alpha（§3.7）、无 AQ/RDO、无码控（固定锚
            # qp，target_bpp=None）。体积带 ≈ LT ×(1−6~11%)（ADR-C047
            # 全库 LT 锚中位），编码实时（ADR-C050：2K 分段 107 fps、
            # 4K 79 fps）——读侧 GOP context 已随 V9 批 5 交付（prev_sync
            # 定位 + 有界缓存，topos_source v9_micro_gop 路径）。
            tier_id="lp",
            native_profile=3,
            label="Topos 422 LP",
            pixel_format="yuv422p10", format_bases=("yuv422p",),
            bit_depths=(10,),
            target_bpp=None,
            # 展示用参考带（推算值，非定标目标）：LT 档 (5.6, 8.5) MiB/s
            # 按 ADR-C047 体积中位收益 6~11% 折算；实码率随素材冗余度浮动
            # （P 帧占比越高收益越大，切镜密集段趋近 LT）。
            reference_mb_per_s=(5.0, 7.6),
            alpha_budget_ratio=0.0, alpha_hard_cap=0.0,
            chroma_qp_offset=0, qmatrix_id=0,   # 422 家族 flat（ADR-C033）
            available=True, unavailable_reason="",
            use_case="实时粗剪缓存、冗余素材快速转码（无 Alpha、固定锚 qp）",
            alpha_supported=False,
            anchor_qp=72,                        # ADR-C050 LP 锚
        ),
        ToposProfileTier(
            # M4-R5/R7（D3 拍板）：视频 RAW 单 tier + 位深×比率参数化
            #（12 档 qp 锚与图片线共享——比率选择经 crf 传锚，见注册表
            # RAW 档 raw12-*/raw16-*）。帧 profile 7 + pf=3 CFA；编码链
            # R7 落地：ToposVideoEncoder 全 I 帧强制（raw 无帧间形态）+
            # CFA 相位平面直入（RAW→RAW，拒 packed 合成输入）+ V7-R2/
            # V1 载体（bd16 宽域熵冻结）。
            tier_id="raw",
            native_profile=7,
            label="Topos RAW",
            pixel_format="topos_cfa12le/16le", format_bases=("topos_cfa",),
            bit_depths=(12, 16),
            target_bpp=None,
            reference_mb_per_s=(0.0, 0.0),   # CQ 比率标签语义——见注册表 RAW 档
            alpha_budget_ratio=0.0, alpha_hard_cap=0.0,
            chroma_qp_offset=0, qmatrix_id=0,    # TRAW qm0 冻结
            available=True, unavailable_reason="",
            use_case="相机 RAW CFA 中间片（RAW→RAW 直出语义，M4-R2/R9）",
            alpha_supported=False,
            anchor_qp=None,   # 比率表驱动：比率选择经 crf 传锚（raw12-4 等）
        ),
    )
}

# 当前可选择（native 可实现）的档位 id 列表，序 = UI 展示序（质量升序，
# netproxy 为体积优先的独立网络档排首（不参与 ProRes 对标）；末位 lp 为
# 独立帧间模式档、raw 为视频 RAW 声明档（available=False，R7 编码链落地
# 后激活）——均排尾注册）。
AVAILABLE_TOPOS_TIERS: Tuple[str, ...] = (
    "netproxy", "proxy", "lt", "standard", "hq", "4444", "4444xq", "lp",
    "raw",
)

# v1 受限近似 Alpha 的默认位深（mode 2 允许 8/10/12；12 为最高保真，
# 且阶段 4 实测其码率占比与无损 mode 1 几乎相同，质量_report §4）。
TOPOS_DEFAULT_ALPHA_MODE = 2
TOPOS_DEFAULT_ALPHA_BIT_DEPTH = 12


def get_topos_tier(tier_id: str) -> ToposProfileTier:
    """按 id 查询档位；未知 id 抛 KeyError（显式失败，不猜测）。"""
    try:
        return TOPOS_PROFILE_TIERS[tier_id]
    except KeyError:
        known = ", ".join(TOPOS_PROFILE_TIERS)
        raise KeyError(
            f"Unknown Topos profile tier: {tier_id!r} (known: {known})"
        ) from None


# —— 容器元数据 tier id（container_spec v1.7 tpcD / 图片 TMET chunk）——
# 与 native TC_TIER_* 常量逐一同值（topos_codec.h）；0 = 未声明。
# netproxy 复用 id 1（TC_TIER_PROXY）：native tpcD tier 域校验上限 8，
# 新增 id 属容器规范变更（旧读端判 MALFORMED）；档位身份由 label 区分。
TOPOS_TIER_IDS: Dict[str, int] = {
    "proxy": 1,
    "netproxy": 1,   # P2：网络代理档（非 ProRes 对标；同 id 反查让位 proxy）
    "lt": 2,
    "standard": 3,
    "hq": 4,
    "4444": 5,
    "4444xq": 6,
    "lp": 7,   # v1.8（ADR-C056；帧间档，容器 tpcD/TMET 同域）
    "raw": 8,  # M4-R5（D3 单 tier；v1.9 tpcD 域 0..8）
}
# 反查表保持首见优先（netproxy 与 proxy 同 id 1 → 反查返回 proxy，
# 读端展示以 tpcD label 串为准）
_TOPOS_TIER_KEYS_BY_ID: Dict[int, str] = {}
for _k, _v in TOPOS_TIER_IDS.items():
    _TOPOS_TIER_KEYS_BY_ID.setdefault(_v, _k)


def topos_tier_numeric_id(tier_id: str) -> int:
    """档位 id（'hq'）→ 容器 tier id（4）；未知 id 显式失败。"""
    return TOPOS_TIER_IDS[tier_id]


def topos_tier_label_by_numeric_id(numeric_id: int) -> Optional[str]:
    """容器 tier id → 展示名（'Topos 422 HQ'）；0/未知 → None（不猜测）。"""
    key = _TOPOS_TIER_KEYS_BY_ID.get(int(numeric_id))
    return TOPOS_PROFILE_TIERS[key].label if key else None


def get_topos_tier_or_none(tier_id: Optional[str]) -> Optional[ToposProfileTier]:
    """宽松查询：None/未知 → None（调用方自行决定默认或报错）。"""
    if not tier_id:
        return None
    return TOPOS_PROFILE_TIERS.get(str(tier_id).strip().lower())


def available_topos_tiers() -> List[ToposProfileTier]:
    """当前可用的档位（质量升序）。"""
    return [TOPOS_PROFILE_TIERS[t] for t in AVAILABLE_TOPOS_TIERS]


def tier_pix_fmt_choices(tier_id: str, with_alpha: bool = False) -> List[str]:
    """档位可选 pix_fmt 域（R7 导出 UI 选择器单一来源）。

    无 Alpha：format_bases × bit_depths 的裸名（如 'yuv444p12le'——隐式
    alpha 探测语义，见 topos_encoder R3）。
    带 Alpha：追加 O1 编码域显式名字（topos_yuva444p10a8…、topos_gbrap12a16），
    即 pix_fmt 带 a{N} 后缀 = 格式契约。
    基名 → alpha 名规则：前 3 字符后插 'a'（yuv422p→yuva422p、gbrp→gbrap）。
    """
    t = get_topos_tier(tier_id)
    out: List[str] = []
    if with_alpha and not t.alpha_supported:
        # no-alpha 档（LP/raw）：alpha 域为空——能力声明不猜测（v1.8）
        return out
    for base in t.format_bases:
        for bd in t.bit_depths:
            out.append(f"{base}{bd}le")
            if with_alpha:
                alpha_base = f"topos_{base[:3]}a{base[3:]}"
                for ad in (8, 10, 12, 16):
                    out.append(f"{alpha_base}{bd}a{ad}")
    return out


def default_topos_tier_id() -> str:
    """默认档位：Standard（计划 §2.1 常规剪辑与渲染缓存）。"""
    return "standard"


def topos_capability_summary() -> Dict[str, dict]:
    """能力摘要（预检/诊断/UI 展示用；全部字段显式，无隐式默认）。"""
    return {
        "_codec": {
            "label": TOPOS_CODEC_LABEL,
            "bitstream_profiles": {
                "v1": "Standard（profile 3，YUV 4:2:2 10-bit）",
                "v1.2": "Standard + 12-bit 枚举扩展（R4.1，帧头 minor=1）",
                "v1.3": "Standard + YUV 4:4:4 枚举扩展（R4.2，帧头 minor=2）",
                "v1.4": "Standard + GBR 4:4:4 枚举扩展（R4.3，帧头 minor=3，matrix=0）",
                # 复验 P2-09：profile 5/6 必须出现在同一张表里——区别是
                # 帧头 profile 字节 + 格式约束（4:4:4 / 4:4:4+12-bit），
                # 量化矩阵维持 id=1（ADR-C023 产品决策，非独立矩阵）
                "profile5": "Topos 4444（profile 5，YUV/GBR 4:4:4 10/12/16-bit）",
                "profile6": "Topos 4444 XQ（profile 6，YUV/GBR 4:4:4 12-bit）",
            },
            # 复验 P2-09：只有 Proxy/LT/Standard/HQ 是 profile 3 之上的质量
            # 预设；4444/4444XQ 是独立帧头 profile（非预设）。逐档见
            # 各 tier 的 is_preset 字段。
            "tiers_are_presets": False,
            # ADR-C038（2026-09-11）：产品编码默认 V7-R2 上下文 rANS
            # （version_major=7/entropy 7）——ADR-C036 实测同画质较 rans
            # 再省 1.2~2.8% payload（qp20-84）、解码 ~1×、差分逐位一致；
            # V 代际收纳（2026-09-13）：写面收缩为 {rans2, v2, v1} 三入口
            # 一默认；rans(=V7-R)/intra(=V3)/intra-range(=V6) 写端退役
            # （构造期显式报错，em 编号封存；历史文件仍可解码——码流
            # 自描述）。P5 tier 目标为 ProRes 锚定的 bpp 值，与熵无关——
            # sized 搜索自适应落点（同目标下熵更优 → 落点 qp 更低 =
            # 同码率质量更高）。
            "entropy_default": "v7r2-rans2",
            "entropy_selectable": ["rans2", "v2", "v1"],
            # V2.x：逐带自适应量化（色度专属，编码器内部决策，位流零格式
            # 变更）。2026-09-03 钉扎：切片粒度天花板 = 422 家族同码率
            # luma +0.19~+0.23 dB（≈2.5% 码率）、444 家族无增量（已有静态
            # chroma_qp_offset=4）——不足以触发档位重定标，默认关。
            # 2026-09-11（ADR-C039）：qp≥64 域 sized 探针失配已修
            # （m7_probe 变体钳位源 fh.qp_base→候选 qp）——该域 AQ 由
            # "不可用"改判"可用"（2K/4K proxy 实测贴目标）；默认关不变
            # （收益天花板 2.5% 未动）。
            # 2026-09-21（P3，画质对齐计划 §5）：亮度空间 AQ 入场（块
            # 均值活动度 1/8 倍频程 log、强度 3×±4、纹理优先方向）。同
            # 母版真实语料 A/B（p3_spatial_aq_ab.json）：同码率 ΔPSNR_Y
            # −0.05~−0.27 dB、SSIM ≤0、块指数持平、编码吞吐 −8%~−23%
            # ——带粒度亮度重分配两方向均负收益，默认关维持；块级 QP
            # 信令（位流版本变更）归 P5 评估。
            "aq_default": "off",
            "aq_selectable": ["off", "on"],
            # V2.x：逐系数 level RDO 精修（须 V2 熵；纯编码端决策，码流
            # 零格式变更）。2026-09-04 实测同码率：422 luma +0.15 dB；
            # 444 +0.4~+0.6 dB（色度 −1~3 dB 预算交换，μ=50% 中档）。
            # 编码端 +10~20%，解码不变。默认关。2026-09-10 flat+V2 复测：
            # hq 增益分化（4K +0.58 / 2K +0.04），proxy 负收益
            # （2K −0.68 / 4K −2.43 dB）——低码率档禁用方向明确。
            # 2026-09-11（ADR-C040）门控评估完结：收益带 = [无损平台
            # 悬崖边, qp61]（悬崖边 +1.4~3.7 dB，2K/4K 一致），qp≥62
            # Y 净负、≥76 深负、平台域（≤~48）无意义；产品语境（vs 默认
            # rans2）六格 tier 全负（RDO 须 V2，~9% payload 劣势压倒窄带
            # 收益）→ 门控启用判定否，默认关维持；编码开销实测 +35~74%。
            "rdo_default": "off",
            "rdo_selectable": ["off", "on"],
            # v1.1 音频轨（container_spec_v1.1；ADR-C008"渲染缓存/代理纯视频"
            # 语义不变——音频仅时间线导出携带）
            "audio": {
                "supported": True,
                "codecs": ["pcm_s16", "pcm_s24", "pcm_s32", "pcm_f32", "aac"],
                "layouts": ["1.0", "2.0", "5.1", "7.1"],  # v1.6 mono stems
                "sample_rates": [44100, 48000, 88200, 96000, 176400, 192000],
            },
        },
        **{
            t.tier_id: {
                "label": t.label,
                "native_profile": t.native_profile,
                "is_preset": t.native_profile == 3,  # profile3 四档=预设；5/6=独立 profile
                "pixel_format": t.pixel_format,
                "format_bases": list(t.format_bases),
                "bit_depths": list(t.bit_depths),
                "target_bpp": t.target_bpp,
                "reference_mb_per_s": list(t.reference_mb_per_s),
                "alpha_budget_ratio": t.alpha_budget_ratio,
                "alpha_hard_cap": t.alpha_hard_cap,
                "chroma_qp_offset": t.chroma_qp_offset,
                "qmatrix_id": t.qmatrix_id,
                "available": t.available,
                "unavailable_reason": t.unavailable_reason,
                "use_case": t.use_case,
                "audio": {               # v1.1 音频轨（仅时间线导出携带）
                    "supported": True,
                    "codecs": ["pcm_s16", "pcm_s24", "pcm_s32", "pcm_f32", "aac"],
                    "layouts": ["1.0", "2.0", "5.1", "7.1"],  # v1.6 mono stems
                    # v1.4：全家族采样率（ADR-C052 后续，对齐 Apple 交付规范）
                    "sample_rates": [44100, 48000, 88200, 96000, 176400, 192000],
                },
                "alpha_supported": t.alpha_supported,  # v1.8：LP 显式 no-alpha
                "anchor_qp": t.anchor_qp,  # v1.8：固定锚 qp（None = 码控档）
            }
            for t in TOPOS_PROFILE_TIERS.values()
        },
    }


# ============================================================================
# 图片编码档位注册表（.toos / Topos Image；2026-09-21 E3 单一注册表骨架）
# ============================================================================
#
# **命名规则总纲**（全产品线统一，原文见
# docs/float_toos_and_raw_productization_plan_2026-09-21.md §2.1，已落两份
# 白皮书"命名规则"章节）：
#
#   N-1 家族前缀     视频线 `Topos`（大写，MOV 容器）；图片线 `toos`（小写，
#                    `.toos` 文件）
#   N-4 图片整数档   `toos <Quality> <bd>bit`，Quality ∈ {Low, Medium, High,
#                    Ultra}
#   N-5 图片浮点档   `toos <Quality> <bd>bit float`（HALF 域；未来 32-bit
#                    档仅换位深数字，规则不变）
#   N-6 图片 RAW 档  文件内显示名/TMET `toos RAW <bd>bit <N>:1`（小写家族
#                    前缀、位深无连字符）；规格文档/白皮书层一律用 N-3 带
#                    连字符形式 `Topos RAW <bd>-bit <N>:1`
#   N-7 单一注册表   全部输出规格选择面由本表派生，任何入口同名
#   N-8 档位名不承载 Log/Linear、无损、实验性等定位词（只进描述文案）
#
# 真相源对齐：CLI ``kTierPresets``（image_cli.c）与
# docs/image/capability_manifest.json ``tiers``——三方逐值一致（E4 回归
# 测试钉住）；本表为应用/UI 侧取名点。

# 图片像素格式枚举（IDSC pixel_format；与 native topos_image.h 同值）
TOPOS_IMAGE_PF_YUV422 = 0
TOPOS_IMAGE_PF_YUV444 = 1
TOPOS_IMAGE_PF_GBR = 2
TOPOS_IMAGE_PF_CFA = 3

# IDSC image_profile（色彩模式类名）：0=Preview(YUV422)、1=HQ(GBR)、
# 3=RAW(TRAW，2026-09-13 由 reserved 改派)、4=HALF(float16，spec §15)
TOPOS_IMAGE_PROFILE_PREVIEW = 0
TOPOS_IMAGE_PROFILE_HQ = 1
TOPOS_IMAGE_PROFILE_RAW = 3
TOPOS_IMAGE_PROFILE_HALF = 4

# 应用侧像素格式注册名（ffmpeg 风格名；编码器 ``_parse_topos_pix_fmt`` /
# 媒体源 PIXEL_FORMATS 同域解析）
_IMAGE_TIER_PIX_FMT = {
    # (pixel_format, bit_depth, sample_kind) → 注册名
    (TOPOS_IMAGE_PF_YUV422, 10, "int"): "yuv422p10le",
    (TOPOS_IMAGE_PF_GBR, 10, "int"): "gbrp10le",
    (TOPOS_IMAGE_PF_GBR, 12, "int"): "gbrp12le",
    (TOPOS_IMAGE_PF_GBR, 16, "float"): "gbrph16le",
    (TOPOS_IMAGE_PF_CFA, 12, "int"): "topos_cfa12le",
    (TOPOS_IMAGE_PF_CFA, 16, "int"): "topos_cfa16le",
}

# 浮点/RAW 档位名中的名义比率（定位标签，CQ 语义——体积随内容浮动，
# plan §1.1.1；UI 副行只显示标称数据率不承诺精确比率）
TRAW_RATIO_NOMINAL = (2, 4, 6, 8, 12, 16)


@dataclass(frozen=True)
class ToposImageTier:
    """一档 Topos 图片编码能力的完整声明（N-7 单一注册表条目）。

    四向映射：``tier_id``（CLI ``--tier`` id / 应用档位键）↔ ``label``
    （规格名：manifest/白皮书/CLI 帮助）↔ ``file_label``（TMET/文件内
    展示名，N-4/N-5/N-6 形式）↔ ``tm_tier_id``（TMET tier_id 字节，
    topos_meta.TOPOS_IMAGE_TIER_IDS 域）。参数五元组（pf/bd/qm/qp/
    image_profile）与 CLI kTierPresets、manifest tiers 逐值一致。
    """

    tier_id: str                 # '422-low' | 'float-high' | 'raw12-4' …
    label: str                   # 规格名（N-3/N-4/N-5；manifest 同文）
    family: str                  # 'int422' | 'int444' | 'float' | 'raw'
    image_profile: int           # IDSC image_profile（0/1/3/4）
    pixel_format: int            # IDSC pixel_format（0/2/3）
    bit_depth: int               # 10/12/16
    qmatrix: int                 # 0=flat / 1=Standard / 3=422 Low Compact
    qp: int                      # 质量锚（CQ 语义；0..95 域）
    sample_kind: str = "int"     # 'int' | 'float'（HALF 域）
    # TMET tier_id（0 = 不入质量阶梯——444/RAW 家族；读端展示回退
    # image_profile 类名，与 image_cli tier_meta_id_by_params 同值）
    tm_tier_id: int = 0
    # 1080p 锚点单帧体积（MB；整数档 = manifest 实测锚，浮点档 = spec
    # §15.3 三素材均值，RAW 档 = 名义比率反推的定位锚）
    anchor_mb_per_frame_1080p: Optional[float] = None
    default: bool = False        # 无显式档位时的家族默认（raw12-4/raw16-4）

    @property
    def pix_fmt(self) -> str:
        """应用侧像素格式注册名（tier 参数 → _IMAGE_TIER_PIX_FMT）。"""
        return _IMAGE_TIER_PIX_FMT[(self.pixel_format, self.bit_depth,
                                    self.sample_kind)]

    @property
    def ratio_nominal(self) -> Optional[int]:
        """RAW 档名义比率 N（`Topos RAW 12-bit N:1`）；非 RAW 家族 None。"""
        if self.family != "raw":
            return None
        return int(self.tier_id.rsplit("-", 1)[1])

    @property
    def file_label(self) -> str:
        """文件内展示名（TMET label；N-4/N-5/N-6 形式）。

        444 家族不入质量阶梯且与 422 同 tier id 域——文件内不落 label
        （仅 vendor，与 image_cli 现行为一致），读端回退 image_profile
        类名 'Image HQ'。
        """
        if self.family == "int422":
            quality = self.tier_id.split("-", 1)[1].capitalize()
            return f"toos {quality} {self.bit_depth}bit"
        if self.family == "float":
            quality = self.tier_id.split("-", 1)[1].capitalize()
            return f"toos {quality} {self.bit_depth}bit float"
        if self.family == "raw":
            # N-6：图片家族小写前缀 + 位深无连字符；比率档位名承载 N:1
            return f"toos RAW {self.bit_depth}bit {self.ratio_nominal}:1"
        return ""


# 图片质量阶梯的 TMET tier id（topos_meta.TOPOS_IMAGE_TIER_IDS 同源；
# 此处 import 会造成循环依赖——该表与本注册表共享同一词表，E4 测试
# 交叉钉住两侧一致）
TOPOS_IMAGE_TIER_IDS = {"low": 1, "medium": 2, "high": 3, "ultra": 4}

# 22 档全表（顺序 = CLI kTierPresets / manifest 序；UI 展示序用
# available_topos_image_tiers() 的分组排序）。
TOPOS_IMAGE_TIERS: Dict[str, ToposImageTier] = {
    t.tier_id: t
    for t in (
        # —— 整数 422 家族（ADR-I011 修订四；10-bit，Tier id 1..4）——
        ToposImageTier(
            tier_id="422-low", label="Topos 422 Low", family="int422",
            image_profile=0, pixel_format=0, bit_depth=10, qmatrix=3, qp=63,
            tm_tier_id=1, anchor_mb_per_frame_1080p=1.08),
        ToposImageTier(
            tier_id="422-medium", label="Topos 422 Medium", family="int422",
            image_profile=0, pixel_format=0, bit_depth=10, qmatrix=1, qp=61,
            tm_tier_id=2, anchor_mb_per_frame_1080p=1.29),
        ToposImageTier(
            tier_id="422-high", label="Topos 422 High", family="int422",
            image_profile=0, pixel_format=0, bit_depth=10, qmatrix=1, qp=58,
            tm_tier_id=3, anchor_mb_per_frame_1080p=1.71),
        ToposImageTier(
            tier_id="422-ultra", label="Topos 422 Ultra", family="int422",
            image_profile=0, pixel_format=0, bit_depth=10, qmatrix=0, qp=58,
            tm_tier_id=4, anchor_mb_per_frame_1080p=2.86, default=True),
        # —— 整数 444 家族（修订六 2026-09-12：GBR 10-bit，ProRes 4444
        # 同位深口径；不入质量阶梯，tm_tier_id=0）——
        ToposImageTier(
            tier_id="444-high", label="Topos 444 High", family="int444",
            image_profile=1, pixel_format=2, bit_depth=10, qmatrix=1, qp=63,
            anchor_mb_per_frame_1080p=1.30),
        ToposImageTier(
            tier_id="444-ultra", label="Topos 444 Ultra", family="int444",
            image_profile=1, pixel_format=2, bit_depth=10, qmatrix=0, qp=58,
            anchor_mb_per_frame_1080p=3.40),
        # —— 浮点四档（v1.8 ADR-C057：GBR444 float16，pf2/bd16/profile4
        # + qm0 + linear；与整数阶梯共用 Low..Ultra tier id 1..4，浮点域
        # 由 IDSC sample_kind=HALF 自描述。锚点 = spec §15.3 三素材均值：
        # qp44≈5.75 / qp58≈4.45 / qp72≈0.99 / qp82≈0.38 MB）——
        ToposImageTier(
            tier_id="float-low", label="toos Low 16bit float", family="float",
            image_profile=4, pixel_format=2, bit_depth=16, qmatrix=0, qp=82,
            sample_kind="float", tm_tier_id=1, anchor_mb_per_frame_1080p=0.38),
        ToposImageTier(
            tier_id="float-medium", label="toos Medium 16bit float",
            family="float", image_profile=4, pixel_format=2, bit_depth=16,
            qmatrix=0, qp=72, sample_kind="float", tm_tier_id=2,
            anchor_mb_per_frame_1080p=0.99),
        ToposImageTier(
            tier_id="float-high", label="toos High 16bit float", family="float",
            image_profile=4, pixel_format=2, bit_depth=16, qmatrix=0, qp=58,
            sample_kind="float", tm_tier_id=3, anchor_mb_per_frame_1080p=4.45),
        ToposImageTier(
            tier_id="float-ultra", label="toos Ultra 16bit float",
            family="float", image_profile=4, pixel_format=2, bit_depth=16,
            qmatrix=0, qp=44, sample_kind="float", tm_tier_id=4,
            anchor_mb_per_frame_1080p=5.75),
        # —— TRAW 12-bit log 制作档家族（profile 7 / pf3 / qm0 冻结 +
        # transfer=TRAW_LOG0；qp 锚 = 试点 UHD 实测效率点；锚体积 = 名义
        # 比率反推（16-bit 容器名义 12.44 MB/1080p 帧 ÷ N））——
        ToposImageTier(
            tier_id="raw12-2", label="Topos RAW 12-bit 2:1", family="raw",
            image_profile=3, pixel_format=3, bit_depth=12, qmatrix=0, qp=45,
            anchor_mb_per_frame_1080p=2.07),
        ToposImageTier(
            tier_id="raw12-4", label="Topos RAW 12-bit 4:1", family="raw",
            image_profile=3, pixel_format=3, bit_depth=12, qmatrix=0, qp=59,
            anchor_mb_per_frame_1080p=1.04, default=True),
        ToposImageTier(
            tier_id="raw12-6", label="Topos RAW 12-bit 6:1", family="raw",
            image_profile=3, pixel_format=3, bit_depth=12, qmatrix=0, qp=64,
            anchor_mb_per_frame_1080p=0.69),
        ToposImageTier(
            tier_id="raw12-8", label="Topos RAW 12-bit 8:1", family="raw",
            image_profile=3, pixel_format=3, bit_depth=12, qmatrix=0, qp=66,
            anchor_mb_per_frame_1080p=0.52),
        ToposImageTier(
            tier_id="raw12-12", label="Topos RAW 12-bit 12:1", family="raw",
            image_profile=3, pixel_format=3, bit_depth=12, qmatrix=0, qp=68,
            anchor_mb_per_frame_1080p=0.35),
        ToposImageTier(
            tier_id="raw12-16", label="Topos RAW 12-bit 16:1", family="raw",
            image_profile=3, pixel_format=3, bit_depth=12, qmatrix=0, qp=70,
            anchor_mb_per_frame_1080p=0.26),
        # —— TRAW 16-bit linear 归档家族（批 4；锚 qp 见 traw plan 死区
        # 修复：raw16-2 存 qp51 饱和值）——
        ToposImageTier(
            tier_id="raw16-2", label="Topos RAW 16-bit 2:1", family="raw",
            image_profile=3, pixel_format=3, bit_depth=16, qmatrix=0, qp=51,
            anchor_mb_per_frame_1080p=2.07),
        ToposImageTier(
            tier_id="raw16-4", label="Topos RAW 16-bit 4:1", family="raw",
            image_profile=3, pixel_format=3, bit_depth=16, qmatrix=0, qp=72,
            anchor_mb_per_frame_1080p=1.04, default=True),
        ToposImageTier(
            tier_id="raw16-6", label="Topos RAW 16-bit 6:1", family="raw",
            image_profile=3, pixel_format=3, bit_depth=16, qmatrix=0, qp=76,
            anchor_mb_per_frame_1080p=0.69),
        ToposImageTier(
            tier_id="raw16-8", label="Topos RAW 16-bit 8:1", family="raw",
            image_profile=3, pixel_format=3, bit_depth=16, qmatrix=0, qp=78,
            anchor_mb_per_frame_1080p=0.52),
        ToposImageTier(
            tier_id="raw16-12", label="Topos RAW 16-bit 12:1", family="raw",
            image_profile=3, pixel_format=3, bit_depth=16, qmatrix=0, qp=80,
            anchor_mb_per_frame_1080p=0.35),
        ToposImageTier(
            tier_id="raw16-16", label="Topos RAW 16-bit 16:1", family="raw",
            image_profile=3, pixel_format=3, bit_depth=16, qmatrix=0, qp=82,
            anchor_mb_per_frame_1080p=0.26),
    )
}

# UI 分组展示序（家族升序、家族内质量/比率升序）；M2/M3 逐批点亮应用入口，
# 全表先于应用注册（E4 回归从本表枚举）。
AVAILABLE_TOPOS_IMAGE_TIERS: Tuple[str, ...] = (
    "422-low", "422-medium", "422-high", "422-ultra",
    "444-high", "444-ultra",
    "float-low", "float-medium", "float-high", "float-ultra",
    "raw12-2", "raw12-4", "raw12-6", "raw12-8", "raw12-12", "raw12-16",
    "raw16-2", "raw16-4", "raw16-6", "raw16-8", "raw16-12", "raw16-16",
)


def get_topos_image_tier(tier_id: str) -> ToposImageTier:
    """按 id 查询图片档位；未知 id 抛 KeyError（显式失败，不猜测）。"""
    try:
        return TOPOS_IMAGE_TIERS[str(tier_id).strip().lower()]
    except KeyError:
        known = ", ".join(TOPOS_IMAGE_TIERS)
        raise KeyError(
            f"Unknown Topos image tier: {tier_id!r} (known: {known})"
        ) from None


def get_topos_image_tier_or_none(tier_id: Optional[str]) -> Optional[ToposImageTier]:
    """宽松查询：None/未知 → None（调用方自行决定默认或报错）。"""
    if not tier_id:
        return None
    return TOPOS_IMAGE_TIERS.get(str(tier_id).strip().lower())


def available_topos_image_tiers() -> List[ToposImageTier]:
    """全部图片档位（UI 分组展示序）。"""
    return [TOPOS_IMAGE_TIERS[t] for t in AVAILABLE_TOPOS_IMAGE_TIERS]


def default_topos_image_tier_id(family: str = "int422") -> str:
    """家族默认档：整数 422-ultra（CLI 无 --tier 默认）、raw12-4/raw16-4。"""
    for t in TOPOS_IMAGE_TIERS.values():
        if t.family == family and t.default:
            return t.tier_id
    raise KeyError(f"family {family!r} 无默认档")


def topos_image_file_label(tier_id: str) -> str:
    """tier id → TMET/文件内展示名（N-4/N-5/N-6 形式；444 家族空串）。"""
    return get_topos_image_tier(tier_id).file_label
