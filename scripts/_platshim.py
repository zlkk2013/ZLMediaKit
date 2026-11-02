"""跨平台小工具：让验收脚本在 macOS 与 Linux 上都能跑。

为什么需要：脚本最初是按 macOS 写的（`ps -M` 数线程、`/usr/sbin/lsof`、`release/darwin/...`），
拿到 Linux（含 WSL）机器上验硬件编解码时，这些差异会让脚本在第一步就报「服务没起来」，
看起来像产品问题。统一收到这里，脚本只调本模块。

Platform shims so the acceptance scripts run on both macOS and Linux (including WSL).
"""
import os
import platform
import re
import subprocess
import sys

IS_MAC = sys.platform == "darwin"
IS_LINUX = sys.platform.startswith("linux")

WT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def sh(cmd, timeout=120):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout)


def bin_path():
    """按平台找 MediaServer：macOS 在 release/darwin，Linux 在 release/linux。"""
    for rel in ("release/darwin/Release/MediaServer", "release/linux/Release/MediaServer",
                "release/linux/Debug/MediaServer", "release/darwin/Debug/MediaServer"):
        p = os.path.join(WT, rel)
        if os.path.exists(p):
            return p
    raise RuntimeError("找不到 MediaServer，先构建（见 docs/DELIVERY_DEPLOY.md）")


def bin_dir():
    return os.path.dirname(bin_path())


def lsof():
    """lsof 在 macOS 的 /usr/sbin，Linux 的 /usr/bin —— 本机 PATH 精简，必须绝对路径。"""
    for p in ("/usr/sbin/lsof", "/usr/bin/lsof", "/bin/lsof"):
        if os.path.exists(p):
            return p
    raise RuntimeError("找不到 lsof")


def pid_on_port(port):
    """端口上的监听进程 pid；没有返回 None。"""
    out = sh(f"{lsof()} -nP -tiTCP:{port} -sTCP:LISTEN").stdout.split()
    return int(out[0]) if out else None


def thread_count(pid):
    """线程数：macOS 用 `ps -M`，Linux 用 /proc/<pid>/task。"""
    if IS_LINUX:
        try:
            return len(os.listdir(f"/proc/{pid}/task"))
        except OSError:
            return None
    out = sh(f"ps -M -p {pid} 2>/dev/null | wc -l").stdout.strip()
    return int(out) if out.isdigit() else None


def rss_kb(pid):
    out = sh(f"ps -o rss= -p {pid}").stdout.strip()
    return int(out) if out.isdigit() else None


def fd_count(pid):
    if IS_LINUX:
        try:
            return len(os.listdir(f"/proc/{pid}/fd"))
        except OSError:
            return None
    return int(sh(f"{lsof()} -p {pid} 2>/dev/null | wc -l").stdout.strip() or 0)


def load_avg():
    try:
        with open("/proc/loadavg") as f:
            return float(f.read().split()[0])
    except OSError:
        out = sh("/usr/sbin/sysctl -n vm.loadavg").stdout.split()
        return float(out[1]) if len(out) > 1 else 0.0


def ncpu():
    return os.cpu_count() or 1


def nvidia_smi():
    """nvidia-smi 的绝对路径。

    坑：WSL 里它装在 /usr/lib/wsl/lib/，**不在精简 PATH 里** —— 只写 `nvidia-smi`
    会「找不到命令」，于是脚本以为机器没有 GPU、把有 NVENC 的机器当无卡平台跳过。
    WSL keeps nvidia-smi in /usr/lib/wsl/lib, which is not on a trimmed PATH; calling it
    bare silently reports "no GPU" and skips the very checks this box exists for.
    """
    for p in ("nvidia-smi", "/usr/lib/wsl/lib/nvidia-smi", "/usr/bin/nvidia-smi",
              "/usr/local/nvidia/bin/nvidia-smi"):
        if "/" not in p:
            r = sh(f"command -v {p} || true", timeout=10).stdout.strip()
            if r:
                return r
            continue
        if os.path.exists(p):
            return p
    return None


def gpu_count():
    """NVIDIA 显卡数；没有 nvidia-smi 或非 NVIDIA 平台返回 0。"""
    smi = nvidia_smi()
    if not smi:
        return 0
    try:
        out = sh(f"{smi} --query-gpu=index --format=csv,noheader", timeout=20).stdout
    except Exception:
        return 0
    return len([l for l in out.splitlines() if l.strip().isdigit()])


def gpu_name():
    smi = nvidia_smi()
    if not smi:
        return ""
    try:
        out = sh(f"{smi} --query-gpu=name --format=csv,noheader", timeout=20).stdout.strip()
        return out.splitlines()[0] if out else ""
    except Exception:
        return ""


def supports_device_selection():
    """是否支持按序号选卡：只有 NVENC（Linux/Windows + NVIDIA）有 gpu 选项。"""
    if IS_MAC:
        return False
    return gpu_count() > 0


def hw_encoder_expected(codec):
    """该平台该 codec 期望的硬件编码器名；没有则 None。"""
    codec = codec.lower()
    if IS_MAC:
        return {"h264": "h264_videotoolbox", "h265": "hevc_videotoolbox"}.get(codec)
    if gpu_count() > 0:
        return {"h264": "h264_nvenc", "h265": "hevc_nvenc", "av1": "av1_nvenc"}.get(codec)
    return None


def kill_pid(pid, sig=9):
    try:
        os.kill(pid, sig)
    except Exception:
        pass


def kill_group(p):
    import signal
    try:
        os.killpg(os.getpgid(p.pid), signal.SIGKILL)
    except Exception:
        pass
