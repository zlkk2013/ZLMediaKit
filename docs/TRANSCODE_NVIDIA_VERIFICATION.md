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

## 容量：方法还没定，数字不对外

**当前不能给出「单机多少路」**，两次尝试都被证明方法有问题：

1. 第一版用 `tracks[].frames` 算帧率，得出「12 路跑满 30fps」。但在同一窗口内
   服务端计数 248 帧 / 3.9s（63fps），拉流端只有 135 包 / 5s（27fps），源是 30fps ——
   该计数按「编码输出帧/NAL」累加，比真实帧率高约一倍，用它判容量会把容量放大一倍。
2. 第二版改拉流实测 + 单路基线（27fps），4 路掉到 21.3fps。这个下跌可能是真实的，
   也可能是「逐路顺序拉流」自身的测量偏差（拉流期间其余路仍在转码）。

**下一版要做的**：并发拉流（N 个 ffmpeg 同时拉）+ 更长窗口（≥10s）+ 不计入加入等待，
再给出数字。在此之前不对外报容量。

## 环境坑（都实际踩过）

| 坑 | 现象 | 处理 |
|---|---|---|
| WSL 空闲关实例 | `/tmp` 素材消失，测试报「缺素材」 | 素材存 `/root/assets`，每次跑前恢复 |
| WSL 崩溃留 0 字节二进制 | `PermissionError`，且 `cmake` 认为目标已最新不再重编 | `rm` 掉二进制再 build |
| `nvidia-smi` 不在 PATH | 脚本/Linux 服务都看不到 GPU | 用 `/usr/lib/wsl/lib/nvidia-smi` 绝对路径 |
| WSL 内存 5.8GB | 大并行度编译 OOM | `-j4` |
| macOS tar 带 `._*` 资源叉 | 2350 个资源叉被当源码编译 | `find -name '._*' -delete`，打包加 `COPYSOURCE_DISABLE=1` |
| 三层 shell 引号 | Windows cmd 吃掉 `|`、`>`、引号，命令静默失效 | 一律「写脚本文件 → scp → `wsl bash /root/x.sh`」 |

## 未完成

- **ffmpeg 4.4 兼容**：本仓代码用了 5.1+ 的 `ch_layout`（33 处），Ubuntu 22.04 自带的
  4.4 编不过 —— 客户直接 `apt install libav*-dev` 会失败。要么加版本分支，要么在交付
  文档里明确要求 ffmpeg ≥ 5.1。
- 容量口径（见上）。
- Windows 原生（MSVC）与 VAAPI 仍未验证/未接入。
