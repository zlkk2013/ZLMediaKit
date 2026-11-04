# 分片调度器（shard dispatcher）

单进程的 DVPP VENC 通道上限是 **16 路**，超过就得跑多个 MediaServer 进程（分片）。
分片一多，「这路流该推到哪一片、从哪一片拉」就没人回答了 —— 这个服务只做这件事。

它**不转发媒体流**：推流/拉流仍然直连分片端口，调度器只负责给地址。
（媒体走 TCP 多一跳不仅浪费，还会让 RTCP 与缓冲策略变复杂。）

## 接口

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/assign?stream=<名字>` | 分配分片。**幂等**：同名流永远返回同一片（粘性，不迁移） |
| GET | `/release?stream=<名字>` | 释放分配（流下线时调用） |
| GET | `/shards` | 分片清单 + 实时健康 + 负载 + `stranded_streams` |
| GET | `/healthz` | 调度器自身存活 |
| GET | `/metrics` | Prometheus 纯文本指标 |

`/assign` 成功返回：

```json
{"code": 0, "shard": "a", "http": "http://127.0.0.1:57980", "sticky": false, "reason": "new-stream"}
```

容量满时返回 **HTTP 503**：

```json
{"code": 503, "error": "all shards full", "capacity": 16, "shards": [ ... ]}
```

## 分配策略

1. 只用**健康**且**未满**的分片（每片 `capacity` 默认 16）；
2. 已分配过的流**保持粘性**，只要原分片还健康就绝不迁移 —— 迁移意味着断流重推；
3. 新流按**当前负载最少**分配，同负载按流名哈希稳定打散；
4. 全部满 → 503，由上层决定加片还是拒绝。

## 运维要点

- 分片挂掉后 `assign` 不再选它，但**已分配在它上面的流不会自动迁移**：
  `/shards` 的 `stranded_streams` 会列出来（`stranded_count` 供告警），需要重新推流。
  这是刻意设计 —— 偷偷迁移会让客户端莫名断流且没人知道。
- 状态落盘到 `state_file`，重启不丢粘性；`--gc` 会按分片的 `getMediaList`
  清掉「推流方已断开但没调 release」的陈旧分配。

## 用法

```bash
# 1) 起两个分片（各自一份配置，端口错开）
./release/linux/Release/MediaServer -c /etc/zlm/cfg_asc.ini  -l 1 &
./release/linux/Release/MediaServer -c /etc/zlm/cfg_asc2.ini -l 1 &

# 2) 起调度器
python3 shard_dispatcher.py -c config.json --gc

# 3) 推流前问一次该推哪
curl 'http://127.0.0.1:8090/assign?stream=cam01'
# → {"code":0,"shard":"b","http":"http://127.0.0.1:58980",...}
#   然后把流推到 shard "b" 的 rtsp 端口（58554），拉流也从 58554 拉

# 4) 流下线
curl 'http://127.0.0.1:8090/release?stream=cam01'
```

## 自测

```bash
python3 selftest.py     # 本地起假分片，覆盖均分/容量/粘性/故障/stranded/metrics
```

预期 `8/8 通过`，不需要昇腾硬件。

## 实测容量（720p，硬解+硬编，192.168.210.89）

| 拓扑 | 路数 | 聚合 | 每路 |
|---|---|---|---|
| 单分片 | 16 | 400 fps | 25.0 fps |
| **2 分片 16+8** | **24** | **597 fps** | **24.9 fps** |
| 2 分片 16+16 | 32 | 548 fps | 17.1 fps（NPU 饱和） |

建议按 **每片 12~16 路、总 24 路**留余量。
