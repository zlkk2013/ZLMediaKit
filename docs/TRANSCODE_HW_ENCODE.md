
## 硬件编码（第 4 项，2026-10-29 增）

### 能力

优先用硬件编码器，**拿不到或打不开时自动回落软件编码** —— 永远不因为 GPU 不可用而让一条流失败。

```ini
[transcode]
hw_encode=1     ; 默认 0（关闭），保持与旧行为一致
```

API：`hw=1`。响应回显 `hw_encode`。

平台选择：macOS `h264_videotoolbox` / `hevc_videotoolbox`；Linux/Windows `h264_nvenc` / `hevc_nvenc`。
没有硬件实现的 codec（VP8/VP9/AV1）直接走软件路径，不报错。

### 实测效果（Apple M2，1280x720@30 → H265）

| 编码方式 | 服务进程 CPU |
|---|---|
| libx265（软件） | **150%** |
| hevc_videotoolbox（硬件） | **5.7%** |

约 **26 倍**差距 —— 同样一台机器能多带两个数量级的 1080p 流。

### 三个实现要点（都踩过）

1. **硬件编码器内部有帧缓冲，第一帧不一定吐包**：实测 `hevc_videotoolbox` 只喂 1 帧时
   一个包都不出，warmup 判定失败 → 目标轨永远 unready → muxer 丢弃整条视频轨。
   症状极具迷惑性：**转码 CPU 几乎为 0、拉流 404、日志只说 warmup failed**。
   修法：warmup 循环喂帧（上限 12 帧）直到拿到包。软编 x265/x264 首帧就出，不受影响。
2. **硬件编码器必须给明确码率**：`bit_rate` 未设时 VideoToolbox/NVENC 没有可预期的默认质量
   （实测画面质量极差）。`hw_encode=1` 且没配码率时按分辨率给默认值
   （`≥1080p → 6M`、`≥720p → 4M`、`≥480p → 1.5M`、其它 `800k`）。
3. **回落必须覆盖两种失败**：①平台没有该硬件编码器（`avcodec_find_encoder_by_name` 返回空）；
   ②硬件编码器存在但 `avcodec_open2` 失败（驱动异常、并发会话数上限、分辨率超限）。
   两者都要静默转软件并打 Warn，不能抛异常。

### 验收

`scripts/hw_encode_test.py`（12 项）。三层判据，缺一不可：

| 层 | 判据 | 实测 |
|---|---|---|
| 真的用了硬件 | 日志 `transcode video encoder: hevc_videotoolbox (hardware)` | ✅ |
| 正确性 | 输出 `hevc,1280,720`、拉流 0 解码错误 | ✅ |
| **代价更低** | CPU 明显低于软编 | 150% → 5.7% ✅ |

外加：H264 也走硬件（`h264_videotoolbox`，640x360 源）、
VP9 无硬件实现时回落 `libvpx-vp9` 且服务端确实在编码、
`hw=0` 时走软编（开关有效）。

### 已知边界

- **只做了硬件编码，没做硬件解码**：解码仍走 CPU，帧拷贝仍是系统内存 ↔ 显存
  （那属于第 8 项「全 GPU 零拷贝」）。编码是大头，实测已把 CPU 从 150% 降到 5.7%，
  剩余开销主要在解码与拷贝。
- **只验证了 macOS/VideoToolbox**。Linux 的 VAAPI 需要 hw_frames_ctx（与 NVENC 的
  「软帧输入、驱动内部上传」不是同一套），代码里优先 NVENC、VAAPI 尚未接入；
  Windows/NVENC 也未实测。
- 硬件编码器的 GOP 只有 IDR 没有 CRA，某些播放器对 HEVC 硬件流的追帧行为略有差异。
