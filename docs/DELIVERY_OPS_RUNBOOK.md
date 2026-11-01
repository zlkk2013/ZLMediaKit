# 转码功能运维手册（现场排障）

面向现场：**现象 → 先查什么 → 原因 → 处理**。每条都来自本项目实测踩过的坑，
不是推测。

---

## 0. 三分钟自检：转码到底有没有在跑

按顺序做，**每一步都能独立否决**，不要跳步：

```bash
S=<secret>; V=__defaultVhost__; A=live; ST=<流名>

# 1) 流在不在线（-500 说明流没推上来，跟转码无关）
curl -s "http://127.0.0.1/index/api/getTranscode?secret=$S&vhost=$V&app=$A&stream=$ST"

# 2) 看 transcoding 与 tracks[].encoder
#    transcoding=false → 参数没生效或没配；encoder 为空 → 没建编码器

# 3) 拉流端看实际编码（唯一可信口径，别看服务端自报）
ffmpeg -v error -rtsp_transport tcp -i rtsp://127.0.0.1/live/$ST -t 5 -c copy -f null -
ffprobe -v error -rtsp_transport tcp -show_entries stream=codec_name,width,height \
        -of json rtsp://127.0.0.1/live/$ST
```

判据：`tracks[].transcoding=true` **且** `frames` 在增长 **且** 拉流端 codec 真的变了。
三个条件缺一，就是「看着在转、观众拿不到」。

---

## 1. 现象速查表

| 现象 | 先查 | 常见原因 | 处理 |
|---|---|---|---|
| 拉流端编码没变，服务端说转码成功 | `getTranscode` 的 `tracks[].transcoding` | **源 codec == 目标 codec**，走透传（设计如此） | 换一个不同的目标 codec 复验 |
| 同上，且配的是 RTSP 推流源 | SDP 里是 `a=control:streamid=` 还是 `trackID=` | `directProxy` 开着，源 RTP 直接透传 | 对该拓扑关 direct-proxy（本仓转码场景已自动关） |
| 服务端 `transcoded N frames` 在涨，观众没画面 | 服务端日志有没有 `warmup produced no packet` | 硬编首帧不吐包 → 目标轨不 ready → muxer 丢掉整条视频轨 | 已在代码里循环喂帧修复；若复现，检查编码器是否真的打开 |
| 转码 CPU 几乎 0 + 拉流 404 | 同上 | 同上（轨被丢弃） | 同上 |
| 配了 h265/opus，拉流端还是 h264/aac | 用的什么协议拉 | **RTMP/FLV 装不下 HEVC 与 Opus** | 用 RTSP 拉 |
| 5 秒内容解出 31.88 秒（慢放） | 目标是 G711 且源采样率 == 目标采样率 | `frame_size==0` 被当成变长帧，ffmpeg 补零到 1024 | 已修（所有音频一律 FIFO 分帧）；同类新增 codec 要注意 |
| MP3 播放 2 倍速 / 时长翻倍 | SDP 有没有 `a=rtpmap:14 MPA/44100/2` | 静态载荷 pt<96 的 RFC 默认时钟与实际不符（MP3 默认 90k、实际 44.1k） | 已修（补显式 rtpmap） |
| G722 播放 2 倍速 | SDP 有没有 `a=rtpmap:9 G722/16000/1` | 同上（默认 8000、实际 16000） | 已修；客户端忽略 rtpmap 的兼容性风险另记 |
| 缩放/滤镜/帧率配了没反应 | 目标 codec 与源是否相同 | 同 codec 透传，压根没建编码器 | 换目标 codec |
| 滤镜生效但画面糊/字看不清 | 滤镜在缩放前还是缩放后 | 滤镜必须在缩放之后（OSD 字号按输出分辨率算） | 已固定在缩放后 |
| logo 没出现 | 坐标表达式 | `x=w-W-20` 把主画面与 logo 的宽高弄反 | 用 `W-w-20`（`W` 主画面、`w` logo） |
| 改了配置行为没变 | 服务是否重启过 | 多数配置只在启动时读 | 重启；或走 API（注意 pending 语义） |
| API 传了参数不生效 | 流当时是否已在线 | 转码参数在轨就绪时生效，在线流只写覆盖、下次推流生效 | 断开重推，或先下发再推流 |
| `hw=1` 但 CPU 没降 | `tracks[].hardware` | 硬件编码器建不起来已回落软件 | 看 Warn 日志；看平台是否支持该 codec 的硬编 |
| `hw_devices` 配了没反应 | `tracks[].hw_device` 是否为 -1 | 该平台无设备索引（macOS/VAAPI） | 正常，只有 NVENC 平台按序号选卡 |
| 摄像机重连后观众画面参数不对 | 是否在 15 秒宽限期内重推 | 沿用了第一次的 SDP | `close_streams?force=1` 后让摄像机重推 |
| 转码后重连报 `406 Not Acceptable` | 服务端 `ANNOUNCE: Already publishing` | 推流端对象没被回收（引用环） | 已修；若复现跑 `rtsp_reconnect_test` |
| 服务没崩但 API 不响应、新观众连不上 | 调 `getServerConfig` 计时 | 自死锁（持锁 flush 编解码器） | 已修；用 `teardown_liveness_test` 复查 |
| 内存/句柄缓慢上涨 | 长跑采样 | 泄漏 vs 线程池一次性扩张 | 见 §4 长跑判据 |

---

## 2. 常用操作

### 下发/修改转码参数

```bash
curl -s "http://127.0.0.1/index/api/addTranscode?secret=$S&vhost=$V&app=live&stream=cam1\
&video_codec=h265&video_fps=15&scale=0.5&hw=1"
```

### 清除 API 覆盖，回落到配置文件

```bash
curl -s "http://127.0.0.1/index/api/addTranscode?secret=$S&vhost=$V&app=live&stream=cam1&reset=1"
```

### 强制关流（换分辨率重连、清理僵尸流）

```bash
curl -s "http://127.0.0.1/index/api/close_streams?secret=$S&vhost=$V&app=live&stream=cam1&force=1"
```

### 确认硬件到底有没有用上

```bash
# 编码：看 tracks[].hardware 与 encoder 名
# 解码：看 tracks[].hw_decode 与 hw_decode_frames（>0 才算真下载过硬件帧）
curl -s "http://127.0.0.1/index/api/getTranscode?secret=$S&vhost=$V&app=live&stream=cam1" \
  | grep -E '"encoder"|"hardware"|"hw_decode"|hw_device'
```

---

## 3. 现场部署后的验收清单

按顺序跑，全部通过才算可交付：

| # | 检查 | 判据 |
|---|---|---|
| 1 | `getServerConfig` 返回 | `code=0` 且耗时 < 3 秒 |
| 2 | 推一路流 + 配转码 | `getTranscode` 的 `transcoding=true`、`frames` 增长 |
| 3 | 拉流端编码 | `ffprobe` 的 `codec_name` == 目标 codec |
| 4 | 拉流端解码错误 | 10 秒拉流 `-c copy` 落盘再解码，硬错误计数 **0** |
| 5 | 抽帧看图 | 画面完整、OSD/角标位置正确 |
| 6 | 断流重推 | 重推后转码仍生效、不报 406 |
| 7 | 停推流 20 秒 | 媒体列表里该流消失（无泄漏） |
| 8 | 硬件（如启用） | `hardware=true` 且进程 CPU 明显低于软编（720p30→H265 软编约 150% vs 硬编约 6%） |

---

## 4. 长跑与泄漏定性

```bash
env -u PYTHONPATH python3 scripts/soak_test.py 20    # 分钟
```

五条判据：服务全程可响应（每轮 <3s）、内存稳态涨幅 <15%、fd 增长 <20、线程增长 ≤2、
停推流并等过宽限期后媒体源清空。

**三种定性**：

| 曲线形状 | 结论 |
|---|---|
| 中途一次性上升，之后一直平 | **池扩张，正常**（实测 308s 时 fd 120→147、线程 29→37，之后 15 分钟不变） |
| 持续单调上升 | 真泄漏，按引用环 / 连接 / 线程类别分别查 |
| 与负载同步起伏、停机即回落 | 环境噪声（宿主在跑别的重负载） |

判据基线要取**中点**、取**中位数**：拿「跑满 20% 处」的单点做基线，会把中途的池扩张判成泄漏。

---

## 5. 一键回归（发版前必跑）

```bash
./scripts/final_regression.sh            # 15 套，约 35 分钟
./scripts/final_regression.sh --fast     # 跳过慢的几套（改代码时高频用）
./scripts/final_regression.sh --list     # 列套件与端口
./scripts/final_regression.sh hw_encode_test multi_gpu_test   # 只跑指定套件
```

脚本会做前置检查：二进制是否比源码新（否则在跑旧二进制）、宿主负载、测试素材是否齐全、
端口是否被本 worktree 的残留进程占用（按二进制绝对路径精确匹配清理，不会误杀别的实例）。

**纪律**：跑全量回归期间**不要改代码、不要重编** —— 二进制被中途替换会得到一堆假红。

---

## 6. 日志关键行

```bash
# 转码是否启动（含目标参数）
grep -i transcode <日志> | grep -iE 'enable|option'

# 每路已编码帧数（判活）
grep 'transcoded .* frames' <日志>

# 硬件编码是否真的走 GPU
grep -E '\(hardware\)|videotoolbox|nvenc' <日志>

# 回落（想用硬件但失败）
grep -iE 'hw .*fail|fallback|回落' <日志>

# 媒体注册/注销（排查「流还在不在」最可靠的是写方日志，不是读方 API）
grep -E '媒体注册|媒体注销' <日志>

# 关键错误
grep -iE 'warmup produced no packet|未找到解码器|filter init failed' <日志>
```

---

## 7. 已知不可用/未验证（交付时必须说明）

| 项 | 状态 |
|---|---|
| G722.1 / SVAC 编解码 | **不支持**（ffmpeg 无实现） |
| 全 GPU 零拷贝 | **未做**：硬解后帧仍下载回内存，单换硬解几乎不省 CPU |
| 多卡真分配 | 代码已实现，**只在 macOS 验过参数面/诚实性**；真 0/1/0 分配需 NVIDIA 机器复验 |
| VAAPI 硬件编解码 | **未接入**（需 `hw_frames_ctx` 与 render node 路径） |
| Windows 平台 | 代码有对应分支（NVENC/cuda），**未实测** |
