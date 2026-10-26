# 转码功能 — 第二阶段开发计划

> 基线：`dev/transcode` @ `2bb5901a`，音频 AAC→Opus 已端到端验证通过。
> 上一阶段记录见 §1「已完成」，本计划只谈**还没做的**。
> 纪律不变：新增文件为主，上游文件改动收尾必须 `git diff --stat upstream/master` 量过。

## 0. 结论先说

| 顺序 | 做什么 | 为什么这个顺序 | 工作量 |
|---|---|---|---|
| **P6** | Opus 静音帧兜底 | 音频已通，但源端断流时观众会**断流**而非静音——生产必现故障，改动最小 | 0.5–1 天 |
| **P7** | 视频 H264→H265 | 唯一「代码写了但没跑过」的部分，风险最高；需要实测 Annex-B/像素格式 | 2–3 天 |
| **P8** | 按流配置 + HTTP API | 功能稳定了才做运维面，否则接口跟着逻辑改两遍 | 1.5–2 天 |
| **P9** | 多目标轨 | 会重构 `TranscodeSink` 核心结构，必须等前面稳定 | 2–3 天，可选 |

**关键判断：不改上游插件层。** 我本来以为视频转码必须改 `ext-codec/H265.cpp` 才能拿到宽高，查证后发现 `H265Track::inputFrame` 会解析 SPS 自动回填尺寸（§3.1）。这个假设的推翻让 P7 的风险从「改上游、长期 rebase 痛」降到「改自己新增的文件」，值得单独记一笔。

**不用 brew 的 ffmpeg。** 你装的 `/opt/homebrew/bin/ffmpeg` 是静态命令行二进制（6.0，45MB，`otool -L` 只有系统框架），没有 `avcodec.h` 也没有 `libavcodec.dylib`，**无法用于编译链接**。编译链继续用 conda 的 `~/miniconda3/envs/zlm-ffmpeg`（ffmpeg 7.1.1，头文件+dylib+pkgconfig 齐全，已实测编过）。你装的那个保留当推流/ffprobe 测试工具用。

---

## 1. 已完成（不在本计划范围内）

| 项 | 证据 |
|---|---|
| `FFmpegEncoder` + `TranscodeAudioFifo` | 编译通过，opus 48kHz/2ch/960 |
| `TranscodeSink` 音频路径挂进 muxer | 拉流 ffprobe 得 `codec_name=opus` |
| 换轨不换流 | 流 id 不变，服务端 `mpeg4-generic -> opus` |
| 按需转码 | `readers=0` 转码 0 帧，`readers=1` 开工 |
| 基线回归 | `enable=0` 零转码日志，拉流回 aac 44100Hz |

实测数据：10 秒拉流视频 150 包（=10s×15fps）、音频 885 包、音频音量损耗 0.1dB、`send_frame` 失败 0 次。

---

## 2. 环境决定（已拍板）

**用 conda，不用 brew。**

```bash
# 已就绪，无需重做
~/miniconda3/envs/zlm-ffmpeg/          # ffmpeg 7.1.1，含头文件 + dylib + pkgconfig
export PKG_CONFIG_PATH=~/miniconda3/envs/zlm-ffmpeg/lib/pkgconfig
```

理由（实测）：

- `/opt/homebrew/bin/ffmpeg` 是**静态命令行二进制**（6.0，45MB，`otool -L` 只有系统框架），无 `avcodec.h`、无 `libavcodec.dylib`，**不能用于链接编译**。
- `brew info ffmpeg` 显示 9.0.2「Not installed」，本机 macOS 14.8 属 Homebrew Tier 3，无 bottle，源码编译需数小时且要升级 8 个依赖。
- 该 brew 二进制**保留作为测试工具**（推流/拉流/ffprobe），和生产编译链互不干扰。

---

## 3. 代码现状盘点（决定优先级的事实）

### 3.1 视频转码的真实缺口

不是「代码写了没跑」，而是**目标轨尺寸这一环需要靠真实码流驱动**：

1. **目标轨尺寸不是构造时给，而是编出码流后由 SPS 解析得到。**

   `ext-codec/H265.cpp` 的 `getTrackByCodecId` 忽略所有参数、`H265Track()` 无参构造，一开始 `getVideoWidth()` 返回 0。但它**不需要 setter**——`inputFrame` 里 `parse_hevc_sps()` 会解析 SPS 并回填 `_width/_height`：
   ```cpp
   int H265Track::getVideoWidth() const { return _width; }   // inputFrame 后才有值
   ```
   `H265Track::ready()` 也是这个语义（尺寸/extra_data 齐了才算 ready）。

   ⇒ **结论：不需要改 `ext-codec/`**。只要 `FFmpegEncoder` 编出的第一帧带 VPS/SPS/PPS，`H265Track` 自己会补齐尺寸。这也解释了为什么音频能一次跑通——Opus 同样靠 encoder 参数在运行时定型。

   **验收判据要改**：不能在 `addTrack` 时就断言尺寸，得在拉流端看 `width/height`，且必须等到关键帧之后。

2. **`openVideoCodec` 确实硬编码了源参数。** 写死 `framerate = 25/1`、`gop_size = 25`、`bit_rate` 不跟源。源是 15fps 时输出会按 25fps 打时间戳，下游音画不同步。**这个必须改**，且能独立验证。

3. **像素格式只取 `codec->pix_fmts[0]`。** 源若为 yuv420p 而 H265 首选是别的格式，`FFmpegSws` 可能要额外转换；现在没有校验转换是否真的发生。需要打日志确认 `sws` 实际输入输出格式。

### 3.2 静音帧的缺口

`MuteAudioMaker`（`src/Common/MediaSink.cpp:286`）**只产 AAC**：

```cpp
#define MUTE_ADTS_DATA_MS 128
auto aacFrame = std::make_shared<FrameToCache<FrameFromPtr>>(CodecAAC, ...);
```

`addMuteAudioTrack()` 也只 `Factory::getTrackByCodecId(CodecAAC)`。一旦某条轨转成 Opus 后源端断流，`MuteAudioMaker` 补的是 AAC 静音帧，与已换成的 Opus 轨 codec 不符 ⇒ 观众端**断流**而不是静音。

### 3.3 配置粒度

`TranscodeOption::loadFromIni()` 读全局 `[transcode]`，所有流共用一套参数，无法「只转这一路」或「这路转 H265、那路转 Opus」。

### 3.4 运行时控制

只有 `config.ini`，改配置对新流生效但要依赖 reload；无 HTTP API，运维无法按需开关或查询当前转码状态。

---

## 4. 实施顺序（按「能不能出错」排序，不按功能大小）

### P6 — Opus/任意目标编码的静音帧（先做，风险最低、收益最直接）

**为什么先做**：它是音频链路**唯一还漏的一环**。音频转码已经跑通，但源端抖动/断流时观众会断，这在生产上是必现故障。且改动面最小、可独立验证。

做法：
- 不碰 `MuteAudioMaker`（它服务于未转码的 AAC 场景，改了会伤基线）。
- 在 `TranscodeSink` 里新增**转码轨专用**的静音兜底：记录每条转码轨最后一次输出帧的 `dts`；超过阈值（如 200ms）没有新帧输出时，按目标 codec 生成静音帧。
- 静音帧生成：Opus 用 `FFmpegEncoder` 编一段全零 PCM（复用现有编码链，不手写比特流）；其他 codec 同理。**不硬编码字节数组**，因为 Opus 的 TOC 字节依赖配置（带宽/立体声/帧长）。
- 阈值与开关进 `[transcode]`：`silence_timeout_ms=200`、`enable_silence=1`。

验收：
- 推 AAC 流，中途 kill 推流进程，观众端 RTSP 不断开、持续收到 opus 静音包。
- 对照：`enable_silence=0` 时观众端应在原超时后断开。
- 判据用包数（静音期间音频包数仍在增长）+ 音量（解码后 `mean_volume` 应显著低于 -60dB 而不是无数据）。

### P7 — 视频转码 H264→H265（含上游插件层的尺寸修复）

**为什么第二**：这是「代码写了没跑过」的部分，且涉及 Annex-B/AVCC 格式与像素格式转换这类只有真跑才能确认的环节，不懂的地方必须实测，不能靠推断。

做法：
- **不改 `ext-codec/`**。尺寸由 `H265Track::inputFrame` 解析 SPS 自动补齐（见 §3.1），前提是首帧必须带 VPS/SPS/PPS——需要确认 `FFmpegEncoder` 输出的 H265 是 Annex-B 还是 AVCC，ZLM 的 `H265Track` 期望 Annex-B。
- `openVideoCodec` 参数化：帧率、GOP、码率全部跟随源轨，不再硬编码 25fps。
- 像素格式：从源轨实际 `pix_fmt` 走 `FFmpegSws` 转换，不要只取 `codec->pix_fmts[0]`；打日志记录 `sws` 实际 in/out 格式。
- 目标码率：`bitrate` 为 0 时按「源码率 × 1.2」估算并设下限，避免 0 码率让 x265 走 CRF 后画质不可控。

验收：
- 推 H264 320x240@15fps，拉流得 `codec_name=hevc`，`width=320 height=240`（**必须在第一个关键帧之后读**）。
- 视频包数 ≈ 时长×15，**帧率不能变**。
- 与音频同时转时音画同步，用 `ffprobe -show_packets` 比对首尾 pts 差值 < 100ms。
- `H265Track::ready()` 能在首帧后变 true；若为 false 说明 extra_data 没带上，需在 `onEncoded` 里补 VPS/SPS/PPS 提取。
- 基线回归：`video_codec=` 为空时拉流仍为 h264。

### P8 — 按流配置 + HTTP API

**为什么第三**：功能都通了才谈运维接口，否则接口跟着逻辑一起改两遍。

做法：
- 配置从全局 `[transcode]` 扩展为「全局默认 + 按流覆盖」：
  ```ini
  [transcode]
  enable=1
  audio_codec=opus
  video_codec=
  # 按流覆盖，格式 app/stream:key=value;key=value
  stream_rule=live/cam1:video_codec=h265,bitrate=800000
  stream_rule_2=live/cam2:audio_codec=g711a
  ```
  （ZLM 的 mINI 不支持数组，用 `stream_rule_N` 递增键，和它既有风格一致）
- 新增两个 API，挂进 `server/WebApi.cpp`，与既有 `getMediaList` 同风格：
  - `addTranscode?secret=..&vhost=..&app=..&stream=..&audio_codec=opus&video_codec=h265`
  - `getTranscode?secret=..&vhost=..&app=..&stream=..` → 返回各轨是否在转、目标 codec、累计帧数、失败数
- 状态查询直接复用 `TranscodeSink::transcodeFrameCount()` / `isTranscoding()` / `failedTrackCount()`，这几个接口上一阶段已经写好了。

验收：
- `addTranscode` 后**无需重启**，该流新推上来即按新参数转码。
- `getTranscode` 返回的帧数与服务端日志 `transcoded N frames` 一致。
- 无 API 调用、仅配 `[transcode]` 时行为与 P6/P7 一致。

### P9 — 多目标轨（最后，可选）

一路源同时输出多种规格（如 Opus + G711、H264 720p + 480p）。

**为什么最后**：它需要改 `TranscodeSink` 的 `_contexts` 结构（当前 `int index -> TrackContext` 是一对一），改成「一源轨 → 多目标轨」，会影响 P6/P7/P8 所有已验收逻辑。等前三项稳定再动。

做法要点：
- `TrackContext` 改为 vector，每个目标轨独立 index（需分配不冲突的 index，避开源轨占用）。
- 每个目标轨独立的 encoder 实例与 frame_count。
- 按需开关变为「任一目标轨有观众就转」。
- 上游钩子不变（仍是 `onTrackReady` 换轨 + `onTrackFrame` 拦截），但 `addTrack` 要返回多个 track ⇒ 需要改 `MultiMediaSourceMuxer::onTrackReady` 的签名或增加 `addTracks()`。**这是唯一会碰钩子语义的地方，风险最高。**

---

## 5. 每阶段的固定验收动作（不许跳过）

1. **编译基线**：`cmake --build build-ffmpeg -j8` 零 error。
2. **基线回归**：`enable=0` 时无任何 `TranscodeSink`/`FFmpegEncoder` 日志，拉流编码与上游一致。
3. **端到端三件套**：拉流包数（≈ 时长×帧率）+ 音量比对（差 <1dB）+ 服务端 `dts` 严格单调。
4. **上游改动量**：`git diff --stat upstream/master` 记录行数，超过 300 行要说明原因。
5. **氧分**：每阶段独立提交，commit message 写清「实测什么、判据是什么、结论」。

---

## 6. 结构纪律（不变）

| 文件 | 改动性质 |
|---|---|
| `src/Codec/Transcoder.h/.cpp` | 新增，可自由演进 |
| `src/Common/TranscodeSink.h/.cpp` | 新增，可自由演进 |
| `src/Common/MultiMediaSourceMuxer.cpp/.h` | **只改钩子**，当前 111 行，上限 ~150 行 |
| `src/Common/config.cpp/.h` | 只加常量 |
| `ext-codec/*` | **一行不改**（尺寸由 SPS 解析自动补齐，见 §3.1）；若真必须改，单独提交并标注 rebase 风险 |
| `server/WebApi.cpp` | P8 才碰，只加 API 注册块 |

`./scripts/sync-upstream.sh dev/transcode` 保持 rebase，不 merge。

---

## 7. 明确不做

- ❌ 不改 `3rdpart/` 任何 submodule
- ❌ 不重构 `MediaSink` / `MediaSource` 内部
- ❌ 不给 `ProtocolOption` 加字段（作者自己分支这么干，5 个月没合回 master）
- ❌ 不为对齐商业版行为去分析其二进制（只参考公开文档与公开分支）
- ❌ 不上 GPU 编码（VideoToolbox/NVENC），先保证软编稳定
