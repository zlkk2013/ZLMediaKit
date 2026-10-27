# 第三阶段：集成与参数化（进行中）

> 第二阶段把 P6/P7/P8 在各自 worktree 并行做完了。本文档记录**集成结果**与**三线并行补全**的进展。
> 各条线的实测细节见各自文档：`TRANSCODE_PHASE3_P8.md`（配置/API）、`TRANSCODE_AUDIO_ROBUSTNESS.md`（音频）、`TRANSCODE_VIDEO_PARAMS.md`（视频参数）。

## 1. 集成结果（已完成）

### 1.1 三个阶段的完成状态

| 阶段 | 分支 | 提交 | 内容 | 验收证据 |
|---|---|---|---|---|
| P6 | `dev/p6-silence` | `13cd7b00` | 音频静音帧兜底 | 推流端 `kill -9` 后观众端仍收到 ~10 秒音频；对照组 PCM 冻结在 252128 字节；静音段 `-91 dB` |
| P7 | `dev/p7-video` | `121dae5d` | H264→H265 视频转码 | RTSP 10 秒 146 包（15fps 理论 150）、RTP 错误 0、解码错误 0、抽帧画面正常 |
| P8 | `dev/p8-config` | `c76dda8c` | 按流配置 + addTranscode/getTranscode | 9 组场景实测，含离线预设、覆盖优先级、热更新如实回报 |

### 1.2 合并过程

P6 与 P7 都改了 `src/Codec/Transcoder.cpp`，cherry-pick 时**只有一处冲突**，在 `FFmpegEncoder::onEncode` 的时间轴选择：

```
<<<<<<< HEAD (P7)                    ======= (P6)
if (codec_type == VIDEO) {           if (!_silence_encoding && ...) {
    next_ms = pkt_index * vstep;         next_ms = last_src_ms - base_ms;
} else if (last_src_ms ...) {        
```

两边逻辑都需要，解法是**保留两个分支**：视频走 P7 的匀速推进，音频走 P6 的 `_silence_encoding` 守卫。合并后：

- 集成分支 `dev/integ1` → 提交 `a69cd4a5`
- 主干 `dev/transcode` → 提交 `029f67b8`
- **上游文件改动 136 行**（上限 150）
- 主干编译通过，二进制 10,442,664 字节

### 1.3 音视频联合验证（集成分支实测）

配置 `video_codec=h265` + `audio_codec=opus` + `on_demand=0`，源 H264 320x240 15fps + AAC 44.1k，RTMP 推流，端口 41900/40554/44080：

| 项 | 结果 |
|---|---|
| 轨登记 | `H264 -> H265`、`mpeg4-generic -> opus`（两条都成功） |
| warmup | `config_nals=3, ready=1, size=320x240` |
| 视频 | `hevc 320x240`，**153 包**（10 秒 @15fps 理论 150） |
| 音频 | `opus 48000Hz 2ch`，**500 包** |
| RTP 错误 | **0** |
| 解码错误 | **0** |
| 转码计数 | 视频 `index=0`、音频 `index=1`、`silence=0` |

结论：音频静音兜底与视频转码可同时工作，互不干扰。

## 2. 三线并行补全（进行中）

集成完成后按**互不重叠的代码区域**分三条线并行，各自独立 worktree，避免再次冲突：

| 线 | worktree | 端口（RTMP/RTSP/HTTP） | 范围 | 只改 |
|---|---|---|---|---|
| 音频 | `a1` / `dev/a1` | 51900 / 51554 / 54080 | codec 覆盖（AAC/G711A/G711U/MP3）、采样率声道边界、静音兜底通用性 | `TranscodeAudioFifo`、`openAudioCodec`、`onSilenceTick` |
| 视频 | `a2` / `dev/a2` | 52900 / 52554 / 55080 | 码率控制、H264 输出、分辨率缩放、高分辨率高帧率 | `openVideoCodec`、`formatFrame` 视频分支、`warmup` |
| API | `a3` / `dev/a3` | 53900 / 53554 / 56080 | API 分离到新文件（解决 324 行超标）、合入 120 行改进、重测视频转码 | `WebApi.cpp`（≤20 行）、新建 `src/Transcode/` |

**分工原则**：不按「阶段」划分，按**函数/文件边界**划分。三条线共享同一个基线 `a69cd4a5`，各自只在指定区域内改动，集成时冲突面最小。

## 3. 已知问题清单

### 3.1 待解决

| 问题 | 现状 | 归属 |
|---|---|---|
| P8 上游改动 324 行，超 150 上限 | `WebApi.cpp` 独占 198 行，需要分离到新文件 | a3 |
| 码率控制未验证 | `bit_rate` 只在 open 时设一次，libx265 可能忽略，需实测 | a2 |
| 只出 H265，未验证 H264 输出 | H264 无 VPS，`splitAnnexb`/`warmup` 需适配 | a2 |
| 不支持分辨率缩放 | `FFmpegSws` 是否支持目标尺寸不同未验证 | a2 |
| 音频只验证 Opus | AAC/G711/MP3 目标 codec 未测 | a1 |
| 静音兜底只验证 Opus | 其他 codec 的 `frame_size` 特性未验证 | a1 |

### 3.2 设计边界（不是 bug）

| 边界 | 说明 |
|---|---|
| RTMP/HTTP-FLV 不承载 HEVC | 配 H265 时 FLV 出口回退源 H264，这是容器限制 |
| `MediaSource` 元数据仍是源编码 | 「换轨不换流」设计使然，`getMediaList` 显示源 codec |
| `on_demand=1` 无观众不转码 | 设计行为，节省 CPU |
| 已建编码器参数不可热改 | 改 codec 需等重推，API 如实返回 `pending` |
| API 覆盖不落盘 | 进程重启丢失，回到 ini 的值 |
| `FFmpegSwr` 上下文首次创建后固定 | 源格式运行中变化会出错 |

## 4. 验收纪律（三条线都必须遵守）

1. **编译基线**：零 error
2. **基线回归**：`enable=0` 时零转码日志，拉流编码与上游一致
3. **端到端三件套**：包数（≈ 时长×帧率）+ 音量/画面比对 + dts 严格单调
4. **零错误**：RTP 错误 0、完整解码错误 0（出现 `Illegal temporal ID`/`Multi-layer HEVC`/`Could not find ref` 即未通过）
5. **不许自述成功**：必须贴真实命令输出
6. **上游改动量**：`git diff --stat upstream/master`，总上限 150 行
7. **改完必须重启**：`pkill` 旧进程 + `/usr/sbin/lsof -nP -iTCP:<port> -sTCP:LISTEN` 确认端口释放 + 核对二进制 mtime

## 5. 三线合并结果（已完成）

合并顺序 a3 → a2 → a1，每步都编译 + 跑验收：

| 步骤 | 合并提交 | 冲突 | 处理 |
|---|---|---|---|
| a3 | `4b1e6ab4` | 无 | — |
| a2 | `78d741cb` | `TranscodeSink.cpp/.h`、`.gitignore` | 丢掉 a2 的 `loadFromIni`（解析已归 `TranscodeConfig`），把 4 个视频参数键接进 `TranscodeConfig::loadGlobalTranscodeOption` |
| a1 | `c879d4d7` | `Transcoder.cpp`、`TranscodeSink.cpp`、`.gitignore` | 丢掉 a1 的 `parseCodecName`（`TranscodeConfig` 的别名表更全），保留 a1 的 `openAudioCodec(..., target)` —— 函数体确实用它给 AAC/opus 目标轨回填 extradata，否则轨永远不 ready |

### 合并时才暴露的两个问题

1. **a3 重构时静默丢了 P6 的静音配置读取**：`TranscodeConfig` 没读 `enable_silence` / `silence_timeout_ms`，字段有默认值所以不报错、功能看着也正常，但配置文件里改这两个键完全无效。已补。
2. **a1/a2 用 `cat > .gitignore` 整文件重写**，合并后把上游 51 行忽略规则全冲掉了（`*.o`、`*.d`、`/X64/`、`*.DS_Store` 等只剩 12 行），还误提交了 `www/.DS_Store`。已恢复上游内容并在末尾追加本项目条目。合并进主干前必须逐文件看 diff，不能只看"编译过了"。

### 联合验收（`scripts/joint_accept.py`，38 项全绿）

专门验证三线合到一起后会不会互相踩：

| 组 | 内容 | 结果 |
|---|---|---|
| T1 | API 按流规则两路不同 codec（PCMA / opus）、`config_source` 命中规则、热改报 `pending` 且拉流仍是旧 codec | 6/6 |
| T2 | 5 个音频 codec 经 API 覆盖下发（opus/AAC/PCMA/PCMU/MP3），属性 + 0 硬解码错误 + 音量损耗 <1dB | 15/15 |
| T3 | H264 输出、缩放 640x480 与 160x120、码率 150k→1200k 输出 254,866→1,413,449 字节（5.5 倍） | 10/10 |
| T4 | 双轨同时转码 tracks 明细正确、推流 kill -9 后流维持 9.99s 且兜底段 -91.0dB 静音 | 5/5 |
| T5 | 720p 双轨 + 两个 RTSP 客户端连断，**6 轮 0 崩溃** | 1/1 |

### 最终形态

- 主干 `dev/transcode`，上游文件改动 **165 行**（`.gitignore` 14 + `WebApi.cpp` 18 + `MultiMediaSourceMuxer` 77 + `config` 57），略超 150 目标（4 个视频参数键 + 2 个静音键顶上来）
- 新增文件不受额度约束：`Transcoder` 1392+318、`TranscodeSink` 695+282、`src/Transcode/` 1003 行
- 部署注意：新增 `src/Transcode/` 子目录后**必须重跑 `cmake`**（`src/CMakeLists.txt` 用 `file(GLOB)`，configure 期展开），否则链接期报 `TranscodeRuntime` / `addTranscodeApi` 未定义

### 遗留

- **8k 源 → 8k G711 内容放大 6.4 倍**（详见 a1 的 `TRANSCODE_AUDIO_ROBUSTNESS.md` §5）。已隔离到转码路径的「免重采样」分帧支路，同素材纯转发正确。**下一轮第一优先。**
- 目标 codec 与源相同时走透传、不补静音帧（设计边界，已在文档说明）
- 4K 源、>4 路并发、Windows 平台、长时（>1 小时）稳定性均未测
