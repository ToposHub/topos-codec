# ADR-C057：浮点档产品化——float 四档质量阶梯与命名（弃 HF 词）

日期：2026-09-21 · 状态：**已采纳**
关联：ADR-C056（LP 产品化，tier 命名同日定调）、spec §15（HALF 样本域）、
ADR-I011（整数 422 质量阶梯与 --tier 预设）

## 1. 背景

浮点（HALF，spec §15）自 2026-09-19 激活以来以 **HF** 命名，预设为
无损导向的比率档：`hf-lossless`（qp20）/ `hf-2` / `hf-4` / `hf-8` /
`hf-16`。两个产品问题：

1. **命名不直观**："HF"（half float）是内部缩写，与整数阶梯已建立的
   Low/Medium/High/Ultra 产品词表（ADR-I011 + TMET 'toos X Nbit'）割裂；
2. **定位偏差**：无损导向的比率档名把"逐位无损"当作阶梯顶格卖点，而
   浮点中间片的真实需求是**发挥中间片编码优势——较小体积 + 非常好画质
   + 编解码实时**（实测 qp44 已视觉透明 ±2 half-ULP 且体积小于 EXR
   zip16 无损；无损档反而大于 EXR zip16，见 spec §15.3）。

## 2. 决定

**D1（四档质量阶梯）**：浮点预设收敛为四档，锚点取自 spec §15.3 实测：

| CLI id | TMET 展示名 | qp | 实测定位 |
|---|---|---:|---|
| `float-ultra` | toos Ultra 16bit float | 44 | 视觉透明 ±2 half-ULP，~2:1，< EXR zip16 |
| `float-high` | toos High 16bit float | 58 | PSNR ≈71 dB，~2.6~3.2:1 |
| `float-medium` | toos Medium 16bit float | 72 | PSNR ≈44 dB，~8~26:1 |
| `float-low` | toos Low 16bit float | 82 | 审片代理 |

**真无损不入阶梯**——`--half --qp 20`（或 32）显式达成（qp≤36 自然内容
逐位无损，spec §15.3）；qp20/32 档从预设菜单移除。

**D2（命名规则）**：弃 "HF" 词。浮点域展示名 = `toos <Quality>
<bit_depth>bit float`（如 "toos Ultra 16bit float"）。**未来 FLOAT32
（sample_kind=2，v1 预留）解锁时同词表换位深**：`toos Low 32bit float` 等。

**D3（tier id 复用 + 域自描述）**：浮点阶梯与整数阶梯**共用 TMET tier id
1..4**（Low..Ultra）；浮点域由 **IDSC sample_kind=HALF 自描述**，不入
tier id——读端展示名 = tier id（质量词）× IDSC（位深 + 样本域）组合。
旧 reader 对 HALF 按 sample_kind 保留域干净拒绝的既有语义不变；TMET
校验域（≤7）不变。

**D4（TMET 落盘）**：`tier_meta_id_by_params` 按**最终生效参数**回配
（pf2+qm0+qp∈{44,58,72,82}+half ⇒ tier 4/3/2/1——与整数 422 阶梯的
qm/qp 组合互不碰撞），label 带 ' float' 后缀；读端 `topos_image_tier_label`
加 `floating` 位，与 IDSC sample_kind 联合判定。

## 3. 理由

- 体积/画质锚点全部来自既有实测（spec §15.3），零新增标定；
- tier id 复用使读端词表单一（Low..Ultra 一套），位深/样本域本就是
  IDSC 权威字段，避免 tier id 语义重复编码；
- 无损移出阶梯与整数域先例一致（422 家族"档位不设真无损，真无损走
  显式 qp"）——浮点域同理，且无损在浮点域性价比更低（> EXR zip16）。

## 4. 被否备选

- **保留 hf-* 作为别名**：开发期无需旧素材兼容（项目既定）；双词表
  并存会让 manifest/文档/帮助文本永久承担两套口径。
- **为浮点新开 tier id 5..8**：质量词与整数阶梯重复而 id 分叉，读端
  词表 ×2；且 TMET 共用载荷校验域（≤7）需扩，纯增表面积无收益。
- **32-bit 档同步设计**：sample_kind=2 仍预留（v1 拒绝），锁定词表
  即可，实现另立 ADR。
