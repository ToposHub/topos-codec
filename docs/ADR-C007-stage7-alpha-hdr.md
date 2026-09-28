# ADR-C007 — 阶段 7：Alpha 与 HDR 渲染链补齐

- 日期：2026-08-29 ｜ 状态：已采纳
- 输入：ADR-C001 C-14（GPU/Alpha 缺口清单）、risk_register R-05/R-23/R-24、
  ADR-C006（阶段 6 交付的 plane_infos 契约与 extra 标志）、计划 §12 阶段 7。
- 交付：`src/shared/video/yuv_gpu_upload.py` alpha 管线（池槽/上传/shader/
  render 绑定）、Color 页 `async_renderer.py`、Edit 页
  `edit_gpu_preview_renderer.py` + `timeline_clip_frame_provider.py`、
  Composite 页 `video_frame_node.py`、`tests/media/test_topos_alpha_pipeline.py`
  （13 项）、`docs/alpha_pipeline_report.md`、`run_tests.sh` 阶段 7 门禁。
- 决策编号：C-52…C-58（承接 ADR-C006 的 C-51）。

## 1. 决策

### C-52 Alpha 纹理 = `_uv_slots` 池的 `plane_type='a'` 槽位

不建第四个池。`YUVTexturePool` 的槽位匹配已按 `(尺寸, components,
plane_type)` 三元组隔离，U/V/UV 本就混用 `_uv_slots`；Alpha（全分辨率单
通道）作为 `'a'` 类型挂同一列表，LRU/池满替换/`release_all`/GC 后备释放
全部复用现有机制，零新增生命周期代码。`acquire_a_texture()` 为唯一新入口。
验证：`test_alpha_slot_distinct_and_reused`（与 y/u 槽位互斥、跨帧复用、
release 清空）。

### C-53 shader 可选 alpha：`has_alpha` uniform + unit3 占位绑定 + 独立 `alpha_bit_depth`

四套 YUV→RGB shader（float planar / r16ui planar / NV12×2）统一登记：

```glsl
float alpha = 1.0;
if (has_alpha == 1) {
    alpha = texture(a_plane, source_uv).r;                    // float 路径（CPU 已归一化）
    alpha = float(texture(a_plane, source_uv).r) / (pow(2.0, float(alpha_bit_depth)) - 1.0); // r16ui
}
```

- **单套 shader 双形态**：不编译 alpha/no-alpha 两份 program；uniform 分支
  在现代 GPU 近零成本，且避免 render 路径按形态分叉。
- **unit3 占位**：`has_alpha==0` 时仍绑 Y 纹理到 unit 3，保证已声明 sampler
  的完整性（GL 规范对 uniform 分支内采样器的完整性验证不豁免）。
- **`alpha_bit_depth` 独立 uniform（关键正确性）**：Topos `a8/a10/a12/a16`
  的 Alpha 位深与 luma 的 10 独立（`plane_infos[3].bit_depth` 权威）。共用
  `bit_depth` 会把 a8 按 /1023 归一化（整体压暗 ~4×）、a16 大面积饱和为 1.0
  ——这是阶段 6 就埋好的 per-plane 契约的消费侧落实。
- float16 路径 alpha 在 CPU `_normalize_plane` 按自身满刻度归一化（shader
  无需位深）；r8 路径 sampler 自动 /255（8-bit YUVA 的 alpha 定义即 8-bit）。

### C-54 内部约定：全程 straight alpha；预乘源在 YUV→RGB 边界一次性转换

Topos header `alpha_premultiplied`（flags bit0，`extra['topos_alpha_premultiplied']`
透出）为 1 时，shader 输出处 unpremultiply：

```glsl
if (alpha_premultiplied == 1) {
    out_rgb = (alpha > 0.0001) ? rgb / alpha : vec3(0.0);
}
```

alpha≈0 的像素颜色不可观测（全透明），输出 0.0 规避除零/NaN。选择边界
转换而非下游透传标志的理由：Edit 页 blend `mix`、Composite 页
`_BLEND_FRAGMENT` straight 输入、节点 `premultiplied=False` 上报、导出 CPU
straight mix——下游四处已统一按 straight 假设，单点转换使全部下游零改动。
CPU 分支（Composite `_convert_decoded_frame`）用等价 numpy 实现。验证：
`test_premultiplied_unpremultiply`（半透明列 rgb≈2×、透明列 0）。

### C-55 R16UI Alpha = NEAREST，与 Y 平面一致（R-24 关闭）

GL 整数纹理强制 NEAREST。现有 10-bit luma/chroma 即 NEAREST；alpha 单独
手动双线性会造成同帧内 alpha 边缘与 luma 边缘质量不一致。1:1 渲染
（track/FBO @ 源分辨率）精确无损；缩放时四平面同为 nearest，视觉一致。
硬边缘实证：`test_hard_alpha_edge_no_fringe`（alpha 严格二值、合成结果
严格 ∈ {前景, 背景}，无黑边/白边）。若阶段 9 统一引入手动双线性上采样，
alpha 随 Y 一并处理。

### C-56 R-23 范围决策：Edit 页保持 RGBA8，记录为文档化限制

Edit 页 track/graded/transform/crop/合成 scratch/present 全链 `f1`。决策
**不升级**：(a) 合成背景为不透明黑，最终 alpha 恒 1 是正确结果，alpha 语义
由 blend 因子保证（本阶段已贯通视频 alpha 到 overlay.a）；(b) 全链 f2 显存
翻倍，威胁 4K 多轨实时预览基线；(c) 导出走 timeline_export CPU float32
管线，精度不受预览影响；(d) HDR/alpha 全精度职责由 Color 页 RGBA16F 与
Composite 页 f4 承担。限制记录于 alpha_pipeline_report.md §4；HDR 预览
升级与 FX-M01（Effect scratch f2）统一评估，归阶段 9/10。

### C-57 NV12 shader 同步登记 alpha uniform（接口一致性，非功能需求）

NV12/P010 2-plane 源无第四平面，alpha 功能上不需要；但 C-14 清单列出全部
四处 `vec4(rgb, 1.0)`，且三页共享 uploader——四套 shader 接口统一
（`a_plane`/`has_alpha`/`alpha_premultiplied`，NV12 用 unit2）消除「哪套
shader 支持 alpha」的记忆负担。`render_nv12_to_rgb` 的外部 shader 参数
（G-P0-01 缓存的旧 program）无新 uniform 时按 KeyError 跳过 alpha 设置，
保持兼容。

### C-58 CPU fallback 修复：alpha 位深独立 + f2 输出（条件性）

`async_renderer._do_upload_planar_video_frame_cpu_fallback` 两处修复：

1. **归一化位深**：alpha 用 `plane_infos[3].bit_depth`（旧代码
   `getattr(decoded_frame,'bit_depth',8)` 取格式级位深——对 a8/a16 全错）；
2. **输出 dtype**：带 alpha 帧改 `f2`（RGBA16F，rgb 不 clip 保留 HDR 余量，
   alpha 10/12/16-bit 不量化）；无 alpha 帧维持 `f1`（内存画像不变）。
   `VideoTexturePool` 按 `(w,h,components,dtype)` 匹配槽位（P1-8），f1/f2
   共存无冲突。

验证：`TestCPUFallback` 三项（a16 f2 精度 / a8 位深 / 无 alpha f1+255，
duck-typed AsyncRenderer 免起渲染器）。

## 2. 消费点映射（对 C-14 GPU/Alpha 清单逐项）

| C-14 清单项 | 落点 | 状态 |
| --- | --- | --- |
| YUVTexturePool 第四平面纹理 | C-52 `'a'` 槽位 | ✅ |
| upload_yuv422_r16ui / float16 alpha 上传 | `_upload_alpha_plane` 三模式统一辅助 + 9 个入口函数可选 `a`/`alpha_bit_depth` | ✅ |
| 三处 shader alpha 采样（+NV12 两处） | C-53 四套全改 | ✅ |
| `_do_upload_video_planes` alpha 分支 | 检测（has_alpha‖len≥4 + 形状/dtype 守卫）+ 六分支穿参 + 状态四元组 | ✅ |
| CPU fallback 8-bit 降级替换 | C-58 | ✅ |
| Edit `_classify_planar_format` YUVA 分类 | yuva444p10/12/16le、yuva422p、yuva422p10/12/16le、yuva420p10/12/16le、topos_yuva422p10a{8,10,12,16} 显式登记（原靠 '422' 子串兜底） | ✅ |
| Edit track FBO 'f1'→'f2' | C-56 决策：不升级，文档化 | ✅（决策型） |
| Composite video_frame_node planar 分支 | `_PlanarFrameData.a/alpha_bit_depth/alpha_premultiplied` + GPU/CPU 双分支 + straight 契约 | ✅ |
| R16UI NEAREST + macOS 自检门控回退 float16 | 原有 probe/self-test 机制不变；float16 回退路径 alpha 同样贯通（测试强制 float 覆盖） | ✅ |

计划 §12 阶段 7 完成门槛对照：
- decode→GPU→composite→encode→decode alpha 结果符合定义：decode→GPU 读回
  精确一致（a10/a8 两素材）；encode→decode alpha 保真由阶段 4 native 多代
  测试覆盖，应用层 encode 接入（阶段 8）后补全链端到端；
- straight/premultiplied 无黑边/白边：C-54 + 硬边缘测试；
- 失败/取消/关闭不泄漏纹理或 native frame：池槽复用既有 LRU/deferred
  release（alpha 槽位随池释放，`test_release_clears_all_pools_and_reusable`）；
  native 句柄释放为阶段 6 已验（close→reopen 测试）。

## 3. 修正与偏差记录

1. **D-3（fbo.read dtype）**：测试初版 `fbo.read(components=4)` 默认返回
   f1 字节导致 reshape 失败；RGBA16F FBO 读回必须显式 `dtype='f2'`。
2. **D-4（_disable_r16ui 不彻底）**：测试辅助只清 pool/passed 不置
   `_r16ui_auto_tested`，首次 >8-bit 上传仍触发自动自检（对已清空的 pool
   报"disabled"噪音）；测试辅助补置标志。生产代码无需改动。
3. **D-5（timeline_clip_frame_provider 上传分支首行注释失准）**：
   rgb_planar 分支旧注释只提 G/B/R 交换；gbrap 第四平面即 alpha，本次
   穿参时同步注释。

## 4. 遗留（非本阶段门禁）

- Color 页调色 shader 链逐级 alpha 保真审计（显示向，不阻塞）；
- Edit 页 HDR 预览位深（C-56，与 FX-M01 统一评估，阶段 9/10）；
- Topos 含 alpha 磁盘代理生成（阶段 8，decode→encode 须走自研后端）；
- 阶段 8 ToposVideoEncoder 接入后的全链 alpha 端到端（export 路径）。
