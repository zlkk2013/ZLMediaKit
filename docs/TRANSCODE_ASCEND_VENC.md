# 昇腾 310P 硬编码（DVPP VENC）实测记录

> 机器：192.168.210.89 / openEuler 24.03 LTS-SP1 / aarch64 / 128 核 / 124G / **2× Ascend 310P3** / CANN 8.2.RC2
> 目的：判断 ZLMediaKit 转码模块能否在客户昇腾机上用硬件编解码，并量化到什么程度。

## 结论先行

| 项 | 状态 | 证据 |
|---|---|---|
| 机器上是否有可用 VENC | ✅ 有 | `hi_mpi_venc_create_chn` / `start_chn` 均返回 0 |
| 能否真出 H.264 码流 | ✅ 能 | 落盘 21194 字节，`ffprobe → h264,640,360,15 帧` |
| 帧吞吐 | ✅ 全部吃下 | 640x360 **3263 fps** / 720p H.264 **980 fps** / 720p H.265 **981 fps**（300 帧全部编码，ffprobe 校验帧数一致） |
| ffmpeg 侧是否有昇腾编解码器 | ❌ 没有 | `ffmpeg 6.1.1 -encoders` 里 ascend/dvpp 计数 = 0；hwaccels 只有 vdpau/cuda/vaapi/drm/opencl |
| 能否走 AscendFFmpegPlugin | ❓ 未验证 | 机器上未安装；该插件锁 ffmpeg 4.4.1，与 CANN 8.2 兼容性未知；需华为社区账号门控下载 |

## 已验证的初始化链（缺一步就失败）

```
aclInit → aclrtSetDevice(dev) → aclrtCreateContext → aclrtSetCurrentContext → hi_mpi_sys_init
```

- 只做 `aclInit` 就 `create_chn`：返回 `0xA0088015`（SYS_ERROR）。
- 与 VDEC 封装里验证过的顺序完全一致（见 `_ascend_vdec_wrap.c` 第 12 行注释）。

## 三个必踩的坑

1. **`hi_mpi_dvpp_malloc` 返回的是设备内存**，host 直接写 = **SIGSEGV（exit 139）**。
   必须 host 缓冲填好 → `aclrtMemcpy(..., ACL_MEMCPY_HOST_TO_DEVICE)`。
2. **输出码流的 `pack.addr` 也是设备内存**，`fwrite` 直接写会得到 0 字节文件（长度算出来了、文件却是空的）。
   必须 `aclrtMemcpy(..., ACL_MEMCPY_DEVICE_TO_HOST)` 拷回再落盘。
3. **`hi_mpi_venc_send_frame_ex` 的 `user_rc_info.frame_type` 必须显式设成 `VENC_FRAME_TYPE_NONE`**；
   留 0（不设）时 **一帧都不收**（30 帧全失败）。

## 曾经的坑：只接受一半帧 —— 根因是 `time_ref` 必须是偶数

现象：严格交替，第 0/2/4 帧被编码，1/3/5 帧返回 `0xA0088003`。

**根因**：官方样例 `sample_encoder.cpp:370` 有一行注释

```c
videoFrameInfo->v_frame.time_ref = (g_send_frame_cnt[chnId] * 2); // 2:timeref must be even
```

即 **`v_frame.time_ref` 必须是偶数**。我们按帧号递增（0,1,2,3…）传，奇数帧被驱动判为非法参数。
一个字段的取值约束，伪装成了「驱动只吃一半帧」，查了十几轮。

修法：`time_ref = frame_index * 2`，`pts` 仍按 `frame_index * (1e6/fps)` 严格递增。

> 另外两个由此暴露的相关约束：`pts` 必须严格递增（恒定 pts 时只有首帧被接受）；
> `user_rc_info.frame_type` 必须显式设 `VENC_FRAME_TYPE_NONE`，留 0 时一帧都不收。

### 排查过程中被证伪的假设（都真跑过，留着省下一轮）

缓冲轮换（4/8/固定同一份）、`is_by_frame` TRUE/FALSE、`send_frame` vs `_ex`、
超时 -1/0/1000、失败后 drain 再重试、带 10ms 间隔重试 50 次、双线程独立取流 + 2s 退避重投、
25fps 节流、pts 步长 40/80/200ms、`dynamic_range`/`compress_mode`/`color_gamut` 对齐官方。

**教训**：这类「参数级拒绝」不要靠试错硬碰，去拿官方样例逐字段对照 —— 官方样例在本机编译后
60/60 帧一次通过，直接证明是调用方问题而不是硬件问题，比继续猜快得多。

## 关键实现要点（可直接复用）

```c
// 初始化（缺一步 create_chn 报 0xA0088015）
aclInit → aclrtSetDevice → aclrtCreateContext → aclrtSetCurrentContext → hi_mpi_sys_init

// 通道属性（310P 实测可用组合）
attr.venc_attr.type = HI_PT_H264;          // 或 HI_PT_H265
attr.venc_attr.is_by_frame = HI_TRUE;
attr.venc_attr.buf_size = 4 * 1024 * 1024; // H264 官方给 4M，H265 2M
attr.venc_attr.h264_attr.rcn_ref_share_buf_en = HI_FALSE;
attr.rc_attr.rc_mode = HI_VENC_RC_MODE_H264_CBR;
attr.rc_attr.h264_cbr = {gop=fps, stats_time=1, src_frame_rate=fps, dst_frame_rate=fps, bit_rate=kbps};
attr.gop_attr.gop_mode = HI_VENC_GOP_MODE_NORMAL_P;

// 每帧
fi.v_frame.time_ref = frame_index * 2;                  // ★ 必须偶数
fi.v_frame.pts      = frame_index * (1000000 / fps);    // ★ 必须严格递增
fi.v_frame.dynamic_range = HI_DYNAMIC_RANGE_SDR8;       // 官方样例对齐
fi.v_frame.compress_mode = HI_COMPRESS_MODE_NONE;
fi.v_frame.color_gamut   = HI_COLOR_GAMUT_BT709;
aclrtMemcpy(dev, size, host, size, ACL_MEMCPY_HOST_TO_DEVICE);  // 设备内存，不能直接写
ufi.user_frame = fi;
ufi.user_rc_info.frame_type = VENC_FRAME_TYPE_NONE;      // ★ 必须显式设
hi_mpi_venc_send_frame_ex(chn, &ufi, 0);

// 取流（独立线程，设备内存需 D2H）
hi_mpi_venc_get_stream(chn, &stream, 0);
aclrtMemcpy(host, len, pack.addr, len, ACL_MEMCPY_DEVICE_TO_HOST);
hi_mpi_venc_release_stream(chn, &stream);
```

## 多路并发（`tools/ascend/ascend_venc_multi.cpp`）

310P 的 VENC 通道是**整机共享**资源（同 VDEC），所以既要看总吞吐，也要看每路是否还够用。
N 个通道各起一个线程并发喂 300 帧 720p H.264：

| 路数 | 总吞吐 | 每路平均 | 丢帧 |
|---|---|---|---|
| 1 | 572 fps | 572 | 0 |
| 4 | 1844 fps | 461 | 0 |
| 8 | 1850 fps | 231 | 0 |
| 16 | 1852 fps | 116 | 0 |

- **聚合上限约 1850 fps（720p）**，4 路就已打满；再往上只是摊薄，不增总量。
- 16 路时每路仍有 116 fps ≈ 源 25fps 的 **4.6 倍余量**，对"16 路同时转码"完全够用。
- 上限很可能在 **H2D 拷贝**而非编码器本身：1850 fps × 1.38MB ≈ 2.5GB/s，与 CPU 侧 memcpy 瓶颈量级一致
  （后续若上零拷贝/大页可再抬，未验证）。

## 容量参考（210 真机，2×310P3）

| 分辨率 | codec | 帧数 | 吞吐 |
|---|---|---|---|
| 640x360 | H.264 | 300 | 3263 fps |
| 1280x720 | H.264 | 300 | 980 fps |
| 1280x720 | H.265 | 300 | 981 fps |

口径说明：纯编码 + CPU 内存喂帧，不含解码/抽帧/NV12 转换，是**编码器上限**而非端到端吞吐。

## 编码后端封装（`src/Transcode/AscendVenc.*`，已实现并真机验证）

把 DVPP VENC 封成可复用的编码器类，对外只有三件事：`open()` / `encode(NV12, pts)` / `close()`，
调用方看不到任何 CANN 头文件。取流在内部线程里跑（非阻塞轮询 + D2H 拷贝），逐包回调 Annex-B
码流并标记关键帧。

编译开关 `-DENABLE_ASCEND_VENC=ON`（**默认 OFF**）：关闭时该 .cpp 编译成空文件，
对上游构建零影响，CMake 会自动探测 CANN 路径，找不到就退回 OFF 并告警。

自测程序 `tools/ascend/ascend_venc_selftest.cpp`，真机（2×310P3）结果：

| 用例 | 结果 |
|---|---|
| H.264 1280x720 × 300 帧 | **PASS** 300/300，关键帧 12 个，612.8 fps |
| H.265 1280x720 × 300 帧 | **PASS** 300/300，关键帧 12 个，574.5 fps |
| H.264 1920x1080 × 200 帧 | **PASS** 200/200，关键帧 8 个，276.1 fps |

三份产物 ffprobe 全部校验通过（codec/分辨率/帧数一致）。

封装里踩到的两个自身缺陷（都已修）：
- **`aclInit` 不能调用两次**：`available()` 探测过一次后，`open()` 再调就返回失败。
  改为「初始化只做一次，后续只 `aclrtSetCurrentContext` 重绑线程」。
- **关键帧判断不能只看首个 NAL**：IDR 访问单元通常以 SPS/PPS 打头，
  只看第一个 NAL 会把关键帧全判成非关键帧（实测 `keys=0`），必须扫描整个访问单元。

## 解码+编码闭环（`tools/ascend/ascend_transcode_roundtrip.cpp`）

把项目的 VDEC 封装（已在生产跑的 `_ascend_vdec_wrap.so`）和本仓的 `AscendVenc` 串起来，
证明「硬解 → NV12 → 硬编」这条链在这台机器上是通的：

```
源码流 60 个访问单元 → VDEC 硬解 60 帧 → VENC 硬编 60 帧 → 输出 h264 1280x720 60 帧
```

ffprobe 校验：`h264,1280,720,60`，与输入帧数一致，0 丢帧。

闭环过程中修掉的三个缺陷（都是"能跑但结果不对"这一类）：

1. **`encode()` 没有为调用线程绑定 ACL context** —— 单线程自测碰不到，多路并发或多线程喂帧时
   300/300 帧全部失败。`aclrtMemcpy` 的参数校验依赖当前线程的 context。
2. **EOS 之后必须继续取流** —— 否则 VDEC 里残留 1~2 帧被丢掉（实测 60 帧只出 59 帧）。
3. **只喂整帧** —— 取流返回的 `out_len` 小于整帧大小时喂进编码器会产出坏码流
   （ffprobe 报 `cbp too large`）。判据：`out_len == width*height*3/2` 才送编码。

另一个环境坑：`pkill -f <脚本路径>` 会匹配到执行它的 shell 自身，导致命令静默自杀、输出为空 ——
远端清理一律用 `pkill -x <二进制名>`。

## 与 ZLMediaKit 的关系（架构结论）

ZLM 的转码是**进程内 libavcodec**，而昇腾没有 libavcodec 后端（无 encoder、无 hwaccel）。
所以只有两条路：

- **A. AscendFFmpegPlugin**：给 ffmpeg 装昇腾编解码器，ZLM 侧只在候选编码器表里加名字，改动极小。
  代价：锁 ffmpeg 4.4.1（本仓兼容层已能编 4.4），与 CANN 8.2 兼容性未知，且下载需华为账号。
- **B. 在 ZLM 里新增 DVPP 后端**：解码走已闭环的 VDEC 封装（169fps），编码走 VENC。
  代价：要写 `AscendEncoder`（本文件已验证它可行并给出了可复用的实现要点）。

**当前判断**：B 的路已经打通（300/300 帧、720p ~980fps），A 仍依赖华为账号下载且兼容性未知。
**建议走 B**：编码侧我们已经有可复用代码，解码侧 VDEC 封装也已在生产用（169fps）。
剩下的是工程量（把 VENC 包成 ZLM 的编码后端 + 生命周期/背压），不是可行性问题。

**仍未验证**：4K、多路并发下的编码器争用（310P 的 VENC 容量上限官方说 30 路 1080p）、
以及与 ZLM 转码链路的实际对接（本文件只验到"裸编码"这一层）。

## 复现方式

```bash
# 源码：tools/ascend/venc_probe.c（单线程，含各实验开关）、tools/ascend/venc_dual.c（双线程）
TC=/usr/local/Ascend/ascend-toolkit/latest
INC=$TC/aarch64-linux/include; LIB=$TC/aarch64-linux/lib64; DRV=/usr/local/Ascend/driver/lib64/driver
gcc -O2 -o venc_dual venc_dual.c -I$INC -I$INC/acl/dvpp -L$LIB -L$DRV \
    -lascendcl -lacl_dvpp_mpi -ldvpp_op_base -lruntime -lascend_hal -lpthread \
    -Wl,-rpath,$LIB -Wl,-rpath,$DRV
export LD_LIBRARY_PATH=$LIB:$DRV:$LD_LIBRARY_PATH
./venc_dual /tmp/out.h264 0
ffprobe -v error -show_entries stream=codec_name,width,height -of csv=p=0 /tmp/out.h264
```

## 接进 FFmpegEncoder 的实测状态（2026-11-03）

开关 `-DENABLE_ASCEND_VENC=ON`，默认 OFF，对非 aarch64 构建零影响。

### 已验证

| 项 | 证据 |
|---|---|
| 分支选择正确 | 日志 `transcode: video will be encoded by Ascend DVPP VENC` |
| DVPP 通道打开成功 | `ascend venc opened: 1280x720 H265 ch=0 fps=25` |
| 预热产出配置帧 | `warmup done: config_nals=8, ready=1, size=1280x720` |
| 目标轨就绪 | `transcoded video track ready=1, size=1280x720` |
| 客户端能拉到转码流 | `ffprobe` 拉流返回 `hevc,1280,720` |
| 无崩溃 | 清理版连续跑 20 秒 `crash=0` |

### 接线期踩到的 6 个坑（全部已修，教训可复用）

1. **`aclInit` 只能调一次**：`available()` 探测过后 `open()` 再调必失败 → 初始化只做一次，之后每线程 `aclrtSetCurrentContext`。
2. **`encode()` 必须给调用线程绑 context**：不绑则多线程下一帧都送不进。
3. **独立取流线程在本进程内拿不到数据**：`hi_mpi_venc_get_stream` 恒 `0xA008800e`（BUF_EMPTY），而同一线程同步取流稳定成功（探针 `send=1 got_packs=1`）→ 改为 `encode()` 内同步排空。
4. **`get_stream` 超时不能取 0**：非阻塞路径会卡住不返回；也不能取大（5ms×20 轮把 25fps 限流到 8fps）→ 取 1ms + 极少次重试。
5. **`onEncode` 里 `_context` 为空**：昇腾路径没有 libavcodec 上下文，`_context->codec_type` 直接 SIGSEGV → 判空。
6. **`goto fail` 不能跨过变量初始化**：C++ 错误「跳转至标号 fail」。

### 端到端验收（2026-11-03 收口）

```
addTranscode(video_codec=H265, hw=1, on_demand=0) → 200
服务端：onEncoded | transcoded 100/200/300/400 frames（持续产出）
客户端：ffprobe → hevc,1280,720
崩溃：0
```

### 最关键的一个堵点（第 7 个坑，也是真正卡了三小时的那个）

`FFmpegEncoder::inputFrame()` 第一行是：

```cpp
if (!_context || !frame || !frame->get()) {
    return false;      // ← 昇腾路径根本没有 _context，于是每一帧都被丢掉
}
```

昇腾路径不创建 libavcodec 上下文，所以真实推流帧在进入 `inputFrame_l` 之前就被**静默丢弃**：
`warmup()` 直接调 `encodeFrame()` 所以能出首包、目标轨能 ready，而直播帧一帧都到不了编码器。
外部表现极具误导性——DVPP 通道正常、预热成功、无报错，只是「永远只有第 1 帧」。

修法：`if (!_context)` 分支里放行 `_ascend`；`inputFrame_l` 开头在 `_ascend && !_context` 时
直接 `return encodeAscend(frame->get())`。

### 排查这类问题的有效顺序

1. 入口计数（`TranscodeSink::inputFrame` 调用次数）→ 源帧到没到
2. `onDecoded` 的 hit / no-encoder 计数 → 解码出来的帧有没有配上编码器
3. `AscendVenc::encode` 送帧计数 → 有没有真送进 DVPP
4. 卡住时 `gdb -p <pid> -batch -ex "thread apply all bt"` 看线程栈（对「卡住」和「SIGSEGV」都适用）

### 仍未闭环

- 帧率与源 25fps 的对应关系还需在长窗口上测准（当前里程碑显示输出快于源，需确认是否有重复帧）。
- 多路并发（多流同时转码）在 ZLM 进程内未测。
- 按需转码（`on_demand=1`，默认）下观众何时接入才会真正启用编码，需要产品口径上明确。
