# 上游改动量评估（rebase 可维护性）

目标：长期跟随上游 `master`，因此希望 `git diff upstream/master --diff-filter=M` 尽量小
（最初定的健康值是 ~150 行）。本文记录现状、每一项的**必要性依据**，以及为什么 150 行这个
目标在当前功能集下不可达 —— 避免以后有人为了凑数字做没收益的搬运。

## 现状（2026-11-01）

```
18 files changed, 707 insertions(+), 19 deletions(-)
```

| 文件 | 新增行 | 为什么必须改上游 |
|---|---|---|
| `src/Common/MultiMediaSourceMuxer.cpp` | +139 | 转码接入点本体：`onTrackReady` 换轨、按需判断、muxer 侧源创建、`releaseMediaSources()` |
| `src/Common/MultiMediaSourceMuxer.h` | +61 | 上述接口声明 + 转码节点持有 |
| `src/Codec/Transcode.cpp` | +105 | 硬件解码：`hw_device_ctx` + `get_format` 回调 + 硬件帧下载 |
| `src/Rtsp/RtspMediaSource.cpp/.h` | +60/+62/+39 | 推流端弱引用回找、`releaseFromPool()`、重推接管（a6 修 406 的三层根因） |
| `src/Common/config.cpp` | +42 | 转码键的默认值登记（键定义已搬进 `src/Transcode/TranscodeConfig.cpp`） |
| `src/Common/PacketCache.h` | +36 | 并发写环形缓冲的锁（1/3 概率 SIGSEGV 的真因） |
| `src/Rtsp/RtspSession.cpp` | +31 | ANNOUNCE 弱引用回落 + `getOwnership()` 接管 |
| `src/Rtsp/Rtsp.cpp` | +29 | MP3(pt=14)/G722(pt=9) 的显式 rtpmap（否则慢放/快放） |
| `src/Common/MediaSource.h` | +22 | `releaseFromPool()` 公开入口（打破引用环） |
| `src/Codec/Transcode.h` | +20 | 解码器硬件参数与状态 getter |
| `server/WebApi.cpp` | +18 | 两个 API 的注册钩子 |
| `ext-codec/VP8Rtp.cpp` | +13 | VP8/VP9 作源的时间轴修正 |
| `CMakeLists.txt` | +12 | 链接 `libavformat`（logo 解码）与 `libavfilter` |
| `src/Rtsp/RtspMediaSourceMuxer.h` | +9 | SDP 刷新钩子 |
| `src/Common/config.h` | +2 | 保留的少量声明 |
| `.gitignore` | +26 | 测试配置不入库（非代码） |

## 已经做过的收敛

- **23 个配置键的定义**从 `config.h`/`config.cpp` 搬到 `src/Transcode/TranscodeConfig.cpp`
  （全仓只有那一处引用）。`config.cpp` 从 +52 降到 +42、`config.h` 从 +27 降到 +2，共省约 35 行。
- API 实现整体在 `src/Transcode/`，`server/WebApi.cpp` 只留注册钩子（+18）。

## 为什么 150 行不可达

剩下的改动几乎全是**「必须改在既有函数体内部」的接缝**，不是可以外提的逻辑：

1. **换轨点**：只有 `MultiMediaSourceMuxer::onTrackReady` 内部能拿到「即将注册的轨」，
   在外部拿不到，也就无法在 `src/Transcode/` 里完成换轨。
2. **并发锁**：`PacketCache` 的写入点在头文件模板里，锁必须加在写入处。
3. **rtpmap**：SDP 拼装是 `Rtsp.cpp` 的 inline 逻辑，只能就地补。
4. **硬件解码**：`avcodec_open2` 之前的上下文装配在 `FFmpegDecoder` 构造函数内。

把这些强行搬到新文件，只能改成「上游函数里多一次回调」—— 行数下降有限（估计能到 550~600），
但会显著提高理解成本和出错概率，**收益不成比例**。

## 结论与建议

- 把「150 行」改成**「按文件记录必要性，任何新增都要能说清为什么不能外提」**；
  当前 17 个文件每一项都有明确依据（见上表），这比追一个数字更有意义。
- 若将来确实要压低，唯一值得做的一项是把 `MultiMediaSourceMuxer` 的转码胶水抽成
  `src/Transcode/MuxerHook.{h,cpp}` + 上游里 3~4 处一行调用，预计省 120~150 行；
  代价是一次回归（约 30 分钟）与一次出错风险。建议放在功能冻结之后做，而不是在功能开发期。
- **每次 rebase 前先跑一遍全量回归**（`scripts/final_regression.sh`，13 套 30 分钟），
  这是判断「上游更新有没有被我们的改动破坏」的唯一可靠手段。
