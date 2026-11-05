# S3 云存储模块设计

目标：录制的音视频**直接写进 S3/MinIO（全内存流，不落磁盘）**，并能通过 ZLM 自己的 HTTP
服务和点播能力把云上文件放出来。

## 1. 需求对照

| # | 需求 | 设计要点 | 状态 |
|---|---|---|---|
| 1 | s3/minio 内存流直写，解决录像 IO 瓶颈 | 录制侧内存缓冲 + 分片上传（每片 8MB），不做临时文件 | **MP4/HLS 均已完成并真机 E2E（真实 SeaweedFS）** |
| 2 | 通过 ZLM 的 HTTP 服务下载/点播云存储文件 | ZLM HTTP 路由代理对象字节（支持 Range/断点续传），不暴露密钥 | **已完成并真机验证（含 Range/大对象流式）** |
| 3 | 遍历云存储生成 HTTP 菜单页 | `ListObjectsV2` + 静态 HTML 模板（按前缀分组、可点播/下载） | **已完成并真机验证（逐级下钻）** |
| 4 | 读写全内存流、不经磁盘 | 分片上传直接从内存发；读取用 Range 拉到内存，不写临时文件 | **已验证：录制全过程本地零文件** |
| 5 | mp4/hls 录制到云；从云点播 mp4 为 rtsp/rtmp/flv/webrtc | 录制：MP4/HLS 写 S3；点播：对象 → MP4FileMemory → MP4Demuxer → MediaSource | **录制两态已完成**；云点播待开发 |
| 6 | 绝大部分文件都支持云存储读写 | 以「存储后端」抽象收口，MP4/HLS/快照都走同一后端 | 录制侧已收口；快照待做 |

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

同一套用例在昇腾盒子（arm64）对**真实 SeaweedFS S3** 跑也是 11/11，20MB/3 片 350ms（57 MB/s）。
详见第 7 节。

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

## 6. 进度与待办

已完成（都有真机证据，见下节）：

1. ~~`S3Storage` 门面~~（对象命名 + 上传 + exists）
2. ~~MP4 录制直写云~~（内存缓冲 + 整块/分片上传，全程不落盘）
3. ~~HLS 录制直写云~~（切片/init/播放列表全部写对象，全程不落盘）
4. ~~HTTP 出口 `/s3/<key>`~~（Range 断点续传 + 大对象分块流式，不暴露密钥）
5. ~~HTTP 菜单页 `/s3/<prefix>/`~~（列对象、逐级下钻、播放/下载链接）

待做（按依赖顺序）：

6. **云点播**：对象 → `MP4FileMemory` → `MP4Demuxer` → 媒体源，让云上的 mp4 能像点播文件一样
   出 rtsp/rtmp/flv/webrtc（现在只能下载或浏览器直接播）。
7. **快照（snap）写云**：`onPublish`/`onSnap` 的 jpg 也走 `S3Storage`，把需求 6 的「绝大部分文件」补齐。
8. **容量/长跑**：多路并发录制 + 上传失败重试 + 桶满/网络抖动下的行为，与本地录制的 CPU/IO 对照。

### HLS 直写云存储的做法

`HlsMakerImp` 里所有落盘动作只有几个出口：`makeFile()`（切片/init/m3u8）、
`File::saveFile()`（目录级 vod.m3u8）、`File::delete_file()`（滚动删除）。
云存储模式下改为：切片数据攒在内存（`_s3_seg_buf`），`onFlushLastSegment()` 时整片
异步上传（`WorkThreadPool`，不阻塞 muxer 线程）；`onWriteHls()` 每次把播放列表整体覆盖写到
同名对象（观众端永远拿到最新一份）；`init.mp4` 与目录级 `vod.m3u8` 同样写对象。

对象名与本地目录结构一一对应（`<prefix>/<app>/<stream>/<日期>/<小时>/<mm-ss>_<idx>.ts`），
所以 HTTP 菜单页 / 点播的路径拼法与本地完全一致。

## 7. 真机验证（昇腾 arm64 + 真实 SeaweedFS，2026-11-05）

环境：192.168.210.89（openEuler arm64，2×310P3），对象存储用 **SeaweedFS 的 S3 网关**
（`127.0.0.1:19020`，AK/SK 走 SigV4 校验，不是 mock）。验证脚本都在 `tools/s3/`：

| 脚本 | 验什么 |
|---|---|
| `s3_selftest.cpp` | 客户端 11 项（PUT/GET/Range/HEAD/LIST/分片/预签名/删除） |
| `box_s3_record_verify.py` | MP4 录制写云 → 用 S3 协议列对象 + 取回 + ffprobe |
| `box_s3_hls_verify.py` | HLS 录制写云 → 还原整份播放列表 + ffprobe |
| `box_s3_http_verify.py` | `/s3/` 出口 18 项（菜单/下载/Range/流式/HLS/404） |

`s3_selftest` 新增了四个「单功能模式」，专门给录制/回放链路做校验（跑完即退，不建桶不自测）：

```
--list <prefix> [--max N]   一行一个对象：key<TAB>size<TAB>lastModified
--head <key>                只取大小/etag
--get  <key> [--out F]      下载对象
--put  <key> --file F       上传本地文件（>8MB 自动分片）
```

### 7.1 录制写云（MP4 / HLS）

```
startRecord: code=0
--list record/ → 本次流 1 个对象
   record/live/boxs3.../2026-11-05/2026-11-05-16-27-25-0.mp4  289001 字节
取回 → 289001 字节（与对象声明一致）
ffprobe: 时长=12.8s 流=[('aac',None),('h264',1280)]
本地录制目录 www/record 新增文件数: 0
```

```
startRecord(HLS): code=0
--list record/ → 本次流 23 个对象（m3u8 2，ts 12）
  播放列表 hls.fmp4.m3u8 → 引用 8 段，桶内缺失 0 → ffprobe 14.6s h264+aac
  播放列表 hls.m3u8       → 引用 10 段，桶内缺失 0 → ffprobe（按 EXTINF 求和）20.3s
本地 hls 目录新增文件数: 0
```

两个踩过的验证坑（都不是功能问题，是验证方法错）：

- **别翻 SeaweedFS 的数据目录找文件**：对象存在 volume 的 `.dat` 里，文件系统上看不到 `.mp4`。
  必须用 S3 协议 `ListObjectsV2` + `GetObject` 来验，否则会误判「录制失败」。
- **fMP4 单个切片 ffprobe 看不出轨道**：moov 在 `init.mp4` 里。要么验 TS 变体，要么把
  m3u8 + 全部引用段 + `EXT-X-MAP` 一起取下来按原相对路径还原成一份本地播放列表整体验。
  另外滚动窗口下播放列表只有 `segNum` 段，不能拿「时长 ≈ 录制时长」当判据。

### 7.2 HTTP 出口（`/s3/`）

```
  OK   /s3/ 返回菜单页                     200, 1440 字节
  OK   流目录菜单列出日期子目录               200, 2402 字节
  OK   日期目录菜单列出文件名                 200, 1896 字节
  OK   整对象 200 且字节一致                200, 243809 字节, sha=627d2b428fd0
  OK   Content-Length 正确               243809
  OK   Content-Type 是 video/mp4         video/mp4; charset=utf-8
  OK   带 Accept-Ranges
  OK   Range 100-199 → 206 且字节一致      206, 100 字节
  OK   Content-Range 正确                bytes 100-199/243809
  OK   Range -128 → 206 且等于末尾 128 字节
  OK   Range 1000- → 206 且等于尾部
  OK   越界 Range → 416
  OK   6MB 对象流式下载字节一致              200, 6291456 字节（sha 一致）
  OK   6MB 对象中间区间一致                 206, 2500001 字节
  OK   m3u8 200 且非空                    200, 引用 6 段
  OK   m3u8 引用的切片经 /s3/ 可取          3/3 段
  OK   不存在的对象 → 404
结果: 18/18 通过
```

## 8. HTTP 出口的实现要点与坑

实现在 `src/S3/S3HttpService.{h,cpp}`，接线只有一处：
`HttpFileManager::onAccessPath()` 最前面判断 `S3HttpService::match(parser)` 后转交，
**放在 `getFilePath()` 之前** —— 这条路完全不碰本地文件系统，不该走本地目录校验。

传输策略：

- `<= 2MB`：一次 `GetObject` 整块读进内存，`HttpStringBody` 直接回（省一次往返）。
- `> 2MB`：`HttpS3RangeBody` 按 1MB 分块 `Range` 读，`remainSize()` 返回剩余字节数，
  所以 `Content-Length` 是准确的（keep-alive 保持、浏览器进度条准）；每块在工作线程池取，
  既不阻塞 poller，也不会把整个文件读进内存。
- 没有走「大对象 302 到预签名 URL」：现场往往只有 ZLM 的端口对外，S3 在内网另一台机器，
  302 会把浏览器指到一个不可达的地址；而且预签名 URL 会过期、无法按目录授权。

对接时踩到的细节（都是编译期/语义层面的，下次不用重新摸）：

- `HttpFileManager::invoker` 的签名是
  `void(int code, const std::string &content_type, const StrCaseMap &response_header, const HttpBody::Ptr &body)`
  —— 不是 `(code, headers, body)`，参数顺序和个数都要对上。
- `Parser` 是 **`mediakit::Parser`，不是 `toolkit::Parser`**（写 `toolkit::Parser` 编译不过）。
- `Parser` 没有 `path()`；`parser.url()` 就是「不含 query 的路径」，**且 `HttpSession` 在分发前
  已经对它做过 `UrlDecodePath`** —— 在 `/s3/` 里再解一次会把文件名里的 `%` 解两遍。
- `HttpSession::sendResponse()` 会给所有给定了 content-type 的响应用 `; charset=...` 后缀，
  所以 `video/mp4` 会变成 `video/mp4; charset=utf-8`。播放器都忽略它，但要清楚不是笔误。
- `HttpSession` 的 body 发送统一走 `readDataAsync`（`AsyncSender`），实现该虚函数就能做流式；
  只想同步实现 `readData()` 也能跑，但会占住 poller 线程。
- 菜单页的分组按「前缀后第一段」拆子目录，只是把 `ListObjectsV2` 的平铺 key 表现得像目录树；
  一次最多列 1000 个（`--max` 可调），到上限时页面会提示进子目录。
- **鉴权口径**：`/s3/` 与静态文件目录一致 —— ZLM 的 HTTP 端口对谁开放，`/s3/` 就对谁开放。
  需要收紧时用反代或 `http.secret`，不在这一层自造协议。
