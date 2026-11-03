# 工作日志（转码模块）

> 约定：每轮实质性工作追加一节，写清「做了什么 / 怎么验的 / 踩了什么 / 还差什么」。
> 只写可复现的事实和命令，不写过程感慨。

---

## 2026-11-03 昇腾 310P DVPP 硬编码接入 ZLMediaKit（dev/ascend-wire → dev/transcode）

### 目标

在 fork 的 `dev/transcode` 上，用「新增文件 + 极薄钩子」的方式接入昇腾 DVPP VENC，
让转码目标编码（H.264/H.265）可以走 NPU 硬编，并保持上游可 rebase。

### 最终结果

| 项 | 状态 |
|---|---|
| 昇腾硬编端到端 | ✅ 源 H.264 720p/1080p → 客户端拉到 `hevc` |
| 客户端帧率 | ✅ 25.1 fps（源 25fps） |
| 音画同步 | ✅ 视频跨度 11.76s / 音频 11.90s |
| 并发容量（720p / 1080p） | ✅ 1/4/8/16 路全部 25.0fps，聚合 400fps |
| 崩溃 | ✅ 全部测试 `crash=0` |
| 非昇腾构建 | ✅ macOS 默认构建 `[100%] Built target MediaServer` |

### 代码改动

| 文件 | 改了什么 |
|---|---|
| `src/Transcode/AscendVenc.{h,cpp}` | **新增**。对外只有 `open/encode/flush/close` + `available/acquireChannel/releaseChannel`；内部同步排空，不暴露任何 CANN 头文件 |
| `src/Codec/Transcoder.{h,cpp}` | 钩子：`isVideo()`、`openAscendVideo()`、`encodeAscend()`；`inputFrame` 放行无 `_context` 的昇腾路径；warmup 等首包 |
| `CMakeLists.txt` | `option(ENABLE_ASCEND_VENC ... OFF)`，非 aarch64 自动关，开时探测 CANN 头与 `acl_dvpp_mpi` |
| `docs/TRANSCODE_ASCEND_VENC.md` | 完整实测记录（原理、坑、容量、测量口径） |

上游文件改动集中在 `Transcoder.cpp` 既有函数体内，属「换轨点只在既有函数里可见」的接缝，
与 `docs/MAINTAINABILITY_UPSTREAM_DIFF.md` 里的论断一致。

### 六个真 bug（按发现顺序，全部已修）

1. **帧入口被静默丢弃**（最费时，约 3 小时）
   `FFmpegEncoder::inputFrame()` 第一行 `if (!_context) return false;` —— 昇腾路径没有 libavcodec
   上下文，于是每一帧直播帧在进 `inputFrame_l` 前就被丢掉。`warmup()` 直接调 `encodeFrame()`
   所以首包能出、目标轨能 ready，看上去一切正常，实际只有第 1 帧。
   修：`!_context` 分支放行 `_ascend`；`inputFrame_l` 开头直接走 `encodeAscend`。

2. **`onEncode` 里 `_context` 为空 → SIGSEGV**
   症状伪装成「卡住」：进程不退出（ZLM 的 `sig_crash` 处理器在跑 `addr2line`），
   只能用 `gdb -p <pid> -batch -ex "thread apply all bt"` 才看到 `sig_crash` 帧。

3. **独立取流线程在本进程内恒拿不到数据**
   `hi_mpi_venc_get_stream` 恒 `0xA008800e`(BUF_EMPTY)，而同线程同步取流正常
   （探针 `send=1 got_packs=1`）。修：删掉取流线程，`encode()` 内同步排空。

4. **`get_stream` 超时不能取 0**
   非阻塞路径会卡住不返回；给大了（5ms×20 轮≈120ms）又会把发送节奏限流。
   最终：1ms + 极少次重试，收尾清理循环必须有上界。

5. **一个访问单元被当成多帧**
   `get_stream` 一次返回的多个 pack（AUD/VPS/SPS/PPS/SEI/切片）属于同一帧，
   按 pack 逐个回调等于拆帧。修：拼成一个 AU 再回调一次。

6. **pts 用墙上时钟 → 时间轴被压缩**
   解码突发时输出时间轴快约 1.6 倍（dts 间隔 25ms 而非 40ms）。
   修：按输出帧率匀速推进（AU pts 实测 `0/40000/80000…`）。
   > 这一条是我自己先改错、后来又改回来的：当初为解「编码器丢帧」误判成 pts 问题，
   > 真因是上面的 #1。

另外修了两个并发/构建问题：

7. **通道号 `计数器 % 16` 会打停在跑的流**
   第 17 路复用通道 0/4，18 路测试时原有两路视频在 1.8 秒后断流（音频直通照旧，极易误判）。
   修：通道池 `acquireChannel/releaseChannel`，`close()` 归还；池满时明确告警并回落软编。

8. **`_ascend_wanted` 写在 `#ifdef` 外**，默认构建（开关关）直接编译失败。

### 实测数据（可复现）

```bash
# 单路验收
bash /root/av_sync.sh
#   视频 295 包 / 11.76s = 25.0fps，音频跨度 11.90s，crash=0

# 并发（墙上时间窗口版，参数：路数 窗口秒数 素材）
bash /root/multi_wall.sh 16 12 /tmp/src1080.mp4
#   16/16 路 25.0fps，聚合 400fps，opened=16 refused=0 crash=0
```

测量口径（血泪）：`ffprobe -read_intervals "%+10"` 按**媒体时间**取窗口，
时间轴一旦和墙钟不同步，客户端 2~3 秒就自己 TEARDOWN，看起来像「所有流都断了」。
并发测试一律用**墙上时间窗口**，并同时报「媒体口径 fps」与「墙钟口径 fps」。

### 待办

- 同一进程 >16 路时的行为是「回落软编」，要不要改成「拒绝注册转码」需要产品口径；
  已验证通道号按进程隔离（两进程各用通道 0 互不影响），所以扩容走多进程分片。
- 1080p 30fps / 4K 未测。
- 昇腾硬解（VDEC）接入转码链路未做（当前源解码仍是 ffmpeg 软解）。
- 多进程分片方案未落地（需要端口/通道区间/共享状态的划分约定）。

### 环境备忘（192.168.210.89）

- `curl` 不可用（`libldap.so.2: undefined symbol: EVP_md2`）→ 探活/下载用 Python `urllib`。
- ffmpeg 的 RTSP 客户端间歇 `Operation not permitted`，取证优先 `ffprobe` 或裸 socket。
- 推流素材必须 `-stream_loop -1`，否则素材放完即断流，会被误判成产品问题。
- 测试实例只按 `[c]cfg_asc.ini` 精确匹配 PID 清理；**绝不能 `pkill -f MediaServer`**。
