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

## 2026-11-03 追加：DVPP 硬解接入（硬解 → 硬编闭环）

### 结果

| 项 | 实测 |
|---|---|
| 客户端视频 | 25.1 fps（源 25fps） |
| 音画对齐 | 视频 11.64s / 音频 11.90s |
| 进程 CPU | **17%**（单路 720p，硬解+硬编） |
| 4 路并发 | 4/4 全 25.0fps，crash=0 |

### 结构改动

- 新增 `AscendEnv`：`aclInit`/`hi_mpi_sys_init` 进程内只能成功一次，VENC/VDEC 共用 refcount 初始化
- 新增 `AscendVdec`：`open/decode/flush/close` + 32 通道池，送 Annex-B 出紧凑 NV12
- `FFmpegDecoder` 接管：`hw_decode` + H264/H265 + VDEC 可用时完全不碰 libavcodec

### 五个坑（按踩到顺序）

1. 缺 `hi_mpi_vdec_start_recv_stream` → `send_stream` 恒 `0xA0058009 (NOT_PERM)`
2. `pic.vir_addr = 0` → `0xA0058011 (BAD_ADDR)`，必须给驱动真实输出缓冲
3. 送包失败只退避不排空 → `0xA005800F (BUF_FULL)` 永远解不了锁，要先取帧腾空
4. NV12 的 `virt_addr[1]` 恒 NULL（UV 在 Y 后面的连续内存）→ 不兜底就整帧丢
5. 逐行 `aclrtMemcpy`（每帧上千次）把硬解拖到比音频慢 1.5 秒 → 改整块拷贝

### 测量陷阱（新）

`pgrep -f cfg_asc.ini | head -1` 会取到过期进程，CPU 测出「98%」假数；
同一脚本改用自己启动的 `$!` 后是 17%。**测 CPU 只用自己启动的 PID。**

### 待办

- 1080p 硬解未单独测（720p 已验证）
- 硬解 + 多路（>4）容量待测
- 解码掉线重连、码流参数中途变化（分辨率切换）未处理

## 2026-11-04 追加：硬解 1080p 与并发补测

| 场景 | 结果 |
|---|---|
| 单路 1080p 硬解+硬编 | 客户端 25.0fps（430 包/17.16s），编码器 1920x1080@6000kbps，crash=0 |
| 8 路 1080p 并发 | 8/8 全 25.0fps，聚合 200fps，crash=0 |
| 4 路 720p 并发 | 4/4 全 25.0fps，聚合 100fps，crash=0 |

坑：12 秒素材循环会让音频 PTS 回环重置，音频跨度被撑到 27s，误看像音画不同步。
判 A/V 同步必须在一个循环周期内取样。

待办剩余：解码侧分辨率中途变化、断线重连未处理；>8 路 1080p 未测。

## 2026-11-04 追加：断线重连与通道归还

20 轮「推流→断流」压测（共开关 VDEC/VENC 各 21 次，已超 16 路上限）：

- 每轮推流期间视频包稳定 204~205，无逐轮衰减
- 第 20 轮后恢复 25.0 fps
- `no free DVPP channel` **0 次**、crash=0

结论：通道池的 acquire/release 没有泄漏。之前只验过单轮，不足以说明长时间运行是否安全。

## 2026-11-04 追加：码流中途变分辨率

720p→1080p 拼接素材（TS，SPS 中途变化）循环推流：

- 客户端 799 帧 / ~32 秒连续、25.0fps、无空洞（跨 2 个切换点）
- 输出分辨率全程 1280x720（转码目标锁定配置尺寸，1080p 段由 sws 缩下来）
- 解码丢帧 4（启动期空帧）、crash=0

坑：首轮测出「客户端 0 包」其实是取样太早（推流后 2 秒就拉，转码轨还没 ready）。
推流后等 10 秒再拉即正常。**判定"切换后断流"前必须先排除取样时序。**

未实现：跟随源分辨率变化（需重开编码器 + 重建目标轨）。

## 2026-11-04 追加：DVPP 运行指标暴露到状态接口

`/index/api/getTranscode` 的 `tracks[]` 新增（仅昇腾构建有值）：

| 字段 | 含义 |
|---|---|
| `decode_backend` | `ascend` / `ffmpeg` / `none` |
| `vdec_in` / `vdec_out` / `vdec_dropped` | 送进 VDEC / 解出来 / 取到句柄但无有效图像 |
| `vdec_channel` / `venc_channel` | 占用通道号（-1 = 未占用） |
| `venc_sent` / `venc_out` / `venc_retries` | 送编码器 / 回调包数 / 驱动忙重投 |

实测一轮 720p 硬解+硬编：`vdec_in 420 / vdec_out 415 / dropped 3`、`venc_sent 413 = venc_out 413`、
`retries 0`、通道 0。

**修正一个误导项**：`hw_decode` 原本只统计 libavcodec 的硬件帧下载，走 DVPP 硬解时恒 `false`
（现象是「配置开了硬解、接口说没硬解」），已对齐为硬解时 `true`、`hw_decode_frames` 同步 `vdec_out`。

## 2026-11-04 追加：API 参数矩阵（硬解+硬编）

| 用例 | 请求 | 实测 |
|---|---|---|
| 1080p→720p | `width=1280&height=720&video_bitrate=4000000` | 1280x720 / 25.0fps / 787kbps ✅ |
| 720p→1080p | `width=1920&height=1080` | 1920x1080 / 25.0fps / 1413kbps ✅ |
| 帧率 15 | `video_fps=15` | 15.0fps ✅ |
| 码率 2Mbps | `video_bitrate=2000000` | 1920x1080 / 1621kbps ✅ |
| 倍数 0.5 | `scale=0.5` | 960x540 / 25.0fps ✅ |

**踩坑（自找的）**：第一轮把配置键名当 API 参数传（`video_width`/`video_scale`），
结果全部被静默忽略、编码器仍按源尺寸打开，差点当成产品缺陷报出去。
正确 API 参数是 `width`/`height`/`scale`；`video_bitrate` 单位是 **bps**（写 4000 = 4kbps）。

## 2026-11-04 追加：16 路 1080p + 「跟随源分辨率」结论

**16 路 1080p 并发**：16/16 全部 25.0fps、聚合 400fps、`opened=16 refused=0 crash=0`。
1080p 与 720p 上限相同（都是 VENC 16 通道）。

**跟随源分辨率变化 —— 结论是不做**，有两组实证：

1. ZLM 源轨对象不跟随：推 720p→1080p 切换素材，1080p 段期间源轨仍报 `1280x720`。
   轨道尺寸来自第一份 SPS，中途不会更新，SDP/muxer/客户端都基于它 ⇒ 我们这层重开编码器也没用。
2. 重推流会跟随：同流名先 720p 再 1080p 重推，客户端依次拿到 `hevc,1280,720` 和 `hevc,1920,1080`，
   编码器重开 720p→1080p、通道复用，crash=0。

现场口径：清晰度切换按「断流 → 新清晰度重推」，不要指望同一条码流里改 SPS。
