# 视频转码参数化（真机实测）

> 在 `a2` worktree 完成，基线 `a69cd4a5`（P6 静音兜底 + P7 视频转码已集成）。
> 本文数字全部来自 `scripts/a2_accept.py` 的真实输出，**22 项断言全绿**。
> 复现：`env -u PYTHONPATH python3 scripts/a2_accept.py`（端口 52900/52554/55080）。

## 1. 新增配置项

全部为 0 时表示「跟随源」，行为与此前硬编码路径完全一致。

| 键 | 含义 | 0 的语义 |
|---|---|---|
| `transcode.video_width` / `video_height` | 输出分辨率 | 跟随源；两者必须同时设置 |
| `transcode.video_fps` | 输出帧率 | 跟随源；只支持降帧（抽帧），不插帧 |
| `transcode.video_bitrate` | 输出码率(bps) | 按分辨率/帧率自动估算 |

`video_bitrate` 优先于通用的 `bitrate`：同一进程里「音频保持 64k、视频给 1M」是常见需求，共用一个字段表达不了。

## 2. 实测结果（22/22 PASS）

### 2.1 基线：参数全 0 跟随源

源 320x240@15fps H264 → 输出 `hevc 320x240`，硬解码错误 0。

### 2.2 H264 输出（libx264）

| 检查 | 结果 |
|---|---|
| 输出 codec | `h264` |
| 尺寸 | 320x240（跟随源） |
| 硬解码错误 | **0**（SPS/PPS 正确入轨） |
| 8 秒帧数 | 121（15fps 理论 120） |

H264 没有 VPS，warmup 只喂 SPS/PPS；H265 要喂 VPS/SPS/PPS。两种都实测 ready 成功。

### 2.3 分辨率缩放

| 目标 | 实测输出 | 硬解码错误 | 文件大小 |
|---|---|---|---|
| 320x240 → 640x480 | `640x480` | 0 | 330,530 字节 |
| 320x240 → 160x120 | `160x120` | 0 | — |

放大与缩小都正常，画面有实际数据（不是纯色/黑帧）。

### 2.4 码率控制

源 1280x720@30fps，拉 8 秒存文件比体积：

| `video_bitrate` | 8 秒文件 | ffprobe bit_rate |
|---|---|---|
| 150000 (150k) | 251,870 字节 | 160,280 |
| 1200000 (1200k) | 1,404,856 字节 | 1,310,486 |

**倍数 5.58**，说明码率设置真的生效（libx265 没有忽略它）。低码率档实测值略高于设定（160k vs 150k），属 VBV/关键帧尖峰，可接受。

### 2.5 降帧

源 15fps，`video_fps=5` → 实测 `4.96 fps (2000/403)`，硬解码错误 0。

### 2.6 1080p@30fps

| 检查 | 结果 |
|---|---|
| 输出尺寸 | `1920x1080` |
| 8 秒帧数 | 233（理论 240） |
| 硬解码错误 | 0 |

## 3. 修复：双轨转码 + 客户端连断导致的 SIGSEGV

这是本轮最重要的发现。**它在 P6/P7 集成就已存在**（用未含本线改动的 a3 基线同样 1/3 复现），不是视频参数化引入的。

### 3.1 现象

720p 双轨（H265 + Opus）转码，RTSP 客户端连上再断开，**约 1/3 概率进程 SIGSEGV**。修复前实测 6 次运行崩 4 次。

崩溃栈（a2 二进制）：

```
sig_crash → RtspMediaSource::onWrite → RingBuffer::write → RtspMuxer::onRtp
  → RingDelegateHelper::onWrite → RingBuffer::write → CommonRtpEncoder::inputFrame
  → TranscodeSink::onEncoded → FFmpegEncoder::onEncode → encodeAudio_l
  → FFmpegEncoder::inputFrame_l → TranscodeSink::onDecoded → FFmpegDecoder::decodeFrame
  → ... → TranscodeSink::inputFrame → MultiMediaSourceMuxer::onTrackFrame → AACTrack::inputFrame_l
```

反汇编定位到 `RtspMediaSource::onWrite + 868`（a3 二进制同样偏移 +868），对应指令序列是往 `PacketCache` 的内部链表插入节点。

### 3.2 根因

`TranscodeSink::onEncoded` 在 `_mtx` 之外调用 `_on_output(frame)`（刻意如此：编码过程会回调 `onEncoded` 再取 `_mtx`，持锁会自锁）。但**音频与视频的编码完成回调落在不同线程**：

| 轨 | 回调线程（日志可见） |
|---|---|
| 音频 index=1 | `event poller 4`（推流线程） |
| 视频 index=0 | `decoder thread` |

两个线程同时进入同一个 `MultiMediaSourceMuxer` → 同一个 `RtspMuxer` → 同一个 `RtspMediaSource::onWrite`，并发往非线程安全的 PacketCache 链表插入节点，链表指针被写坏，低概率 SIGSEGV。客户端连/断会加剧竞态（reader 增删与写入并发）。

### 3.3 修法

在 `TranscodeSink` 里加一把**独立的**输出串行锁，只包住 `_on_output(frame)` 回调，不包编码过程：

```cpp
std::mutex _output_mtx;   // 与 _mtx 分开：编码回调会重入 onEncoded 取 _mtx
...
if (_on_output) {
    lock_guard<mutex> lck(_output_mtx);
    _on_output(frame);
}
```

透传分支（不转码的轨）与转码分支共用同一把锁，因为两者也可能来自不同线程。

### 3.4 修复验证

同一复现场景（720p 双轨 + 两个 RTSP 客户端先连后断），跑 **12 轮：0 次崩溃**。修复前同场景 6 轮崩 4 轮。按基线 1/3 概率计算，12 轮全不崩的运气概率约 0.7%。

修复后 a2 全套 22 项、a3 全套 33 项均全绿，无回归。

### 3.5 遗留

- 这把锁是「我方串行化」，没有解决上游 `PacketCache` 本身不可并发写的问题；如果将来把转码输出改到多线程并行编码（例如按轨多线程），仍然要保证每个 muxer 只有单一写入线程。
- 互斥锁每帧加一次，实测 1080p 转码 CPU 无明显变化（未做量化基准，凭观察）。

## 4. 已知的良性警告

推流刚建立的瞬间，ffmpeg 客户端做**完整解码**（`-f null`，不是 `-c copy`）时偶发报
`Application provided invalid, non monotonically increasing dts to muxer in stream 1`，每次约 15 条，`-c copy` 时从不出现。

取证结论：**不是输出流的问题**。

| 检查 | 结果 |
|---|---|
| 线上时间戳（framecrc，不重写时间戳） | 386 包（视频 86 / 音频 300），dts 非单调 **0 处**，pts==dts，首尾 0→534420 / 0→287040 |
| 解码音频时长 | **精确 6.000000 秒**（48000Hz 双声道） |
| `-an` / `-vn` / 全量 三种方式单独解码 | 各 0 条警告（说明是建立瞬间的竞态） |
| 本地 opus 文件直接解码到 null muxer | 0 条（与 opus 本身无关） |
| enable=0 纯转发对照 | 0 条 |

判定为客户端 ffmpeg 在流建立瞬间的时间戳处理产物，不影响播放。验收脚本把它单独计数，不计入硬失败——否则测试会随机红。

## 5. 未验证项

- `video_fps` 升帧（低于源帧率以外的情况）未实现也未验证，明确只支持降帧
- 缩放 + 降帧 + 码率三者同时设置只验证了分别生效，未测组合下的画质/码率互相影响
- 4K 源未测
- 奇数宽高（如 321x241）的缩放对齐未验证
- 长时间（>1 小时）连续转码的内存/句柄增长未观察
- Windows 平台未验证

## 分辨率倍数与 API 宽高（2026-10-29 增）

### 配置（`[transcode]` 段）

| 键 | 含义 | 默认 |
|---|---|---|
| `video_scale` | 分辨率倍数，按源等比缩放，如 `0.5` / `1.5` / `0.25` | `0`（不缩放） |
| `video_width` / `video_height` | 绝对输出尺寸，必须成对且为偶数 | `0`（跟随源） |

**两者同时配置时以绝对宽高为准** —— 绝对尺寸语义更明确，倍数只是"相对源"的便捷写法。

### HTTP API（`/index/api/addTranscode`）

| 参数 | 含义 |
|---|---|
| `scale` | 分辨率倍数，`0` 表示取消缩放，有效范围 `0.1~4.0` |
| `width` / `height` | 绝对输出尺寸，必须成对、偶数，`0` 表示跟随源 |

非法值一律报错（`code=-300`），不静默当成"跟随源"：
`scale=abc`、`scale=0.05`、`scale=5`、只给 `width`、`width=641`。

响应里回显 `video_width/video_height/video_fps/video_bitrate/video_scale`，
便于调用方确认参数真的生效了（只回 codec 无法判断分辨率有没有变）。

### 实现要点

- 缩放只在**没有显式宽高**时生效；YUV420 要求偶数，统一向上取偶。
- 源尺寸未知（SPS 还没解析到）时打 `WarnL` 并退回跟随源，不猜。
- **同编码目标不缩放**：源 H264、目标也 H264 时走透传，`scale` 不会生效 ——
  这是既有设计（同 codec 不建编码器），验收时必须用不同 goal codec。
- 验收脚本 `scripts/scale_param_test.py`（11 项）：判据是**拉流端解出的实际宽高**，
  不是服务端日志里的 target_size。
  坑：kill 推流端后 15 秒内重推会接管旧流水线并沿用旧参数，多例必须用不同流名，
  否则只有第一例生效。

## 视频滤镜 / OSD（2026-10-29 增）

### 能力

在「解码 → 缩放/像素格式转换 → **滤镜** → 编码」之间插入一条 **libavfilter 单输入滤镜链**，
支持任意不改变尺寸的滤镜：OSD 文字（`drawtext`）、`negate`、`hflip`、`boxblur`、`delogo` 等。

**位置很关键**：滤镜在缩放之后 —— OSD 字号按**输出分辨率**算才有意义，
放在缩放之前、缩小后文字会跟着糊掉。

### 配置与 API

```ini
[transcode]
; OSD 示例：左上角打摄像头名，白字黑底框
video_filter=drawtext=text='CAM-01':x=40:y=40:fontsize=64:fontcolor=white:box=1:boxcolor=black@0.8:boxborderw=8
```

```
/index/api/addTranscode?secret=..&app=live&stream=cam1&video_codec=H265&filter=<urlencoded 滤镜串>
```

响应里回显 `video_filter`。空串 = 不启用。

### 硬约束

| 约束 | 原因 |
|---|---|
| 滤镜**不能改变输出尺寸/像素格式** | 编码器是按输入尺寸配置的，尺寸变了 `avcodec_send_frame` 返回 EINVAL，报错信息完全看不出是滤镜导致 |
| 只支持单输入滤镜 | `overlay`（叠加 logo 图片）需要第二路输入，暂不支持 |
| 建图失败只停用滤镜、不断流 | 观众宁可没有 OSD，也不该整路黑掉；失败会打 `ErrorL` 且不重试（避免刷屏） |
| 改尺寸请用 `video_scale` / `video_width` | 见上一条 |

### 验收

`scripts/filter_test.py`（12 项）：

- 基线可解；`negate` 后 YAVG ≈ 255 − 原始（实测 125.7 → 129.4）
- `drawtext` 后 YAVG 有可见变化（125.7 → 123.9），尺寸不变、0 硬错误
- 服务端日志出现 `transcode filter ready`
- 非法滤镜名（`no_such_filter=1`）→ 流照常、按原样直通
- 改变尺寸的滤镜（`crop=iw:ih/2`）→ init 阶段被拒，并给出可读原因
- 抽帧看图：OSD 文字 `CAM-01 AEGIS 2026-10-29` 清晰可读（人工确认）

判据用 **luma 均值** 而不是逐像素比对：编码器输出不保证逐字节可复现，亮度统计是稳定量纲。

坑：`signalstats,metadata=print` 会把每帧统计打到 stdout，错误统计必须排除
`lavfi.signalstats.*` 与 `frame:` 开头的行为，否则会凭空多出 1300+ 条「硬错误」。

## logo 角标（overlay 双输入，2026-10-29 增）

### 配置与 API

```ini
[transcode]
video_logo=/var/lib/zlm/logo.png   ; png/jpg，空=不叠加
video_logo_x=-20                   ; 负数表示距右边缘 20
video_logo_y=20                    ; 负数表示距下边缘
video_logo_width=240               ; 0=用原图宽度；高度按比例，且不超过画面 1/3
```

API 同名参数：`logo=`、`logo_x=`、`logo_y=`、`logo_width=`（`logo_x/y` 允许负数）。

可以与 `video_filter`（如 drawtext）**同时使用**：图里先跑滤镜链，再叠 logo。

### 三个实现坑（都实测踩过）

1. **建图后不能立刻给 logo 输入发 EOF 并指望 `eof_action=repeat`**：
   实测那样 overlay 在第一帧主画面到达前就结束，输出与输入完全一致 —— 看起来像「logo 没生效」。
   正确做法是**每个主帧喂一次 logo 帧**（`av_frame_clone` + 设成同一个 pts）。
2. **只有 logo、没有滤镜串是合法配置**：`init()` 里如果先把「空滤镜串」当非法返回，
   纯角标场景会被静默关掉，日志是 `filter init failed, disabled: ` 后面跟个空串，非常费解。
   判断顺序必须是「有 logo → 建 logo 图」在前。
3. **坐标表达式别把 W/H 和 w/h 弄反**：`x=w-W-20` 是错的（叠加到画面外），
   正确是 `x=W-w-20`（W/H=主画面，w/h=logo）。

### 依赖

logo 解码走 libavformat 的 demuxer（png/jpg），所以 **CMakeLists 里要额外链接 libavformat**；
仅 avcodec 能编译，但链接期 `Undefined symbols: _avformat_open_input`。

### 验收

`scripts/filter_test.py` 扩展到 19 项，logo 部分：
- 右上角（`logo_x=-20,logo_y=20`）区域 YAVG 157.1 → 184.4
- 贴底边（`logo_y=-120`）区域亮度明显变化
- 尺寸不变、0 硬错误、日志出现 `transcode logo ready`
- logo 文件缺失时降级为不叠加，流照常

判据用**区域**亮度而不是全画面：logo 只覆盖一角，全画面均值会被稀释。
量测区域必须落在 logo 实际覆盖范围（`y=-120`、高 80 ⇒ y=520..600，量 590 之后就是空的）。
