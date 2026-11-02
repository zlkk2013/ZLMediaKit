# NVIDIA 真机验证记录（RTX 5060 Ti / WSL2）

## 环境

| 项 | 值 |
|---|---|
| 机器 | 5060 = WIN-20241022PGP，经 `s5060` 跳板（`127.0.0.1:22060`） |
| GPU | NVIDIA GeForce RTX 5060 Ti，16GB，驱动 596.49 |
| 运行环境 | WSL2 Ubuntu 22.04.5（32 核可用，**内存仅 5.8GB**） |
| 编译用 ffmpeg | conda-forge ffmpeg 7.1（`/root/miniforge3/envs/zlmff`） |
| 产物 | `release/linux/Release/MediaServer` 7.37MB |

WSL 里 `nvidia-smi` 可用（GPU 直通走 `/dev/dxg`），NVENC 实测可用：

```
$ ffmpeg -f lavfi -i testsrc=d=2:s=1280x720 -c:v h264_nvenc -f null -
NVENC_OK
```

## 验收结果

| 套件 | 结果 | 关键证据 |
|---|---|---|
| `hw_encode_test` | **21/21** | `hevc_nvenc` / `h264_nvenc` 真在跑；CUDA 硬解下载 261~462 帧；硬编+硬解 44.2% vs 硬编+软解 47.7% |
| `multi_gpu_test` | **10/10** | 3 路在单卡机器上正确收敛 `[0,0,0]`；设备号上报与实际一致 |

硬件编码 CPU 对比（720p30 → H265）：

| 配置 | 进程 CPU |
|---|---|
| 软编 libx265 | 62.8% |
| 硬编 hevc_nvenc | 50.0% |

> 注意：这台机器 32 核，CPU 百分比按「占单核比例」看会显得差距不大；判据改用
> 「不显著变差」+ 编码器名与 `hardware=true` 为主，CPU 只作辅助证据。

## 这一轮修掉的 4 个缺陷

### 1. 上游选解码器会把整条流转死（最严重）

现象：转码完全不出帧，拉流超时，服务端日志刷

```
got decoder:h264_qsv
打开解码器成功:h264_qsv
avcodec_send_packet failed:Unknown error occurred   ← 每个包都失败
```

根因：上游候选列表把 `h264_qsv`/`hevc_qsv` 排在软件解码之前，且**不检查设备是否存在**。
只要 ffmpeg 编译时带了 QSV（conda-forge、Ubuntu 的 ffmpeg 都带），在没有 Intel 核显的机器上
就会选中它：`avcodec_open2` 返回成功，但每个包都送不进去，一帧都出不来。

macOS 的 ffmpeg 不带 qsv，所以这个问题一直没暴露 —— **只在 macOS 上验过的功能，
换到 Linux 就直接不可用**。

修法：`checkIfSupportedQsv()` 只在 sysfs 里查到 PCI vendor `0x8086` 时才认为有 Intel 显卡，
否则跳过 qsv 候选。**不能只看 `/dev/dri` 是否存在** —— WSL2 的 GPU 直通同样会为 NVIDIA 卡
创建 `card0`/`renderD128`，一开始按目录判断照样中招。

### 2. 状态接口设备号恒报 -1

`st.hw_device = st.hardware ? ... : -1;` 写在 `st.hardware` 赋值**之前**，永远读到默认
`false`。日志显示 `picked=0` 而接口报 `-1`。

### 3. `reset=1` 与参数同时给时，用的是「清空前」的快照

先设 `hw_device=1`，再 `reset=1&video_codec=...`，设备号 1 会残留在新覆盖里，
看起来像 reset 没生效。修法：清空后重新 `resolveRaw()` 取基线。

### 4. 单卡机器上会分配出不存在的卡

`hw_devices="0,1"` 是运维的声明，不等于机器真有 2 张卡。nvenc 把越界索引当默认卡用
（照样能跑），但接口报出一个不存在的卡号，运维会以为在用第二张卡。
修法：`availableGpuCount()`（`/proc/driver/nvidia/gpus` → `nvidia-smi -L`，
含 WSL 的 `/usr/lib/wsl/lib` 绝对路径），分配时跳过不存在的设备。

## 顺带加固的测试侧

- 新增 `scripts/_platshim.py`：`ps -M` / `lsof` / `release` 路径 / `nvidia-smi` 绝对路径 /
  fd 与线程采样全部收口，23 个脚本改用它。否则在 Linux 上第一步就报「服务没起来」。
- `multi_gpu_test` 的期望分配按实际卡数计算，不再写死 `[0,0,1]`。
- `hw_encode_test` 拉流重试耗尽后 `res=None` 会抛 `AttributeError`，改为报可读失败。
- `camera_scenario_test` 同样的超时改成重试 + 可读失败。

## 容量：单机 12 路 1280x720@30（实测）

### 口径（走过三条弯路才定下来）

| 版本 | 做法 | 结论 |
|---|---|---|
| v1 | 服务端 `tracks[].frames` / 墙钟 | ❌ 该计数按 NAL 累加，虚高 2.2 倍（63fps vs 真实 27fps） |
| v2 | 拉流发包数 / 墙钟 | ❌ 3 秒窗口含加入等待，单路也只有 27/30 |
| v3 | 并发拉流 + 媒体时长 | ❌ 干净但要起 N 个 ffmpeg，WSL 上测量自身成为干扰（对照流瞬间 6.5fps） |
| **v4（采用）** | **每路自己跟自己比** | ✅ 零外部进程、无窗口偏置 |

v4 依赖两个**单调累加**的服务端计数（都在 `getTranscode` 的 `tracks[]`）：

- `frames`：编码器输出的**包数 = 帧数**（本次修过，见下）
- `input_frames`：解码后送进编码器的帧数（本次新增）

判据：输入 ≥ 源×0.95 **且** 输出/输入 ≥ 97%，且无 `failed_tracks`。
这样天然把「源没喂满」和「编码跟不上」分开，不需要对照组。

### 实测（RTX 5060 Ti，WSL2，32 核，5.8GB 内存）

| 路数 | 每路输入 | 每路输出 | 追平率 | 进程 CPU | GPU 编码器 | RSS |
|---|---|---|---|---|---|---|
| 4 | 30.1fps | 30.0fps | 99.6% | 99.5% | 11% | 871MB |
| 8 | 30.1fps | 30.0fps | 99.6% | 144% | 21% | 1.69GB |
| **12** | **30.1fps** | **30.1fps** | **100%** | **210%** | **42%** | **2.57GB** |
| 16 | 17.9fps | 17.9fps | 100% | 325% | 40% | 3.29GB |

**结论：12 路 1280x720@30 全部满帧**（CPU 2.1 核、GPU 编码器 42%、内存 2.6GB）。

**16 路时瓶颈不在转码**：输出仍是输入的 100%，但输入掉到 17.9fps —— 是 16 个推流进程 +
WSL 调度喂不满。换句话说，**转码本身的余量还没用尽**：GPU 编码器只到 42%，
按线性外推单卡可撑 20+ 路（需裸机 Linux 复验）。

> 旁注：这台是 WSL，内存只有 5.8GB（宿主机 34GB），12 路已用 2.6GB；真机部署若开满内存，
> 上限会更高。裸机 Linux 的容量需要另外测，不要把这组数字当 Linux 服务器的容量。

### 顺带修掉的计数缺陷

`getTranscode` 的 `tracks[].frames` 原来在 **NAL 级回调** 里累加，而视频是拆 NAL 后才逐 NAL
回调的（x265 每帧带 AUD+VCL，IDR 再加 VPS/SPS/PPS）⇒ 虚高约 2.2 倍。
现改为在 `FFmpegEncoder::onEncode`（一个 AVPacket = 一帧）计数，并新增 `input_frames`。
复测：服务端 30.2fps vs 拉流端 29.6fps（源 30），一致。

## 环境坑（都实际踩过）

| 坑 | 现象 | 处理 |
|---|---|---|
| WSL 空闲关实例 | `/tmp` 素材消失，测试报「缺素材」 | 素材存 `/root/assets`，每次跑前恢复 |
| WSL 崩溃留 0 字节二进制 | `PermissionError`，且 `cmake` 认为目标已最新不再重编 | `rm` 掉二进制再 build |
| `nvidia-smi` 不在 PATH | 脚本/Linux 服务都看不到 GPU | 用 `/usr/lib/wsl/lib/nvidia-smi` 绝对路径 |
| WSL 内存 5.8GB | 并发编译 OOM（-j4 中途被杀、日志断在半行） | 降到 `-j2` |
| macOS tar 带 `._*` 资源叉 | 2350 个资源叉被当源码编译 | `find -name '._*' -delete` |
| 三层 shell 引号 | Windows cmd 吃掉 `|`、`>`、引号，命令静默失效 | 一律「写脚本文件 → scp → `wsl bash /root/x.sh`」 |
| 带空格的脚本参数 | `run_hw.sh "x --max 16"` 把参数拼进文件名，直接失败且留下旧日志 | 单独写 runner 脚本传 `"$@"` |

## 未完成

- ~~**ffmpeg 4.4 兼容**~~ —— 已解决：按 ZLM 既有的 `FF_CODEC_VER_7_1` 开关给
  `ch_layout`/`channel_layout` 两套 API 各写一份，在本机（WSL Ubuntu 22.04，系统 ffmpeg 4.4）
  编译 0 错误，并实测转码成功（H264→H265 + AAC→Opus，拉流 `hevc,1280,720 | opus`）。
  注意系统 4.4 不带 NVENC，硬件编码仍需 conda-forge 的 7.x。
- 裸机 Linux 的容量未测（本组数字来自 WSL，内存仅 5.8GB）。
- Windows 原生（MSVC）与 VAAPI 仍未验证/未接入。
