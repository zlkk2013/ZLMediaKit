# 音频分帧缺陷修复与矩阵测试

> 这是 `dev/transcode` 主干上的一次真实缺陷修复记录。
> 测试代码：`scripts/audio_matrix_test.py`（5 源 × 4 目标 = 20 组，逐组重启服务真机拉流）
> 复现：`env -u PYTHONPATH python3 scripts/audio_matrix_test.py`

## 1. 缺陷是什么

**8kHz 源 → 8kHz G711 时，内容被放大约 6.4 倍**：拉 5 秒音频，解码出来是 31.88 秒、1589 个包（正确值 250 包 / 5.000 秒）。

用户侧表现：G711 通路的对讲/广播声音被拉慢、音调变低、延迟越来越大。

## 2. 为什么只有这一种组合出问题

| 源 | 目标 | 目标采样率 | 是否重采样 | 结果 |
|---|---|---|---|---|
| 8k 单声道 | PCMA | 8000 | 否 | **放大 6.4 倍** |
| 8k 单声道 | opus | 48000 | 是 | 正确（250 包 / 5.000s） |
| 44.1k 立体声 | PCMA | 8000 | 是 | 正确 |

G711 的 RTP 载荷规格固定 8000Hz 单声道，所以**源本身就是 8k 时才不触发重采样**。不重采样时 `formatFrame()` 认为"格式已经一致"，把源帧原样返回，接着走了「变长帧直通」分支，把 160 样本的帧直接塞进编码器。

## 3. 真正的根因

`PCM_ALAW` / `PCM_MULAW` 的 `frame_size` 报 **0**，看起来像"变长帧、随便多长都收"。但它们**没有** `AV_CODEC_CAP_VARIABLE_FRAME_SIZE` 能力 —— ffmpeg 会按默认帧长 **1024** 把握手帧补零。

实测证据（临时插桩，已移除）：

```
输入帧： nb=160  rate=8000   （20ms，AAC 解码出来的帧长）
输出包： size=1024 pts=0,1024,2048,3072...   ← 每包 1024 样本，pts 每次 +1024
```

1024 / 160 = **6.4** —— 与观测到的放大倍数完全一致。

## 4. 修法

**所有音频 codec 一律走 FIFO 分帧，切帧长度由编码器自身能力决定，绝不因为 `frame_size==0` 就直通。**

新增单一来源函数：

```cpp
int FFmpegEncoder::audioChunkSamples() const {
    // frame_size > 1 的编码器（libopus 960、AAC 1024、libmp3lame 1152）用它自己的；
    // frame_size 为 0/1 的（G711 这类裸 PCM）用 20ms = rate/50，
    // 与 RTP 打包粒度一致，也避开 ffmpeg 默认 1024 帧长补零。
    if (_context->frame_size > 1) return _context->frame_size;
    auto rate = _context->sample_rate > 0 ? _context->sample_rate : 8000;
    auto samples = rate / 50;
    return samples > 0 ? samples : 1;
}
```

配套改动：
- `inputFrame_l` 的音频分支删掉「变长帧直通」整段，一律 `encodeAudio_l()`
- `encodeAudio_l()` 删掉早退，切帧长度改用 `audioChunkSamples()`
- `flush()` 的 FIFO 尾部补齐条件去掉 `!_variable_frame_size`（原来 G711 的尾部数据不补，会丢最后一帧）
- 静音兜底（P6）本来就用 `rate/50`，与新切帧长度一致，两条路径共用同一个值

## 5. 顺带修掉的三个连带问题

### 5.1 MP3 目标轨与载荷规格不一致，整轨被丢

`src/Rtsp/Rtsp.h` 的载荷表把 MP3 写死为 **pt=14 / 44100Hz / 双声道**。但目标轨是按「源采样率/声道」建的 —— 16kHz 源转 MP3 时轨声明 16k/1ch，编码器却按 44.1k 出包，RTSP 封装找不到对应 payload 类型，观众端报：

```
Decoder (codec none) not found for input stream #0:0
```

音频整条丢失。修法：把「载荷规格强制」抽成 `forcedAudioSpec()`，**目标轨和编码器共用同一份**，不再两处各算一套。

### 5.2 G711 的载荷规格之前只落在编码器上

同样的结构性缺陷：`forcedAudioSpec()` 现在同时约束目标轨，避免出现"轨声明 8k、编码器按 44.1k 出包"这类不一致。

### 5.3 单声道↔立体声的音量判据

`volumedetect` 的 `mean_volume` 是**每声道**平均。swr 的上混/下混是能量守恒的：

| 转换 | 每声道 | 总能量 |
|---|---|---|
| 单声道 → 立体声 | **-3dB**（各声道 ×1/√2） | 不变 |
| 立体声 → 单声道 | 不变 | **-3dB** |

所以直接比单声道 RMS 会把这当成"掉了 3dB"。测试改用总能量口径（`mean + 10·log10(声道数)`），并且声道数变化时容差放宽到 3.5dB，声道数不变时仍是 1dB。

## 6. 测试矩阵（`scripts/audio_matrix_test.py`）

只测 44.1k→PCMA 是抓不到本缺陷的，必须按采样率扫。矩阵设计：

- **源**（AAC 封装，正弦 440Hz，20 秒）：8k/1ch、16k/1ch、22.05k/2ch、44.1k/2ch、48k/2ch
- **目标**：PCMA、PCMU、opus、MP3（AAC 目标与源相同 → 走透传，单独用例覆盖）
- **每组四项判据**：
  1. **时长不变量**：拉 5 秒，解码出的音频时长必须 ≈5 秒（这是抓本缺陷的核心判据）
  2. **包数自洽**：`包数 × 每包时长 ≈ 时长`（每包时长落在 10~140ms）
  3. **属性正确**：codec / 采样率 / 声道符合载荷规格或跟随源
  4. **硬解码错误 0**、**音量无损**

每组都重启服务并核对配置（防旧进程占端口导致"测的是别的配置"），失败项与已知缺陷分开登记。

## 7. 实测结果

修复前后对照（同一拉流脚本）：

| 组合 | 修复前 | 修复后 |
|---|---|---|
| 8k/1ch → PCMA | 1589 包 / 31.88s | **250 包 / 5.000s** |
| 8k/1ch → PCMU | — | **250 包 / 5.000s** |
| 44.1k/2ch → PCMA | 266 包 / 5.30s | **250 包 / 5.000s** |
| 8k/1ch → opus | 250 包 / 5.000s | 250 包 / 5.000s（不变） |
| MP3（各源） | 无音频 | 有音频，但见 §8 |

PCMA/PCMU 在全部 5 种源采样率下都是 **250 包 / 5.000 秒**，时长不变量全部满足。

## 8. MP3 时长翻倍（已修复）

### 8.1 现象

MP3 目标：拉 5 秒，解码出 **10.211 秒 / 392 包**（应为 ~193 包 / 5 秒），且**所有源采样率都一样**。

### 8.2 排查过程（三层排除）

| 假设 | 取证 | 结论 |
|---|---|---|
| 编码器重复出包 | 插桩：`chunks=350 → pkts=350`，pts 步进 1152 | **排除**，编码器 1:1 |
| 双 sink 实例 | 日志只有 1 次 `encoder ready` / 1 次 `track registered` | **排除** |
| 服务端超速推流 | 量墙钟：拉 5 秒实际花了 **10.28 秒**，内容 10.21 秒 | **排除**，是 1 倍速推的 |

关键推论：拉流耗时和内容长度都翻倍，但包的间隔正常（26ms/包）—— 说明**时间戳的速率是真实速率的一半**，客户端按这个时间戳判断「5 秒」时其实收了 10 秒内容。

### 8.3 根因：SDP 缺少 `a=rtpmap`

裸 socket 抓服务端的 DESCRIBE 响应：

```
m=audio 0 RTP/AVP 14          ← 静态载荷 pt=14（MPEG 音频）
（没有任何 a=rtpmap 行）
```

`src/Rtsp/Rtsp.cpp` 的 `DefaultSdp` 对静态载荷（`payload_type < 96`）**直接 return**，不写 rtpmap。而 RFC 3551 里 pt=14 的**默认时钟是 90000**，我们实际的 RTP 时间戳是按 44100 递增的 ⇒ 客户端按 90000 解释，播放速率变成 44100/90000 ≈ 0.49 倍，正好是 2 倍时长。

PCMA（pt=8）一直正常，是因为它的 RFC 默认就是 8000，与实际一致 —— 这也是这个缺陷长期没被发现的原因。

### 8.4 修法

只对 MP3 补 rtpmap，其余静态载荷保持原样（不动基线）：

```cpp
if (payload_type < 96 && track.getCodecId() != CodecMP3) {
    return;
}
// MPEG 音频（pt=14）在 RFC 3551 里的编码名是 MPA，不是 MP3
auto encoding_name = track.getCodecId() == CodecMP3 ? "MPA" : track.getCodecName();
_printer << "a=rtpmap:" << payload_type << " " << encoding_name << "/" << getSampleRate();
```

⚠️ **编码名必须是 `MPA`**：先写成 `MP3/44100/2` 时 ffmpeg 直接报
`Audio: none, 44100 Hz, stereo ... unknown codec`、连解码器都找不到 —— 比不写 rtpmap 更糟。
RFC 3551 里 pt=14 的名字就是 MPA。

### 8.5 修复效果

| 检查 | 修复前 | 修复后 |
|---|---|---|
| SDP | 无 rtpmap | `a=rtpmap:14 MPA/44100/2` |
| 拉 5 秒的墙钟耗时 | 10.28 s | **5.09 s** |
| 解码音频时长 | 10.211 s | **5.023 s** |
| 包数 | 392 | **193**（26ms/包）|

矩阵里 MP3 的 5 个组合全部通过，**已知缺陷清零（100 通过 / 0 失败）**。

## 8-旧. 原始缺陷记录（保留以备追溯）


| 观察 | 数值 |
|---|---|
| 应有 | 5s × 38.3 帧/s ≈ 196 包 |
| 实测（全部 5 种源一致） | **392 包 / 10.211 秒** |
| 编码器侧帧数 | **244 帧 / 6.4 秒（正确）** |

关键隔离证据：**编码器侧帧数是正确的**（38 帧/秒，与 44.1k/1152 相符），而线上包数是它的 2 倍 —— 说明重复发生在 **RTSP 打包层**，不在编码层。

已排除：静音兜底（`silence_frames=0`）、时间戳（每个包都带 26ms 内容）。

未修原因：这属于 RTSP 打包层对 MP3 payload 的处理，需要单独排查该通路；且 MP3 在本项目里不是主用格式（对讲/广播用 G711，监控用 AAC/opus）。**登记为下一轮该项的第一优先。**

## 9. 未验证项

- MP3 之外的其他"载荷规格写死"的 codec（如 L16）未实测
- 采样率在运行中变化（源切换）未测
- 多路并发下 FIFO 内存增长未观察
- Windows / Linux 平台未跑本矩阵

## 10. codec 覆盖补全（第一项：任意转码）

### 10.1 G722 作为目标（已通）

| 改动 | 位置 |
|---|---|
| 编码器分支 `AV_CODEC_ID_ADPCM_G722` | `Transcoder.cpp` |
| 载荷规格强制 16000/1（pt=9） | `Transcoder.h::forcedAudioSpec` |
| 配置别名 `g722` / `adpcm-g722` | `TranscodeConfig.cpp::codecAlias` |
| SDP 补 `a=rtpmap:9 G722/16000/1` | `Rtsp.cpp::DefaultSdp` |

**为什么必须显式 rtpmap**：RFC 3551 给 pt=9 的默认时钟是 8000，而实际流是 16000 ——
不声明客户端按 8000 解释，播放速度变成 2 倍（实测 `-t 5` 只解出 2.5 秒音频）。
声明后 8k/44.1k/48k 三种源都是拉 5 秒→5.000 秒。

> 遗留风险：G722 的 RFC 规范写法是 `G722/8000`（音频 16k、时间戳按 8k 计）。
> 我们采用的是「时间戳按 16k + 声明 16000」的自洽方案，忽略 rtpmap、只按静态表
> 走老规矩的客户端仍会播成 2 倍速。彻底解决要把 RTP 时钟与编码器采样率解耦。

### 10.2 MP3/G722/L16 作为**源**（已修）

`src/Codec/Transcode.cpp` 的解码器映射只有 9 种 codec，缺 MP3/G722/L16 ——
源流是这三种时 `setupTrack` 直接抛「未找到解码器」，整条轨回落到透传，
**配了目标 codec 也不生效且不报错**（实测 MP3 源配 opus，观众端拿到的仍是 mp3）。
补上三种解码器后实测：

| 源 → 目标 | 结果 |
|---|---|
| G711A → opus | opus 48k/2ch，5.000s |
| G711A → aac | aac 44.1k/1ch，5.048s |
| MP3 → opus | opus 48k/2ch，5.000s |
| MP3 → G711A | pcm_alaw 8k/1ch，5.000s |

> 注：FLV 头里没有 G711A 的采样率，ZLM 建轨时按 44100 声明，所以 AAC 目标跟随到
> 44.1k。要 8000 需在配置里显式 `sample_rate=8000`。

### 10.3 仍缺

| 项 | 状态 | 原因 |
|---|---|---|
| H265 源 → H264（RTSP 进 RTSP 出） | **已修复**（见 10.4） | direct-proxy 把源 RTP 直接透传，转码输出进不了观众的链路 |
| VP8 / VP9 | **已打通** | 旧归因「`Rtsp.h` 里没有对应载荷类型」是错的：动态 PT 96 + rtpmap 一直是对的，真正的卡点是 warmup 拿不到关键帧（裸帧 codec 没有 SPS/PPS）与 libvpx 默认离线档。`h264/hevc → vp8/vp9` 实测 0 解码错误 |
| AV1 | **已打通** | 接 libaom-av1（lag-in-frames=0）；ffmpeg 客户端无 AV1 的 RTP 解包器，用 `scripts/av1_rtp_verify.py` 自解包验证 |
| H266 | 打不通（依赖缺失） | ffmpeg 无 VVC 编码器（只有解码器），且 ZLM 侧无 H266 插件/打包器 |
| G722.1 / SVAC | 未做 | 需要 ffmpeg 二次开发（私有编解码器） |

### 10.4 H265 源 → H264（RTSP 进 RTSP 出）：已修复

**现象**：H265 经 RTSP 推入、配 `video_codec=H264`，转码日志显示成功（`H265 -> H264`
已登记、libx264 出了 400+ 帧），但拉流端 `DESCRIBE` 仍返回 H265 的 SDP，客户端按
hevc 解码「0 错误」——观众拿到的是**源的 H265 RTP**。

**根因**：RTSP → RTSP 默认走 direct-proxy（`[rtsp] directProxy=1`）。这条模式下

- `RtspMediaSourceImp::onWrite` 把源 RTP 原样透传给观众（`RtspMediaSource::onWrite`）；
- `setProtocolOption` 把 `_option.enable_rtsp = !directProxy` ⇒ **muxer 根本不建 RTSP 输出源**；
- 而转码换轨只发生在 `MultiMediaSourceMuxer` 内部。

三条合起来：观众连的是推流端那个 `RtspMediaSourceImp`（SDP 是推流端的原始 SDP，
`a=control:streamid=` + `sprop-vps/sps/pps`），转码输出只流向 rtmp/hls/ts 等 muxer 打开的
协议。两条路不交叉，改 SDP 缓存也没用。

**改法**（4 处；只在"本流启用转码"时改变行为）：

| 位置 | 改动 |
|---|---|
| `RtspMediaSourceImp::resolveTranscodeEnable()`（新增） | 用 `TranscodeRuntime::instance().resolve(getMediaTuple(), &source)` 解析本流是否启用转码，结果缓存进 `_transcode_enabled`。**不是**每个 RTP 包解析一次配置（`resolve` 会遍历 mINI + 解析所有 `stream_rule`） |
| `RtspMediaSourceImp::setProtocolOption()` | `_option.enable_rtsp = direct_proxy ? _transcode_enabled : true` —— 启用转码时让 muxer 建自己的 RTSP 输出源（SDP 由 muxer 生成，带 `a=control:trackID=`） |
| `RtspMediaSourceImp::onWrite()` | `if (directProxy && !_transcode_enabled)` 才透传源 RTP；启用转码时只做 `_demuxer->inputRtp()`，帧经 muxer 里的 `TranscodeSink` 换轨后由 muxer 的 `RtspMediaSource` 重新打 RTP 发给观众 |
| `RtspMediaSourceMuxer::addTrackCompleted()`（上一提交已加） | `updateSdpCache(getSdp())` 刷新对外 SDP。之前它"没生效"，是因为观众连的是上面那个 Imp；现在观众连的就是这个 muxer 源，SDP 自然是转码后的轨 |

`_all_track_ready && !_muxer->isEnabled()` 分支语义保持不变（无人观看且输出按需时跳过解复用），
只是该分支下不再有透传写入；关键帧判定仍归 `_demuxer->inputRtp()` 与 muxer 侧的
`keyFrame()/configFrame()`，秒开/开播花屏的风险点没有被改动。

**顺带修掉的并发缺陷（必现崩溃）**：开通 RTSP 输出后，转码帧在解码线程产出、未转码的轨
（例如只转视频时的音频）仍在 RTP 线程，两条线程同时写同一个下游 muxer（fmp4 的
`BufferLikeString` 等）。实测撑不到 5 秒就 `malloc` abort：

```
[7]  std::string::__grow_by                     ← 堆被踩坏
[9]  mediakit::MP4FileMemory::onWrite
[10] mov_write_styp
[11] fmp4_write_fragment
[12] MP4MuxerMemory::inputFrame
[13] MultiMediaSourceMuxer::onTrackFrame_l
[15] TranscodeSink::onEncoded                   ← 解码线程
```

`MultiMediaSourceMuxer::onTrackFrame_l()` 现在加递归锁，把下游分发串行化（源线程 / 解码线程 /
平滑发送定时器线程都汇到这里）。锁**不**包住 `_paced_sender->inputFrame()` —— 平滑发送的帧是
`FramePacedSender` 的定时器线程回调进 `onTrackFrame_l`，如果外层持锁再抢 `FramePacedSender::_mtx`
就会与它交叉死锁。

**实测（真机，macOS，H265 640x360@15fps，RTSP 推 → RTSP 拉）**

用例全部由 `scripts/rtsp_transcode_check.py` 跑（裸 socket 发 DESCRIBE、ffprobe 属性、包数、
`-f null` 解码错误、落盘后逐帧 framemd5）：

| 用例 | 拉流 ffprobe | DESCRIBE SDP | 5 秒包数 | 解码错误 | 拉流落盘 |
|---|---|---|---|---|---|
| H265 → H264 | **h264 640x360** | `rtpmap:96 H264/90000` + `a=control:trackID=0/1`，无 `streamid=`、无 `sprop-vps` | 75 | 0 | 5.017s / 197531 B / 77 帧 |
| H264 → H265 | **hevc 640x360** | `rtpmap:96 H265/90000` + `sprop-vps/sps/pps` + `trackID=0/1` | 77 | 0 | 5.017s / 147765 B / 77 帧 |
| H265 → H264 + AAC→opus | h264 640x360 | `rtpmap:96 H264/90000` + `rtpmap:97 opus/48000/2` + `trackID=0/1` | 77 | 0 | 5.017s / 197694 B |

转码后 SDP 的 video 段（H265→H264，`sprop-parameter-sets` 是 libx264 新产出的 SPS/PPS，
不是源的 `sprop-vps`）：

```
m=video 0 RTP/AVP 96
a=fmtp:96 packetization-mode=1; profile-level-id=640016; sprop-parameter-sets=Z2QAFqy0BQF/ywgAAAMACAAAAwD0eLF1,aO88sA==
a=rtpmap:96 H264/90000
a=control:trackID=0
m=audio 0 RTP/AVP 97
a=rtpmap:97 mpeg4-generic/44100/1
a=control:trackID=1
```

**画面取证**：拉流转码流的 2s 处抽帧是一帧正常的彩条测试图（含时间码叠加），不是绿屏/花屏。

**确认真的重编码**：把拉流解码帧的 framemd5 与源解码帧比对 —— H265→H264 命中 0/77、
H264→H265 命中 0/77（0%），即输出帧与源帧完全不同；对照组（不开转码）是 100% 命中。

**基线回归（不开转码，direct-proxy 行为必须一模一样）**

改前二进制（`git stash` 干净树编译）vs 改后二进制，同源同参数，同一条推拉拓扑：

| 源 | 改前 | 改后 |
|---|---|---|
| H264 | h264 640x360，73 包，5.162s，**337346 B**，72 帧 | h264 640x360，73 包，5.162s，**337346 B**，72 帧 |
| H265 | hevc 640x360，57 包，5.156s，**512613 B**，76 帧 | hevc 640x360，57 包，5.156s，**512613 B**，76 帧 |

落盘文件字节数逐个相同；解码帧 72/72、76/76 全部命中源解码帧（未重编码、未丢帧）；
DESCRIBE 仍返回推流端原始 SDP（`a=control:streamid=`），即 direct-proxy 没被误关。

**持续吞吐（15 秒连续拉流）**：H265→H264 单视频 227 包 / 期望 225，音视频同转 229 / 225，
均 0 解码错误 —— 转码不会是 "前 5 秒正常、后面卡住"。

---

## 10.5 RTSP 推流断连重推 406（`Already publishing`）：已修复

现场最痛的一条：**启用转码后，摄像机断开重连（重新 ANNOUNCE）拿 406 立即退出，再也不上来**；
不开转码时重推正常。本节是 §10.4 遗留限制的修复记录，复现与验收脚本为
`scripts/rtsp_repush_check.py`（自带断点、跨断点观众、断点后再拉流验证）。

### 10.5.1 复现（修复前，真机）

H265 源 → `video_codec=H264`，推流端 `kill -9` 后 2 秒重推：

```
== 重推 ffmpeg 返回码: 1
   | [rtsp @ 0x13d80ff50] method ANNOUNCE failed: 406 Not Acceptable
   | [out#0/rtsp @ 0x13d80fe90] Could not write header (incorrect codec parameters ?): Server returned 4XX Client Error, but not one of 40{0,1,3,4}
   | Conversion failed!
服务端: RtspSession.cpp | ANNOUNCE: Already publishing:__defaultVhost__/live/xxx
```

### 10.5.2 机制（三条叠加，缺一条都到不了“永久 406”）

1. **池里的键换了主人**：启用转码后 `RtspMediaSourceImp` 不再透传源 RTP（§10.4），
   它的 `_ring` 因此从不创建，`RtspMediaSource::onWrite` 里的 `regist()` 也就从不触发；
   池里 `RTSP/live/<stream>` 这个键由 muxer 建的 `RtspMediaSource` 注册
   （`RtspMediaSourceMuxer::addTrackCompleted` 写入 SDP 后首次 flush 即注册）。
2. **ANNOUNCE 只认推流端对象**：`handleReq_ANNOUNCE` 拿到源后做
   `dynamic_pointer_cast<RtspMediaSourceImp>(src)`，拿不到就 `push_failed = true` → 406。
   于是 15 秒（`continue_push_ms`）宽限期内的重推必挂。
3. **键永远不释放**（这才是“再也不上来”的原因，实测 gap=20s / 75s 仍然 406）：
   推流端 `RtspMediaSourceImp` 会在宽限期结束时正常析构（加探针确认过），但 `MultiMediaSourceMuxer`
   不死 —— 它 `_transcode`（`TranscodeSink`）里的解码器/编码器持有**源 Track**，而源 Track 的
   `FrameDispatcher::_delegates` 存的是 muxer 的 `shared_ptr`（`track->addDelegate(_muxer)`），
   构成引用环 `muxer → TranscodeSink → 编解码器 → 源 Track →(delegate)→ muxer`。
   实测断开 25 秒后 `getStatistic` 里 `MultiMediaSourceMuxer` 仍为 1、
   `isMediaOnline` 对 rtsp/rtmp/ts/fmp4/hls **全部 true**；重推在 gap=75s 时仍 406。
   （不开转码时 muxer 正常析构，所以基线不受影响。）

顺带查出的第二个坑：修第 3 条时第一次实现（在推流端析构里 `_transcode = nullptr`）直接把 poller
线程卡死 —— `sample` 取样 1749/1749 帧停在：

```
~RtspMediaSourceImp → MultiMediaSourceMuxer::releaseMediaSources
  → ~TranscodeSink → __deallocate_node → ~FFmpegDecoder → FrameMerger::flush
    → FFmpegDecoder::decodeFrame → TranscodeSink::onDecoded → std::mutex::lock()
```

`~TranscodeSink()` 持着 `_mtx` 销毁 `TrackContext`，解码器析构会 flush 残留帧，flush 同步回调
`onDecoded()` 里再取同一把**非递归** `_mtx` ⇒ 同一线程自死锁。

### 10.5.3 修法（4 处，均只在“muxer 占着对外 RTSP 源”时生效）

| 文件 | 改动 |
|---|---|
| `Rtsp/RtspMediaSource.h` | 新增 `setPushSource()` / `isPushShadow()` / `getPushSource()`：影子源上一个弱引用指回推流端；`_has_push_src` 在推流端销毁后仍为 true，用于区分“rtmp 推转 rtsp 生成的源” |
| `Rtsp/RtspMediaSourceImp.cpp` `setProtocolOption` | 建完 muxer 后，若 `_option.enable_rtsp`（转码，或 `directProxy=0`），把本 Imp 挂到 muxer 的 RTSP 输出源上（`MultiMediaSourceMuxer::getRtspMediaSource()` 新增） |
| `Rtsp/RtspSession.cpp` `handleReq_ANNOUNCE` | 直接 cast 失败时，顺着影子源的弱引用找回 `RtspMediaSourceImp`，**继续走既有的 `getOwnership()` 接管路径**（拿不到所有权仍 406，即“真有人在推”的语义不变） |
| `Rtsp/RtspMediaSourceImp.cpp` 析构 + `Common/MultiMediaSourceMuxer.*` | 推流端析构（= 宽限期到期，这条流真的结束）调用 `releaseMediaSources()`：先 `_transcode = nullptr` 断环，再对 rtsp/rtmp/ts/fmp4/hls 输出源逐个 `releaseFromPool()`（`MediaSource` 新增的公开入口，把键从池里摘掉，不依赖对象析构） |
| `Common/TranscodeSink.cpp` 析构 | 先把 `_contexts` 换出来再在锁外 `stopThread` / 析构，消掉上面那个自死锁 |

设计取舍：**宽限期内不重建流水线**，而是接管原来的 `RtspMediaSourceImp`（和 direct-proxy 完全一致的语义），
因此观众在断点前后继续看同一条流，转码参数与 demuxer 也都沿用；宽限期过后才释放全部输出源、
下一次推流是全新的源（与基线“流结束就注销”一致）。

### 10.5.4 实测（真机，macOS，RTSP 推 → RTSP 拉，`scripts/rtsp_repush_check.py`）

修复后同一用例（gap=2s，推流端 SIGKILL 后重推）：

```
== 重推 ffmpeg 返回码: 仍在运行 (None)
   | frame=    0 fps=0.0 q=-1.0 size=       0kB time=-00:00:00.13 bitrate=  -0.0kbits/s
   | frame=   17 fps= 17 q=-1.0 size=N/A time=00:00:01.00 bitrate=N/A speed=0.995x
   | frame=   48 fps= 16 q=-1.0 size=N/A time=00:00:03.04 bitrate=N/A speed=   1x
   | frame=   86 fps= 15 q=-1.0 size=N/A time=00:00:05.57 bitrate=N/A speed=   1x
```

| 用例 | 重推 | 重推后 SDP | 重推后拉流 | 5 秒包数 / 解码 | 跨断点观众 |
|---|---|---|---|---|---|
| H265→H264（gap 2s） | 存活，无 406 | `H264/90000` + `trackID=0` | h264 640x360 | 77 包 / 0 错误 | 录像 14.01s / 217 包，**不断流** |
| H264→H265（gap 2s） | 存活，无 406 | `H265/90000` + `trackID=0` | hevc 640x360 | 77 包 / 0 错误 | 录像 14.05s / 219 包，**不断流** |
| H264→VP8（gap 2s） | 存活，无 406 | `VP8/90000` + `trackID=0` | vp8 640x360 | 76 包 / 0 错误 | 录像 13.99s / 212 包（存 `.ivf`，mp4 装不了 VP8） |
| H265→H264（gap 25s，宽限期外） | 存活，无 406 | `H264/90000` + `trackID=0` | h264 640x360 | 75 包 / 0 错误 | 断点后观众被断开（流真结束，基线同） |

宽限期外重推后 `getStatistic`：`MultiMediaSourceMuxer` 1 → 0、`MediaSource` 8 → 1，
`isMediaOnline` 全部 false —— 池里没有残留键，所以新推流能正常注册。

**基线回归（不开转码，direct-proxy 语义必须一模一样）**

| 用例 | 重推 | 重推后 SDP | 拉流 | 5 秒包数 / 解码 |
|---|---|---|---|---|
| H264 基线（gap 2s） | 存活，无 406 | 仍是推流端原始 SDP（`a=control:streamid=0`） | h264 640x360（未重编码） | 73 包 / 0 错误 |
| H265 基线（gap 2s） | 存活，无 406 | 同上（`streamid=0`） | hevc 640x360（未重编码） | 57 包 / 0 错误 |

基线路径本身没被改：`_option.enable_rtsp` 为 false 时 `~RtspMediaSourceImp` 不做任何释放，
`ANNOUNCE` 的第一分支（直接拿到 Imp）照旧命中，转码节点根本不存在。

**负向用例（不能为了修重推就放开接管）**

| 场景 | 结果 |
|---|---|
| 原推流端还在推，第二个推流端 ANNOUNCE 同一路 | 406（影子源找回了 Imp，但 `getOwnership()` 失败） |
| rtmp 推流 `live/rto` 进行中，rtsp ANNOUNCE 同一路 | 406（该 muxer 源不是影子源，无弱引用可跟） |

日志会把占键的源类型打出来，便于现场区分这两种情况：

```
ANNOUNCE: take over published RTSP source of __defaultVhost__/live/dup1 (muxer-owned, transcode/direct-proxy off)
ANNOUNCE refused, stream already in use by rtsp://__defaultVhost__/live/dup1 (mediakit::RtspMediaSource)
```

### 10.5.5 本次未做 / 已知仍有问题

- **rtmp 推流 + 转码同样有那个引用环**：`RtmpMediaSourceImp` 没有对应的 `releaseMediaSources()`，
  所以 muxer 仍然泄漏（rtmp 键由 Imp 自己占着，重推不受影响，真正受影响的是 rtmp/ts/hls
  输出源残留在池里）。修法同构，未做也未验证。
- `TranscodeSink::teardown()`（`resetTracks` 路径）同样是“持 `_mtx` 调 `decoder->flush()`”，
  理论上有同款自死锁，未实测、未改。
- 宽限期内重推沿用的是第一次的 demuxer/SDP：摄像机重连后若 SDP 变了（改分辨率/换编码），
  要等宽限期（默认 15s）过后重推才会按新 SDP 建源。未实测。
- 只验证 RTSP 推 → RTSP 拉；转码叠加 `directProxy=0` 的 rtmp/webrtc 拉流未重跑。
- Windows / Linux 未跑。

---

**未验证（10.4 当时的口径，重推这一项已在 10.5 修掉并验收）**

- 只验证了 RTSP 推 → RTSP 拉；RTSP 推 → RTMP/HLS/WebRTC 拉在关掉 direct-proxy 后走既有非代理
  路径，本次未重跑回归（之前 `[rtsp] directProxy=0` 的既有行为未变，但转码叠加后未实测）
- 多路并发（≥2 路同时 H265→H264）未测；`TranscodeRuntime` 规则表与 `_transcode_enabled` 缓存
  是按流解析的，但并发下的 CPU/延迟未观测
- `on_publish` 返回 `stream_replace` 同时启用按流转码规则时：`RtspMediaSourceImp` 用推流原始
  tuple 解析，muxer 用替换后的 tuple 解析，两边判定可能不一致（未实测）
- Windows / Linux 未跑
- 运行环境噪声提示：本机同时有其他任务在跑 `MediaServer`（会 `pkill -f MediaServer`），
  测试脚本因此内置了"服务被杀就整轮重试"，最终数据都来自完整跑完的轮次

## 10.6 RTMP 引用环泄漏：实测不复现，**未修改**

上一轮（a6）在报告里提到「rtmp 推流 + 转码有同款引用环，muxer 仍泄漏」。按这条去修之前先做了实测，
**结论：复现不出来，因此没有改代码。**

### 实测数据（转码开启，RTMP 推入 → 转 H265）

| 场景 | 推流中 | 断开 20s 后 | 35s 后 |
|---|---|---|---|
| 无观众断开（改前二进制，无 `~RtmpMediaSourceImp`） | `fmp4/hls/rtmp/rtsp/ts` 5 条 | **[] 空** | **[] 空** |
| 带两个观众断开（同上） | 5 条 | **[] 空** | — |
| 改后（加了显式释放） | 5 条 | [] 空 | — |

改前改后行为**完全一致**，说明 RTMP 侧本来就会随推流端销毁把源释放掉。

### 为什么 RTSP 侧会泄漏、RTMP 侧不会

RTSP 侧的泄漏不是引用环本身造成的，而是 **a4 改动引入的连带后果**：启用转码后对外 RTSP 源变成
muxer 的新源，推流端 `RtspMediaSourceImp` 在池里找不回来 → 它自己**永远不被销毁** → 挂在它身上的
muxer 也就永远不释放。RTMP 侧没有这个「找不到推流端对象」的环节，Imp 正常析构，muxer 跟着走。

**教训**：转述别人的「同款问题」之前先自己复现；复现不出来就别发补丁 —— 否则为了一个不存在的问题
改动生命周期代码，风险远大于收益。本次相关补丁已丢弃（`git stash drop`），源码保持原样。

### 检查方法（已固化为脚本）

`scripts/source_release_check.py`：推流 → 记 `getMediaList` → 断开 → 等 20s（超过 15s 宽限期）→
列表必须为空 → 再确认同名流能重推。无观众 / 带观众两种场景都跑，实测 6/6 通过。

以后动 `MultiMediaSourceMuxer` / 各 `*MediaSourceImp` 的生命周期时，跑这个脚本即可确认没引入泄漏。

## 10.7 AV1 + fmp4 堆损坏：**已不复现**（0/20），未改代码

a5 分支曾记录「AV1 套目标 + 默认开 fmp4」偶发堆损坏（6 次 1 次崩），并归因到 libmov 写 AV1。
本轮先查归因、再复现：

### 归因不成立

`3rdpart/media-server/libmov` **是支持 AV1 的**：有 `MOV_OBJECT_AV1`、`mov-stsd.c` 里有 av1C 分支、
`mov_write_av1c()` 写 extra_data 时还有 `if (entry->extra_data_size > 0)` 保护。
所以「libmov 写 AV1 越界」这个说法不成立。

### 复现结果：0/20

当前代码（已含 a4 的多轨输出串行化锁 + a5 的 PacketCache 合写锁）：
AV1 目标、默认开 fmp4、每轮推流 + 观众拉流触发落盘，**连跑 20 轮 0 崩**。

如果真实崩溃率仍是 1/6，20 轮全不崩的概率约 **2.6%** —— 可以判定该问题在当前代码上已不复现。

最可能的解释：那两次锁修复之一作用在了同一处 —— 堆损坏的根因是**并发写同一个 muxer 的缓冲区**
（转码帧在解码线程、透传帧在推流线程），而不是 libmov 对 AV1 的处理。这与 §17 记的多轨并发坑是同一类。

### 检查方法

`scripts/av1_fmp4_stability.py [轮数]`：起服务 → AV1 转码推流 → 观众拉流触发 fmp4/hls 落盘 →
检查进程存活与日志 crash 标记。偶发问题必须多轮重复，单次通过没有意义（本次跑 20 轮）。

## 10.8 宽限期内重推沿用旧 SDP：**不改代码，给运维手段**（已实测）

### 现象

推流端断开后，媒体源会保留 15 秒宽限期（`RtspMediaSourceImp` 的延迟析构），目的是让
短暂的网络抖动不至于中断观众。代价是：**这 15 秒内重连的推流端如果换了分辨率或编码，
观众拿到的仍然是第一次的 SDP。**

### 实测（RTSP 推 H265，观众用 ffprobe 看实际尺寸）

| 操作 | 观众看到 |
|---|---|
| ① 摄像机推 640x360 | `hevc,640,360` |
| ② 断开后**宽限期内**换 320x240 重连 | **`hevc,640,360`（旧尺寸）** |
| ③ 断开后**静等 17 秒**再推 320x240 | `hevc,320,240`（已重建） |
| ④ 宽限期内调 `close_streams?force=1` 后重推 320x240 | `hevc,320,240`（立即重建） |

### 处理决定：不改

缩短宽限期能修这个边界，但代价是**网络抖动 5 秒就会让所有观众断流** —— 现场摄像机
掉线重连比「换分辨率重启」常见得多，拿常见场景的稳定性换罕见场景的几秒延迟不划算。

### 运维手段（④，已实测有效）

摄像机换配置重启后若画面尺寸/编码不对，调一次关流接口即可立即重建：

```
/index/api/close_streams?secret=<secret>&vhost=__defaultVhost__&app=live&stream=<流id>&force=1
```

`force=1` 会连观众一起断开，之后摄像机重连就是全新协商。实测调用后立即返回 `code=0`，
摄像机重推即得到 320x240 的正确 SDP。

### 现场判断依据

- 画面尺寸/编码在摄像机重启后**一直不对**（不是十几秒后自己好）→ 需检查是否频繁重连，
  每次重连都在宽限期内，就会一直沿用旧 SDP
- 画面**十几秒后自己变对** → 正常，宽限期已过，源已重建，无需干预

## 10.9 转码模式重连后 DESCRIBE 404：**源在池里，但拿不到 SDP**（实测定位到这一层）

### 决定性证据：媒体源时间轴

测试脚本现在每 200ms 采样一次 `getMediaList`（`_sampler()` / `timeline_dump()`），
一次失败用例的时间轴：

```
 1.45s  []                                    # 推流前
 1.86s  ['fmp4','rtmp','rtsp','ts']           # 首推注册
 3.69s  ['fmp4','hls','rtmp','rtsp','ts']
24.53s  []                                    # 用例结束、停服务
```

**整段 404 期间（约 6~22 秒）该流的协议源一直挂在池里。**

⇒ 之前的两个假设都被否掉：
1. 「关流（on_demand）把源下架」—— 源没下架；
2. 「注册空窗，键被摘掉又重建」—— 键一直在。

真正的问题变成：**`MediaSource::find()` 能找得到源，但 DESCRIBE 仍然回 404**。
在 ZLM 里 DESCRIBE 404 的另一个来源是**源对象的 SDP 为空**（已注册 ≠ 已就绪）。

### 为什么会这样（现有证据支持的推断）

重连发生在 15 秒宽限期内时走「接管」路径：
- `vcodec()` 在重连后 +5 秒仍能探到 `h264` ⇒ 那一刻 DESCRIBE 是**成功**的（命中的还是宽限期里
  那个旧源对象，它有 SDP）；
- 随后宽限期到期 / 接管完成，旧源被释放，**新源在池里但 SDP 还没建好**；
- 于是紧接着的连续拉流全部 404，直到新 SDP 就绪（实测约 16 秒）。

这条链路与 a6 的改动直接相关（宽限期由它引入的接管/释放路径决定），但**尚未插桩证实**。

### 下一步（一条命令能验）

在 `RtspSession::handleReq_DESCRIBE` 的 404 分支前打一行日志，把 `src.get()`、
`src->getSdp().size()`、`src->ready()` 一起输出。若 404 都伴随 `sdp=0`，
结论即成立，修法方向是「接管完成前不要让 SDP 空的新源对外可见」
（或接管时把旧 SDP 一并搬过去）。

**在此之前不写进对外问题清单。**（本条曾被我误报为「源被摘掉的产品缺陷」，已纠正两次。）

### 服务端日志定性（2026-10-28 补）

在测试里把服务端 404 相关日志一并捞出来后，答案不再是「SDP 空」：

```
RtspSession.cpp:62 onError | RTSP播放器(__defaultVhost__/live/rc_h265_h264)断开:
                          no such stream:__defaultVhost__/live/rc_h265_h264,耗时(s):15
```

走的是**「找不到源」分支**（`MediaSource::findAsync` 返回空），不是「SDP 里无有效 track」
（那支会打 `sdp中无有效track，该流无效`，本次没有出现）。

于是留下一个**自相矛盾的观测**：

| 观测手段 | 结果 |
|---|---|
| `getMediaList`（每 200ms 采样） | 该流的 `fmp4/hls/rtmp/rtsp/ts` 全程都在 |
| 服务端 `findAsync`（DESCRIBE 路径） | `no such stream` |

同一时刻「池里有」和「find 找不到」并存 ⇒ 差异只可能在**查找键**上：
`_media_info`（vhost/app/stream，来自 DESCRIBE 的 URL）与池中条目的 tuple 不一致，
或者 `findAsync` 走的不是 `s_media_source_map` 这一份表，或者转码路径下 RTSP 源
被注册到了一个 `getMediaList` 也会列、但 `find` 不认的位置。

### 下一步（仍是插桩，别猜）

在 `RtspSession::onAuthSuccess` 的 `!rtsp_src` 分支打印：
`_media_info.vhost/app/stream` + `MediaSource::find` 各协议表的命中情况；
再和池里该条目实际的 vhost/app/stream 对比。**两个 tuple 一比就出结论。**

### 再排除一条：查找键一致

让采样器记录完整 tuple（vhost/app/schema）后：

```
1.85s  ['__defaultVhost__/live/fmp4', '__defaultVhost__/live/rtmp',
        '__defaultVhost__/live/rtsp',  '__defaultVhost__/live/ts']
```

池中 rtsp 源的 vhost/app 与 DESCRIBE 的 `__defaultVhost__/live/rc_h265_h264`
**完全一致** ⇒ 「tuple 不一致导致 find 不到」也排除。

### 至此已排除（4 条）

| # | 假设 | 排除依据 |
|---|---|---|
| 1 | on_demand 把源下架 | 强制 on_demand=0 后依旧；且池里一直在 |
| 2 | 注册空窗（键被摘又重建） | 200ms 采样显示整段时间键都在 |
| 3 | SDP 为空（无有效 track） | 服务端日志是 `no such stream`，不是 `sdp中无有效track` |
| 4 | 查找键（vhost/app/stream）不一致 | 采样到的 tuple 与 DESCRIBE 完全一致 |

### 剩下的唯一方向：池里那个 weak_ptr 与 find 的实际路径

`getMediaList` 能列出来、`findAsync` 却返回空，而两者的键完全一样 ⇒ 差异只可能在
**对象存活 / 查找实现**这一层。首要怀疑：池里存的是 `weak_ptr`，对象已被销毁但条目还在
（`getMediaList` 枚举时可能顺手 `lock()` 复活或读到未过期的临时强引用），而 `find` 拿到
空 `lock()` 就当没有 ⇒ 报 `no such stream`。

**验证方法（C++ 插桩，下一步）：** 在 `RtspSession::onAuthSuccess` 的 `!rtsp_src` 分支打印
该键对应 weak_ptr 的 `expired()` 与 `use_count()`。若是 `expired()==true`，则根因是
「转码接管路径释放了 RTSP 源对象，但池里的键没被摘干净」。

> 记录：本条先后被误判为 on_demand、注册空窗、SDP 为空、键不一致，四次都被实测否掉。
> 教训是**每次都要先问「这个假设用什么观测能证伪」，而不是顺着上一个假设往下推**。

### 第五轮：进程内枚举 vs HTTP 枚举，结论相反（关键矛盾）

在 `RtspSession::onAuthSuccess` 的 404 分支插入诊断（对 app+stream 匹配的源全部打印）：

```
DIAG pool | no source at all for __defaultVhost__/live/rc_h265_h264 (schema=rtsp)
```

即：**进程内 `MediaSource::for_each_media` 一条都没有**。而同一时刻脚本通过
`/index/api/getMediaList` 反复读到该流的 `fmp4/hls/rtmp/rtsp/ts` 五条都在。

两者的键（`__defaultVhost__/live`）完全一致，`regist()` 对过期 weak_ptr 也会覆盖，
所以「过期条目卡住」不成立。**唯一还能同时成立的解释是：HTTP 与 RTSP 落在了两个不同的
MediaServer 进程上**（例如上一用例的进程没退干净，仍占用 HTTP 端口，于是 `getMediaList`
回答的是旧进程；而 DESCRIBE 走 RTSP 端口，命中的是新进程，它确实什么都没有）。

### 验证方法（下一步，一次运行即可）

采样时**持续**记录 `lsof -nP -tiTCP:57080` 和 `:56554` 的 pid，两者不同即坐实。
（本次尝试失败：pid 只在第一次采样时取，那时服务还没起来，缓存成了 `?`。
 教训：诊断代码本身也要有判据，不能「取一次就缓存」。）

### 第六轮：确认同一进程，双实例假设否掉

持续采样端口归属（每 ~1 秒一次，不再缓存）：

```
2.40s  pid http=86486 rtsp=86486
3.72s  pid http=86486 rtsp=86486
24.40s pid http=86486 rtsp=86486
```

HTTP 与 RTSP 是**同一个 pid** ⇒「两个实例互相看不见」不成立。

### 现在的矛盾（第 6 条假设后的剩余事实）

同一个进程里：
- `getMediaList`（HTTP 接口）在整个窗口内都能列出该流的 5 条协议源；
- `for_each_media`（DESCRIBE 路径内部）打出来的是 `no source at all`。

两者都自称读的是 `s_media_source_map`。这说明 **`getMediaList` 并不只是枚举注册表**，
它可能触发/返回了别的东西（例如按 `MediaSource::find(..., from_mp4=true)` 创建/复活源，
或枚举的是另一个缓存层）。这一点尚未读透 `server/WebApi.cpp` 里 `getMediaList` 的实现。

### 下一步（精确到函数）

1. 读 `server/WebApi.cpp` 的 `getMediaList` 实现，确认它到底遍历哪个结构；
2. 在 DIAG 那行把时间戳一起打全（测试日志尾部截断丢了前缀，无法与 404 时刻对齐）；
3. 若 `getMediaList` 走的是 `find(from_mp4=true)`，那它显示的「源在」本身就是假象 ——
   **采样器这个观测手段从根上就不可靠**，得换成在 `regist()/unregist()` 里打点。

### 复盘：六条假设

| # | 假设 | 结果 |
|---|---|---|
| 1 | on_demand 下架 | 否 |
| 2 | 注册空窗 | 否（键一直在，但那是假象） |
| 3 | SDP 为空 | 否（no such stream） |
| 4 | 查找键不一致 | 否（tuple 相同） |
| 5 | weak_ptr 过期卡键 | 否（regist 会覆盖） |
| 6 | HTTP/RTSP 双实例 | 否（同一 pid） |

六条全否，说明**观测手段本身有问题**：`getMediaList` 被当成了「池里有没有」的判据，
而它显然不等于 `for_each_media`。下一步应先把观测手段换成 `regist/unregist` 打点，
再谈根因 —— 否则还会继续在错误的观测上推假设。

### 第七轮：更正 —— getMediaList 用的就是 for_each_media，矛盾是「时间对不上」

`server/WebApi.cpp:1056`：

```cpp
api_regist("/index/api/getMediaList", [](API_ARGS_MAP_ASYNC) {
    ...
    MediaSource::for_each_media([&](const MediaSource::Ptr &media) { ... });
```

与 DIAG 那行**是同一个函数**。所以上一节「两个枚举结构不同 / 采样手段不可靠」的说法**是错的**，
予以更正。

同一函数不可能同时返回「有 5 条」和「一条都没有」⇒ 唯一的解释是**这两个观测不在同一时刻**。
而现在的测试日志把服务端行截断到末 160 字符，**丢掉了时间戳前缀**，无法与采样时间轴对齐 ——
这才是本轮真正的缺口。

### 下一步（一步到位）

把 DIAG 那行的完整时间戳保留（不要截断），并让采样时间轴也记录绝对时间，
两者对齐后就能看出 404 到底发生在「源存在」的那一段还是「源已消失」的那一段。

如果 404 落在「源已消失」的时段（例如用例结束、推流端被 kill 之后），
那它就是**测试时序问题**（拉流发起得太晚），不是产品缺陷 —— 这与 §10.6、§10.8 的教训同源。

### 复盘（七轮）

| # | 假设 | 结果 |
|---|---|---|
| 1 | on_demand 下架 | 否 |
| 2 | 注册空窗 | 否 |
| 3 | SDP 为空 | 否 |
| 4 | 查找键不一致 | 否 |
| 5 | weak_ptr 过期卡键 | 否 |
| 6 | HTTP/RTSP 双实例 | 否（同一 pid） |
| 7 | 观测手段不可靠（两个结构） | 否（同一 for_each_media） |

七条全否，唯一的缺口是**日志时间戳被截断，没法做时间对齐**。

### 第八轮：时间戳对齐，矛盾更尖了

服务端日志（已保留完整时间戳）：

```
23:00:45.756 W RtspSession.cpp:62 | 243-49 RTSP播放器(__defaultVhost__/live/rc_h265_h264)
                                   断开:no such stream...,耗时(s):15
23:01:00.786 W RtspSession.cpp:488 | DIAG pool | no source at all for .../rc_h265_h264
23:01:00.786 W RtspSession.cpp:62 | 311-49 ... 耗时(s):15
```

阅读方式：`findAsync` 的 15 秒重试耗尽后才回调，所以
**DESCRIBE 发起于 23:00:45.7，DIAG 打印于 23:01:00.8（15 秒后）**。
用例起点 23:00:41 左右 ⇒ 发起时刻约为用例内 **+4.7 秒**。

而采样时间轴显示该流在 **+1.85 秒 ~ +24.4 秒** 之间都在池里 —— 即 +4.7 秒时确实存在。

⇒ 发起时刻源在池里、`find_l` 却当场找不到；15 秒后重试耗尽时，DIAG 那一刻池里也确实空了。
这两件事拼在一起说明：**`find_l` 在 +4.7 秒那一次就没命中**，而不是「等待期间源消失」。

所以问题重新回到**查找路径本身**：`find_l(schema=rtsp, vhost=__defaultVhost__, app=live, id=rc_h265_h264)`
没有命中，而同一个 `for_each_media`（不带 schema/vhost 过滤）能枚举到它。
差异只剩两个入参：**`schema` 过滤** 与 **`vhost` 过滤**（`enableVhost` 为假时会被强制改成
`DEFAULT_VHOST`，两者此时相同）。

⇒ 下一步只查一件事：在 DIAG 里把 `for_each_media` **带上 `schema=rtsp` 过滤**再枚举一次，
若带过滤就找不到，则说明池中该条目注册的 `_schema` 不是 `"rtsp"`（例如为空或别的值），
而 `getMediaList` 显示的 schema 字段来自别处 —— 这就同时解释了一切。

### 复盘（八轮，全部走了死路）

on_demand / 注册空窗 / SDP 空 / 键不一致 / weak_ptr 过期 / 双实例 / 观测结构不同，全部否定。
现在线索收窄到一个具体可测的点：**池中 RTSP 条目的 `_schema` 实际值**。

### 第九轮：schema 对照跑完，仍是 0/0 —— 矛盾无法在本轮收敛

```
23:04:59.548 DIAG pool | no source at all for __defaultVhost__/live/rc_h265_h264
             | unfiltered=0 filtered=0
```

同时核对了 `MediaTuple` 的定义（`src/Record/Recorder.h:22`）：字段就是 `vhost/app/stream`，
**我的判据没有用错字段**。因此「那一刻池里一条都没有」是真实的观测，不是插桩 bug。

而采样器（同一进程、同一 `for_each_media`、0.2 秒间隔）在重叠的时间段里一直读到 5 条源，
且**没有任何状态变化的记录**（采样器只在变化时打印）。

同一个函数、同一个进程、时间重叠，却给出相反结果 —— 这已经超出本轮能用现有手段解释的范围。

### 留给下一个人的完整事实（不要重复已验证的否定）

**已排除**：on_demand、注册空窗、SDP 空、键不一致、weak_ptr 过期、双实例、
观测结构不同、时间错位、字段用错。

**已确定**：
1. 转码两例必现，基线必不现（`h265->h264`、`h264->h265` 各失败一次；`enable=0` 通过）。
2. 服务端走的是 `no such stream` 分支（不是 SDP 无 track）。
3. `findAsync` 15 秒重试耗尽才回调，404 在耗尽时才发出。
4. 池里 tuple 与请求完全一致（`__defaultVhost__/live/...`）。

**建议的下一步（换手段，别再读同一个 API）**：
直接在 `MediaSource::regist()` 和 `unregist()` 里打点（带 `this` 指针、schema、tuple、时间戳），
把「这个流在池里的生命周期」由**写方**记录下来，而不是由读方（`for_each_media`）去猜。
写方日志能一次性回答「DESCRIBE 那个时刻键在不在」，不会再有观测角度的歧义。
