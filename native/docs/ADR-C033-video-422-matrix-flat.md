# ADR-C033：422 视频档矩阵切换 flat（P5.1）

- **状态**：当前（2026-09-10）
- **范围**：产品层矩阵选择（`topos_profiles.py` + `topos_encoder.py`）；
  **native 矩阵表零变更**（flat/Standard/444Compact/422LowCompact 冻结表不动）
- **前置**：ADR-C004（Standard 矩阵的 HVS 设计）、ADR-C032（P5 码率对齐）
- **测量**：真实素材（2K/4K prores422HQ 各 120 帧）、V2 熵、P5 档位目标码率

## 背景与测量

ADR-C004 为 422 家族选定 Standard 矩阵（几何频率加权，HVS 导向）。
优化战役第 2 步在同码率下横向比较矩阵（V2 熵、sized 钉同目标）：

| 档位 | 2K flat−Standard | 4K flat−Standard |
|---|---:|---:|
| Proxy | +0.24 dB | +0.34 dB |
| LT | +1.02 dB | +0.42 dB |
| Standard | +1.13 dB | +0.59 dB |
| HQ | +1.61 dB | +1.06 dB |

**flat 全档占优，且码率越高增益越大**（PSNR 口径）。Standard 的高频粗量化
是 HVS/控噪取向，把码率从高能量低频挪走——PSNR 上不划算；V2 VLC 的熵
效率让 flat 的高频精细量化成本变得可负担（qp 略升即可钉住同码率）。

### 超越 flat 的搜索（未采纳）

几何族 luma=16·b_l^(u+v)、chroma=16·b_c^(u+v) 网格搜索
（b_l∈{0.85..1.08}，b_c∈{0.9..1.3}，4 代表格均值）：

- b_l=1.0 为亮度轴峰值（b<1 伤低码率档，b>1 是 Standard 方向已证差）；
- 色度轴微弱信号：b_c=1.1 均值 +0.12 dB（集中在 2K HQ +0.54）。

**不采纳**：+0.12 dB 不值得新增冻结枚举面（新 qmatrix_id + spec + 全
解码器矩阵表）；flat（id=0）即可拿到 95% 收益。

## 决策

1. `ToposProfileTier` 新增 `qmatrix_id` 字段；**proxy/lt/standard/hq
   = 0（flat）**，编码器 movie_config/frame_config 同源取值；
2. **pro444/extreme 保持 1（Standard）**——无 4444 真实素材锚点，flat
   在 444 未验证（后续补测再定）；
3. 图片档 P2 选择（id=2/3）不受影响；CLI 显式 `--qm` 不变；
4. capability summary 暴露 `qmatrix_id`（显式可查询）。

## 兼容性与后果

- **零格式风险**：qm=0 是最老的字段值，任何历史解码器（含 v1.0 前身）
  都可解码；四张冻结矩阵表逐字节未动，老码流解码头也不动。
- 切换后 P5 档位的输出字节/质量变化（这是目的）：V2 口径 2K 四档
  PSNR 全部追平/反超 ProRes（+0.2~+1.5 dB vs ProRes 实测），
  4K 差距缩至 −0.9~−2.7 dB（见报告 §五）。
- 解码速度持平或略优（flat 同码率 qp 更高 → 零系数更多；4K 实测
  +4~10%）。
- **RDO 咬合点漂移**：flat 下逐系数 level 精修的净收益点随内容波动
  （合成小夹具在 qp44/48 恰净零）；真实素材上仍 +0.13% 字节差。
  `test_rdo_mode_default_off_optin_changes_bytes` 夹具改种子噪声 +
  crf 34→50（功能未变，仅夹具区分度）。
- TPIC 代理（proxy 档）输出字节/质量随之变化。

## 复现

```bash
# 同码率矩阵对比（V2 熵；--qm 0/1/3）
TOPOS_SLICE_THREADS=16 topos_encoder_cli --width 1920 --height 1080 --fps 25 \
  --qm {0|1|3} --target-mb 138 --frames 120 --entropy vlc \
  --input src2k.raw --output out.mov   # + ffmpeg psnr 对源
```
