# 音视频转码功能 — 开发计划

> 基线：`zlkk2013/ZLMediaKit` fork，分支 `dev/transcode`，起点 `fcb2889a`（追平官方 master）。
> 结构纪律：**新增文件为主，上游文件只留极薄钩子**（见 §6），保证长期能同步上游。

---

## 1. 目标（分阶段，每阶段可独立验收）

| 阶段 | 交付 | 验收判据 |
|---|---|---|
| **P0** 环境 | ffmpeg 可用 + ZLM 能以 `ENABLE_FFMPEG=ON` 编过并能起 | 原功能不退化：RTSP/RTMP 推拉流正常 |
| **P1** 编码器 | `FFmpegEncoder` + `FFmpegAudioFifo` + 单元测试 | 单测：AAC→Opus 帧数/时间戳正确，无泄漏 |
| **P2** 音频转码节点 | `TranscodeSink`（音频路径）+ 挂进 muxer | **同流 id**，rtsp 音轨从 AAC 变 Opus，ffplay 可听 |
| **P3** 按需开关 | 无人观看不转，有人看才转 | 日志/CPU 对比：无人看时无编码活动 |
| **P4** 视频转码 | H.265→H.264 + 透明转码 | 同流 id，分辨率/帧率保持，播放器无感 |
| **P5** 对外接口 | `[transcode]` 配置段 + `addTranscode`/`delTranscode` API | 配置/API 生效，与 Pro 公开文档用法对齐 |

先做 P0→P1→P2，**P2 是关键验证点**：它一旦通过，"换轨不换流"这个核心范式就被证实，P4/P5 只是参数和接口包装。

---

## 2. P0 环境

需确认/完成：

- [ ] `brew install ffmpeg`（含 libopus、libx264）
- [ ] 用独立 build 目录，不动现有 `build/`（保留一份已知可用的非 ffmpeg 版本做对照）
  ```bash
  cd ~/ZLMediaKit
  cmake -S . -B build-ffmpeg -DCMAKE_BUILD_TYPE=Release \
        -DENABLE_FFMPEG=ON -DENABLE_X264=ON
  cmake --build build-ffmpeg -j8
  ```
- [ ] `MediaServer` 能起、能推拉流（回归）

风险：`ENABLE_FFMPEG` 打开后 3rdpart 可能自动拉 ffmpeg 源码编译（慢/可能失败）。对策：优先让它用系统 ffmpeg；实在不行再单独处理。

---

## 3. P1 编码器

主干 `src/Codec/Transcode.h` 已有 6 个类，**缺 encoder**：

```
已有：FFmpegFrame / FFmpegSwr / FFmpegSws / FFmpegDecoder / FFmpegUtils / TaskManager
需补：FFmpegAudioFifo   ← AAC 要 1024 采样/帧，pcm 长度不齐必须攒
需补：FFmpegEncoder     ← 音视频编码，setOnEncode 回调出 Frame
```

设计要点（照范式，不照抄）：

- `FFmpegEncoder(const Track::Ptr &track, int thread_num)`：构造时按 track codec 选 encoder，`openVideoCodec` / `openAudioCodec` 分开。
- `inputFrame(const FFmpegFrame::Ptr&, bool async)`：异步走 `TaskManager` 线程，**不阻塞分发线程**。
- `encodeFrame(AVFrame*)` → `avcodec_send_frame` → `onEncode(AVPacket*)` → `avcodec_receive_packet`。
- `flush()`：流结束时清空编码器内部缓冲（**否则尾巴几帧丢掉**）。
- 时间戳：输出 Frame 的 dts/pts 必须**继承输入**，否则播放器时间轴乱。
- 错误处理：`avcodec_open2` 失败要明确报出 codec 名 + 原因，不要静默返回 nullptr。

验收：单测（`tests/test_transcode.cpp`）不依赖网络和真流，纯内存喂 PCM/AAC 帧，断言输出帧数与时长。

---

## 4. P2 音频转码节点（关键验证点）

`TranscodeSink` 是 `MediaSink` 的子类（不是新框架）：

```
输入 track(AAC/G711) ──► FFmpegDecoder ──► FFmpegEncoder(Opus) ──► 同名 muxer 的 inputFrame
                                                                      （流 id 不变）
```

- 只在音频轨上挂；视频轨原样透传。
- 转换后 track 的 codec/采样率/声道要重新登记给下游 muxer（`onTrackReady` 阶段）。
- **静音帧**：目标 codec 是 Opus 时，`MuteAudioMaker` 只会出 AAC，必须补 Opus 的静音帧（20ms）。这是"视频通了但播放器卡住"的典型原因。

验收：
1. 推一路带 AAC 的 RTSP 流进来；
2. `ffprobe` 播 `rtsp://127.0.0.1/live/test`，**URL 完全不变**，音频 codec 显示 `opus`；
3. `ffplay` 能正常听，无断续。

---

## 5. P3/P4/P5

- **P3 按需**：`totalReaderCount() == 0` 时跳过解码/编码（不是缓存，是直接不转）。要有日志能证明"确实没转"。
- **P4 视频**：先 `FFmpegSws` 做解码后缩放（若需），再 H.265→H.264 编码；关键帧间隔、码率、profile 要对齐输入。
- **P5 接口**：`[transcode]` 段名/字段、`addTranscode`/`delTranscode` 的**参数名和语义对齐 Pro 公开文档**（接口兼容，运维习惯一致）。

---

## 6. 结构纪律（保证长期能同步上游）

实测：`onTrackReady` / `onTrackFrame` 近 6 个月上游改动 **0 次**；`MediaSink.*`、`Codec/Transcode.*` 也是 **0 次**。所以：

**允许改的上游文件，只有 3 个，每处 ≤ 3 行：**

| 文件 | 改动 |
|---|---|
| `src/Common/MultiMediaSourceMuxer.cpp` | `onTrackReady` +2 行、`onTrackFrame` +2 行 |
| `src/CMakeLists.txt` | +1~2 行加源文件 |
| `src/Common/config.cpp` | +N 行注册 `[transcode]` 段（P5） |

**明确不做**：
- ❌ 不给 `MediaSource.h` 的 `ProtocolOption` 加字段（作者在 own 分支这么干，结果那分支 5 个月没合回 master）
- ❌ 不重构 muxer 内部成员
- ❌ 不动 `3rdpart/` 下任何 submodule

同步流程：`./scripts/sync-upstream.sh dev/transcode`（rebase，不 merge）。

---

## 7. 任务拆解（按提交粒度，每个可独立回滚）

1. `chore(build): 加 build-ffmpeg 目录与编译脚本`
2. `feat(transcode): FFmpegAudioFifo`  + 单测
3. `feat(transcode): FFmpegEncoder`   + 单测
4. `feat(transcode): TranscodeSink 音频路径` + 单测
5. `feat(transcode): 挂到 MultiMediaSourceMuxer（2+2 行钩子）`
6. `feat(transcode): Opus 静音帧`
7. `test(transcode): 端到端脚本（推流→ffprobe→断言 codec）`
8. `feat(transcode): 按需开关`（P3）
9. `feat(transcode): 视频转码`（P4）
10. `feat(transcode): [transcode] 配置 + HTTP API`（P5）

---

## 8. 已解决与仍不确定

### 已证伪/已解决

1. ✅ `ENABLE_FFMPEG=ON` 在 arm64 macOS 能编过。**但 brew 的 ffmpeg 装不上**（macOS 14.8 = Homebrew Tier 3，无 bottle，8 个依赖要升级）。正解是 conda-forge：
   ```bash
   conda create -y -n zlm-ffmpeg -c conda-forge 'ffmpeg=7.*' pkg-config
   export PKG_CONFIG_PATH=~/miniconda3/envs/zlm-ffmpeg/lib/pkgconfig
   cmake -S . -B build-ffmpeg -DENABLE_FFMPEG=ON -DENABLE_X264=ON \
         -DCMAKE_PREFIX_PATH=~/miniconda3/envs/zlm-ffmpeg
   ```
   自带 libx264/libx265/libopus/aac，无需编译。
2. ✅ 端到端已跑通：ffmpeg 推 RTMP(AAC+H264) → ZLM 转码 → 拉 RTSP 得 Opus+H264。

### 仍不确定

1. 视频转码（H264→H265）只验证了"非目标轨透传"，没有真转。
2. 未跑过 Pro 试用版，行为对齐只能靠公开文档和作者公开分支推断。

---

## 9. 实测结果（2026-10-26）

环境：macOS 14.8.9 arm64，conda-forge ffmpeg 7（libavcodec 61.19.101），`ENABLE_FFMPEG=ON ENABLE_X264=ON`。

### 用例：AAC 44.1kHz 立体声 → Opus 48kHz

输入：`ffmpeg -f lavfi` 生成 320x240@15fps H264 + AAC 65kbps，RTMP 推到 `live/fin`。

| 指标 | 结果 |
|---|---|
| 拉流视频包（10 秒） | **150**（= 10s × 15fps，精确） |
| 拉流音频包（10 秒） | **885**（opus 20ms/帧） |
| 输出音频编码 | `opus`，48000Hz，2 声道 |
| 输出视频编码 | `h264`（原样透传） |
| 音频音量 | 转码后 mean **-24.1 dB** / max -20.7 dB；源 -24.1 / -20.6 dB（差 0.1 dB，有损编码正常损耗） |
| `avcodec_send_frame` 失败 | **0** |
| `avcodec_receive_packet` 失败 | **0** |
| 服务端 dts 推进 | 14219 → 16239，步进 20ms，严格单调 |

### 按需转码验证

| 场景 | 结果 |
|---|---|
| `readers=0`（无观众） | `transcoded` 计数 **0**，不消耗 CPU |
| `readers=1`（有观众） | 开始转码，每 100 帧打点 |

### 基线回归（`enable=0`）

| 指标 | 结果 |
|---|---|
| `transcode enabled` 日志 | **0** |
| `transcoded` 帧日志 | **0** |
| `TranscodeSink`/`FFmpegEncoder` 任意日志 | **0** |
| 拉流音频编码 | **aac 44100Hz**（未改动，与上游一致） |

结论：**不配置 `[transcode]` 时，行为与上游完全一致**。

---

## 10. 实施中踩到的坑（均已修复）

1. **`_variable_frame_size` 必须在 `avcodec_open2` 之后判断**。libopus 的 `frame_size` 由 open2 填成 960，open2 之前是 0，会让固定帧长编码器被误判为可变帧长，把 1024 采样的帧直接送进去，`avcodec_send_frame` 返回 `EINVAL`。症状是拉流只有 1 个包。

2. **目标轨必须继承源轨 `index`**。`Factory::getTrackByCodecId` 新建的 track index 默认 0，音频轨会与视频轨撞号，下游按 `frame->getIndex()` 分发时帧被丢。

3. **`onTrackFrame` 不能对全部帧 early return**。只有 `isTrackTranscoding(index)` 为真的轨才该被拦截；否则 `modify_stamp`、`paced_sender`、GOP 缓存 `_ring` 全部失效，观众只剩首帧，还会触发 `H264Rtmp.cpp:26 Assertion failed (pkt->size() > 9)`。

4. **转码输出时间戳必须跟随源流**。自维护时钟从 0 起算会让下游 `Stamp` 判定帧"过旧"而丢弃，症状是视频 0 包（音频因为一帧 20ms 短、容忍度高，反而能过）。改为跟随源帧 dts 后视频包数恢复正常。

5. **`needAudioTranscode` / `needVideoTranscode` 必须校验轨道类型**。否则拿视频 codec 去问音频规则会误判，产生 "transcode requested but unavailable" 的假警告。

6. **ffmpeg 头必须放在 `extern "C"` 块内**。`libavutil/opt.h` 不在上游 `Transcode.h` 的包裹范围内，漏掉会让 `av_opt_set` 被当 C++ 符号，链接报 undefined。

7. **macOS 没有 `timeout` 命令**。用 `ffmpeg -t N` 限时，否则 `timeout 30 ffmpeg ...` 直接 command not found，且被 grep 过滤后看起来像推流失败。

8. **`kTranscode*` 常量不能插在 `namespace RtpProxy` 内**。`kGopCache` 之后就在 RtpProxy 段里，插错位置报 `undeclared identifier`。要放在 `namespace mediakit` 顶层、`Client` 段之后。
