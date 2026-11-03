// 昇腾 310P 多路并发硬编码压测：N 个通道同时编，量每路 fps 与总吞吐
// 用法：ascend_venc_multi <路数> [宽] [高] [每路帧数] [h264|h265]
//
// 目的：回答「这台机器能扛多少路转码」。310P 的 VENC 通道是整机共享资源，
// 所以既要看总吞吐，也要看每路是否还能跑到源帧率。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "../../src/Transcode/AscendVenc.h"

using namespace mediakit;

int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 4;
    int W = argc > 2 ? atoi(argv[2]) : 1280;
    int H = argc > 3 ? atoi(argv[3]) : 720;
    int frames = argc > 4 ? atoi(argv[4]) : 300;
    bool h265 = argc > 5 && strcmp(argv[5], "h265") == 0;
    int fps = 25;

    if (!AscendVenc::available(0)) {
        printf("FAIL: 不是昇腾环境\n");
        return 2;
    }
    if (n < 1 || n > 32) {
        printf("FAIL: 路数需在 [1,32]（通道号整机共享，上限 32）\n");
        return 2;
    }

    struct Chan {
        AscendVenc enc;
        std::atomic<long> bytes{0};
        std::atomic<int> out{0};
        int keys = 0;
        bool ok = false;
        std::string err;
    };
    std::vector<Chan> chans(n);

    // ---- 依次打开 N 个通道（通道号 0..n-1）----
    for (int i = 0; i < n; i++) {
        AscendVenc::Config cfg;
        cfg.width = W;
        cfg.height = H;
        cfg.h265 = h265;
        cfg.fps = fps;
        cfg.channel = i;
        auto &c = chans[i];
        c.ok = c.enc.open(cfg, [&c](const char *, size_t len, uint64_t, bool key) {
            c.bytes += (long) len;
            c.out++;
            if (key) {
                c.keys++;
            }
        }, c.err);
        if (!c.ok) {
            printf("FAIL: 通道 %d 打开失败：%s\n", i, c.err.c_str());
            printf("（通道号是整机共享的，可能被别的进程占用）\n");
            return 3;
        }
    }
    printf("打开 %d 路 %dx%d %s 成功\n", n, W, H, h265 ? "H265" : "H264");

    // ---- 并发喂帧 ----
    size_t ysize = (size_t) W * H;
    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> ths;
    std::atomic<int> failed_send{0};
    for (int i = 0; i < n; i++) {
        ths.emplace_back([&, i]() {
            std::vector<uint8_t> nv12(ysize * 3 / 2);
            for (int f = 0; f < frames; f++) {
                for (int y = 0; y < H; y++) {
                    for (int x = 0; x < W; x++) {
                        nv12[(size_t) y * W + x] = (uint8_t) ((x + y + f * 7 + i * 13) & 0xFF);
                    }
                }
                memset(nv12.data() + ysize, 128, ysize / 2);
                if (!chans[i].enc.encode(nv12.data(), (uint64_t) f * (1000000 / fps))) {
                    failed_send++;
                }
            }
        });
    }
    for (auto &t : ths) {
        t.join();
    }
    // 先把每路编码器里的在途帧排空，否则统计会少算最后几帧（实测 299/300）
    for (auto &c : chans) {
        c.enc.flush();
    }
    auto dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    long total_out = 0, total_bytes = 0;
    int min_out = frames, max_retry = 0;
    printf("\n%-6s %-8s %-10s %-8s %-10s\n", "通道", "出帧", "码流(KB)", "重试", "状态");
    for (int i = 0; i < n; i++) {
        auto &c = chans[i];
        total_out += c.out;
        total_bytes += c.bytes;
        if (c.out < min_out) min_out = c.out;
        if ((int) c.enc.sendRetries() > max_retry) max_retry = c.enc.sendRetries();
        printf("%-6d %-8d %-10ld %-8u %s\n", i, c.out.load(), (long) (c.bytes / 1024),
               c.enc.sendRetries(), c.out >= frames ? "OK" : "丢帧");
    }
    printf("\n合计：%ld 帧 / %.2fs = %.1f fps（每路平均 %.1f fps，最差 %d/%d 帧）\n",
           total_out, dt, total_out / dt, total_out / dt / n, min_out, frames);
    printf("发帧失败 %d 次，最大单通道重试 %d 次\n", failed_send.load(), max_retry);

    for (auto &c : chans) {
        c.enc.close();
    }
    bool ok = (failed_send.load() == 0) && (min_out >= frames);
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
