# VP8 / VP9 / AV1 / H266 作为转码目标的实测矩阵

> 分支 `dev/a5`（基于主干 085c9e5a）。本文所有数据都是**真机实测**：起服务 → 推流 →
> RTSP 拉流 → ffprobe 看 codec/分辨率 → 数包 → 完整解码数错误行。
> 复现命令见 §7，脚本在 `scripts/`。

## 1. 结论速览

| codec | 作为**目标**（源 H264/HEVC → 该 codec） | 作为**源**（该 codec → H264） |
|---|---|---|
| **VP8** | ✅ 打通（真机验证） | ✅ 链路通，但时间轴 2× 速（见 §6.6） |
| **VP9** | ✅ 打通（真机验证） | ✅ 链路通，但时间轴 2× 速（见 §6.6） |
| **AV1** | ✅ 打通（真机验证，服务端 RTP 合规；ffmpeg 客户端无 AV1 解包器，用自写解包器验证） | ⚠️ 未实测（ffmpeg 造不出 AV1 的 RTSP 源） |
| **H266** | ❌ 打不通：**依赖缺失**，ffmpeg 无 VVC 编码器 | ❌ 打不通：ZLM 侧无 H266 插件/解包器，ffmpeg 造源也无从谈起 |

**没有一条是靠「能出包」蒙过去的**：每一格都要求 ①拉流端 codec/分辨率正确
②包数 ≈ 时长×帧率 ③`ffmpeg -v error -t 5 -f null -` 完整解码 0 错误。

## 2. 先纠正一个旧结论（§10.3 的「载荷表缺失」）

`docs/TRANSCODE_AUDIO_FRAMING_FIX.md` §10.3 曾把 VP8/VP9 不能投递归因于
「`src/Rtsp/Rtsp.h` 里没有对应载荷类型」。**这个归因是错的**，实测 SDP 一直是对的：

```
m=video 0 RTP/AVP 96
a=rtpmap:96 VP8/90000      ← 动态 payload type 96 自动分配
a=rtpmap:96 VP9/90000
a=rtpmap:96 AV1/90000
```

`RTP_PT_MAP` 是**静态载荷表**（pt<96 的 RFC 3551 固定映射）；`RtspMuxer::addTrack`
对表里没有的 codec 取 `getPayloadType()==-1`，自动用 `96 + track_index` 动态 PT，
rtpmap 名字取自 `Track::getCodecName()`。H264/H265 一直就是这么走的。所以
**根本不需要往载荷表里加条目**（加了反而会破坏「pt→codec」的反查语义）。

真正拦路的是三件事，全部在下面定位到具体代码行。

## 3. 四个卡点与修法

### 3.1 卡点一：目标轨永远 unready → RTSP muxer 直接丢轨（VP8）

现象（修复前基线日志）：

```
Transcoder.cpp:1220 warmup | warmup done: config_nals=0, ready=0, size=0x0
TranscodeSink.cpp:286 | warmup failed, target track not ready, video will be dropped by muxers
RtspMuxer.cpp:70 | VP8 unready!
```

根因：`FFmpegEncoder::warmup()` 只会按 **Annex-B 起始码**拆 NAL，再把 H264 的
SPS/PPS（或 H265 的 VPS/SPS/PPS）喂给目标轨，让 `VideoTrackImp::ready()` 变真
（`ready()` 要求宽高 > 0）。VP8/VP9/AV1 **没有起始码、没有 SPS/PPS**，宽高只能从
关键帧自身（VP8/VP9 帧头、AV1 sequence header OBU）解析，于是 `fed_config=0`、
宽高恒为 0，`RtspMuxer::addTrack` 见到 `ready()==false` 就整轨丢弃。

修法：`warmup()` 里增加裸帧 codec 分支 —— 把编码器**首包整帧**交给目标轨解析宽高，
但不往 downstream 转发（pts=0 与真实时间轴不连续，会打乱 RTP 环形缓冲的时序）。

### 3.2 卡点二：libvpx 默认「离线高质量」档，首帧不出包（VP8/VP9）

修复前 VP9 的日志与 3.1 同一个结局，但原因不同：

```
warmup produced no packet     ← 编码器第一帧没吐包
```

libvpx 默认 `deadline=good` + `lag-in-frames=25`（前向参考），首帧会被缓冲住。直播
必须显式切实时档，且 `cpu-used=8`、`lag-in-frames=0`、`auto-alt-ref=0`
（auto-alt-ref 依赖 lookahead，不同时关掉 open2 会失败）。

### 3.3 卡点三：AV1 没有编码器分支（+ 同样的 warmup 坑）

`findEncoder()` 里原本只有 VP8/VP9 的 case。现在接 `libaom-av1`
（`lag-in-frames=0` + `usage=realtime` + `cpu-used=8`）。

**为什么不用更快的 libsvtav1**：SVT-AV1 有内部 lookahead，第一帧一个包都不吐，
warmup 必然失败（"warmup produced no packet"），目标轨永远 unready。libaom 在
`lag-in-frames=0` 下首帧就出包，实测 320×240@15 每秒内容编码耗时 < 0.5s，够用。

### 3.4 卡点四：编码包被当成 Annex-B 处理（三个 codec 都中）

`onEncode()` 原来对所有 codec 都做「前置 4 字节起始码 + prefixSize=4」。对
VP8/VP9/AV1 这等于在裸帧前面塞了 4 个字节垃圾：下游 `VP8RtpEncoder` 会把它们当成
**VP8 payload descriptor**、`AV1RtpEncoder` 会当成 **aggregation header**，观众端
必然解不出来。现在裸帧 codec 原样交出、`prefixSize=0`。

同时 `TranscodedFrame` 的关键帧判定原本只认 H264/H265 的 NAL 类型，对裸帧 codec 会
按首字节乱猜。现改为由编码器包的 `AV_PKT_FLAG_KEY` 直接给出（`setKeyFrameFromPacket`），
否则 GOP 缓存不从关键帧起、观众中途接入解不出来。

### 3.5 顺带修掉的两个真 bug（不修则 VP8/VP9 只是「看起来能跑」）

**(a) `PacketCache` 数据竞争 → SIGSEGV（先决条件）**

RTSP 推流进来的源 + 转码时，视频帧来自编码器线程、音频帧来自推流端线程，两条线程
同时进到同一个 `PacketCache`。`flush()` 里 `onFlush(std::move(_cache))` 与随后
重建 `_cache` 之间有窗口，另一条线程在窗口里 `emplace_back` 就踩空指针。实测多次在
**50~100 个包处崩溃**（不是每次都崩，取决于两条线程的交错），栈顶是 `RtspMediaSource::onWrite`：

```
[2] RtspMediaSource::onWrite + 816
[3] RingBuffer<RtpPacket>::write
[4] RtspMuxer::onRtp
```

对照组（同样拓扑但**不转码**）不崩，H265 目标同样崩 —— 证明与 codec 无关，是这条
「RTSP 进 + 转码出」拓扑暴露出来的通用竞争。修法：`PacketCache::inputPacket/flush/
clearCache` 全部加锁，内部用 `flush_l()` 避免自锁。修完后同一组用例连跑多轮不再崩，
之前偶发的 `MPEG.cpp:136 Assertion failed` 也消失了（它是被踩坏的下游状态）。

**(b) VP8 的 N 位反了**

`VP8RtpEncoder` 原来写 `info.nonReference = !frame->dropAble()`。N 位语义是
「这一帧不被后续帧参考（可丢）」，取反等于把所有关键帧/参考帧都标成「可丢」，
浏览器 jitter buffer 会据此丢帧。现按 `dropAble()` 取值。

## 4. 实测矩阵（最终一轮）

命令：`env -u PYTHONPATH python3 scripts/video_codec_matrix_test.py`
（每组独立进程、独立推流、跑完杀进程后重启；判定见 §1）

| 用例 | 源 | 目标 | 拉流端 codec | 分辨率 | 5s 包数 | 解码帧数 | 解码错误 |
|---|---|---|---|---|---|---|---|
| h264→vp8 | h264 320×240@15 | VP8 | vp8 | 320×240 | 76 | 76 | 0 |
| h264→vp9 | h264 320×240@15 | VP9 | vp9 | 320×240 | 76 | 76 | 0 |
| hevc→vp8 | hevc 640×360@15 | VP8 | vp8 | 640×360 | 76 | 75 | 0 |
| hevc→vp9 | hevc 640×360@15 | VP9 | vp9 | 640×360 | 76 | 75 | 0 |
| h264→av1 | h264 320×240@15 | AV1 | av1 | 320×240 | 90¹ | 90 | 0 |
| hevc→av1 | hevc 640×360@15 | AV1 | av1 | 640×360 | 77¹ | 77 | 0 |
| h264→vp8（RTMP 推流） | h264 320×240@15 | VP8 | vp8 | 320×240 | 76 | 72 | 0 |
| h264→h266 | h264 320×240@15 | H266 | **h264（透传）** | 320×240 | 73 | 71 | 0 |

**32 项判据全过（PASS 32 / FAIL 0）**。期望包数是 `5s × 15fps = 75`；
连跑 3 轮结果一致（每轮都是 32/0）。

¹ AV1 这一列是**解码出的 temporal unit 数**（ffmpeg 客户端没有 AV1 的 RTP 解包器，
拿不到 RTP 包数）。AV1 的原始 RTP 包数是 117~327（一帧被切成多个包），不能直接和
帧率×时长比。

### 4.1 AV1 是怎么验的（重要）

`ffprobe -i rtsp://...` 对 AV1 会报 `unknown`：

```
$ ffprobe ... -show_entries stream=codec_name   # 自造 SDP 的最小复现
AV1 -> unknown
VP9 -> vp9
VP8 -> vp8
```

`strings libavformat.dylib | grep -x AV1` 无命中，而 `VP8`/`VP9` 有 —— ffmpeg 7.1
的 libavformat 根本没注册 AV1 的 RTP 解包器。**这是验证工具的限制，不是服务端的问题**，
所以另写了 `scripts/av1_rtp_verify.py`：

1. 裸 socket 走 RTSP：DESCRIBE → SETUP(interleaved TCP) → PLAY
2. 按 RFC 9628 解聚合头（Z/Y/W/N）与 OBU 元素（除最后一个外都有 leb128 长度）
3. 跨包重组被切开的 OBU，按 RTP 时间戳聚成 temporal unit
4. 跳过中途接入的半个 GOP（等第一个带 sequence header 的单元，和播放器行为一致）
5. 写 IVF → ffmpeg 解码 → 数帧、数错误

服务端打出来的聚合头与 RFC 9628 一致：

```
agg=0x50 (Z=0 Y=1 W=1)  → 帧 OBU 被切开，续到下一个包
agg=0x90 (Z=1 Y=0 W=1)  → 续包
agg=0x10 (Z=0 Y=0 W=1)  → 完整帧
```

重组后 `ffprobe` 识别为 `av1,320,240` / `av1,640,360`，解码 0 错误。

## 5. H266：依赖缺失，不是没接

```
$ ~/miniconda3/envs/zlm-ffmpeg/bin/ffmpeg -hide_banner -encoders | grep -Ei 'vvc|h266'
（无输出）
$ ... -decoders | grep -Ei 'vvc'
 V....D vvc    VVC (Versatile Video Coding)   ← 只有解码器
```

链接的是 ffmpeg 7.1.1（conda `zlm-ffmpeg`，libavcodec 61.19.101）。ffmpeg 至今
**没有自带 VVC 编码器**，编码要靠外部 `libvvenc`，主流发行版/conda 构建都没开。

服务端行为（已核实）：配 `video_codec=h266` 时 `setupTrack` 抛
`未找到编码器: H266`，整条视频轨**回落透传**（观众拿到 h264，不黑屏、不报错）：

```
TranscodeSink.cpp:298 setupTrack | transcode setup failed for H264 -> H266 : 未找到编码器: H266
TranscodeSink.cpp:369 addTrack | transcode requested but unavailable, fallback to passthrough
```

另外 ZLM 侧也没有 H266 的 codec 插件（无 `H266Track`/`H266RtpEncoder`），所以即使
ffmpeg 有编码器，还得补一个 RTP 打包器（RFC 9627 的 VVC payload）才能投递。两件事
都没做，如实记为「打不通」。

## 6. 已知问题 / 未修项

### 6.1 RTSP 推流 + 转码的 teardown（已缓解，非本次引入）

`[rtsp] directProxy=1`（默认）时 RTSP 推流进来的源 RTP 被直接透传给观众，
转码输出根本不进这条链路（§10.4 已分析）。测试因此把 `directProxy` 关掉。
关闭后这条拓扑在**断流收尾**时偶发崩溃（`FMP4MediaSource::onWrite` /
`RtspMediaSource::onWrite`），H265 目标同样复现 —— 属既有问题，不在本次范围。
`PacketCache` 加锁后，实测本轮 8 个用例 ×3 轮未再出现。

### 6.2 TS/HLS 对 VP8/VP9/AV1 没有可用输出

`MultiMediaSourceMuxer` 会为每条流建 TS/HLS muxer。`getMpegIdByCodec` 把
VP8/VP9/AV1 映射到 `PSI_STREAM_VP8/VP9/AV1`，`MpegMuxer::addTrack` 因而接受该轨，
但播放器侧没有可用的 HLS 输出。测试矩阵里显式关掉 `enable_hls`/`enable_ts`
（只判定 RTSP 投递这一件事）；**默认配置下（TS/HLS 开着）VP9 目标同样能正常
拉 RTSP、未再出现断言崩溃**（`PacketCache` 修完后复测通过）。

### 6.3 AV1 + fMP4：libmov 的 AV1 写入会踩坏堆（**未修，默认配置有风险**）

默认配置里 `enable_fmp4=1`，`MultiMediaSourceMuxer` 会把每条轨都塞给 fMP4/MP4 muxer。
AV1 走的 `MP4FileMemory::onWrite` 会偶发堆损坏：

| 场景 | 结果 |
|---|---|
| AV1 目标 + `enable_fmp4=1`（默认） | **6 次里 1 次** SIGABRT/SIGSEGV，栈顶分别是 `MP4FileMemory::onWrite`（std::string 扩容时 free 报错）与 `libaom aom_memalign` |
| AV1 目标 + `enable_fmp4=0` | 6/6 干净 |
| VP8/VP9 目标 + `enable_fmp4=1` | 4/4 干净（不触发） |

结论：**RTSP 投递本身没问题**（fmp4 关掉时 6/6 通过、拉流 0 错误），坏在 libmov 的 AV1
写 MP4 路径。测试矩阵因此显式关掉 fmp4（与关 hls/ts 同一个理由：只判定 RTSP 投递）。
真正的修法有两条：①fMP4/MP4 muxer 对不支持的 codec 直接拒绝该轨（`addTrack` 返回
false）；②修 libmov 的 AV1 sample 写入。两条都没在本轮做 —— 属第三方库/架构层。

### 6.4 libaom 的 usage=realtime（nonrd）路径也会踩坏堆（已规避）

最初按「直播要快」给 libaom 设了 `usage=realtime`，实测 6 次里 1 次在
`av1_alloc_pmc <- av1_nonrd_use_partition` 崩。改成默认 `usage=good` + `cpu-used=8`
（并 `thread_count=1`）后 6/6 稳定，320×240@15 的编码耗时仍远低于实时。

### 6.5 AV1 作为源：未实测

ffmpeg 既没有 AV1 的 RTP 解包器也没有**打包器**，造不出 AV1 的 RTSP 推流源。
ZLM 侧的解码链路是齐的（`AV1RtpDecoder` + `AV1Track` 已注册），但没有可用的造源
工具，所以记为「未实测」，不写成「支持」。

### 6.6 VP8/VP9 作为源：转码时间轴 2× 速（**未修**）

实测：VP8 经 RTSP 推入（15fps），配 `video_codec=h264`，拉 5.0 秒内容
**实际花了 9.97 秒**，输出 153 帧 / 5.016 秒（= 30.5fps）。

根因：VP8/VP9 的码流和 SDP 里都没有帧率信息，`VideoTrackImp(CodecId)` 的默认
`_fps=30` 被 `TranscodeSink` 当成源帧率传给编码器，于是每帧按 33ms 打时间戳，
而真实帧是 66ms 一帧 → 输出快一倍。H264/H265 不受影响（SPS/VUI 里有帧率）。

修法需要按 RTP 时间戳估算源帧率（或在 `RtspDemuxer` 里给轨设 fps），属于解复用层
改动，未在本轮做。**因此上表的「作为源 ✅」只代表「能解、能转、能拉」，
不代表时间轴正确。**

## 7. 复现

```bash
export PKG_CONFIG_PATH=$HOME/miniconda3/envs/zlm-ffmpeg/lib/pkgconfig
cd <worktree>
cmake -S . -B build-wt -DCMAKE_BUILD_TYPE=Release -DENABLE_FFMPEG=ON -DENABLE_X264=ON -DENABLE_TESTS=OFF
cmake --build build-wt -j4 --target MediaServer

# 目标方向矩阵（VP8/VP9/AV1/H266 × H264/HEVC 源）
env -u PYTHONPATH python3 scripts/video_codec_matrix_test.py
# 反向矩阵（VP8/VP9 作为源）
env -u PYTHONPATH python3 scripts/codec_source_matrix_test.py
# AV1 单独验证（自写 RFC 9628 解包器）
python3 scripts/av1_rtp_verify.py rtsp://127.0.0.1:60554/live/<stream> 5 320 240 15
```

端口：RTMP 60900 / RTSP 60554 / HTTP 61080（脚本独占，起服务前 `pkill -9 -f
MediaServer` 并核对监听 pid）。`config.codec_matrix.ini` 由脚本从 `config.av.ini`
派生：`[transcode]` 全局段清空（codec 走 `addTranscode` 接口按流下发）、
`[rtsp] directProxy=0`、`enable_hls/enable_ts=0`。

## 8. 改了哪些文件

| 文件 | 改动 |
|---|---|
| `src/Codec/Transcoder.cpp` | ①`findEncoder` 加 AV1（libaom-av1）、H266（显式失败并说明）②`openVideoCodec` 给 libvpx 上实时档、给 libaom 关 lookahead ③`warmup` 加裸帧 codec 分支 ④`onEncode` 裸帧 codec 原样输出（prefixSize=0）⑤`TranscodedFrame` 支持编码器给出的关键帧标志 |
| `src/Common/PacketCache.h` | 合写缓存加锁（修 SIGSEGV 竞争，先决条件） |
| `ext-codec/VP8Rtp.cpp` | 修正 N 位（non-reference）取反 |
| `scripts/video_codec_matrix_test.py` | 新增：目标方向矩阵 |
| `scripts/codec_source_matrix_test.py` | 新增：反向矩阵（作为源） |
| `scripts/av1_rtp_verify.py` | 新增：AV1 over RTSP 的独立验证器（RFC 9628 解包） |
| `docs/TRANSCODE_CODEC_MATRIX.md` | 本文 |
| `docs/TRANSCODE_AUDIO_FRAMING_FIX.md` | §10.3 旧结论加更正指引 |
