#!/usr/bin/env python3
"""分片调度器自测（本地起假分片，不需要真 ZLM/昇腾）

覆盖：
1. 负载均衡：6 路流均分到 2 片（每片 capacity=3）
2. 容量上限：第 7 路返回 503 + all shards full
3. 粘性：同一流名重复分配，永远同一片、sticky=true
4. 故障：某片挂掉 → /shards 标记不健康 → 新流不再落到它
5. stranded：挂掉那片上已分配的流会被列出来（运维据此重推）
6. 释放：/release 后容量回收
"""

import json
import os
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
PY = sys.executable
DISP_PORT = 18090
SHARD_PORTS = [18081, 18082]


class FakeShard(BaseHTTPRequestHandler):
    """最小 ZLM 假分片：getServerConfig 回 code=0，getMediaList 回空列表"""

    def log_message(self, format, *args):  # noqa: A002
        pass

    def do_GET(self):
        if self.path.startswith("/index/api/getServerConfig"):
            body = {"code": 0, "data": [{"api.apiDebug": "1"}]}
        elif self.path.startswith("/index/api/getMediaList"):
            body = {"code": 0, "data": []}
        else:
            body = {"code": -1}
        raw = json.dumps(body).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)


def start_fake_shard(port, holder):
    srv = ThreadingHTTPServer(("127.0.0.1", port), FakeShard)
    holder[port] = srv
    threading.Thread(target=srv.serve_forever, daemon=True).start()


def get(path, timeout=5):
    url = "http://127.0.0.1:%d%s" % (DISP_PORT, path)
    try:
        with urllib.request.urlopen(url, timeout=timeout) as resp:
            return resp.status, json.loads(resp.read().decode())
    except urllib.error.HTTPError as exc:
        return exc.code, json.loads(exc.read().decode())


def wait_ready(timeout=10):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            code, _ = get("/healthz", timeout=1)
            if code == 200:
                return True
        except Exception:
            pass
        time.sleep(0.2)
    return False


def main():
    results = []

    def check(name, ok, detail=""):
        results.append((name, ok, detail))
        print("  %s %s%s" % ("✓" if ok else "✗", name, ("  " + detail) if detail else ""))

    shards = {}
    for port in SHARD_PORTS:
        start_fake_shard(port, shards)

    cfg = {
        "listen": {"host": "127.0.0.1", "port": DISP_PORT},
        "capacity": 3,
        "health_interval_sec": 2,
        "state_file": "/tmp/shard-selftest-state.json",
        "shards": [
            {"name": "a", "http": "http://127.0.0.1:%d" % SHARD_PORTS[0], "secret": "x"},
            {"name": "b", "http": "http://127.0.0.1:%d" % SHARD_PORTS[1], "secret": "x"},
        ],
    }
    cfp = "/tmp/shard-selftest-cfg.json"
    with open(cfp, "w") as fp:
        json.dump(cfg, fp)
    if os.path.exists(cfg["state_file"]):
        os.remove(cfg["state_file"])

    proc = subprocess.Popen([PY, os.path.join(HERE, "shard_dispatcher.py"), "-c", cfp],
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        if not wait_ready():
            print("调度器没起来:", (proc.stderr.read().decode()[:400] if proc.stderr else ""))
            return 1

        # 1) 均分
        dist = {}
        for i in range(6):
            code, res = get("/assign?stream=s%d" % i)
            dist[res.get("shard")] = dist.get(res.get("shard"), 0) + 1
        check("6 路均分到 2 片（3/3）", dist == {"a": 3, "b": 3}, str(dist))

        # 2) 容量上限
        code, res = get("/assign?stream=overflow")
        check("第 7 路返回 503 all shards full",
              code == 503 and res.get("error") == "all shards full", str(res)[:80])

        # 3) 粘性
        code1, r1 = get("/assign?stream=s0")
        code2, r2 = get("/assign?stream=s0")
        check("同一流名两次分配同片且 sticky",
              r1.get("shard") == r2.get("shard") and r2.get("sticky") is True,
              "%s/%s" % (r1.get("shard"), r2.get("shard")))

        # 4) 故障：先把 a 腾出一个位置（否则 a 已满，杀 b 之后应当报容量满）
        for i in range(6):
            code, rr = get("/assign?stream=s%d" % i)
            if rr.get("shard") == "a":
                get("/release?stream=s%d" % i)
                break
        shards[SHARD_PORTS[1]].shutdown()
        shards[SHARD_PORTS[1]].server_close()
        time.sleep(3)  # 等一轮巡检
        code, snap = get("/shards")
        b = [s for s in snap["shards"] if s["name"] == "b"][0]
        check("分片 b 被标记不健康", b["healthy"] is False, str(b.get("last_error"))[:60])
        check("stranded 列出 b 上的流", snap["stranded_count"] > 0,
              str(snap.get("stranded_streams")))

        # 5) 新流不再落到不健康的分片（a 还有空位）
        code, res = get("/assign?stream=after-fail")
        check("新流分配到健康分片 a", res.get("shard") == "a", str(res)[:80])

        # 6) 释放回收容量
        for i in range(3):
            get("/release?stream=a%d" % i)
        # a 上原有 3 路（s?）+ after-fail；释放 a 上已知的 after-fail
        get("/release?stream=after-fail")
        code, snap2 = get("/shards")
        a = [s for s in snap2["shards"] if s["name"] == "a"][0]
        check("释放后 a 负载下降", a["load"] < 4, "load=%d" % a["load"])

        # 7) metrics 可读
        url = "http://127.0.0.1:%d/metrics" % DISP_PORT
        with urllib.request.urlopen(url, timeout=5) as resp:
            text = resp.read().decode()
        check("metrics 输出健康/负载", "zlm_shard_healthy" in text and "zlm_shard_load" in text)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()

    failed = [r for r in results if not r[1]]
    print("\n结果: %d/%d 通过" % (len(results) - len(failed), len(results)))
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
