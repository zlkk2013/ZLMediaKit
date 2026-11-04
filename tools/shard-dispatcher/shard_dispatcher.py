#!/usr/bin/env python3
"""ZLMediaKit 昇腾分片调度器（shard dispatcher）

背景
----
单进程的 DVPP VENC 通道上限是 16 路，扩容靠多进程分片（每片一套端口）。
一旦分片多了，就没人回答「这路流该推到哪一片、从哪一片拉」这个问题。
本服务只做这一件事：**按流名分配分片**，并顺手做健康检查与容量控制。

它不代理媒体流——推流/拉流仍然直连分片端口，调度器只给地址。
（媒体走 TCP 转发会白白多一跳，还会让 RTCP 时序变复杂。）

接口
----
GET  /assign?stream=<name>     分配（幂等：同名流永远返回同一片）
GET  /release?stream=<name>    释放（流下线时调用）
GET  /shards                   分片清单 + 实时健康 + 当前负载
GET  /healthz                  调度器自身存活
GET  /metrics                  纯文本指标（给监控/巡检脚本用）

分配策略
--------
1. 只考虑**健康**且**未满**（已分配路数 < capacity）的分片；
2. 已分配过的流保持粘性，只要它所在的片还健康就绝不迁移（迁移=断流重推）；
3. 新流按「当前负载最少」分配，同负载用流名哈希稳定打散，避免全压在第一片；
4. 所有分片都满 ⇒ 返回 503 + 明确错误码，让上层决定是加片还是拒绝。

运维要点
--------
- 分片挂掉时，assign 不会再选它；**已分配在它上面的流需要重新推流**（本服务不迁移媒体）。
  `/shards` 里会把这些流标出来（`stranded`），巡检脚本据此告警。
- 状态落盘（state_file），重启不丢粘性；`--gc` 会按分片 `getMediaList` 清掉已下线的流。
"""

import argparse
import hashlib
import json
import os
import sys
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

DEFAULT_CONFIG = {
    "listen": {"host": "0.0.0.0", "port": 8090},
    "capacity": 16,
    "health_interval_sec": 10,
    "shards": [
        # {"name": "a", "http": "http://127.0.0.1:57980", "secret": "..."},
    ],
    "state_file": "/var/lib/zlm-shard/state.json",
}


def _now():
    return int(time.time())


def _hash_index(stream, modulo):
    """流名 → [0, modulo) 的稳定索引：同一流名永远落在同一个桶"""
    h = hashlib.md5(stream.encode("utf-8")).hexdigest()
    return int(h[:8], 16) % modulo


class Shard:
    def __init__(self, spec, capacity):
        self.name = spec["name"]
        self.http = spec["http"].rstrip("/")
        self.secret = spec.get("secret", "")
        self.capacity = int(spec.get("capacity", capacity))
        self.healthy = None          # None = 未知（还没探过）
        self.last_check = 0
        self.last_error = ""
        self.latency_ms = -1

    def probe(self, timeout=3.0):
        """健康检查：能拿到 getServerConfig 且 code=0 才算活"""
        url = "%s/index/api/getServerConfig?secret=%s" % (self.http, self.secret)
        started = time.time()
        try:
            with urllib.request.urlopen(url, timeout=timeout) as resp:
                body = json.loads(resp.read().decode("utf-8", "replace"))
            self.latency_ms = int((time.time() - started) * 1000)
            self.healthy = (body.get("code") == 0)
            if not self.healthy:
                self.last_error = "code=%s" % body.get("code")
        except Exception as exc:  # 网络/超时/解析都归为不健康
            self.healthy = False
            self.last_error = str(exc)[:120]
            self.latency_ms = -1
        self.last_check = _now()
        return self.healthy

    def online_streams(self, timeout=3.0):
        """该分片当前在线的流名集合（GC 用；失败返回 None）"""
        url = "%s/index/api/getMediaList?secret=%s" % (self.http, self.secret)
        try:
            with urllib.request.urlopen(url, timeout=timeout) as resp:
                body = json.loads(resp.read().decode("utf-8", "replace"))
        except Exception:
            return None
        names = set()
        for item in body.get("data") or []:
            if item.get("schema") == "rtsp" and item.get("stream"):
                names.add(item["stream"])
        return names


class Dispatcher:
    def __init__(self, cfg):
        self.cfg = cfg
        self.capacity = int(cfg.get("capacity", 16))
        self.shards = [Shard(s, self.capacity) for s in cfg.get("shards", [])]
        self.state_file = cfg.get("state_file") or ""
        self.lock = threading.Lock()
        # stream -> {"shard": name, "since": ts}
        self.assignments = {}
        self._load_state()

    # ---------- 状态持久化 ----------

    def _load_state(self):
        if not self.state_file or not os.path.exists(self.state_file):
            return
        try:
            with open(self.state_file, "r", encoding="utf-8") as fp:
                data = json.load(fp)
            self.assignments = data.get("assignments", {})
        except Exception as exc:
            print("[dispatcher] 读取状态失败（忽略）: %s" % exc, file=sys.stderr)

    def _save_state_locked(self):
        if not self.state_file:
            return
        try:
            os.makedirs(os.path.dirname(self.state_file), exist_ok=True)
            tmp = self.state_file + ".tmp"
            with open(tmp, "w", encoding="utf-8") as fp:
                json.dump({"assignments": self.assignments}, fp)
            os.replace(tmp, self.state_file)
        except Exception as exc:
            print("[dispatcher] 写状态失败（不影响运行）: %s" % exc, file=sys.stderr)

    # ---------- 核心：分配 ----------

    def _by_name(self, name):
        for s in self.shards:
            if s.name == name:
                return s
        return None

    def _loads(self):
        loads = {s.name: 0 for s in self.shards}
        for info in self.assignments.values():
            if info["shard"] in loads:
                loads[info["shard"]] += 1
        return loads

    def assign(self, stream):
        with self.lock:
            info = self.assignments.get(stream)
            if info:
                shard = self._by_name(info["shard"])
                # 粘性：原分片还健康就绝不迁移（迁移意味着断流重推）
                if shard and shard.healthy is not False:
                    return {"code": 0, "shard": shard.name, "http": shard.http,
                            "sticky": True, "reason": "already-assigned"}
                # 原分片判定不健康 → 重新分配，并告知调用方需要重推
                if shard:
                    self.assignments.pop(stream, None)
                    self._save_state_locked()
                    reselect_reason = "previous-shard-unhealthy"
                else:
                    reselect_reason = "previous-shard-removed"
            else:
                reselect_reason = "new-stream"

            loads = self._loads()
            candidates = [s for s in self.shards
                          if s.healthy is not False and loads[s.name] < s.capacity]
            if not candidates:
                healthy_any = any(s.healthy is not False for s in self.shards)
                if not healthy_any:
                    return {"code": 503, "error": "no healthy shard"}
                return {"code": 503, "error": "all shards full",
                        "capacity": self.capacity,
                        "shards": [{"name": s.name, "load": loads[s.name],
                                    "capacity": s.capacity} for s in self.shards]}
            # 负载最少优先；同负载按流名哈希稳定打散
            min_load = min(loads[s.name] for s in candidates)
            tied = sorted([s for s in candidates if loads[s.name] == min_load],
                          key=lambda s: s.name)
            chosen = tied[_hash_index(stream, len(tied))]
            self.assignments[stream] = {"shard": chosen.name, "since": _now()}
            self._save_state_locked()
            return {"code": 0, "shard": chosen.name, "http": chosen.http,
                    "sticky": False, "reason": reselect_reason}

    def release(self, stream):
        with self.lock:
            removed = self.assignments.pop(stream, None)
            if removed:
                self._save_state_locked()
            return {"code": 0, "released": bool(removed)}

    # ---------- 巡检 ----------

    def probe_all(self):
        for s in self.shards:
            s.probe()

    def snapshot(self):
        with self.lock:
            loads = self._loads()
            stranded = []
            for stream, info in self.assignments.items():
                shard = self._by_name(info["shard"])
                if shard and shard.healthy is False:
                    stranded.append(stream)
            return {
                "code": 0,
                "capacity_per_shard": self.capacity,
                "assigned_total": len(self.assignments),
                "shards": [{
                    "name": s.name, "http": s.http,
                    "healthy": s.healthy, "latency_ms": s.latency_ms,
                    "load": loads[s.name], "capacity": s.capacity,
                    "last_error": s.last_error,
                } for s in self.shards],
                "stranded_streams": stranded[:50],
                "stranded_count": len(stranded),
            }

    def gc(self):
        """清掉分片上已经不存在的流（推流方断开后没调 release 的情况）"""
        dropped = 0
        with self.lock:
            snapshot = dict(self.assignments)
        for name in {info["shard"] for info in snapshot.values()}:
            shard = self._by_name(name)
            if not shard:
                continue
            online = shard.online_streams()
            if online is None:
                continue
            with self.lock:
                for stream, info in list(self.assignments.items()):
                    if info["shard"] == name and stream not in online:
                        self.assignments.pop(stream, None)
                        dropped += 1
                if dropped:
                    self._save_state_locked()
        return dropped

    def metrics_text(self):
        snap = self.snapshot()
        lines = [
            "# TYPE zlm_shard_assigned_total gauge",
            "zlm_shard_assigned_total %d" % snap["assigned_total"],
            "# TYPE zlm_shard_stranded_total gauge",
            "zlm_shard_stranded_total %d" % snap["stranded_count"],
        ]
        for s in snap["shards"]:
            healthy = 1 if s["healthy"] else 0
            lines.append('zlm_shard_healthy{shard="%s"} %d' % (s["name"], healthy))
            lines.append('zlm_shard_load{shard="%s"} %d' % (s["name"], s["load"]))
            lines.append('zlm_shard_capacity{shard="%s"} %d' % (s["name"], s["capacity"]))
        return "\n".join(lines) + "\n"


class Handler(BaseHTTPRequestHandler):
    # 运行时由 main() 注入；置 None 只是给类型检查器一个初值
    dispatcher: "Dispatcher" = None  # type: ignore[assignment]

    def log_message(self, format, *args):  # noqa: A002 - 与基类签名保持一致
        # 默认会往 stderr 打每条请求；巡检脚本会高频访问，降噪但保留错误
        if "/healthz" in (args[0] if args else ""):
            return
        print("[dispatcher] %s - %s" % (self.address_string(), format % args), file=sys.stderr)

    def _send(self, obj, code=200):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parsed = urlparse(self.path)
        qs = parse_qs(parsed.query)
        stream = (qs.get("stream") or [""])[0].strip()

        if parsed.path == "/healthz":
            self._send({"code": 0, "uptime_sec": _now() - START_TS})
        elif parsed.path == "/assign":
            if not stream:
                self._send({"code": 400, "error": "missing stream"}, 400)
                return
            res = self.dispatcher.assign(stream)
            self._send(res, 200 if res.get("code") == 0 else 503)
        elif parsed.path == "/release":
            if not stream:
                self._send({"code": 400, "error": "missing stream"}, 400)
                return
            self._send(self.dispatcher.release(stream))
        elif parsed.path == "/shards":
            snap = self.dispatcher.snapshot()
            self._send(snap)
        elif parsed.path == "/metrics":
            body = self.dispatcher.metrics_text().encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; version=0.0.4")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self._send({"code": 404, "error": "unknown path"}, 404)


def _health_loop(dispatcher, interval):
    while True:
        try:
            dispatcher.probe_all()
        except Exception as exc:  # 巡检线程不能死
            print("[dispatcher] 巡检异常: %s" % exc, file=sys.stderr)
        time.sleep(max(1, interval))


START_TS = _now()


def main():
    ap = argparse.ArgumentParser(description="ZLMediaKit 昇腾分片调度器")
    ap.add_argument("-c", "--config", required=True, help="配置文件（JSON）")
    ap.add_argument("--gc", action="store_true", help="启动前按分片在线流清理陈旧分配")
    args = ap.parse_args()

    with open(args.config, "r", encoding="utf-8") as fp:
        cfg = json.load(fp)
    for k, v in DEFAULT_CONFIG.items():
        cfg.setdefault(k, v)
    if not cfg.get("shards"):
        print("配置里没有 shards", file=sys.stderr)
        return 2

    dispatcher = Dispatcher(cfg)
    dispatcher.probe_all()
    if args.gc:
        dropped = dispatcher.gc()
        print("[dispatcher] GC 清掉 %d 条陈旧分配" % dropped, file=sys.stderr)

    threading.Thread(target=_health_loop,
                     args=(dispatcher, int(cfg.get("health_interval_sec", 10))),
                     daemon=True).start()

    Handler.dispatcher = dispatcher
    host = cfg["listen"].get("host", "0.0.0.0")
    port = int(cfg["listen"].get("port", 8090))
    srv = ThreadingHTTPServer((host, port), Handler)
    print("[dispatcher] listening on %s:%d, shards=%s" %
          (host, port, [s.name for s in dispatcher.shards]), file=sys.stderr)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
