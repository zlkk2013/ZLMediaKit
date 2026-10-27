# P8 收口：HTTP API 与配置（真机实测）

> 在 `a3` worktree 完成，基线 `a69cd4a5`（P6 静音兜底 + P7 视频转码已集成）。
> 本文所有数字来自 `scripts/a3_accept.py` 的真实输出，**33 项断言全绿**。
> 复现：`env -u PYTHONPATH python3 scripts/a3_accept.py`（端口 53900/53554/56080）。

## 1. 为什么要收口

上一版 P8 把 API 实现直接写进 `server/WebApi.cpp`，上游改动 324 行，超了 150 行上限一倍多。这一版把实现搬到新目录，`WebApi.cpp` 只留注册钩子。

| 文件 | 改动 | 说明 |
|---|---|---|
| `server/WebApi.cpp` | **+18 行** | 仅 include + 两个 `api_regist` + `CHECK_SECRET()` |
| `src/Transcode/TranscodeApi.{h,cpp}` | 新增 419 行 | `addTranscode` / `getTranscode` 全部实现 |
| `src/Transcode/TranscodeConfig.{h,cpp}` | 新增 564 行 | 规则解析、覆盖表、sink 登记与热更新 |
| `src/Common/TranscodeSink.{h,cpp}` | +189/-89 | 加 getter 与状态查询，帧处理逻辑未动 |
| `src/Common/MultiMediaSourceMuxer.cpp` | +72/-1 | 懒创建改走 `TranscodeRuntime::resolve()` |

**上游文件总改动 139 行**（`git diff --stat upstream/master --diff-filter=M` 的 insertions），在 150 上限内。同时把 `MultiMediaSourceMuxer.h` 里原有的 `setTranscodeSink` 声明删掉了——配置解析统一走 `TranscodeRuntime`，这个旧钩子不再需要，反而省出上游改动额度。

## 2. 配置优先级

```
API 覆盖（addTranscode）  >  按流规则（stream_rule）  >  全局段（[transcode]）
```

`getTranscode` 用 `config_source` 明示当前生效的是哪一层，实测取值：`global`、`stream_rule(live/cam1)`、`api`、`off`。

`stream_rule` 语法：`app/stream:key=value,key=value`，也可写全 `vhost/app/stream:...`；键支持 `enable / audio_codec / video_codec / bitrate / sample_rate / on_demand`。规则里显式写了 codec 就视为要开转码，不必再要求全局 `enable=1`。

## 3. 实测结果（33/33 PASS）

### 3.1 错误处理

| 场景 | 返回 | 判定 |
|---|---|---|
| 错 secret | `code=-100 Please login first` | ✅ 拒绝 |
| 缺 app/stream | `code=-300 Required parameter missed: app, stream` | ✅ |
| 非法 codec | `code=-300 invalid audio_codec: nosuchcodec` | ✅ |
| 非法 bitrate | `code=-300 invalid bitrate, must be a non-negative integer` | ✅ |
| 查离线流 | `code=0` + `online=false` + 无 `tracks` | ✅ **不算错误** |

最后一条是刻意的接口约定：查一个还没上线的流不是错误，返回「若上线会用什么配置」让运维能在推流前预检；只有参数缺失/非法才给错误码。`tracks` 为空而不是编造零值明细。

### 3.2 基线回归（enable=0）

拉流编码 `['aac', 'h264']` 与源一致，`grep -i transcod` 日志计数 **0**。关掉转码时行为与上游完全一致。

### 3.3 按流规则（两路不同 codec 同时推）

`cam1:enable=1,audio_codec=opus,sample_rate=48000` / `cam2:enable=1,audio_codec=g711a,sample_rate=8000`，全局 `video_codec` 留空：

| 流 | 拉流实测 | failed_tracks | config_source |
|---|---|---|---|
| cam1 | `opus 48000Hz 2ch` | 0 | `stream_rule(live/cam1)` |
| cam2 | `pcm_alaw 8000Hz 1ch` | 0 | `stream_rule(live/cam2)` |

cam1 的视频保持 `h264 320x240` 未转——全局 `video_codec` 为空就是不转视频，规则里也没写 `video_codec`，符合预期。两路互不影响。

### 3.4 API 覆盖 ini

给 `ovr` 流（规则里配的是 opus）预设 `audio_codec=g711u`，再推流：

- `addTranscode` 返回 `applied=waiting` + `persistent=false` + `code=0`
- 拉流实测 `pcm_mulaw 8000Hz 1ch` —— **API 覆盖赢了规则里的 opus**
- `config_source=api`

`waiting` 是诚实的答案：流没上线，谈不上「已生效」。上一版设计的 `immediate` 在这里是错的——没有 sink 就没有生效对象。

### 3.5 热更新如实回报

运行中把 opus 改成 g711a：

| 检查 | 结果 |
|---|---|
| `applied` | **`pending`**（不谎报立即生效） |
| 改完立刻拉流 | 仍是 `opus` |
| 返回体 note | 「重推后生效；on_demand/bitrate 立即生效；不落盘，重启回 ini」 |

已有编码器的 codec/采样率不可能热改（AVCodecContext 已 open），所以 `pending` 是唯一诚实的值。`on_demand` 和 `bitrate` 这类不涉及编解码器重建的参数才走 `immediate`。

### 3.6 音视频同时转码

`audio_codec=opus` + `video_codec=h265`，`getTranscode` 的 `tracks` 明细：

```json
{"frames": 139, "index": 0, "source_codec": "H264", "target_codec": "H265", "transcoding": true, "type": "video"}
{"frames": 369, "index": 1, "source_codec": "mpeg4-generic", "target_codec": "opus", "transcoding": true, "type": "audio"}
```

拉流 `['hevc', 'opus']`；6 秒计包 **视频 92 / 音频 301**（理论 90 / 300）。`failed_tracks=0`。

这条同时纠正了上一版文档里「视频转码不可用、failed_tracks=1」的结论——那是 P7 完成前的状态，现在两条轨都真在转。

## 4. 持久化边界

**API 覆盖不落盘**，重启回到 ini 的值。这是有意的：无状态控制面，避免 API 与配置文件两套真相互相覆盖。

为了让调用方不被误导，`addTranscode`/`getTranscode` 的返回体都带 `persistent:false`。实测确认该字段为 `false`。

## 5. 踩坑记录（测试环境本身会骗人）

这一节的坑比产品 bug 更值得记，因为它们会让测试给出**看似合理但完全错误**的结论：

| 坑 | 现象 | 根因 | 对策 |
|---|---|---|---|
| 旧进程占端口 | 改了配置完全没生效，`getTranscode` 报 `enable=false` | `pkill -f 'MediaServer -c <abs path>'` 漏掉用相对路径起的旧进程；新进程绑定失败静默退出，API 打到旧进程 | 先 `pkill -9 -f MediaServer`，再用 `lsof -tiTCP:<port>` 确认端口空，最后校验监听 pid == 自己起的 pid |
| 配置没核对就开测 | 测试全绿但测的是别的配置 | 同上，或 `-c` 路径写错 | 起服务后先用 `getServerConfig` 断言 `transcode.enable/audio_codec/video_codec` 与配置文件一致，不一致直接中止 |
| `on_demand` 落到默认值 | `frames=0`，拉流是源编码，像「转码没生效」 | 测试脚本重写 `[transcode]` 段时把 `on_demand=0` 一起删了，默认值是 `1`（无观众不转码） | 生成测试配置时必须显式写 `on_demand=0` |
| 用 RTMP 验转码 | 配了 h265/opus，拉流还是 h264/aac | **RTMP/FLV 容器不承载 HEVC 与 Opus**，出口回退源编码 | 验转码一律用 RTSP 拉流 |
| ffprobe 多流计数 | 音频 `nb_read_packets=0`，但服务端明明转了 369 帧 | 多流一起数时第二路常给 0 | 音视频分开用 `-select_streams a/v` 各起一个进程数 |

## 6. 未验证项

- `stream_rule` 的 `vhost/app/stream` 三段写法只验证了解析，没有起第二个 vhost 实际推流验证匹配
- 规则的 `stream` 字段留空（匹配该 app 下所有流）未实测
- API 覆盖与 `on_demand` 的 `immediate` 生效路径只看了返回码，没有量化转码启停的时延
- 多路（>2）流同时转码未压测
- Windows 平台未验证
