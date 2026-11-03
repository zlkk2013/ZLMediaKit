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

## 容量参考（210 真机，2×310P3）

| 分辨率 | codec | 帧数 | 吞吐 |
|---|---|---|---|
| 640x360 | H.264 | 300 | 3263 fps |
| 1280x720 | H.264 | 300 | 980 fps |
| 1280x720 | H.265 | 300 | 981 fps |

口径说明：纯编码 + CPU 内存喂帧，不含解码/抽帧/NV12 转换，是**编码器上限**而非端到端吞吐。

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
