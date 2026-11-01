#!/usr/bin/env python3
"""生成/补齐验收所需的素材文件（幂等）。

为什么需要：所有套件都从 /tmp 读固定素材（av_test.mp4、src_h265.mp4 ...），
而 /tmp 会被系统或清理脚本按空闲时间回收。素材没了以后，套件的失败信息是
「缺素材」或者更难看的 ffmpeg 报错，看起来像产品坏了 —— 实测踩过（hw 套件整套
因为 /tmp/src_1280x720.mp4 消失而直接退出）。

用法：
  python3 scripts/test_assets.py          # 只补缺失的
  python3 scripts/test_assets.py --force  # 全部重建

final_regression.sh 的前置检查会自动调用它。
"""
import os
import subprocess
import sys

FFMPEG = os.path.expanduser("~/miniconda3/envs/zlm-ffmpeg/bin/ffmpeg")
if not os.path.exists(FFMPEG):
    FFMPEG = "ffmpeg"

# 名称 -> 生成命令（-y 覆盖、-v error 只报错）
ASSETS = {
    # 320x240@15 + aac：绝大多数音频/视频用例的基础素材
    # 注意：必须 44100Hz **立体声**。原来手工造的素材是双声道，joint_accept 的
    # 声道数/音量断言依赖它；我第一次用默认 sine（单声道）生成，导致 5 项音频断言
    # 全错（声道 1ch、总能量差 3dB），差点被当成产品回归。
    "/tmp/av_test.mp4":
        "-f lavfi -i testsrc2=size=320x240:rate=15:duration=20 "
        "-f lavfi -i sine=frequency=440:sample_rate=44100:duration=20 "
        "-c:v libx264 -preset ultrafast -b:v 400k -pix_fmt yuv420p "
        "-c:a aac -b:a 64k -ar 44100 -ac 2",
    # 640x360@15 H265：H265 源用例（反向转码、重连、硬件编码的 H264 目标）
    "/tmp/src_h265.mp4":
        "-f lavfi -i testsrc2=size=640x360:rate=15:duration=20 "
        "-f lavfi -i sine=frequency=440:duration=20 "
        "-c:v libx265 -preset ultrafast -b:v 500k -c:a aac -b:a 64k",
    # 1280x720@30 H264：缩放倍数、滤镜、自适应降帧、硬件编码基准
    "/tmp/src_1280x720.mp4":
        "-f lavfi -i testsrc2=size=1280x720:rate=30:duration=20 "
        "-f lavfi -i sine=frequency=440:duration=20 "
        "-c:v libx264 -preset ultrafast -b:v 800k -pix_fmt yuv420p -c:a aac -b:a 64k",
    # 1920x1080@15：视频参数用例（高分辨率缩放/码率）
    "/tmp/src_1920x1080.mp4":
        "-f lavfi -i testsrc2=size=1920x1080:rate=15:duration=10 "
        "-f lavfi -i sine=frequency=440:duration=10 "
        "-c:v libx264 -preset ultrafast -b:v 1500k -pix_fmt yuv420p -c:a aac -b:a 64k",
    # 音频矩阵的 5 个采样率/声道源（audio_matrix_test 硬编码这些路径）
    "/tmp/src_s8k_m.mp4":
        "-f lavfi -i testsrc2=size=320x240:rate=15:duration=12 "
        "-f lavfi -i sine=frequency=440:sample_rate=8000:duration=12 "
        "-c:v libx264 -preset ultrafast -pix_fmt yuv420p -c:a aac -ar 8000 -ac 1",
    "/tmp/src_s16k_m.mp4":
        "-f lavfi -i testsrc2=size=320x240:rate=15:duration=12 "
        "-f lavfi -i sine=frequency=440:sample_rate=16000:duration=12 "
        "-c:v libx264 -preset ultrafast -pix_fmt yuv420p -c:a aac -ar 16000 -ac 1",
    "/tmp/src_s22k_s.mp4":
        "-f lavfi -i testsrc2=size=320x240:rate=15:duration=12 "
        "-f lavfi -i sine=frequency=440:sample_rate=22050:duration=12 "
        "-c:v libx264 -preset ultrafast -pix_fmt yuv420p -c:a aac -ar 22050 -ac 2",
    "/tmp/src_s44k_s.mp4":
        "-f lavfi -i testsrc2=size=320x240:rate=15:duration=12 "
        "-f lavfi -i sine=frequency=440:sample_rate=44100:duration=12 "
        "-c:v libx264 -preset ultrafast -pix_fmt yuv420p -c:a aac -ar 44100 -ac 2",
    "/tmp/src_s48k_s.mp4":
        "-f lavfi -i testsrc2=size=320x240:rate=15:duration=12 "
        "-f lavfi -i sine=frequency=440:sample_rate=48000:duration=12 "
        "-c:v libx264 -preset ultrafast -pix_fmt yuv420p -c:a aac -ar 48000 -ac 2",

    # logo 角标用例的素材（白底黑框，和之前手工造的一致）
    "/tmp/logo_wm.png":
        "-f lavfi -i color=c=white:s=240x80:d=1 -vf drawbox=x=0:y=0:w=240:h=80:color=black:t=6 "
        "-frames:v 1 -update 1",

    # G711A 作源（反向转码用例）
    "/tmp/src_g711a.flv":
        "-f lavfi -i testsrc2=size=320x240:rate=15:duration=12 "
        "-f lavfi -i sine=frequency=440:duration=12 "
        "-c:v libx264 -preset ultrafast -c:a pcm_alaw -ar 8000 -ac 1",
    # MP3 作源
    "/tmp/src_mp3.flv":
        "-f lavfi -i testsrc2=size=320x240:rate=15:duration=12 "
        "-f lavfi -i sine=frequency=440:duration=12 "
        "-c:v libx264 -preset ultrafast -c:a libmp3lame -ar 44100 -ac 2",
}


def build(path, args):
    cmd = f'{FFMPEG} -y -v error {args} {path}'
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=300)
    # 图片素材天然很小（一张 240x80 的 PNG 只有几百字节），阈值按类型分开
    min_size = 100 if path.endswith((".png", ".jpg")) else 1024
    if r.returncode != 0 or not os.path.exists(path) or os.path.getsize(path) < min_size:
        print(f"  ❌ 生成失败 {path}: {(r.stderr or '')[:200]}")
        return False
    print(f"  ✅ {path}  ({os.path.getsize(path) // 1024} KB)")
    return True


def main():
    force = "--force" in sys.argv
    def too_small(p):
        return os.path.getsize(p) < (100 if p.endswith((".png", ".jpg")) else 1024)
    missing = [p for p in ASSETS if force or not os.path.exists(p) or too_small(p)]
    if not missing:
        print(f"✅ 素材齐全（{len(ASSETS)} 个）")
        return 0
    print(f"补齐素材 {len(missing)}/{len(ASSETS)} 个（缺失或已被 /tmp 清理）：")
    ok = True
    for path in missing:
        ok = build(path, ASSETS[path]) and ok
    if not ok:
        print("❗ 有素材生成失败，后续套件会因为缺素材而失败（不是产品问题）")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
