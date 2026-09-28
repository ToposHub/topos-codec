# ADR-C042：444 家族 flat 矩阵验证 + pro444/extreme 码率对齐（战役 item 4 收口）

- 状态：**已采纳**（2026-09-11）
- 范围：`src/shared/codec/topos_profiles.py`（pro444/extreme 两档）；编解码位流零变更
- 前置：ADR-C032（P5 422 码率对齐）、ADR-C033（P5.1 422 切 flat）、ADR-C038（rans2 默认）
- 证据：`tools/align444_{2k,4k}_{10bit,12bit}_2026-09-11.json`（harness
  `tools/bench_444_align.py`）

## 1. 背景

战役 item 4：444 两档自 P4 起带两处"待验证"状态——

1. `qmatrix_id=1`（Standard），注释"444 暂保持 Standard（未验证 flat）"。
   422 家族已在 ADR-C033 用真实素材切 flat（+0.2~1.6 dB），444 因无
   4:4:4 素材锚点未跟进；
2. `target_bpp`（pro444 4.11063 / extreme 6.76296）系 P4 旧定标
   （ratio 推算 / qp63 顶格逐帧钉扎），ADR-C032 的 P5 重定标只覆盖
   422 四档（当时无 ProRes 4444 实测锚点）。

2026-09-11 用户补齐真实 4:4:4 素材（Apple ProRes 4444/4444XQ 全片 +
Avid DNxHR 444 10/12bit，2K/4K 同一母带），本 ADR 收口两项。

## 2. 实验设计

- **共享源**：DNxHR 444 解码（10bit/12bit 轴，2K+4K 各 16 帧）。
  选 DNxHR 而非 ProRes 4444/XQ 解码作源：chroma 实测全分辨率细节
  （横向梯度 U/Y = 0.89~0.90；XQ 解码仅 0.16~0.29——其编码链已抹平
  chroma 高频，作源会把 444 轴难度测虚）；
- **锚点**：ffmpeg prores_ks profile 4444 / 4444xq（ADR-C032 同口径），
  Apple 自家编码全片实占作交叉验证；
- **Topos**：产品码控路径（首帧 sized + `ToposRateFeedback` 反馈单遍、
  rans2、profile 5/6、pf=1、qp 域 0..95），按锚点实测码率对齐比较；
- **矩阵臂**：flat(0) vs Standard(1)（产品 `qp_delta_chroma=4`）；
  另设 flat+dc0 验证臂（tier 的 +4 系 P2 随 Standard 选定）；
  注：qmatrix_id=2（444Compact）被帧头规则限定 GBR+12bit，YUV 444
  档不适用，不在 A/B 内；
- PSNR 峰值按位深（1023/4095），合成 = 三平面 MSE 均值（444 像素
  份额 1/3:1/3:1/3）。

## 3. 结果

### 3.1 矩阵：flat 六点全胜（对齐码率，合成 PSNR，dB）

| 对齐点 | 码率 | flat | Standard | Δ |
|---|---|---|---|---|
| 2K10 @4444 | 5.61 bpp | 50.61 | 48.94 | **+1.67** |
| 2K12 @4444 | 5.61 bpp | 50.22 | 48.97 | **+1.25** |
| 2K12 @XQ | 8.35 bpp | 57.99 | 54.74 | **+3.25**（字节还少 4.5%） |
| 4K10 @4444 | 5.55 bpp | 59.16 | 58.86 | **+0.30** |
| 4K12 @4444 | 5.56 bpp | 59.40 | 58.96 | **+0.44** |
| 4K12 @XQ | 7.60 bpp | 64.29 | 63.13 | **+1.16** |

档位落点同向：pro444@4.11bpp flat +0.41~+0.69 dB（四点）、
extreme@6.76bpp flat +1.42~+2.35 dB（两点）。Standard 无一胜点。
结论与 ADR-C033 的 422 实测一致——**频率加权矩阵在 DCT+rANS 管线
上系统性劣于 flat，444 不例外**。

### 3.2 色度偏移：dc0 六点全胜（flat 臂内）

dc0 vs dc4 合成 +0.03~+1.75 dB（最大点 4K12@XQ：66.04 vs 64.29）。
dc4 把预算偏给 Y（+4~6 dB Y）但 chroma 反输 1~5 dB；444 工作流语义
（VFX/键控/合成 = chroma 保真）与 422 家族 tier 全 0 的惯例都指向
归零。chroma 贫内容下 dc4 的 Y 收益随 chroma 变便宜而消失，dc0 是
内容鲁棒选择。→ `chroma_qp_offset` 4→0。

### 3.3 码率重定标（prores_ks 实测，ADR-C032 同口径）

| 档 | 旧 bpp | 实测锚点 | 新 bpp |
|---|---|---|---|
| pro444 | 4.11063 | 2K 5.6114/5.6123、4K 5.5520/5.5645 | **5.58505**（四点均值） |
| extreme | 6.76296 | 2K 8.3462、4K 7.6016（12bit） | **7.97390**（两点均值） |

交叉验证：Apple 自家编码全片实占 4444 = 5.22、XQ = 7.80 bpp
（prores_ks 口径热 ~7%/~2%；tier 定标沿用 prores_ks，与 422 家族
一致）。extreme 旧值的 qp63 顶格钉扎在 v1.5 qp 域 0..95 后不再构成
约束（新定标落点 qp 65~72）。

### 3.4 对齐质量差距：444 家族无差距，反超 +3.4~+6.5 dB

Topos flat（dc0）vs prores_ks 同码率：2K10 +4.95、2K12@4444 +4.66、
2K12@XQ +6.47、4K10 +4.94、4K12@4444 +5.34、4K12@XQ +3.39 dB。
422 家族尚有"高纹理 4K 落点亏"之争，444 家族（chroma 全细节内容 +
rans2 + flat）直接碾压。新定标落点复验：五点（2K/4K × 双档）落
0.991~1.016×，合成 50.35~67.14 dB。

### 3.5 落点轨迹

所有运行 qp 61~77，远离 95 天花板；反馈单遍贴目标，无 qp95 冲顶/
AQ guard 触发（ADR-C039 修复后路径未受压）。

## 4. 变更清单

1. `topos_profiles.py` pro444：`qmatrix_id` 1→0、`chroma_qp_offset`
   4→0、`target_bpp` 4.11063→5.58505（注释含实测锚点与 Apple 交叉值）；
2. `topos_profiles.py` extreme：同上三项 → 0/0/7.97390；
3. 位流、编码器、解码器、码控：零变更。

## 5. 验证

- profiles 相关测试 138/138（deliver/topos_controls、media/
  capability_manifest、media/topos_export）；
- 新定标五点落地复验（§3.3 末）：0.991~1.016×、可解码、域内 qp；
- 证据 JSON 四份入库（harness `bench_444_align.py` 可复现）。

## 6. 备注

- DNxHR 444 "variable ACT flag" 警告：ffmpeg 按固定 444 解码（ACT 自
  适应色度变换被忽略），解出流实测为真 444（chroma 细节满），不影响
  结论；
- GBR（pf=2）+ 444Compact（qm=2）路径未测（无 GBR 素材；帧头规则
  将 qm2 限定 GBR+12bit，产品 tier 未引用）——GBR 档矩阵验证留待
  GBR 素材锚点，不阻塞本项；
- 编码速度参考：4K 444 12bit 16 帧反馈码控全程 ~4s（含首帧 sized
  搜索 + 解码回读），与 422 同量级。
