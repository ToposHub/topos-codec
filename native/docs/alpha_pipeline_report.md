# Topos Video Codec — Alpha 管线报告（阶段 7 交付物，2026-08-29）

> 范围：YUVA 从解码（阶段 6 `ToposMediaSource`）到 GPU 上传、shader 转换、
> 三页合成/预览的全程不丢失；straight/premultiplied 显式处理；CPU fallback
> 降级修复。关联决策：ADR-C007（C-52…C-58）；关联风险：R-05/R-23/R-24 关闭。

## 1. 管线全景（改造后）

```
ToposMediaSource / FFmpegMediaSource（planar YUVA）
  planes = (Y, U, V, A)            # A 全分辨率，per-plane bit_depth
  extra['topos_alpha_premultiplied'] # 源预乘标志（header flags bit0）
        │  DecodedFrame 整对象按引用跨线程（原有机制，planes[3] 本就不丢）
        ▼
YUVGPUUploader（src/shared/video/yuv_gpu_upload.py，三页共享）
  upload_yuv{420,422,444}[_r16ui] / _upload_planar_r8 / upload_yuv_planar
    + a=planes[3], alpha_bit_depth=plane_infos[3].bit_depth
  → (y, u, v, a) 四元组纹理；'a' 槽位挂 _uv_slots 池（plane_type 匹配）
        ▼
四套 YUV→RGB shader（float planar / r16ui planar / NV12 ×2）
  float alpha = 1.0;
  if (has_alpha == 1) alpha = texture(a_plane, source_uv).r;      // float 路径
                     alpha = float(texture(a_plane, uv).r) / (2^alpha_bit_depth - 1); // r16ui
  if (alpha_premultiplied == 1) rgb = (alpha > 1e-4) ? rgb/alpha : vec3(0.0); // → straight
  f_color = vec4(rgb, alpha);
        ▼
Color 页（async_renderer）：RGBA16F 工作纹理 alpha 通道贯通
Edit 页  （edit_gpu_preview_renderer / timeline_clip_frame_provider）：track FBO RGBA8
Composite 页（video_frame_node）：f4 FBO + straight-alpha 节点契约（_BLEND_FRAGMENT）
```

内部约定（C-54）：**全程 straight alpha**——预乘源在 YUV→RGB 边界一次性转
straight；Edit 页 blend `mix` 与 Composite 页 `_BLEND_FRAGMENT` 的 straight
输入假设、节点 `premultiplied=False` 上报、导出 CPU float32 straight mix 全部
天然对齐，无需第二套语义。

## 2. 三条上传路径的 Alpha 处理

| 路径 | 触发 | Alpha 纹理 | 归一化 | 采样 |
| --- | --- | --- | --- | --- |
| r16ui_direct | bit_depth>8 且自检通过 | R16UI（'u2'，NEAREST） | shader 内 `code/(2^alpha_bit_depth−1)`，位深独立 uniform | usampler2D unit3 |
| float16 | >8-bit 且 R16UI 不可用 | R16F（'f2'，LINEAR） | CPU `_normalize_plane(a, (1<<abd)−1)` | sampler2D unit3 |
| r8_direct | 8-bit YUVA | R8（'f1'，LINEAR） | sampler 自动 /255 | sampler2D unit3 |

要点：
- **Alpha 位深独立**（C-53）：Topos `a8/a10/a12/a16` 与 luma 的 10 不同，
  shader 用独立 `alpha_bit_depth` uniform（r16ui）/CPU 按自身满刻度归一化
  （float16）；错用格式级 bit_depth 会把 a8 压暗 4×、a16 大面积饱和——
  `test_r16ui_alpha_ramp_and_independent_bit_depth` / `test_alpha8_uses_alpha_plane_depth_not_luma`
  直接断言该语义。
- **unit3 占位绑定**（C-53）：`has_alpha==0` 时绑 Y 纹理占位，保证 sampler
  完整性；无 Alpha 帧输出 alpha 严格 1.0（回归断言）。
- **NV12 两套 shader 同步登记** alpha uniform（C-57）保持接口一致；
  2-plane 源（NV12/P010）本身无第四平面，`has_alpha` 恒 0 零开销。
- **混合 dtype 守卫**：r16ui 路径 a_plane 是 usampler2D，alpha dtype 非
  uint16 时丢弃 alpha（日志）而不是整帧失败（三页消费点各自实现）。

## 3. 三页消费点

| 页面 | 入口 | 输出面 | 状态 |
| --- | --- | --- | --- |
| Color | `_do_upload_video_planes`（a 分支 + 状态四元组）→ `_render_yuv_planes_to_rgb_texture`（4 元组解包）→ `_render_yuv420_to_rgb`（两分支 alpha） | RGBA16F ring buffer（alpha 通道真实） | ✅ 全精度 |
| Color CPU fallback | `_do_upload_planar_video_frame_cpu_fallback` | **f2**（有 alpha）/ f1（无 alpha，内存画像不变） | ✅ 修复 /255 错误归一化 |
| Color 外部 GPU 帧 | `_wrap_external_gpu_planes` ≥4 平面保留四元组 | — | ✅ |
| Edit | `_classify_planar_format` 显式登记 yuva*/topos* → `_upload_and_render_planar` alpha 穿参 | track FBO RGBA8（R-23 限制） | ✅ 语义正确、8-bit 预览 |
| Edit timeline | `timeline_clip_frame_provider` planar 分支 alpha（含 flipud 同步） | RGBA8 + straight 标记 | ✅ 同上 |
| Composite | `_PlanarFrameData.a/alpha_bit_depth/alpha_premultiplied` → `_upload_frame_gpu`；CPU 分支 `result[:,:,3]` | f4 FBO + straight 节点契约 | ✅ 全精度 |

Edit 页 blend shader 未改：背景为不透明黑（clear alpha=1），`mix(base.rgb,
overlay.rgb, u_opacity*overlay.a)` 对 straight alpha 数学正确，最终 alpha 恒 1
是正确结果而非缺陷。

## 4. R-23 决策：Edit 页保持 RGBA8（文档化限制）

- **现状**：Edit 页 track/graded/transform/合成 scratch/present 全链
  `dtype='f1'`（唯一例外 Effect scratch 可选 f2，FX-M01）。
- **决策**：阶段 7 不升级。理由：(a) Edit 合成背景不透明，最终 alpha 恒 1，
  alpha 正确性在 blend 因子，已实证；(b) 全链 f2 显存翻倍，4K 多轨实时预览
  （项目基线）风险大于收益；(c) 导出走 timeline_export CPU float32 管线，
  全精度不受预览影响；(d) Color 页 RGBA16F + Composite 页 f4 承担 HDR/alpha
  全精度职责。HDR 预览升级与 FX-M01 统一评估，留阶段 9/10。
- **跟踪**：alpha_pipeline_report.md 本节 + 风险登记 R-23（已关闭，决策型）。

## 5. 验证（tests/media/test_topos_alpha_pipeline.py，13 项）

GPU 用例（gpu_renderer 标记，standalone context；本机 R16UI 自检通过，
float16 用例强制 float 路径覆盖回退）：

- float16 / R16UI / R8 三路径 alpha ramp 精确读回（≤2e-3，f16 量化级）；
- a8 独立位深（/255 而非 /1023——误用断言 >0.9 vs ~0.25）；
- premultiplied → straight：半透明列 rgb≈1.0、全透明列除零保护 rgb=0；
- 无 alpha 回归：alpha 严格 1.0；
- 硬边缘无黑边/白边：alpha 二值无滤波拖尾，straight 合成结果严格 ∈ {bg, fg}；
- Topos decode → GPU 读回 == 解码 alpha 平面（a10 与 a8 两素材，round-trip
  门禁核心；编码环节 alpha 保真已由阶段 4 多代测试 + native 门禁覆盖，
  应用层 Topos 导出在阶段 8 接入后补端到端）；
- 池 'a' 槽位隔离/复用/release 清理 + 同 context 重建无泄漏。

无 GL 用例：格式分类（topos_yuva*/yuva* → 子采样类）；CPU fallback
（duck-typed AsyncRenderer：a16 f2 精度 / a8 位深 / 无 alpha f1+255）。

门禁：`bash native/topos_codec/run_tests.sh` → ALL STAGE-7 CHECKS PASSED
（ctest 29/29 × debug/asan/ubsan/fuzz + CLI + ffprobe oracle + Python 44 项）。
回归：tests/video + tests/media + tests/native 2250 passed / 53 skipped /
9 failed——9 项全部经 `git stash` 对照证实为 HEAD 既有环境问题（fixtures
缺失等），与本阶段无关。

## 6. 遗留

- Edit 页 HDR 预览位深（本报告 §4，与 FX-M01 统一评估）；
- Color 页调色 shader 链的 alpha 逐级保真审计（当前输出面向显示，不阻塞
  alpha 门禁；导出 alpha 语义在阶段 8 ToposVideoEncoder 接入时端到端验证）；
- Topos 磁盘代理含 alpha 的生成链（阶段 8，须 decode→encode 走自研后端）；
- R16UI 缩放采样若阶段 9 统一改手动双线性，alpha 随 Y 一并处理（R-24 定案）。
