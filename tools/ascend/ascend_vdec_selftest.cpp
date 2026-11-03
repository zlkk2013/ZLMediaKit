/*
 * ascend_vdec_selftest.cpp —— 昇腾 DVPP 硬解独立自测（不依赖 ZLMediaKit）
 *
 * 为什么要有它：硬解第一次接进 ZLM 时，日志显示「解码计数在涨」但编码器拿不到帧，
 * 靠猜是查不出来的。这个工具把 AscendVdec 单独跑起来，直接回答三个问题：
 *   1. 通道能不能建、能不能收流；
 *   2. 解出来的帧宽高是多少、有没有真数据；
 *   3. 解出来的 NV12 能不能被 ffprobe 认出来（写盘后校验）。
 *
 * 用法：
 *   ./ascend_vdec_selftest input.h264 /tmp/out.nv12 [h264|h265] [max_frames]
 *
 * 分帧说明（简化版）：把 Annex-B 按 NAL 切开，遇到 VCL NAL（H.264: 1/5，H.265: 0..31）
 * 就认为一帧开始，把之前攒的 SPS/PPS/SEI 一起发过去。多切片的帧会被拆成多帧发送，
 * 对「能不能解出画面」这个判据没影响（对逐帧计数有影响，故本工具只报驱动返回的帧数）。
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/Transcode/AscendVdec.h"

using namespace std;
using namespace mediakit;

static bool isVclNal(const uint8_t *d, size_t n, bool h265) {
    if (n < 5) {
        return false;
    }
    if (h265) {
        // HEVC: nal_unit_type 在首字节的 bit1..6
        int type = (d[0] >> 1) & 0x3F;
        return type <= 31;
    }
    return (d[0] & 0x1F) == 1 || (d[0] & 0x1F) == 5;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "用法: %s input.h264|h265 out.nv12 [h264|h265] [max_frames]\n", argv[0]);
        return 2;
    }
    const char *in_path = argv[1];
    const char *out_path = argv[2];
    bool h265 = (argc > 3 && strcmp(argv[3], "h265") == 0);
    int max_frames = (argc > 4) ? atoi(argv[4]) : 100;

    FILE *fp = fopen(in_path, "rb");
    if (!fp) {
        fprintf(stderr, "打不开输入 %s\n", in_path);
        return 2;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    vector<uint8_t> es((size_t) sz);
    if (fread(es.data(), 1, es.size(), fp) != es.size()) {
        fprintf(stderr, "读输入失败\n");
        fclose(fp);
        return 2;
    }
    fclose(fp);

    printf("输入 %s: %ld 字节, %s\n", in_path, sz, h265 ? "H.265" : "H.264");

    AscendVdec dec;
    AscendVdec::Config cfg;
    cfg.h265 = h265;
    cfg.width = 1920;
    cfg.height = 1080;
    cfg.channel = AscendVdec::acquireChannel();
    if (cfg.channel < 0) {
        fprintf(stderr, "没有空闲 VDEC 通道\n");
        return 1;
    }

    int frames = 0;
    int first_w = 0, first_h = 0;
    vector<uint8_t> first_nv12;
    string err;
    auto ok = dec.open(cfg, [&](const uint8_t *nv12, int w, int h, uint32_t pts) {
        if (frames == 0) {
            first_w = w;
            first_h = h;
            first_nv12.assign(nv12, nv12 + (size_t) w * h * 3 / 2);
        }
        frames++;
    }, err);
    if (!ok) {
        fprintf(stderr, "open 失败: %s\n", err.c_str());
        AscendVdec::releaseChannel(cfg.channel);
        return 1;
    }
    printf("open ok: channel=%d max %dx%d\n", cfg.channel, cfg.width, cfg.height);

    // 分帧：攒非 VCL NAL，遇到 VCL 就发一个 AU
    vector<uint8_t> au;
    size_t i = 0;
    uint32_t pts = 0;
    int sent = 0;
    while (i + 3 < es.size() && sent < max_frames) {
        // 找起始码
        size_t start = i;
        while (i + 3 < es.size()) {
            if (es[i] == 0 && es[i + 1] == 0 && es[i + 2] == 1) {
                break;
            }
            if (i + 4 < es.size() && es[i] == 0 && es[i + 1] == 0 && es[i + 2] == 0 && es[i + 3] == 1) {
                break;
            }
            i++;
        }
        size_t sc_len = (i + 4 <= es.size() && es[i + 2] == 0) ? 4 : 3;
        size_t nal_start = i + sc_len;
        size_t j = nal_start;
        while (j + 3 < es.size()) {
            if (es[j] == 0 && es[j + 1] == 0 && es[j + 2] == 1) {
                break;
            }
            if (j + 4 < es.size() && es[j] == 0 && es[j + 1] == 0 && es[j + 2] == 0 && es[j + 3] == 1) {
                break;
            }
            j++;
        }
        size_t nal_len = j - nal_start;
        if (nal_len == 0) {
            i = j + 1;
            continue;
        }
        bool vcl = isVclNal(&es[nal_start], nal_len, h265);
        if (vcl && !au.empty()) {
            // 先发攒下的参数集 + 这一帧的 VCL
            au.insert(au.end(), &es[start], &es[j]);
            if (!dec.decode(au.data(), au.size(), pts)) {
                fprintf(stderr, "decode 失败 (第 %d 帧)\n", sent);
            }
            pts += 40;
            sent++;
            au.clear();
        } else {
            au.insert(au.end(), &es[start], &es[j]);
            if (vcl) {
                // 没有参数集前缀的 VCL：单独作为 AU 发
                if (!dec.decode(au.data(), au.size(), pts)) {
                    fprintf(stderr, "decode 失败 (第 %d 帧)\n", sent);
                }
                pts += 40;
                sent++;
                au.clear();
            }
        }
        i = j;
    }
    dec.flush();

    printf("送包 %d，解出 %d 帧，首帧 %dx%d\n", sent, frames, first_w, first_h);
    if (!first_nv12.empty()) {
        FILE *of = fopen(out_path, "wb");
        if (of) {
            fwrite(first_nv12.data(), 1, first_nv12.size(), of);
            fclose(of);
            printf("首帧 NV12 已写入 %s（%zu 字节），校验命令：\n"
                   "  ffplay -f rawvideo -pixel_format nv12 -video_size %dx%d %s\n",
                   out_path, first_nv12.size(), first_w, first_h, out_path);
        }
    }
    dec.close();
    return frames > 0 ? 0 : 1;
}
