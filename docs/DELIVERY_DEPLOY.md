# 转码版本：构建与部署

本文说明怎么把这个转码分支编译出来、装到目标机上、以及交付时该说什么。

---

## 1. 依赖

### 编译期（必须有头文件与 `.so`/`.dylib`）

| 库 | 用途 | 不装的后果 |
|---|---|---|
| `libavcodec` `libavutil` | 编解码核心 | 编译直接失败（`ENABLE_FFMPEG=ON` 找不到） |
| `libswscale` | 像素格式/缩放 | 同上 |
| `libswresample` | 重采样（声道/采样率） | 同上 |
| `libavfilter` | 滤镜链、OSD、logo overlay | 滤镜功能编译不过 |
| `libavformat` | **解码 logo 图片**（png/jpg 走 demuxer） | 编译过、**链接期**报 `Undefined symbols: _avformat_open_input` |
| 编码器实现 | `libx264` `libx265` `libopus` `libmp3lame` `libvpx` | 对应目标 codec 不可用 |

> **「装了 ffmpeg」≠「能用于编译」**：系统里那个 `ffmpeg` 常常是**静态命令行二进制**，
> 只有可执行文件、没有头文件。先确认：
> ```bash
> find /usr /opt /usr/local -maxdepth 5 -name 'avcodec.h' 2>/dev/null
> pkg-config --modversion libavcodec
> ```
> 没有输出就说明得装开发包（或像本仓一样用 conda 装一份）。

> ⚠️ **ffmpeg 版本要求 ≥ 5.1**（见下）。Ubuntu 22.04 / Debian 12 自带的 4.4 编不过，
> 必须额外装新版本（conda 最省事）。

Ubuntu / Debian（**仅当系统 ffmpeg ≥ 5.1 时可用**）：

```bash
sudo apt install -y pkg-config cmake build-essential \
  libavcodec-dev libavutil-dev libavfilter-dev libavformat-dev \
  libswscale-dev libswresample-dev \
  libx264-dev libx265-dev libopus-dev libmp3lame-dev libvpx-dev
```

### ffmpeg 版本：4.4 与 7.1 都已实测（两套 API 均有分支）

本仓沿用 ZLM 既有的版本开关 `FF_CODEC_VER_7_1`，把 `ch_layout`（新）与
`channel_layout`（旧）两套声道布局 API 各写一份。实测结论：

| ffmpeg | 编译 | 转码实测 |
|---|---|---|
| **4.4**（Ubuntu 22.04 / Debian 12 的 apt 源） | ✅ 0 错误 | ✅ codec 级验证：H264→H265（298 帧/输入 297）+ AAC→Opus，拉流 `hevc,1280,720 \| opus` |
| **7.1**（conda-forge / macOS） | ✅ | ✅ 全量回归 15 套 336 断言（含音频矩阵 125 项） |

> ⚠️ **4.4 侧的音频矩阵（125 项）尚未跑通**：在 5060 的 WSL 里跑到第一格就静默退出
> （资源受限，5.8GB 内存跑不下整轮），临时写的单格探针结果不稳定（同一格两次得到 3.6s / 2.88s），
> 不能作为结论。**上线前需在裸机 Linux + ffmpeg 4.4 上跑一次 `audio_matrix_test.py`**。

所以 **Ubuntu 22.04 直接 `apt install libav*-dev` 就能编**，不需要额外装新版本。

> 注意：系统 ffmpeg 4.4 **不带 NVENC**（Ubuntu 的构建没编 nvenc）。要硬件编码请用
> conda-forge 的 ffmpeg 7.x（它带 `h264_nvenc`/`hevc_nvenc`/`av1_nvenc`），或自行编译
> 带 `--enable-nvenc` 的 ffmpeg。软件编解码用系统包即可。

conda（推荐，版本自洽，macOS 上几乎必用）：

```bash
conda create -y -n zlm-ffmpeg -c conda-forge 'ffmpeg=7.*' pkg-config
export PKG_CONFIG_PATH=$HOME/miniconda3/envs/zlm-ffmpeg/lib/pkgconfig
ffmpeg -hide_banner -encoders | grep -E 'libx264|libx265|libopus|libmp3lame|libvpx'
```

### 运行期

- 与编译期同一套 ffmpeg 动态库（版本必须一致，跨大版本会崩）
- `video_logo` 指定的图片文件必须**运行时路径可达**（相对路径按进程工作目录解析）
- 滤镜里的字体：`drawtext` 需要 `fontfile` 或系统 fontconfig；容器/精简系统常缺字体，
  建议显式给 `fontfile=/path/to/font.ttf`

---

## 2. 构建

```bash
export PKG_CONFIG_PATH=$HOME/miniconda3/envs/zlm-ffmpeg/lib/pkgconfig   # 按实际改

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DENABLE_FFMPEG=ON \
      -DENABLE_X264=ON \
      -DENABLE_TESTS=OFF

cmake --build build -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu)" --target MediaServer
```

产物：`release/<平台>/Release/MediaServer`（macOS 是 `release/darwin/Release/`）。

**新增了 `src/Transcode/` 目录后必须重跑 `cmake`**：`CMakeLists.txt` 用 `file(GLOB)` 在
configure 期展开，只 build 不 reconfigure 会在**链接期**报
`Undefined symbols: TranscodeRuntime / addTranscodeApi`（编译全绿，极易误判成代码写错）。

---

## 3. 配置与启动

1. 复制一份配置（服务首次启动会自动生成 `config.ini`，再用它做基线）

2. 加 `[transcode]` 段（**默认关闭**，不加就与上游行为完全一致）：

```ini
[transcode]
enable=1
video_codec=h265
audio_codec=opus
on_demand=1
```

3. 启动：

```bash
./MediaServer -c /etc/zlmediakit/config.ini -l 1
```

4. systemd（Linux）：

```ini
[Unit]
Description=ZLMediaKit MediaServer (transcode build)
After=network.target

[Service]
Type=simple
WorkingDirectory=/opt/zlm
Environment="LD_LIBRARY_PATH=/opt/ffmpeg/lib"
ExecStart=/opt/zlm/MediaServer -c /etc/zlmediakit/config.ini -l 1
Restart=on-failure
LimitNOFILE=65535

[Install]
WantedBy=multi-user.target
```

> `LD_LIBRARY_PATH` 或 `ldconfig` 要能找到**编译时那套** ffmpeg 库。

---

## 4. 硬件能力自查（部署后第一件事）

```bash
# 平台支持哪些 hwaccel
ffmpeg -hide_banner -hwaccels

# 有哪些硬件编码器
ffmpeg -hide_banner -encoders | grep -E 'videotoolbox|nvenc|vaapi|qsv'

# 注意：VideoToolbox 没有独立的解码器条目（-decoders 查不到），
#      它只作为 hwaccel 存在，所以解码能力要看 -hwaccels
```

| 平台 | 硬件编码 | 硬件解码 | 按序号选卡 |
|---|---|---|---|
| macOS | `h264/hevc_videotoolbox` | `videotoolbox` hwaccel | 否 |
| Linux + NVIDIA | `h264/hevc_nvenc` | `cuda` hwaccel | **是**（`gpu` 选项） |
| Linux + Intel/AMD | `*_vaapi`（**本仓未接入**） | 未接入 | 否（render node） |
| Windows | `*_nvenc`（未实测） | `cuda`（未实测） | 是 |

配上 `hw_encode=1` 后，如果硬件建不起来会**静默回落软件**并打 Warn ——
所以「配了没报错」不代表用了 GPU，必须看 `getTranscode` 的 `tracks[].hardware`。

---

## 5. 交付前必须跑

```bash
./scripts/final_regression.sh          # 15 套，约 35 分钟，任何一套失败非 0 退出
env -u PYTHONPATH python3 scripts/soak_test.py 30    # 长跑 30 分钟
```

现场（客户机器）再按 `DELIVERY_OPS_RUNBOOK.md` §3 的 8 步验收清单跑一遍 ——
开发机全绿不等于客户机器全绿（依赖版本、GPU 型号、网络拓扑都可能不同）。

---

## 6. 交付时需要如实说明的边界

| 项 | 状态 | 说明 |
|---|---|---|
| 硬件编码 | 已实测（macOS/VideoToolbox） | 720p30→H265：软编 ≈150% CPU → 硬编 ≈6% |
| 硬件解码 | 已实测（可观测） | **单换硬解几乎不省 CPU**；真收益在硬编+硬解组合（7.7%→6.5%） |
| 全 GPU 零拷贝 | **未做** | 硬解后帧仍下载回内存 |
| 多卡分配 | 逻辑已实现，**真分配未在 NVIDIA 机器验证** | 参数面与诚实性已验（10/10） |
| VAAPI | **未接入** | 需 `hw_frames_ctx` + render node |
| Windows | 代码有分支，**未实测** | |
| G722.1 / SVAC | **不支持** | ffmpeg 无实现 |
| 目标 codec == 源 codec | 透传（设计） | 此时缩放/滤镜/帧率都不生效 |

---

## 7. 许可证合规

本仓是 ZLMediaKit 的 fork（MIT-like）。分发二进制时必须：

- 保留 `LICENSE` 与源文件头 `Copyright (c) 2016-present The ZLMediaKit project authors`
- 对外说明产品基于 ZLMediaKit
- 转码实现为本仓新增代码；**未复制商业版（Pro）的任何二进制或代码**，仅参考公开接口与
  公开分支（`feature/transcode2`）的实现思路
