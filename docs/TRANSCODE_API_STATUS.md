
## 运行态查询接口（2026-10-29 增）

`getTranscode` 现在除了配置参数，还返回**运行态**——这是运维确认「硬件/自适应到底生效没有」的唯一口径：

```json
{
  "code": 0, "app": "live", "stream": "cam1", "transcoding": true, "frames": 507,
  "tracks": [{
    "type": "video", "source_codec": "H264", "target_codec": "H265",
    "encoder": "hevc_videotoolbox",   // 实际用的编码器
    "hardware": true,                 // 是否硬件编码
    "adapt_fps": 9.83,                // 负载自适应当前帧率（0=未启用）
    "adapt_dropped": 210,             // 因过载丢弃的帧数
    "frames": 507
  }]
}
```

为什么需要它：`hw_encode=1` 只表示「希望用硬件」，实际可能回落到软件；
`adapt_fps=1` 也只表示「允许降帧」。没有运行态字段，这两项在生产里都是黑盒 ——
客户问「到底用没用 GPU」，只能翻日志。

验收里已经断言**接口和实际必须一致**：硬编时 `hardware=true` 且 `encoder` 是
`*_videotoolbox/*_nvenc`；软编时 `hardware=false`；降帧时 `adapt_fps` 低于目标、
`adapt_dropped > 0`。

## 测试素材生成器（2026-10-29 增）

`scripts/test_assets.py`：幂等生成 `/tmp` 下的全部验收素材
（`av_test.mp4`、`src_h265.mp4`、`src_1280x720.mp4`、`src_1920x1080.mp4`、
`src_g711a.flv`、`src_mp3.flv`）。

为什么需要：这些素材被所有套件共用，而 `/tmp` 会按空闲时间被回收。素材一没，
套件要么直接报「缺素材」退出，要么报更难看的 ffmpeg 错误 —— 看起来像产品坏了。
实测 `hw_encode_test` 就整轮因为 `/tmp/src_1280x720.mp4` 消失而退出。

`final_regression.sh` 的前置检查会自动调用它补齐，不再依赖「机器上碰巧还留着」。
