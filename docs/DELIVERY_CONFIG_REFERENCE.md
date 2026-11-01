# 转码功能配置与接口参考（交付用）

本文是给**现场运维与集成方**的单页参考。所有键名、参数名、默认值、错误码均取自代码
（`src/Common/config.cpp`、`src/Transcode/TranscodeConfig.cpp`、`src/Transcode/TranscodeApi.cpp`），
并由 `scripts/api_params_test.py`（18 项）验证。

---

## 1. 配置：`[transcode]` 段

放在服务主配置文件（如 `config.ini`）里。**默认全部关闭**，`enable=0` 时行为与上游完全一致。

```ini
[transcode]
enable=1                     ; 总开关，默认 0
audio_codec=opus             ; 目标音频编码，留空=不转音频
video_codec=h265             ; 目标视频编码，留空=不转视频
on_demand=1                  ; 没人看就不转码
```

### 音频

| 键 | 默认 | 说明 |
|---|---|---|
| `audio_codec` | 空 | `opus` / `aac` / `g711a` / `g711u` / `mp3` / `g722`；也认 `PCMA`/`PCMU`/`hevc` 等规范名 |
| `bitrate` | 0 | 音频码率（bps），0 = 沿用源 |
| `sample_rate` | 0 | 目标采样率，0 = 沿用源 |
| `enable_silence` | 1 | 源端断流时用同一个编码器补静音帧（避免观众断流） |
| `silence_timeout_ms` | 200 | 多久没新帧开始补静音（范围 20~5000） |

**载荷规格是硬约束**（写死在代码里，不可配）：G711 固定 8000Hz 单声道、G722 固定 16000Hz 单声道、
MP3 固定 44100Hz 双声道。源规格不同由重采样处理。

### 视频

| 键 | 默认 | 说明 |
|---|---|---|
| `video_codec` | 空 | `h264` / `h265` / `vp8` / `vp9` / `av1` |
| `video_width` / `video_height` | 0 | 显式输出尺寸，**必须成对给**且为偶数；0 = 跟随源 |
| `video_scale` | 0 | 倍数缩放（0.1~4.0），0 = 不缩放；**显式宽高优先于倍数** |
| `video_fps` | 0 | 目标帧率，0 = 跟随源；降帧时会同步改时间轴步进 |
| `video_bitrate` | 0 | 视频码率（bps），0 = 按分辨率自动估 |
| `video_filter` | 空 | ffmpeg 滤镜串，如 `drawtext=text='CAM-07':x=20:y=20:fontsize=48:fontcolor=white` |
| `video_logo` | 空 | 角标图片路径（png/jpg），叠加在画面上 |
| `video_logo_x` / `video_logo_y` | 10 / 10 | 角标坐标；**负数表示从右/下边缘算**（`-20` = 距右边 20px） |
| `video_logo_width` | 0 | 角标缩放宽度，0 = 原始尺寸 |

滤镜与角标可同时用：**先滤镜链，再叠角标**；两者都在缩放之后执行（OSD 字号按输出分辨率算）。

### 硬件

| 键 | 默认 | 说明 |
|---|---|---|
| `hw_encode` | 0 | 优先硬件编码；建不起来自动回落软件并打 Warn |
| `hw_decode` | 0 | 优先硬件解码；失败同样回落 |
| `hw_devices` | 空 | 可用 GPU 列表，如 `"0,1"`（**只有 NVENC 平台生效**） |
| `hw_device` | -1 | 指定某路用哪张卡，-1 = 自动按负载分配 |

`hw_devices`/`hw_device` 在 macOS(VideoToolbox)/Linux(VAAPI) 上**不生效但不会报错**：
会打一条 Warn 说明原因，状态接口里报 `-1`。

### 自适应降帧

| 键 | 默认 | 说明 |
|---|---|---|
| `adapt_fps` | 0 | 过载时自动降帧（默认关闭，开启后帧率会变，影响既有验收基线） |
| `adapt_min_fps` | 5 | 降帧下限 |

判据是「单帧编码耗时 / 帧间隔」：>0.85 连续若干帧降一档、<0.55 更久才恢复，关键帧永不丢。

### 按流配置（多规则）

```ini
[transcode]
enable=1
audio_codec=opus          ; 全局默认

stream_rule=live/cam1:h265/aac          ; app/stream:视频/音频
stream_rule_2=live/cam2:                ; 只覆盖 enable 等（留空项沿用全局）
stream_rule_3=__defaultVhost__/live/cam3:h265
```

- 格式：`[vhost/]app/stream:视频codec/音频codec`，后面可继续跟 `key=value` 增量项
- 多规则按**序号**排序（`stream_rule_2` 先于 `stream_rule_10`），先匹配先生效
- 优先级：**API 覆盖 > stream_rule > 全局段**

---

## 2. HTTP API

前缀 `/index/api/`，都需要 `secret` 与 `vhost`/`app`/`stream`。

### `addTranscode` —— 下发/修改

| 参数 | 说明 |
|---|---|
| `video_codec` / `audio_codec` | 目标编码；**两个都不给且没有其它参数时报错** |
| `bitrate` / `sample_rate` | 音频码率/采样率 |
| `enable` / `on_demand` | 开关 |
| `enable_silence` / `silence_timeout_ms` | 静音兜底 |
| `width` / `height` | 显式尺寸（成对、偶数） |
| `scale` | 倍数（0.1~4.0） |
| `video_fps`（别名 `fps`） | 目标帧率（0 或 1~120） |
| `video_bitrate` | 视频码率 |
| `filter` / `logo` / `logo_width` / `logo_x` / `logo_y` | 滤镜与角标 |
| `hw` | 同时开硬件编解码 |
| `hw_encode` / `hw_decode` | 分别开 |
| `hw_devices` / `hw_device` | 多卡 |
| `adapt_fps` / `adapt_min_fps` | 自适应降帧 |
| `reset` | 1 = 清除 API 覆盖、回落到 ini |

**`reset` 的两种语义**（曾经是坑）：

- `reset=1` 单独出现 → 清除覆盖，回落配置值
- `reset=1` + 其它参数 → **先清除、再按这些参数重新下发**（不会吞掉参数）

响应是**扁平结构**（没有 `data` 包裹），回显全部生效参数 + `applied` 字段：

| `applied` | 含义 |
|---|---|
| `immediate` | 已生效（codec/采样率本来就匹配在跑的转码） |
| `pending` | 要等推流端重连（流在线但编码器已建好） |
| `waiting` | 流还没推，等它开始推 |
| `noop` | 没有可清除的覆盖（仅 reset 时） |

> **重要**：转码参数在**轨就绪时**生效。给已经在推的流下发参数，只写覆盖、下次推流才生效；
> 响应里的 `msg` 会明说 `stream is online but transcode is not enabled for it`。

### `getTranscode` —— 查询状态

**流必须在线**，否则返回 `code=-500 can not find the stream`。

返回（扁平）：

```json
{
  "code": 0, "online": true, "transcoding": true,
  "video_codec": "H265", "audio_codec": "opus",
  "has_override": true, "config_source": "api",
  "hw_encode": true, "hw_decode": true, "hw_device": -1,
  "adapt_fps": false, "frames": 1234, "failed_tracks": 0,
  "silence_frames": 0,
  "tracks": [
    {"index": 0, "type": "video", "source_codec": "H264", "target_codec": "H265",
     "transcoding": true, "frames": 1234, "encoder": "hevc_videotoolbox",
     "hardware": true, "hw_decode": true, "hw_decode_frames": 1234,
     "hw_device": -1, "adapt_fps": 0, "adapt_dropped": 0}
  ]
}
```

**怎么判断「到底用没用 GPU」**：看 `tracks[].hardware`（编码）与 `tracks[].hw_decode` +
`hw_decode_frames`（解码，按「有过硬件帧下载」而非「配置了开关」）。
`hw` 参数只表示「希望用硬件」，实际可能已回落软件。

---

## 3. 错误码

| code | 场景 |
|---|---|
| `0` | 成功 |
| `-300` | 参数非法（`invalid scale, must be 0 or 0.1~4.0` / `width and height must be given together` / `must be even` / `invalid hw_device, must be >= -1` …） |
| `-500` | 流不存在（`can not find the stream`，`getTranscode` 时流离线） |

参数校验一律**报错而不是静默回落默认**：`abc`、越界、只给一半的成对参数全部 -300。

---

## 4. 参数可观测性矩阵（本仓实测）

| 能力 | 配置 | API | 验收脚本 |
|---|---|---|---|
| 音视频 codec 转换 | ✓ | ✓ | `audio_matrix_test`(125) / `codec_reverse_test`(23) |
| 显式宽高 / 倍数缩放 | ✓ | ✓ | `scale_param_test`(11) |
| 帧率 / 码率 | ✓ | ✓ | `api_params_test`(18) |
| 滤镜链 / OSD | ✓ | ✓ | `filter_test`(19) |
| logo 角标（含负坐标） | ✓ | ✓ | `filter_test` / `api_params_test` |
| 静音兜底 | ✓ | ✓ | `teardown_liveness_test`(20) |
| 硬件编码 | ✓ | ✓ | `hw_encode_test`(21) |
| 硬件解码（可观测） | ✓ | ✓ | `hw_encode_test` |
| 多卡分配 | ✓ | ✓ | `multi_gpu_test`(10，真分配需 NVENC 机器) |
| 自适应降帧 | ✓ | ✓ | `adapt_fps_test`(13) |

---

## 5. 已知边界（不是缺陷，写清楚免得被当 bug 修）

1. **目标 codec == 源 codec 时不转码**（透传，不建编码器）。想验证必须先换一个不同的 codec ——
   用 H264 源转 H264 测缩放/滤镜/帧率永远不生效。
2. **RTSP 推流源 + `directProxy` 开启时转码不进观众链路**（源 RTP 直接透传）。本仓已对转码场景
   自动关闭 direct-proxy，但接入方自定义拓扑时要留意。
3. **源端断开后 15 秒宽限期内重推会沿用旧 SDP**：摄像机换分辨率重连时观众看到的仍是旧参数，
   需要 `/index/api/close_streams?secret=…&app=live&stream=<id>&force=1` 后重推。
4. **RTMP/FLV 装不下 HEVC 与 Opus**，验证转码结果必须用 RTSP 拉流。
5. **G722 的时钟**：本实现声明 `a=rtpmap:9 G722/16000`（音频 16k、时间戳按 16k）。忽略 rtpmap
   当 8000 处理的客户端会 2 倍速 —— 客户端兼容性风险，非服务端缺陷。
6. **第 8 项「全 GPU 零拷贝」未做**：硬解后的帧仍要下载回内存，因此单换硬件解码**几乎不省 CPU**
   （实测软编+软解 186% vs 软编+硬解 200%），真收益在「硬编+硬解」组合。
7. **G722.1 / SVAC 不支持**：ffmpeg 无实现，需要自己写编解码器。
