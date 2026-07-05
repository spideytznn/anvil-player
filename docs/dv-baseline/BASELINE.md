# 杜比视界发紫问题 — 基线确认（阶段 0）

测试片源：`Z:\已整理\外语电影\死侍与金刚狼 (2024)\死侍与金刚狼 (2024) - 2160p.mkv`
采样帧：100s 处的第一帧 I 帧
诊断日期：2026-07-05

## 1. 片源属性（ffprobe）

| 属性 | 值 | 意义 |
|---|---|---|
| 编码 | HEVC Main 10 | 10-bit HEVC |
| pix_fmt | yuv420p10le | 10-bit YUV，走 P010 渲染路径 |
| 分辨率 | 3840×1608 | 2.39:1 宽银幕裁剪 |
| color_range | tv (limited) | 容器层标记 |
| color_space / transfer / primaries | **全部 unknown** | 典型 DV Profile 5，无标准 HDR10 标签 |
| **dv_profile** | **5** | 流媒体压制，纯 DV |
| dv_level | 6 | |
| rpu_present_flag | 1 | 有 RPU |
| **el_present_flag** | **0** | **无增强层 → 阶段 3（EL 合并）跳过** |
| bl_present_flag | 1 | 有基础层 |
| **dv_bl_signal_compatibility_id** | **0** | **BL 完全不兼容 HDR10 → 不做 reshaping 必然发紫** |
| dv_md_compression | none | **CMv2.9**（非 CMv4.0 → 阶段 4 简化） |
| 容器 | matroska (mkvmerge v82) | |
| 码率 | ~24.5 Mbps 视频 | |

## 2. RPU 重塑结构（采样帧）

通过 ffprobe `-show_frames` 提取第一帧的 `Dolby Vision Metadata` side data：

### 2.1 全局参数
- `rpu_type=2`, `rpu_format=18`
- `coef_data_type=0`（FFmpeg 已转定点）
- **`coef_log2_denom=23`** → 系数定点化分母 = 2^23 = 8388608
- `vdr_rpu_normalized_idc=1`
- **`bl_video_full_range_flag=1`**（注意：与容器层 `color_range=tv` 矛盾，reshaping 时以 RPU 的 full range 为准）
- `bl_bit_depth=10`, `el_bit_depth=10`, **`vdr_bit_depth=12`**（reshape 输出 12-bit 精度）
- `disable_residual_flag=1`, `nlq_method_idc=-1`（none）→ 确认无 EL 残差

### 2.2 三分量重塑曲线

**分量 0（Luma / I 分量）**：8 段，9 个 pivots，**全 polynomial order 2**
- pivots: `0 173 283 392 502 611 721 830 1023`
- 8 段系数完全相同：`poly_coef=-612867 9803467 0`
- 实数化（÷2^23）：`c0 ≈ -0.0731, c1 ≈ 1.1687, c2 = 0`
- → 实际是一阶多项式 `y = -0.0731 + 1.1687·x`（c2=0）

**分量 1（Ct）**：4 段，5 个 pivots，**全 polynomial order 2**
- pivots: `0 288 512 736 1023`
- 4 段系数完全相同：`poly_coef=-599187 9584640 0`
- 实数化：`c0 ≈ -0.0714, c1 ≈ 1.1429, c2 = 0`

**分量 2（Cp）**：4 段，5 个 pivots，**全 polynomial order 2**
- pivots: `0 288 512 736 1023`
- 4 段系数与分量 1 完全相同

### 2.3 对阶段 2 实现的重大简化

1. **本片源这一帧完全没用到 MMR** —— 只有 polynomial order 2（且实际 c2=0 退化为一阶）。
2. **同一分量内各段系数相同** —— 这降低了 HLSL 分段查找的复杂度，但实现时仍要支持每段独立系数（其他片源/场景可能不同）。
3. **必须保留 MMR 代码路径** —— 其他 DV 片源（尤其 Profile 7 UHD 蓝光）大概率使用 MMR，不能假设只有 polynomial。
4. **多项式实现极简**：`y = c0 + c1·x + c2·x^2`，每段 3 个 float 系数。

## 3. 参考帧与发紫基线

三组对照帧已生成于 `docs/dv-baseline/`：

| 文件 | 处理方式 | 用途 |
|---|---|---|
| `purple_baseline_no_dv_100s.png` | 直接解码不应用 DV reshaping（RGB24） | **发紫基线**，模拟当前播放器行为 |
| `reference_dv_applied_100s.png` | libplacebo `apply_dolbyvision=1` + bt.2446a tone-map → BT.709 SDR | **色彩正确参考**（SDR 显示器可视） |
| `reference_dv_hdr_pq_100s.yuv` | libplacebo `apply_dolbyvision=1` 保留 BT.2020 PQ（yuv420p10le） | HDR 直出参考（HDR 显示器对照） |

### 视觉确认（AI 图像分析）
- **发紫基线**：树木大面积紫色/粉紫色，地面灰绿色，典型 DV Profile 5 未 reshape 症状。确认发紫问题存在。
- **参考帧**：冬季森林场景正确呈现——雪地冷蓝调、红色帽子鲜明、肤色自然、HDR 层次丰富。色彩完全正常。

## 4. 发紫机制（代码层确认）

当前 `d3d11_video_renderer.cpp` 的 `psNv12Src` 着色器，DV 流因容器色彩标签全 unknown：
- `transferType` / `primariesType` / `matrixType` 默认为 Unknown → 映射为 0
- PS `main()` 走到最后一个 fallback 分支（line 691）：`return float4(saturate(rgb), 1.0);`
- 即把**未重塑的 IPTPQc2 YUV（误当 RGB）**直接输出 → 紫色主导

## 5. FFmpeg 工具链确认

本工程捆绑的 FFmpeg（`third_party/ffmpeg`，n8.1.2 BtbN LGPL shared build）：
- ✅ `--enable-libplacebo --enable-vulkan --enable-libshaderc`
- ✅ libplacebo 滤镜可用，`apply_dolbyvision` 默认 true
- ✅ 因此本工程**可直接用 ffmpeg 产出逐像素参考帧**，无需额外编译 libplacebo

## 6. 对后续阶段的优先级影响

| 阶段 | 状态 | 说明 |
|---|---|---|
| 阶段 2（P5/8 BL reshaping） | **全部核心** | 做完即解决发紫 |
| 阶段 3（P7 EL 合并） | **跳过** | 片源无 EL（el_present_flag=0） |
| 阶段 4（DM + CMv4.0） | **简化** | CMv2.9，无 ext block 处理；DM Level 6 提取仍做 |
| 阶段 5（硬解评估 + MF 预研） | 不变 | |

---

## 7. 实施结果（阶段 1-2 完成后）

### 已实现的完整 DV 链路
1. **DV 元数据提取**（`DolbyVisionMetadata.h` + `ExtractFrameDolbyVisionMetadata`）：从 `AV_FRAME_DATA_DOVI_METADATA` 提取 reshaping 曲线、color 矩阵，流级配置从 `AV_PKT_DATA_DOVI_CONF` 补全 profile/level。
2. **DV 流强制软解**（`PlaybackPlan.cpp` + `OpenVideoDecoder`）：DV 流跳过 D3D11VA，保证 RPU side data 可用。
3. **P010 YUV 直传路径**（`NativeYuvPlanes` + `PublishFrame` + `UpdateYuvTexture`）：绕过 swscale，把 10-bit YUV 直接打包成 P010 布局上传 GPU，保留 IPT 结构供 reshaping。
4. **DV reshaping HLSL**（`psNv12Src` 着色器）：实现 polynomial + MMR 分段 reshaping，`ycc_to_rgb` 矩阵转换，产出 BT.2020 PQ RGB，衔接现有 HDR 直通 / ACES tone-map。
5. **DV 常量缓冲区**（`DoviShaderConstants` + `UpdateDoviConstants`）：每帧上传 reshaping 系数到 register(b1)。

### 验证结果
- 日志确认完整链路：`planned=ffmpeg_software reason=dolby_vision_software_decode_for_reshape` → `dolby_vision_stream profile=5` → `dolby_vision_yuv_path dovi=yes` → `input=p010_yuv dolby_vision=present`。
- 渲染稳定（~48 fps 软解 + GPU reshaping，4K HEVC）。
- **发紫问题已修复**：多次截图（不同场景）均无技术性偏色，色彩自然。对比基线帧（未 reshape 时树木大面积紫色/地面灰绿），当前画面色彩正常。
- HLSL 编译无错误，无运行时崩溃。

### 已知限制
- 输出为 HDR10 PQ（reshape 后），无法点亮设备 DV 指示灯（DXGI swapchain 限制，详见计划文档）。
- 软解性能开销（4K HEVC ~48 fps，CPU 密集）。阶段 5 评估硬解 DV side data 可用性后可能优化。
- MMR 路径已实现但未实测（本片源全 polynomial）；其他 DV 片源（如 Profile 7 UHD 蓝光）可能触发 MMR，需后续验证。
