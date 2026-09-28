# ADR-C044：rans2 编码环免 idiv（速度计划 S4）

- 状态：**已实施**（2026-09-11；回归门数据见 §4）
- 关联：docs/codec/topos_speed_optimization_plan_2026-09-11.md（S4）、
  topos_speed_p0_profiling_2026-09-11.md（M0.2：`tc_rans_put` 占 band
  时间 41%）、transform/fastdiv.h（阶段 9 round-up magic）、ADR-C038
  （rans2 产品默认）、ADR-C041（解码侧平台判定——本 ADR 为编码轴）
- 变更：`tc_rans_put_fast`（rans.h/rans.c）+ `color_tokens_rans2_encode`
  后向环 4 个 put 点（EOB/lvl/run/dc）换用；per-model per-symbol
  `tc_fastdiv` 表（5×29 + 64 + 5×28 = 349 项/slice，与模型同源同寿命，
  init ≈µs 级）。V7-R legacy 编码环保持原 put（非产品路径）。

## 1. 域证明（fastdiv.h 同式推广）

出重整化循环时 `x < x_max = 2^19·f ≤ 2^31`（L=2^23、SCALE=12、
f ≤ 4096）。round-up magic `M = floor(2^51/f)+1`，误差项
`e = M·f − 2^51 ∈ [1, f]`，精确性条件 `n·e < 2^51`：rans 域
`x·e < 2^19·f² ≤ 2^43 ≪ 2^51` **恒成立**——量化调用方（n ≤ 2^25）
之外的域扩展，推导结构不变。q = x/f 精确 ⇔ 余数 `x − q·f` 精确 ⇔
状态转移 `x' = (q<<12) + r + cum` 与 `tc_rans_put` 逐位一致；2 的幂
f 走 shift 分支（magic=0），f=1 恒等。

## 2. 验证

1. **逐字节差分 ✅**：4K 444 pro444 + 4K 422 hq 各 8 帧产品码控包，
   旧/新 dylib 编码输出 SHA256 一致（位流零变化，无兼容性影响）；
2. **定向测试 ✅**：debug 构建 ctest（rans/slice/golden/m10 模式）
   9/9；
3. **系统级交错 A/B ✅（过 +3% 门）**：进程级交替，median-of-medians，
   i9-9900K，稳态 encode_frame 计时：

| 单元 | 16t（产品条件） | 1t（±1% 纪律） |
|---|---|---|
| 4K 444 pro444（qp70） | 41.2→39.3 ms/帧 = **+4.5%** | 252→230 = **+8.6%** |
| 2K 444 pro444（qp70） | 14.9→14.0 = **+6.0%** | 64→58 = **+9.3%** |
| 4K 422 hq（qp62） | 25.6→24.5 = **+4.1%** | — |

16t 数字低于 1t：线程分摊/同步占比稀释（1t 才是内核净收益）。

## 3. 收益归因与边界

- 朴素估计（put 41% × 除法占 put ~60% → 1t ~+17%）实测 +8.6~9.3%
  ——乱序引擎本已部分吸收 idiv 延迟，magic 乘法消掉的是不可重叠的
  吞吐占用；
- fd 表 init 成本已含在净收益内（µs 级/slice）；
- 解码零变化（put 仅编码侧）；golden 位流逐位不变。

## 4. 回归门（2026-09-11 实测）

debug/asan/tsan/fuzz 四配置 ctest **79/79**；ubsan 78/79——唯一失败
`unit_pyramid_transform`（pyramid_transform.c:249 "applying zero offset
to null pointer"）经 stash 在 HEAD 复现，**预存在缺陷与本变更无关**
（pyramid 可缩放路径，不经 rans2 编码），待单独修复。导出符号钉
（360 ≥ 清单 48）、CLI 冒烟、PyAV interop oracle、多线程 golden 平价
（4 线程位流逐位）、perf 数值门 + depth12、SDK roundtrip、pytest
**280/280** 全过。另修两处门禁预存在损坏（run_tests.sh /
interop_oracle.py 硬编码旧库名 libtopos_codec.dylib——陈旧 build 目录
遮蔽多时，独立提交）。

## 5. 复现

```bash
# 差分：旧/新 dylib 各跑 tools/prof_p0.py --dump-packets → shasum 比对
# A/B：tools/prof_p0.py --seconds 10 稳态列，进程级交替 5 轮取中位
```
