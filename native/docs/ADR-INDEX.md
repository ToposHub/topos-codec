# Topos Video Codec — ADR 索引与当前决策 Roll-up（O5）

> 维护规则：新增 ADR 必须入本索引；被取代的决策在原文件头加
> `superseded-by: <file>` 并在本表「当前决策」列指向取代者。
> 每个主题**有且仅有一条当前决策**（复验 P2-07：旧结论不得继续被当作现状）。
> 机器校验：`tests/media/test_capability_manifest.py::TestAdrRollup`。

| ADR | 主题 | 当前决策（roll-up） |
| --- | --- | --- |
| ADR-C001-stage0-decisions.md | 阶段 0 决策冻结 | 现行（C11/C17 取消模型等被后续细化） |
| ADR-C002-stage1-2-transform.md | 阶段 1–2 变换/量化 | 现行 |
| ADR-C003-stage3-bitstream.md | 阶段 3 bitstream v1 冻结 | 现行 |
| ADR-C004-stage4-codec.md | 阶段 4 完整 codec | 现行（sized 搜索由 C022 优化） |
| ADR-C005-stage5-container.md | 阶段 5 容器路线 spike | **被取代** → ADR-C011（自研 MOV 终案） |
| ADR-C006-stage6-app-integration.md | 阶段 6 应用接入 | 现行 |
| ADR-C007-stage7-alpha-hdr.md | 阶段 7 Alpha/HDR 显示链 | 现行（导出链缺陷由 O1 修复，非本 ADR 范畴） |
| ADR-C008-stage8-export-integration.md | 阶段 8 导出接入 | 现行 |
| ADR-C009-stage9-simd-parallel.md | 阶段 9 SIMD/并行 | 现行（GPU 否决的重评条件在案；池化见 C022） |
| ADR-C010-stage10-robustness-distribution-sdk.md | 阶段 10 健壮性/分发/SDK | 现行 |
| ADR-C011-r0-scope-freeze.md | R0 范围冻结/命名/自研 MOV | 现行（六档扩展后的一致性由 O5 manifest 约束） |
| ADR-C012-r1-data-correctness.md | R1 数据正确性 | 现行（时间线边界缺陷由 O1 补关） |
| ADR-C013-r2-timeline-alpha-hdr.md | R2 时间线 Alpha/HDR | 现行（P0-02/03/04 修复在 O1 提交内） |
| ADR-C014-r3-alpha-budget.md | R3 Alpha 预算 tpcB | 现行（六档扩围+饱和标志在 O2 提交内） |
| ADR-C015-r4.1-12bit.md | R4.1 12-bit 枚举 | 现行 |
| ADR-C016-r4.2-444.md | R4.2 4:4:4 枚举 | 现行 |
| ADR-C017-r4.3-gbr.md | R4.3 GBR 枚举 | 现行 |
| ADR-C018-r4.4-profiles.md | R4.4 profile 5/6 激活 | **部分被取代** → ADR-C023（"独立矩阵"预期定案为矩阵不独立） |
| ADR-C019-r4.5-simd-parity.md | R4.5 SIMD 差分扩展 | 现行 |
| ADR-C020-r4.6-negotiation.md | R4.6 六档协商 | 现行（全入口收口在 O2 提交内） |
| ADR-C021-r5-mov-metadata.md | R5 MOV 支持矩阵/oracle | 现行（mdat/containment 收紧在 O3 提交内） |
| ADR-C022-r6-perf-thresholds.md | R6 性能门槛 v2 + 池化 | 现行（产品实时性口径修订见 C024 D-5） |
| ADR-C023-o2-profile-matrix-decision.md | O2 档位差异化定案（矩阵不独立） | **当前** |
| ADR-C024 | O4 并发正确性 + perf 门禁数值化 | **当前** |
| ADR-C025-r7-release-closure.md | R7 CI/跨平台/发布闭环（required 化 + 发布清单 + 产品包坏包修复） | **当前** |
| ADR-C026-m8-acceptance-gates.md | M8 V1 总验收：正式 Go/No-Go 门冻结（G1–G6）+ 判定启动 M9 | **当前** |
| ADR-C027-v2-schema-reconciliation.md | V2 schema reconciliation：major=2 字段注册表唯一真相源（VLC/Micro-GOP 归属裁决） | **当前**（V2 布局以此为准） |
| ADR-C028-encode-selfcheck-crc-fold.md | 编码自检 CRC 折进组装拷贝（§11.5 校验时序重构） | 已实施并按门 4 整案回退（负结果+PCLMUL 推导归档附录） |
| ADR-C029-v7a-band-directory.md | V7-A coefficient-band 目录与可跳过 segment | **已归档** → ADR-C046（V7-A 归档 2026-09-13：生产构建零 V7-A 面；回放需 TOPOS_DEV_REPLAY 构建 + TOPOS_DEV=1） |
| ADR-C030-v7b-shared-transform-pyramid.md | V7-B minor-4 共享分析金字塔（lifting 算术/级联/逃逸通道冻结） | **当前**（规范冻结；scalar 参考 P1-02、写入器 P2、门禁 P4 交付） |
| ADR-C031-qp-domain-extension-v15.md | v1.5 qp 值域扩展 0..95（minor=4；Proxy/LT 低码率档 + fastdiv 域修复） | **当前** |
| ADR-C032-tier-bitrate-realignment-p5.md | P5 产品档位码率对齐重定标（ProRes 实测体积；产品层 qp 域 0..95 打通） | **当前** |
| ADR-C033-video-422-matrix-flat.md | P5.1 422 视频档矩阵切换 flat（同码率 PSNR +0.2~1.6 dB；矩阵表零变更） | **当前** |
| ADR-C034-rans-entropy-v2r.md | V2-R per-slice 自适应 rANS 熵编码（major=7/entropy 6；规范 bitstream_spec_v7r_rans.md） | 验收门全过（体积 −8.7/−9.1%，解码 1.14×/1.19× V2）；**代际已退役** → ADR-C046（默认先由 C035 切入、后被 C038 rans2 取代） |
| ADR-C035-rans-product-default.md | V7-R rANS 转产品默认熵（m7 精确探针 + 444 差分覆盖随附） | **被取代** → ADR-C038（rans2 转默认）；V7-R 代际已退役 → ADR-C046 |
| ADR-C036-v7r-context-models.md | V7-R2 order-1 上下文扩展（entropy=7；lvl/dc 条件模型 per-slice 信令；测量含两次勘误） | 已实施（−1.2~2.8% vs V7-R，解码 ~1×；'rans2' 显式可选，默认切换由 C038 定案） |
| ADR-C037-t16-transform-pilot.md | 16×16 变换试点（Haar2⊗M8 混合构造 + 三方 RD 测量；发现产品工作点处于无损平台） | NO-GO（真实素材主域 +9~13% 码率；报告 docs/codec/topos_t16_pilot_2026-09-11.md） |
| ADR-C038-rans2-product-default.md | V7-R2 'rans2' 转产品默认熵（tier 级复验：hq 同目标 +1.05dB / 固定 qp −1~2.2%）+ 死区加宽否决（P3 前扫频 + spec §7.4 冻结）+ TC_DZ_DEV 旋钮移除 + C037 tier 域勘误 | 已采纳（回归全绿） |
| ADR-C039-aq-sized-probe-v15.md | AQ sized 探针 v1.5 域修复（m7_probe 变体钳位源 fh.qp_base→候选 qp；战役遗留项 2 收口，qp≥64 域 AQ 不可用改判可用） | 已实施（回归钉判别性验证；debug/ASan 79/79） |
| ADR-C040-rdo-qp-gating-evaluation.md | RDO 按 qp 域门控启用评估（战役项 6 收口）：收益带=[无损悬崖,qp61]，产品语境六格全负 → 默认关维持、不实施 native 门 | 评估完结（默认关维持） |
| ADR-C041-rans-interleave-feasibility.md | rANS 交错多状态可行性（负结果）：交错 NO-GO（寄存器同驻 4-lane 慢 24%）、寄存器驻留系统反转（编译器已完成 SROA）→ 解码符号环定性达平台 | 评估完结（零代码入库） |
| ADR-C042-444-family-flat-matrix-and-rate-alignment.md | 444 家族 flat 矩阵验证 + pro444/extreme 码率对齐（战役项 4 收口）：flat 六点全胜 Standard（+0.3~+3.2 dB）、色度偏移归零、bpp 4.11→5.585/6.76→7.974（prores_ks 4444/XQ 实测锚定）；444 对齐口径反超 +3.4~+6.5 dB | 已采纳（profiles 138/138） |
| ADR-C043-tier-bpp-iso-quality-recalibration.md | 六档目标码率重定标：ProRes 同档码率→同档画质（保守规则=最差格等画质比×1.05；−4~24% 文件、编码 +4~29%、解码无感[固定成本主导]） | 已采纳（138/138） |
| ADR-C044-rans2-encode-fastdiv.md | rans2 编码环免 idiv（速度计划 S4）：per-model per-symbol round-up magic（fastdiv.h 域推广证明 x·e<2^43≪2^51），后向环 4 put 点换 tc_rans_put_fast | 已实施（位流逐字节不变；A/B 1t +8.6~9.3% / 16t +4.1~6.0%） |
| ADR-C045-rans2-backward-specialize.md | rans2 后向环按 lvl_model 专化 + 值语义 put 引擎（速度计划 S5 一期）：x/fd 值传递绕 char-store 栈重载、argmin 比较出环、pos_block 仅 POS；P0"38% 桶"勘误（rows_cost <1%，主体=后向环自体）；二期 collect 融合负结果（+1.2/−4.7/−4.7% 未过门，回退零入库） | 已实施一期（位流逐字节不变；A/B 1t +13.0~15.2% / 16t +8.7~10.2%，4K 444 ≈1.08× 实时） |
| ADR-C046-bitstream-generation-lifecycle.md | V 代际收纳与位流代际生命周期（2026-09-13）：写域收敛 reserved[0]∈{0,1,8,9}（V1/V2-VLC/V7-R2/V8），6 退役代读端 UNSUPPORTED_VERSION（em 编号封存），V7-A 归档 TOPOS_DEV_REPLAY 双重门，保留代 golden 零变化硬门 + 版本预算纪律（含 D4 V8 切换合并记录） | **当前** |
| ADR-C047-lp-spike-m1.md | LP Spike M1 拍板（2026-09-14）：V9 只做 zero-motion IP-2（全库 LT 锚中位 6~11% 未达 20% 门；静止/屏幕类 16~35% 达标；MV 探针上界 +0~10% 且横摇为负）→ 批 4 P1 取消、批 6 LP 按 §7 实测评审（预期 experimental）、批 7 IP-4 降级可选；双编码择优确认（切镜帧 ρ1.35 回退按设计工作） | **当前**（V9 窗口产判据） |
| ADR-C048-lp-v7r3-recarriage.md | LP 载体更换（2026-09-13）：帧间微 GOP 从 V8 同构换为 V7-R2 同构子代际 V7-R3 (major 7, em 8, cfg em=11)——V8 载体编码 ~10× 慢 + 同 qp 体积代差 1.04~1.33× 实测；V9 冻结保留读端，不耗 major 10；§7 体积门不重赛 | **当前**（LP 重启底座） |
| ADR-C049-lp-encode-policy-mad-gate.md | LP 编码器策略（2026-09-14）：P 白编预检门（残差 MAD ≥ 1<<(bd−6) 跳过 P 尝试直编 I，校准 P 胜层 ≤5.5 / 全回退层 ≥32.5 两侧 ≥2× 边际；门控 I ≡ 回退 I 逐字节，golden 零变化）+ 全零残差精确跳过（X≡0⇒X′≡0⇒recon=ref）+ 包缓冲 ctx 常驻；纯编码器侧策略，格式/解码端不变 | **当前**（LP 实时化第二步） |
| ADR-C050-lp-gop-segment-parallel.md | LP GOP 分段并行编码（2026-09-14）：应用层按 gop_size 切段 + 段级线程池并行（每段独立 context 段首强制 I；w1≡w4 逐字节确定性；主线程有序回填 mux/pts/预算）；≤2K 默认开 fps×2 帧、4K 显式 opt-in；绑定出包缓冲 8MB 起步按 BTS 增长；流格式/读端零变更。E2E qp72：2K 50.5→107.7 fps（2.1×）、4K 56.4→79.1 fps | **当前** |
| ADR-C051-container-v1.1-audio-track.md | 容器 v1.1 音频轨（2026-09-19）：解冻 §10"音频非目标"——单轨 lpcm/mp4a（mux+demux+绑定+导出接线+规范附录 A）；AAC 编码在应用层（native 只存包）；音频声明复用 movie_config reserved 槽位（零 ABI 变更，纯视频文件字节不变）；回放缓存/代理仍纯视频（ADR-C008 不变）；GOP 分段并行 + 音频显式拒绝 | **当前** |
| ADR-C052-container-v1.4-audio-elst-priming.md | 容器 v1.4 音频 elst 最小子集 + mp4a 声样描述互操作修正（2026-09-20）：解冻 §10 edit list 音频 trak 单条目（AAC priming 对齐；视频 trak 仍禁）；priming=编码器自报值（FFT 校准实证端到端延迟恰=1024，elst 后残差 0，推翻计划期"自报≠端到端"假设）；M-B0 发现的 esds 规范编码/wave 包裹缺陷一并修正（CoreAudio/AVFoundation 由拒转为全绿）；规范升 v1.4（附录 A.2/A.5） | **当前** |
| ADR-C053-segment-parallel-audio-roadmap.md | GOP 分段并行与音轨共存路线（2026-09-20）：采纳"段外独立 pass + native moov 重写追加轨核心件"（实现另立计划）；过渡期维持启动期 fail-fast 并改写诊断文案指向本 ADR；验收锚点钉死（golden_mov_audio 字节形态 + 双引擎识别 + 吞吐不回退） | **当前** |
| ADR-C054-container-v1.6-multitrack-stems.md | 容器 v1.6 多音轨/stems 角色轨（2026-09-20）：音频 trak ≤16 声明序（video=1/audio=2..N+1/tmcd=N+2 恒末轨）；逐轨声明 API（校验与 reserved 槽位同一规则）+ mono 布局解冻 + 轨名 trak/udta/©nam；旧单轨 API 恒轨 0 视图（v1.1 兼容专项测试）；faststart 逐轨重定位；两步走第一步纯重构由 golden 字节冻结钉死；导出侧角色映射 UI 留后续（ADR §4 显式记录） | **当前**（容器/绑定/探测层已实施） |
| ADR-C055-container-v1.5-tmcd-timecode.md | 容器 v1.5 tmcd 时间码轨（2026-09-20）：专业中间片起始时码——moov 可带末位 tmcd trak（单样本 4B 起始帧计数）+ 视频 trak tref；轨序钉死 video=1/audio=2..N+1/tmcd 恒末（M-B8 轨 id 规则就此锁定）；DF 跳帧语义 60DF=4/30DF=2（oracle 实测 + media 层 Timecode 同规）；stsd entry 36B（dri 后保留 u32，oracle 对拍修正）；导出有则写/无则不写零变化；规范升 v1.5（附录 A.6） | **当前** |
| ADR-C056-lp-product-tier.md | LP 产品化——Topos 422 LP 帧间档注册（2026-09-21）：tier 7 = TC_TIER_LP（tpcD/TMET 域 0..6→0..7，规范升 v1.8；纯域扩展 golden 不变）；tier⟹载体（选档即 gop='ip2'，V7-R3）；显式收缩 no-alpha（alpha_supported=False）/无 AQ/RDO/固定锚 qp=72（ADR-C050 锚，显式 crf 覆盖优先）；音频×分段互斥 → lp+include_audio 显式单 GOP（ADR-C053 共存另立）；manifest 先行逐面对齐 + P 帧提取拒绝（不可独立解码不落盘） | **当前** |
| ADR-C057-float-product-ladder.md | 浮点档产品化——float 四档质量阶梯与命名（2026-09-21）：弃 HF 词，预设收敛 float-ultra qp44（视觉透明 <EXR zip16）/high qp58/medium qp72/low qp82，TMET 展示名 toos <Quality> 16bit float；真无损不入阶梯（--half --qp 20 显式）；tier id 与整数阶梯共用 1..4，浮点域由 IDSC sample_kind 自描述（未来 FLOAT32 同词表换位深）；TMET 按最终参数回配（pf2+qm0+qp 组合与整数域不碰撞） | **当前** |
| ADR-C058-video-raw-product-tier.md | 视频 RAW 产品化——Topos RAW 产品档注册（2026-09-21）：tier 8 = TC_TIER_RAW（tpcD 域 0..7→0..8，规范升 v1.9；纯域扩展 golden 不变，LP 同型先例）；单 tier + 参数化（位深×比率 12 档 D3 拍板，锚 qp 与图片 raw12-*/raw16-* 共表，比率=CQ 定位标签）；全 I 帧 + no-alpha + 拒 AQ/RDO（fail-fast）；输入域 topos_cfa12le/16le 相位平面直入；MOV trwm 原子携 as-shot 元数据（RC1）；交付双路径（R2 RAW→RAW 直出 fail-closed / RC6 debayer 再编码） | **当前** |
