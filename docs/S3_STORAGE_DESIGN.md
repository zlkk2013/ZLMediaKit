# S3 云存储模块设计

目标：录制的音视频**直接写进 S3/MinIO（全内存流，不落磁盘）**，并能通过 ZLM 自己的 HTTP
服务和点播能力把云上文件放出来。

## 1. 需求对照

| # | 需求 | 设计要点 | 状态 |
|---|---|---|---|
| 1 | s3/minio 内存流直写，解决录像 IO 瓶颈 | 录制侧内存缓冲 + 分片上传（每片 8MB），不做临时文件 | **MP4 已完成并真机 E2E**；HLS 待做 |
| 2 | 通过 ZLM 的 HTTP 服务下载/点播云存储文件 | ZLM HTTP 路由代理对象字节（支持 Range/断点续传），不暴露密钥 | 待开发 |
| 3 | 遍历云存储生成 HTTP 菜单页 | `ListObjectsV2` + 静态 HTML 模板（按前缀分组、可点播/下载） | 待开发 |
| 4 | 读写全内存流、不经磁盘 | 分片上传直接从内存发；读取用 Range 拉到内存，不写临时文件 | **已验证：录制全过程本地零文件** |
| 5 | mp4/hls 录制到云；从云点播 mp4 为 rtsp/rtmp/flv/webrtc | 录制：MP4/HLS 写 S3；点播：对象 → MP4FileMemory → MP4Demuxer → MediaSource | **录制两态已完成**；云点播待开发 |
| 6 | 绝大部分文件都支持云存储读写 | 以「存储后端」抽象收口，MP4/HLS/快照都走同一后端 | 设计已定 |

## 2. 分层

```
┌───────────────────────────────────────────────┐
│ 录制/回放（MP4Recorder、HlsMakerImp、点播源）   │  ← 只认「存储后端」接口
├───────────────────────────────────────────────┤
│ S3Storage：对象命名、分片、重试、目录遍历        │
├───────────────────────────────────────────────┤
│ S3Client：SigV4 签名 + HTTP/1.1（http/https）   │  ← 已完成并自测
├───────────────────────────────────────────────┤
│ S3Crypto：SHA-256 / HMAC-SHA256                │  ← 已用官方向量验证
└───────────────────────────────────────────────┘
```

### 为什么自己实现客户端

aws-sdk-cpp 编译产物数百 MB，离线/信创交付很难塞进去；我们只需要 PUT/分片/GET 范围读/LIST/HEAD/DELETE
六个操作，SigV4 + HTTP/1.1 约千行可控代码。TLS 复用项目已有的 OpenSSL（`ENABLE_OPENSSL`），
未启用时 https 端点会**明确报错**，不会静默降级成明文。

### 对象命名

```
<root_prefix>/<app>/<stream>/<日期>/<文件名>
例：zlm/record/live/cam01/2026-11-04/2026-11-04-10-00-00-0.mp4
```

与本地录制的目录结构保持一致：现场把「本地目录」换成「桶内前缀」即可，回放/菜单页不用改口径。

## 3. 配置

| 键 | 说明 |
|---|---|
| `s3.enable` | 总开关，0 时行为与本模块引入前完全一致 |
| `s3.endpoint` | `http://127.0.0.1:9000` / `https://s3.cn-north-1.amazonaws.com.cn` |
| `s3.region` | 默认 us-east-1 |
| `s3.bucket` | 桶名 |
| `s3.access_key` / `s3.secret_key` / `s3.session_token` | 凭据（session_token 支持 STS 临时凭据） |
| `s3.path_style` | MinIO 必须 1；AWS 两种都行 |
| `s3.root_prefix` | 对象名前缀 |
| `s3.record_mp4` / `s3.record_hls` | 是否把录制直接写云（不落盘） |
| `s3.timeout_ms` | 单次请求超时，默认 15000 |

配置优先级：运行时 API 下发 > 配置文件。换桶/换密钥不必重启（重启会中断录制）。

## 4. 已完成并验证

- `src/S3/S3Crypto.*`：SHA-256 / HMAC-SHA256，**对照官方测试向量全部通过**
  （SHA-256 空串/abc/448bit、HMAC RFC4231 case 1/2/6）。
- `src/S3/S3Client.*`：SigV4 签名、PUT、分片 PUT、GET、Range GET、HEAD、LIST v2、DELETE、
  预签名 URL、建桶。https 走 OpenSSL（未启用时明确报错）。
- `tools/s3/s3_selftest.cpp`：11 项端到端自测（含 20MB/3 片分片上传与字节比对）。
- `tools/s3/s3_mock_server.py`：**独立实现一遍 SigV4 校验**的 S3 模拟服务，
  用它交叉验证 C++ 端签名（不是自说自话：两边各自实现同一份规范）。
- `src/S3/S3Config.*` + `config.cpp` 默认值：配置接入与运行时下发接口。

自测实测（macOS，本机 mock）：

```
OK createBucket（幂等）      OK putObject            OK getObject 内容一致
OK headObject 大小正确       OK getObjectRange       OK listObjects 含刚写的 key
OK putObjectMultipart 20MB/3 片  169ms (118 MB/s)
OK 分片对象回读字节一致      OK deleteObject         OK 删除后 HEAD 应失败
```

## 5. MP4 录制直写云存储（已完成）

接线点：`MP4Muxer` 增加 `openMP4(MP4FileIO::Ptr)` 重载，`MP4Recorder` 在
`S3Storage::enabledForMp4()` 为真时注入 `MP4FileMemory`（内存 + seek，MP4 需要回填
moov/mdat 大小），`closeFile()` 的后台线程里整块上传（<8MB 单次 PUT，超过走分片），
成功后 `file_path` 记为 `s3://<bucket>/<key>` 并照常触发录制事件。

**端到端实测（macOS + 模拟 S3）**：

```
startRecord: code=0
桶内对象: record/live/s3rec.../2026-11-05/2026-11-05-15-23-36-0.mp4
回读大小: 4,398,239 字节 → ffprobe: 时长 13.1s，h264 1280x720 + aac
本地录制目录文件数: 0        ← 全程不落盘
```

对照组（`s3.enable=0`）：本地生成 5.5s / 1.8MB 的 mp4，h264+aac 可播放
—— 原有本地录制路径未受影响。

## 6. 待办（按依赖顺序）

1. ~~`S3Storage` 门面~~ **已完成**（对象命名 + 上传 + exists）。
2. ~~MP4 录制直写云~~ **已完成并验证**。
3. ~~HLS 录制直写云~~ **已完成并验证**。

### HLS 直写云存储的做法

`HlsMakerImp` 里所有落盘动作只有几个出口：`makeFile()`（切片/init/m3u8）、
`File::saveFile()`（目录级 vod.m3u8）、`File::delete_file()`（滚动删除）。
云存储模式下改为：切片数据攒在内存（`_s3_seg_buf`），`onFlushLastSegment()` 时整片
异步上传（`WorkThreadPool`，不阻塞 muxer 线程）；`onWriteHls()` 每次把播放列表整体覆盖写到
同名对象（观众端永远拿到最新一份）；`init.mp4` 与目录级 `vod.m3u8` 同样写对象。

对象名与本地目录结构一一对应（`<prefix>/<app>/<stream>/<日期>/<小时>/<mm-ss>_<idx>.ts`），
所以后面做「云存储 HTTP 菜单页 / 点播」时，路径拼法与本地完全一致。

实测（macOS + 模拟 S3，HLS 录制 16s）：

```
startRecord(HLS): 0
对象数: 15   hls/live/hlss.../2026-11-05/15/37-28_0.ts ... index.m3u8
m3u8 片段数: 4
首个切片: h264 640x360 + aac，218KB
本地 hls 文件数: 0        ← 不落盘
```
4. HTTP 下载/播放：`/s3/<key>` 路由代理字节（Range、Content-Type、下载文件名）。
5. 菜单页：按前缀列对象生成 HTML（尺寸/时间/时长、点播/下载链接）。
6. 云点播：对象 → `MP4FileMemory` → `MP4Demuxer` → 媒体源（rtsp/rtmp/flv/webrtc）。
7. 真机（昇腾 arm64 + 真实 MinIO/S3）复验 + 容量/长跑。

## 7. HTTP 出口的接续笔记（下次从这里开始）

尝试过在 `HttpFileManager::onAccessPath()` 顶部挂 `/s3/` 路由，本轮未完成，已回退（仓库保持可编译）。
踩到的对接细节（省得下次重新摸）：

- `HttpFileManager::invoker` 的实际签名是
  `void(int code, const std::string &content_type, const StrCaseMap &responseHeader, const HttpBody::Ptr &body)`，
  不是 `(code, headers, body)` —— 顺序与参数个数都要对上，用 `HttpStringBody` 承载内存响应。
- `Parser` **没有 `path()`**：要自己从 `parser.url()`/`fullUrl()` 里截掉 `?query` 再判断前缀。
- `HttpFileManager.cpp` 不 include 自己的头文件，S3 的 include 要加在该文件顶部（已确认位置）。
- 大对象不要塞进 poller 线程：>8MB 且无 Range 时直接 302 到 `presignGet()`。
- 菜单页用 `listObjects(prefix)` + 简单 HTML 表格即可（按对象名分组、给出 `/s3/<key>` 链接）。
