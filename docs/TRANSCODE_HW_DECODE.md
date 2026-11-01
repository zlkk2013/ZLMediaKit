
## 硬件解码（第 8 项前置，2026-11-01 增）

### 能力

解码侧也优先用 GPU：`hw_decode=1`（或 `hw=1` 同时开编解码）。设备创建失败自动回落软解。

```ini
[transcode]
hw_decode=1     ; 默认 0（关闭）
```

API：`hw_decode=0/1`；`hw=1` 表示「尽量用硬件」，同时开编码与解码。

### 实现要点

macOS 走 `AV_HWDEVICE_TYPE_VIDEOTOOLBOX`，Linux/Windows 走 `AV_HWDEVICE_TYPE_CUDA`。
三件必须做的事：

1. **挂 `hw_device_ctx`**：`av_hwdevice_ctx_create()` 后 `ctx->hw_device_ctx = av_buffer_ref(dev)`；
2. **必须自定义 `get_format` 回调**：默认的 `avcodec_default_get_format` 会挑第一个可用的
   **软件**格式 —— 于是「设备挂上了、照样软解」，日志上完全看不出来。这是最容易踩的坑；
3. **硬件帧要下载**：VT 输出 `AV_PIX_FMT_VIDEOTOOLBOX`，sws/滤镜只吃系统内存，
   必须 `av_hwframe_transfer_data` 转到普通帧（顺带拷 pts/pict_type）。
   **这一步正是「硬件解码」与「全 GPU 零拷贝」的分界。**

注意：这个 ffmpeg 构建**没有** `h264_videotoolbox` 这类独立解码器条目（`-decoders` 里查不到），
`videotoolbox` 只出现在 `-hwaccels` 里 —— 所以不能靠「按名字找解码器」，必须走 hwaccel 路线。

### 实测（Apple M2，1280x720@30，H265 目标）

| 配置 | 服务进程 CPU |
|---|---|
| 软编 + 软解 | 185.9% |
| 软编 + **硬解** | 199.9% |
| 硬编 + 软解 | 7.7% |
| **硬编 + 硬解** | **6.5%** |

**如实说明**：在 Apple Silicon 上「只把解码换成硬件」几乎不省 CPU —— 硬解省下的算力被
`av_hwframe_transfer_data` 的下载开销抵消了，软编占大头时甚至看不出差异。真正的收益来自
「硬编 + 硬解」组合。要让解码侧再降一个量级，必须做**真零拷贝**（让滤镜/缩放直接吃硬件帧，
不做下载）—— 那需要各平台都有对应的 GPU 滤镜实现，属于第 8 项的剩余部分。

### 验收

`hw_encode_test.py` 扩到 21 项，解码侧判据：

- **状态接口证明真的在跑**：`tracks[].hw_decode=true` 且 `hw_decode_frames>0`
  （只挂设备没下载过帧不算 —— 拿 `hw_decode_frames` 而不是「是否配置了」）
- `hw_decode=0` 时 `hw_decode=false`、帧数 0
- 硬解路径输出正确（`hevc,1280,720`）、0 解码错误
- 隔离对照：两边都硬编，只差解码（7.7% → 6.5%）

踩坑记录：API **只认 `hw` / `hw_decode`，没有 `hw_encode` 参数**。对照组写成 `hw_encode=1`
会被静默忽略，两组都变成软编，CPU 对照直接作废（实测踩过）。
