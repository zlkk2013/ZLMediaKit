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
| VP8 / VP9 | 无法投递 | 有 libvpx 编码器，但 `Rtsp.h` 里没有对应载荷类型 |
| AV1 / H266 | 未接 | 无编码器分支，也无载荷 |
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

**新增已知限制（本次改动暴露，但不是本次引入）**：转码流的 RTSP **断连续推**会失败。
对外 RTSP 源变成 muxer 的 `RtspMediaSource` 后，`RtspSession::handleReq_ANNOUNCE` 里
`dynamic_pointer_cast<RtspMediaSourceImp>(src)` 拿不到推流端对象，直接 406。实测：

| 配置 | 重推后推流进程 | 日志 |
|---|---|---|
| 开转码（H265→H264） | 立即退出 | `ANNOUNCE: Already publishing` ×1 |
| 不开转码（direct-proxy） | 存活，继续出流 | 无 |

这与上游 `directProxy=0` 的行为完全一致（非 direct-proxy 时 Imp 本来就不注册），但启用转码后
**默认**落到这个模式，运营上必须知道。要修得让 muxer 的 RTSP 源能找回 `RtspMediaSourceImp`
做所有权转移，单独一轮。

**未验证**

- 只验证了 RTSP 推 → RTSP 拉；RTSP 推 → RTMP/HLS/WebRTC 拉在关掉 direct-proxy 后走既有非代理
  路径，本次未重跑回归（之前 `[rtsp] directProxy=0` 的既有行为未变，但转码叠加后未实测）
- 多路并发（≥2 路同时 H265→H264）未测；`TranscodeRuntime` 规则表与 `_transcode_enabled` 缓存
  是按流解析的，但并发下的 CPU/延迟未观测
- `on_publish` 返回 `stream_replace` 同时启用按流转码规则时：`RtspMediaSourceImp` 用推流原始
  tuple 解析，muxer 用替换后的 tuple 解析，两边判定可能不一致（未实测）
- Windows / Linux 未跑
- 运行环境噪声提示：本机同时有其他任务在跑 `MediaServer`（会 `pkill -f MediaServer`），
  测试脚本因此内置了"服务被杀就整轮重试"，最终数据都来自完整跑完的轮次
