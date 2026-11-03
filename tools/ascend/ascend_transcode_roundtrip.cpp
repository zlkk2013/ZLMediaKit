// 昇腾转码闭环验证：H.264 硬解(VDEC) → NV12 → 硬编(VENC) → H.264
//
// 目的：在把编码后端接进 ZLMediaKit 之前，先证明「解码+编码」这条链在硬件上是通的。
// 复用项目里已在生产跑的 VDEC 封装（_ascend_vdec_wrap.so 的 C API）。
//
// 用法：ascend_transcode_roundtrip <输出.h264> [帧数] [宽] [高]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

#include "../../src/Transcode/AscendVenc.h"

extern "C" {
int vdec_wrap_init(int device);
int vdec_wrap_open(int width, int height, int en_type, int ref, int display, int chn);
int vdec_wrap_send_packet(int chn, const unsigned char *data, int len);
int vdec_wrap_send_eos(int chn);
int vdec_wrap_get_frame(int chn, unsigned char *dst, int cap, int *out_len);
int vdec_wrap_close(int chn);
}

using namespace mediakit;

int main(int argc, char **argv) {
    const char *out = argc > 1 ? argv[1] : "/tmp/roundtrip.h264";
    int frames = argc > 2 ? atoi(argv[2]) : 60;
    int W = argc > 3 ? atoi(argv[3]) : 1280;
    int H = argc > 4 ? atoi(argv[4]) : 720;

    // ---------- 第一段：先编出一份真实的 H.264 码流（按 AU 切开留在内存里）----------
    std::vector<std::vector<uint8_t>> aus;
    std::vector<uint64_t> au_pts;
    {
        AscendVenc src_enc;
        AscendVenc::Config cfg;
        cfg.width = W;
        cfg.height = H;
        cfg.h265 = false;
        cfg.fps = 25;
        cfg.channel = 0;
        std::string err;
        if (!src_enc.open(cfg, [&](const char *d, size_t n, uint64_t pts, bool) {
                aus.emplace_back((const uint8_t *) d, (const uint8_t *) d + n);
                au_pts.push_back(pts);
            }, err)) {
            printf("FAIL: 造源码流时打开编码器失败 %s\n", err.c_str());
            return 2;
        }
        size_t ysize = (size_t) W * H;
        std::vector<uint8_t> nv12(ysize * 3 / 2);
        for (int f = 0; f < frames; f++) {
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++)
                    nv12[(size_t) y * W + x] = (uint8_t) ((x + y + f * 7) & 0xFF);
            memset(nv12.data() + ysize, 128, ysize / 2);
            src_enc.encode(nv12.data(), (uint64_t) f * 40000);
        }
        src_enc.flush();
        src_enc.close();
    }
    printf("源码流：%zu 个访问单元\n", aus.size());
    if (aus.empty()) {
        printf("FAIL: 没造出源码流\n");
        return 2;
    }

    // ---------- 第二段：硬解 → NV12 ----------
    if (vdec_wrap_init(0) != 0) {
        printf("FAIL: vdec_wrap_init 失败\n");
        return 3;
    }
    const int VDEC_CHN = 20; // 避开编码用的 0/1 号通道
    int rc = vdec_wrap_open(W, H, 96 /* HI_PT_H264 */, 8, 2, VDEC_CHN);
    if (rc != 0) {
        printf("FAIL: vdec_wrap_open rc=0x%x\n", rc);
        return 3;
    }

    // ---------- 第三段：NV12 → 硬编 ----------
    FILE *fp = fopen(out, "wb");
    if (!fp) {
        printf("FAIL: 打不开 %s\n", out);
        return 4;
    }
    long out_bytes = 0;
    int out_frames = 0, keys = 0;
    AscendVenc dst_enc;
    AscendVenc::Config dcfg;
    dcfg.width = W;
    dcfg.height = H;
    dcfg.h265 = false;
    dcfg.fps = 25;
    dcfg.channel = 1;
    std::string err;
    if (!dst_enc.open(dcfg, [&](const char *d, size_t n, uint64_t, bool key) {
            fwrite(d, 1, n, fp);
            out_bytes += (long) n;
            out_frames++;
            if (key) keys++;
        }, err)) {
        printf("FAIL: 打开输出编码器失败 %s\n", err.c_str());
        fclose(fp);
        return 4;
    }

    size_t fsize = (size_t) W * H * 3 / 2;
    std::vector<uint8_t> nv12(fsize);
    int decoded = 0, fed = 0;
    for (size_t i = 0; i < aus.size(); i++) {
        rc = vdec_wrap_send_packet(VDEC_CHN, aus[i].data(), (int) aus[i].size());
        if (rc != 0) {
            printf("  送第 %zu 帧给 VDEC 失败 rc=0x%x\n", i, rc);
            continue;
        }
        // 反复取，直到这一帧取出来为止（VDEC 是异步的）
        for (int t = 0; t < 20; t++) {
            int out_len = 0;
            rc = vdec_wrap_get_frame(VDEC_CHN, nv12.data(), (int) fsize, &out_len);
            if (rc == 0 && out_len > 0) {
                decoded++;
                // 只喂整帧：out_len 小于整帧说明数据不完整，喂进去会产出坏码流（实测 cbp too large）
                if (out_len < (int) fsize) {
                    break;
                }
                if (dst_enc.encode(nv12.data(), au_pts[i])) {
                    fed++;
                }
                break;
            }
            usleep(1000);
        }
    }
    // EOS 之后必须继续取，否则最后 1~2 帧留在 VDEC 里丢掉（实测少 1 帧）
    vdec_wrap_send_eos(VDEC_CHN);
    int miss = 0;
    while (miss < 10) { // 连续 10 次取不到就认为排空（防无界循环）
        int out_len = 0;
        rc = vdec_wrap_get_frame(VDEC_CHN, nv12.data(), (int) fsize, &out_len);
        if (rc == 0 && out_len > 0) {
            decoded++;
            if (out_len < (int) fsize) {
                miss = 0;
                continue;
            }
            if (dst_enc.encode(nv12.data(), au_pts.empty() ? 0 : au_pts.back())) {
                fed++;
            }
            miss = 0;
            continue;
        }
        miss++;
        usleep(2000);
    }
    dst_enc.flush();
    dst_enc.close();
    vdec_wrap_close(VDEC_CHN);
    fclose(fp);

    printf("解码 %d 帧，送入编码 %d 帧，编码输出 %d 帧（关键帧 %d），码流 %ld 字节\n", decoded,
           fed, out_frames, keys, out_bytes);
    printf("%s: %s\n", (decoded > 0 && out_frames > 0) ? "PASS" : "FAIL", out);
    return (decoded > 0 && out_frames > 0) ? 0 : 5;
}
