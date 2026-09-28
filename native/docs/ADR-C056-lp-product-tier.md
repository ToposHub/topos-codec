# ADR-C056：LP 产品化——Topos 422 LP 帧间档注册（tier 7）

日期：2026-09-21 · 状态：**已采纳**
关联：ADR-C047（IP-2 范围）、ADR-C048（V7-R3 载体）、ADR-C049（编码策略）、
ADR-C050（GOP 分段并行）、container_spec v1.7→v1.8（tpcD tier 域扩展）

## 1. 背景

LP（帧间微 GOP，zero-motion IP-2）自 V9 窗口交付以来只是编码器内部能力：
`FFmpegEncoderConfig.gop='ip2'` 实验开关 + `ToposGopContext` 绑定面，不在
导出枚举/能力清单/容器档位表中的任何一处。ADR-C048 明确"LP 是否注册
产品档是独立的产品决策"。读侧（ToposMediaSource 的 `v9_micro_gop` GOP
context 路径，V9 批 5）与 mux 链校验（多 GOP）均已就绪，产品化的剩余
缺口只在"档位注册"本身。

## 2. 决定

**D1（档位注册）**：新增 **Topos 422 LP**（tier id `'lp'`，容器 tier_id
**7 = TC_TIER_LP**）：

- 像素域：YUV 4:2:2 10-bit 唯一（帧头 profile 3，载体 V7-R3 = major 7,
  em 8）——帧间性由包头自描述，tier 只承载档位语义；
- **tier ⟹ 载体**：应用层选 lp 档即强制 `gop='ip2'`（tier 是产品语义，
  gop 只是实现开关；lp 不存在帧内形态）；
- 显式收缩（fail-fast，禁静默降级）：**no-alpha**（§3.7；alpha_supported
  = False，UI 隐藏控件 + 编码器/导出链双重拦截）、无 AQ/RDO、无码控；
- **固定锚 qp = 72**（ADR-C050 基准锚，体积带 ≈ LT ×(1−6~11%)，
  ADR-C047 中位）——调用方显式 crf/qp 覆盖仍拥有最高优先级（质量旋钮
  语义 = 锚 qp 覆盖）。

**D2（容器）**：tpcD tier_id 域 0..6 → **0..7**（写侧 setter + 读侧
parse 同步放宽），container_spec 升 **v1.8**；图片 TMET 载荷同构域同步
（TIER_ID_MAX = 7）。golden / 既有产物字节不变（纯域扩展，未声明行为
不变）。LP 源帧提取（.toos 单帧导出）转发 tier=7；P 帧提取显式拒绝
（无状态解码入口本就 TC_ERR_STATE——不拦截会产出"写成功但永远解不开"
的图片，属静默数据丢失）；I 帧不受影响。

**D3（音频 × 分段）**：GOP 分段并行与音轨声明互斥（ADR-C053 共存路线
未落地）——导出链对 lp + include_audio 显式 `gop_size=0`（单 GOP 保音频
可用；分段收益集中在 ≤2K 摇移类素材，静音时间线不受影响）。

**D4（注册面全链）**：单一真相源 `capability_manifest.json` 先行
（tiers + ui_codec_ids），随后 topos_profiles / ExportCodec /
Deliver 面板目录 / Quick Export 容器表 / 容器矩阵 / job & controller
codec 映射 / i18n（codec_topos_lp）逐面对齐（manifest 一致性门即红→绿）。

## 3. 理由

- 读侧/容器/分段并行/实时化全部已交付，产品化是纯注册工作，边际风险低；
- tier 与帧间性正交设计（D1）避免给 tier 语义引入"必须查包头才知道
  能不能随机访问"的耦合——随机访问性由读侧 GOP 路径统一处理；
- 锚 qp 而非码控（D1）：ip2 无 sized 搜索路径（P/I 决策与残差合成在
  原生侧），引入码控等于新编 ADR；72 锚的体积/实时特性已有 ADR-C050
  实测背书。

## 4. 被否备选

- **注册为 profile 3 的第五个帧内预设**：语义错误——LP 是帧间档，
  同 bpp 对比无意义，且 no-alpha/无码控与其余四档能力面冲突。
- **暴露 gop='ip2' 为独立 UI 开关**（帧内六档 × 帧间切换）：能力组合
  爆炸（444/12-bit/HF × 帧间 = 未验证域），且 4K 帧间收益不稳定；
  单档注册 + 显式收缩更诚实。
- **音轨走 ADR-C053 立即落地**：范围失控；D3 的显式单 GOP 是零风险
  过渡，段收益损失可接受。
