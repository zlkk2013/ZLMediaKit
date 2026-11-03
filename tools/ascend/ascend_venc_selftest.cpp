// AscendVenc 自测：合成 NV12 → 硬编 → 落 Annex-B，统计吞吐与帧数
// 用法：ascend_venc_selftest <输出文件> [h264|h265] [帧数] [宽] [高] [设备号] [速率]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
#include <vector>

#include "../../src/Transcode/AscendVenc.h"

using namespace mediakit;

int main(int argc, char **argv) {
    const char *out = argc > 1 ? argv[1] : "/tmp/ascenc.h264";
    bool h265 = argc > 2 && strcmp(argv[2], "h265") == 0;
    int frames = argc > 3 ? atoi(argv[3]) : 100;
    int W = argc > 4 ? atoi(argv[4]) : 640;
    int H = argc > 5 ? atoi(argv[5]) : 360;
    int dev = argc > 6 ? atoi(argv[6]) : 0;
    int fps = argc > 7 ? atoi(argv[7]) : 25;

    if (!AscendVenc::available(dev)) {
        printf("FAIL: 本机 DVPP 初始化失败（不是昇腾环境或设备不可用）\n");
        return 2;
    }

    FILE *fp = fopen(out, "wb");
    if (!fp) { printf("FAIL: 打不开 %s\n", out); return 2; }

    long bytes = 0;
    int keys = 0;
    AscendVenc enc;
    AscendVenc::Config cfg;
    cfg.width = W;
    cfg.height = H;
    cfg.h265 = h265;
    cfg.fps = fps;
    cfg.device = dev;
    cfg.channel = 0;

    std::string err;
    if (!enc.open(cfg, [&](const char *data, size_t len, uint64_t pts, bool key) {
            fwrite(data, 1, len, fp);
            bytes += (long) len;
            if (key) keys++;
        }, err)) {
        printf("FAIL: open 失败 %s\n", err.c_str());
        fclose(fp);
        return 3;
    }
    printf("open ok: %s %dx%d %s fps=%d bitrate=%dkbps\n", h265 ? "H265" : "H264", W, H,
           h265 ? "hevc" : "avc", cfg.fps, AscendVenc::defaultBitrateKbps(W, H));

    size_t ysize = (size_t) W * H;
    std::vector<uint8_t> nv12(ysize * 3 / 2);
    auto t0 = std::chrono::steady_clock::now();
    int failed = 0;
    for (int f = 0; f < frames; f++) {
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                nv12[(size_t) y * W + x] = (uint8_t) ((x + y + f * 7) & 0xFF);
            }
        }
        memset(nv12.data() + ysize, 128, ysize / 2);
        if (!enc.encode(nv12.data(), (uint64_t) f * (1000000 / fps))) {
            failed++;
        }
    }
    enc.flush();
    auto dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("sent=%u out=%u keys=%d failed=%d retries=%u bytes=%ld 用时%.2fs %.1f fps\n",
           enc.sentFrames(), enc.outFrames(), keys, failed, enc.sendRetries(), bytes, dt,
           dt > 0 ? enc.outFrames() / dt : 0);

    enc.close();
    fclose(fp);

    bool ok = enc.outFrames() > 0 && failed == 0 && bytes > 0;
    printf("%s: %s\n", ok ? "PASS" : "FAIL", out);
    return ok ? 0 : 1;
}
