# Topos Bitstream Specification v2.0

- 状态：V2.0 草案冻结（M9；布局裁决见 ADR-C027）
- 基线：`bitstream_spec_v1.md`（全部未在本文显式修订的条款对 V2 同样有效）
- 原则：major=2 与 V1 **不兼容**；旧解码器必须 `TC_ERR_UNSUPPORTED_VERSION`
  且不产出帧。V1 解码路径与 V1 golden 永久保留。
- 归属：字段布局唯一真相源 = ADR-C027 注册表。本文只冻结 V2.0 已实现语义
  （Intra + canonical VLC）；Micro-GOP（frame_type=1/gop_id/ref_distance/
  coding_mode）与 tile_layout=1 为**占位字段**，V2.0 一律拒绝，语义由后续
  ADR + 本 spec 增补冻结，不在此预定义。

## 1. 与 V1 的差异总表

| 项 | V1 | V2.0 |
| --- | --- | --- |
| `version_major`（byte 6） | ==1 | ==2 |
| `version_minor`（byte 7） | 0..3 枚举 | ==0 |
| `frame_type`（15） | ==0 | ==0（P 为 V2.1 占位） |
| `gop_id`（16–17）/`ref_distance`（18） | ==0 | ==0（占位） |
| `flags` bit1 `tile_layout` | 必须 0 | ==0（tile 为占位） |
| `reserved0`（45–48，u32） | 必须 0 | 拆分为 `entropy_mode`(45) / `codebook_version`(46) / `coding_mode`(47) / `reserved_v2_0`(48)，见 §2 |
| 颜色 slice 熵编码 | 有界 Rice | `entropy_mode` 二选一：0=Rice（V1 语义）/ 1=canonical VLC |
| Alpha slice 熵编码 | 有界 Rice | 不变（恒 Rice；Alpha-VLC 须 codebook_version 递增） |
| tpcC 原子 version | 1 | 2（原子 schema 不变，版本号即轨级 major 声明） |

V2 流中 `entropy_mode=0` 的帧：重建必须与相同量化系数的 V1 **bit-exact**
（parity 锚；slice payload 应逐位等于 V1 编码器输出）。

## 2. Frame header 增量字段

校验顺序不变（size→magic→header_crc32→逐字段按偏移序）。新增字段规则：

| 偏移 | 字段 | 类型 | V2.0 约束 |
| ---: | --- | --- | --- |
| 45 | `entropy_mode` | u8 | 0=Rice / 1=canonical VLC；其他 → `TC_ERR_UNSUPPORTED_VERSION` |
| 46 | `codebook_version` | u8 | mode=0 → 必须 0；mode=1 → 1（已冻结表组版本）；其他 → `TC_ERR_UNSUPPORTED_VERSION` |
| 47 | `coding_mode` | u8 | ==0；非 0 → `TC_ERR_UNSUPPORTED_VERSION`（V2.1 占位） |
| 48 | `reserved_v2_0` | u8 | ==0；非 0 → `TC_ERR_MALFORMED` |

header_crc32 覆盖范围不变（偏移 0..48，49 字节）。

## 3. Slice header 增量语义（17 字节布局不变）

| 字段 | entropy_mode=0 | entropy_mode=1 |
| --- | --- | --- |
| `k1`（offset 10） | 颜色：DC 残差 Rice k（0..14） | 颜色：`dc_book`（0..3，>3 → `MALFORMED`） |
| `k2`（offset 11） | 颜色：AC level Rice k | 颜色：`level_book`（0..3，>3 → `MALFORMED`） |
| `k3`（offset 12) | 颜色：run Rice k | 颜色：`run_book`（0..3，>3 → `MALFORMED`） |
| alpha slice 的 k1/k2 | level/run Rice k（同 V1） | **同左**（alpha 恒 Rice） |

slice 排列、覆盖、独立性、CRC 覆盖范围、字节对齐与末尾零填充：全部同 V1 §4.3。

## 4. VLC token 语法（entropy_mode=1，颜色 slice）

符号语义（DC 帧内差分、AC (run,level) 沿 zigzag、EOB 终结）与 V1 §7.5 完全
一致；仅把 Rice 码字换成静态 canonical VLC。位序与 bitio 一致（MSB-first）。

### 4.1 映射与类别

有符号值的映射同 V1 §6.1：`m = v≥0 ? 2v : −2v−1`。

- **DC_CAT**（编码映射后幅度 m）：
  - `cat=0`：m=0（v=0），无后缀；
  - `cat≥1`：码字后跟 `cat−1` 位字面后缀（MSB-first），
    `m = (1<<(cat−1)) | suffix`；
  - cat 域 0..28。解码后校验 `m ≤ 2^27`（V1 `M_MAX_DC`），越界 →
    `TC_ERR_MALFORMED`（cat=28 仅能表示边界值 m=2^27，即 suffix 全 0）。
- **RUN**（先于 level 解码）：符号 0..62 为前导零 run；**63 = EOB**。
  解码 run<63 后立即校验 `pos + run ≤ 63`，违反 → `MALFORMED`。
- **LEVEL_CAT**：cat 域 1..27，**必须 >0**（cat=0 不在表中，出现即
  `MALFORMED`）；后缀 `cat−1` 位，`m = (1<<(cat−1)) | suffix`；
  校验 `m ≤ 2^26`（V1 `M_MAX_AC_LEVEL`）。

**codebook_version=1 不设 ESC**：上述有界域已覆盖全部合法符号；任何越域
类别/前缀直接 `MALFORMED`。扩域必须以新 codebook_version 重新分配，禁止
含糊的「全 1 长码」escape（计划 §12.1）。

每块符号数上界同 V1：最多 63 个 (run,level) 对 + 1 个 EOB（全零块也输出
EOB）；解码完成性由该界 + EOB 强制。

### 4.2 canonical 码表（冻结规则）

- 每族表（DC_CAT 29 符号 / RUN 64 符号 / LEVEL_CAT 27 符号）各冻结
  **4 组**（book id 0..3），由 `tools/gen_vlc_tables.py` 以 package-merge
  生成长度受限（≤20 bit）canonical 码：码长满足 Kraft 不等式取等，同长按
  符号值升序分配连续码字（canonical 标准指派）。
- 生成输入 = 训练集符号直方图；训练集 manifest、逐文件 SHA、生成器版本与
  生成输出 hash 随表冻结入库。表一经发布**永不改码字**。
- 编码端选表（确定性，无运行时建树）：在现有 slice 统计遍中累计三族
  histogram，逐族独立计算
  `total_bits = Σ_s freq[s]·(code_len[s] + suffix_bits(s))`
  （DC/LEVEL 的 suffix_bits(s)=cat(s)−1，RUN 为 0），取最小；**同分取最小
  book id**。三族独立选择，结果写入 slice header 的 book 字段。

### 4.3 两级解码表（规范布局）

每族每 book 一张解码表，构建为纯函数（确定性、进程级缓存，模式同 V1
Rice LUT）：

- **primary**：`uint32_t primary[4096]`，以 12-bit 前缀窗索引。entry 打包：
  `kind:2 | nbits:5 | symbol:6 | has_secondary:1 | secondary_base:18`
  （kind：0=DC_CAT / 1=RUN / 2=LEVEL_CAT / 3=invalid）。
  - 码长 ≤12：该码全部 `2^(12−len)` 个后缀 entry 直接填 kind/nbits/symbol；
  - 码长 13..20：primary entry 置 has_secondary，`secondary_base` 指向
    该族表 `uint32_t secondary[N][256]` 的行；解码再取 8 位
    （`len−12` 有效位）查 secondary；
  - 未分配前缀：kind=3（invalid sentinel），立即 `TC_ERR_MALFORMED`。
- 编码侧为 `code[symbol] + len[symbol]` 平表，直接供 64/128-bit writer。

## 5. 兼容、golden 与测试（计划 §12.5 摘要 + 本 spec 补充）

- major=1 解码规则永久冻结于 `frame_header.c`；major 分流单点（ADR-C027 D-8）。
- tpcC 原子 version=2 的轨：仅支持 V1 的库必须干净拒绝；轨内每帧 packet
  major 与原子版本必须一致。
- golden：V1 golden 原名不动；新增 `v2_vlc_i`（Intra+VLC）矩阵
  10/12-bit × 422/444/GBR × alpha 有/无；V2-Rice 帧以 V1 golden 逐位复用
  （parity 锚）。
- fuzz：每截断位、无效前缀、类别/run 极值、EOB 缺失、非零 pad、book 字段
  越界（4..255）、codebook_version 未知值。
- 1/2/4/8 线程编码 packet 逐字节一致；解码像素与状态一致。
- `topos_transcode --bitstream-major 2`：不覆盖源文件、不静默重写旧文件。

## 6. 验收（计划 §12.6，引用不改）

- V2 native decode 相对 M8 完成后的 V1 ≥1.5×，相对计划起点 current ≥3×；
- 重建与相同量化系数的 V1 bit-exact；
- 码率相对 V1 不恶化 >1%（或经正式 profile/质量决策接受）；
- encoder p95 不退化 >10%；
- 达 ProRes 门后才评估默认编码 V2；V1 decode 永不删除。
