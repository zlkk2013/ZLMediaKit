# 昇腾 310P 硬编码（DVPP VENC）实测记录

> 机器：192.168.210.89 / openEuler 24.03 LTS-SP1 / aarch64 / 128 核 / 124G / **2× Ascend 310P3** / CANN 8.2.RC2
> 目的：判断 ZLMediaKit 转码模块能否在客户昇腾机上用硬件编解码，并量化到什么程度。

## 结论先行

| 项 | 状态 | 证据 |
|---|---|---|
| 机器上是否有可用 VENC | ✅ 有 | `hi_mpi_venc_create_chn` / `start_chn` 均返回 0 |
| 能否真出 H.264 码流 | ✅ 能 | 落盘 21194 字节，`ffprobe → h264,640,360,15 帧` |
| 帧吞吐 | ⚠️ 只吃下一半 | 30 帧送进去，15 帧被编码，15 帧返回 `0xA0088003` |
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

## 未解决：只接受一半帧

现象：严格交替 —— 第 0/2/4/… 帧被接收并编码，1/3/5/… 帧返回 `0xA0088003`。

已排除的实验（每条都真跑过，不是推断）：

| 变量 | 结果 |
|---|---|
| 缓冲轮换（4 份 / 8 份 / 固定同一份） | 交替规律不变 → 与缓冲无关 |
| `is_by_frame` TRUE / FALSE | 不变 |
| `send_frame` vs `send_frame_ex` | 不变 |
| 超时 -1 / 0 / 1000ms | 不变 |
| 失败后先 drain 再重试 | drain 返回 `0xA008800e`（缓冲空），重试仍失败 |
| 失败后带 10ms 间隔重试 50 次 | 仍失败；但**下一帧立刻成功** |
| 双线程（独立取流线程 + 2s 退避重投同一帧） | 仍然交替；重投的同一帧永不成功 |
| 按 25fps 节流 | 不变 |
| pts 恒定 | **只有首帧成功**（说明 pts 必须严格递增） |
| pts 间隔 40000 / 80000 | 都是交替 → 与 pts 步长无关 |

`query_status` 观察：`left_pics` 在 0/1 之间跳、`cur_packs=1`，即编码器内部队列深度只有 1。
但队列空着时重投同一帧仍然被拒 ⇒ 不是单纯背压。

## 下一步（按性价比排序）

1. **拿官方 VENC 样例对照**（CANN 8.2 没随包带，`tools/` 下只有算子样例）。官方样例的喂帧/取流循环是判据来源。
2. 若样例也复现 ⇒ 是这套 `hi_mpi_venc_*`（海思血统、单设备）在 310P 上的已知限制，需评估"隔帧编码"是否可用（12.5fps）。
3. 若样例不复现 ⇒ 逐字段 diff 我们的 `hi_venc_chn_attr` / `hi_video_frame` 与样例。

## 与 ZLMediaKit 的关系（架构结论）

ZLM 的转码是**进程内 libavcodec**，而昇腾没有 libavcodec 后端（无 encoder、无 hwaccel）。
所以只有两条路：

- **A. AscendFFmpegPlugin**：给 ffmpeg 装昇腾编解码器，ZLM 侧只在候选编码器表里加名字，改动极小。
  代价：锁 ffmpeg 4.4.1（本仓兼容层已能编 4.4），与 CANN 8.2 兼容性未知，且下载需华为账号。
- **B. 在 ZLM 里新增 DVPP 后端**：解码走已闭环的 VDEC 封装（169fps），编码走 VENC。
  代价：要写 `AscendEncoder`（本文件验证的就是它的可行性），且必须先解决"隔帧"问题。

**当前判断**：B 的可行性已用实码验证（真出 H.264），A 依赖外部下载。建议先把"隔帧"定位清楚，
再决定投入哪条；在编码侧稳定前，**不应该对客户承诺昇腾硬编**。

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
