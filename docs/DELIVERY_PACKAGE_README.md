# 转码版 MediaServer 交付包说明

本包是 ZLMediaKit 的转码版本（新增音视频转码能力），二进制 + 配置模板 + 安装/自检脚本 + 文档。

## 包内容

```
bin/MediaServer              服务端二进制（Linux x86_64）
config/config.ini            服务端完整配置（含转码段的默认值，默认关闭）
config/TRANSCODE_SNIPPET.ini 转码段示例，可直接粘到 config.ini
install.sh                   安装脚本（铺文件 + 依赖检查 + systemd）
check.sh                     装完自检（API / 编码器 / 真实转码冒烟）
systemd/                     服务单元模板
docs/                        配置参考、运维手册、部署说明
SHA256SUMS                   校验和
```

## 三步装好

```bash
tar -xzf zlm-transcode-*.tar.gz && cd zlm-transcode-*
sha256sum -c SHA256SUMS          # 先验完整性
sudo ./install.sh                # 默认 /opt/zlm + /etc/zlmediakit
```

然后把 `config/TRANSCODE_SNIPPET.ini` 的内容粘到 `/etc/zlmediakit/config.ini`，按需改
`video_codec` / `audio_codec`，再：

```bash
sudo systemctl start zlmediakit-transcode
./check.sh --config /etc/zlmediakit/config.ini --push /path/to/test.mp4
```

`check.sh` 会打印三条判据：API 是否响应（并量耗时）、本机编码器是否齐、真实转码后拉流
codec 是否真的变了。**三条全绿才算装好**（通过时退出码 0）。

> ⚠️ **secret 有个坑**：ZLM 首次启动时若发现 secret 仍是打包默认值，会**自动改写成随机值**并写回
> 配置文件。所以「启动前读到的 secret」启动后就失效了（表现为 `Please login first`）。
> `install.sh` 已在安装时直接生成随机 secret 并打印出来，避免这个问题；`check.sh` 用
> `--config` 从文件现读，比手抄 secret 可靠。

## 依赖

服务端运行需要系统里的 ffmpeg 动态库：

```bash
# Ubuntu / Debian（22.04 的 4.4 与 24.04 的 6.x 都已验证可编译运行）
sudo apt install -y libavcodec-dev libavutil-dev libavfilter-dev libavformat-dev \
                    libswscale-dev libswresample-dev
```

- **软件编码**用系统 ffmpeg 即可。
- **硬件编码**需要带 NVENC 的 ffmpeg（Ubuntu 的包**不带**）。用 NVIDIA 卡做硬件编码时请装
  conda-forge 的 `ffmpeg=7.*`（含 `h264_nvenc`/`hevc_nvenc`/`av1_nvenc`），并让
  `ldd bin/MediaServer` 指向那一套库。

## 端口与安全

默认 RTSP 554 / RTMP 1935 / HTTP 80。**务必改 `secret`**（`config.ini` 里 `[api] secret`），
否则任何人都能调管理接口。

## 现场踩坑清单（本包已在真机 E2E 跑通，这些是不看就会重复踩的）

| 现象 | 原因 | 处理 |
|---|---|---|
| `Please login first` | ① secret 不对 ② 用了启动前的旧 secret（服务会自动改写默认 secret） | 用 `--config` 现读 secret |
| 改了端口没生效 | ZLM 配置是**分段式**（`[rtsp]`/`[rtmp]`/`[http]` 各自的 `port=`），不是 `rtsp.port=xx` | 改对应段里的 `port=` |
| 明明装了编码器 `check.sh` 却说缺 | 脚本里 `ffmpeg … \| grep -q` 在 `set -o pipefail` 下会被 SIGPIPE(141) 判成失败 | 本包已修（改为先取变量再匹配） |
| 改了配置重启后还是老行为 | 旧进程没被杀（`pkill -f 绝对路径` 匹配不上 `./bin/MediaServer`） | 用 `fuser -k <端口>/tcp` 或 `systemctl stop` |
| `config.ini` 读到值带 `^M` | 文件是 CRLF（Windows 编辑过） | 本包脚本都会 `tr -d '\r'`，自己写脚本要注意 |

## 已知边界（交付时必须说明）

| 项 | 状态 |
|---|---|
| G722.1 / SVAC | **不支持**（ffmpeg 无实现） |
| 全 GPU 零拷贝 | **未做**：硬解后帧仍下载回内存，单换硬解不省 CPU，收益在「硬编+硬解」组合 |
| 多卡分配 | 代码已实现，单卡机器实测通过；**多卡真分配未验证** |
| VAAPI（Intel/AMD 核显硬编） | **未接入** |
| Windows 原生 | 未验证（本包是 Linux） |
| 目标 codec 与源相同 | 走透传（不建编码器），此时缩放/滤镜/帧率不生效 —— 设计如此 |

## 容量参考（实测，非承诺）

RTX 5060 Ti + 32 核 + 5.8GB 内存（WSL）：**12 路 1280x720@30 满帧**，CPU 2.1 核、
GPU 编码器 42%、内存 2.6GB。16 路时瓶颈转为推流端喂不满，转码侧仍有余量。
**这是 WSL 环境的数字，裸机 Linux 需另行实测。**

## 排障

先看 `docs/DELIVERY_OPS_RUNBOOK.md` 的「现象速查表」——18 条常见现象对应「查什么 / 原因 / 处理」。
最常踩的三个：

1. **拉流端 codec 没变** → 目标 codec 与源相同（透传），或用了 RTMP 拉（RTMP 装不下 HEVC）。
2. **改了配置没生效** → 转码参数在「轨就绪时」生效，给在线流下发只对下次推流有效；改 ini 必须重启。
3. **`hw=1` 但 CPU 没降** → 硬件编码器建不起来已回落软件，看 `getTranscode` 的
   `tracks[].hardware` 与日志里的回落 Warn。

## 合规

本包基于 ZLMediaKit（MIT-like 许可），保留了 `LICENSE` 与源文件头版权声明。
转码实现为本项目新增代码，未使用商业版（Pro）的任何二进制或代码。
