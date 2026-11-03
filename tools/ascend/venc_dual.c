// 昇腾 310P DVPP 硬编码 v3：双线程（发送 + 独立取流）
//
// 前面单线程版本的两个实测结论：
//   ① pts 恒定 ⇒ 只有首帧成功；pts 递增 40000 ⇒ 隔帧成功
//   ② query_status 显示 left_pics 在 0/1 之间跳 ⇒ 编码器内部队列深度为 1
// 结论：send 会被「上一帧还没被取走」挡住，必须在另一个线程持续 get_stream，
// 主线程遇失败就退避重投同一帧（pts 不能变）。
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "acl/acl.h"
#include "acl/dvpp/hi_mpi_sys.h"
#include "acl/dvpp/hi_media_common.h"
#include "acl/dvpp/hi_dvpp_venc.h"

static int W = 640;
static int H = 360;
#define FRAMES 30
#define CHN 0
#define FPS 25

static aclrtContext g_ctx = NULL;
static volatile int g_stop = 0;
static volatile int g_frames_out = 0;
static volatile long g_bytes = 0;
static FILE *g_fp = NULL;
static unsigned char *g_xfer = NULL;

// ---- 取流线程：持续排空 ----
static void *get_thread(void *arg) {
    aclrtSetCurrentContext(g_ctx); // 线程内必须重绑上下文
    while (!g_stop) {
        hi_venc_pack packs[8];
        hi_venc_stream stream;
        memset(&stream, 0, sizeof(stream));
        memset(packs, 0, sizeof(packs));
        stream.pack = packs;
        stream.pack_cnt = 8;

        // 非阻塞取流 + 1ms 轮询：阻塞式 get 会占住驱动内部资源（实测影响下一帧 send）
        hi_s32 ret = hi_mpi_venc_get_stream(CHN, &stream, 0);
        if (ret != 0) { usleep(1000); continue; }

        for (hi_u32 i = 0; i < stream.pack_cnt; i++) {
            size_t len = packs[i].len;
            if (!packs[i].addr || len == 0) continue;
            if (aclrtMemcpy(g_xfer, len, packs[i].addr, len,
                            ACL_MEMCPY_DEVICE_TO_HOST) != ACL_SUCCESS)
                continue;
            g_bytes += (long)fwrite(g_xfer, 1, len, g_fp);
        }
        g_frames_out++;
        hi_mpi_venc_release_stream(CHN, &stream);
    }
    return NULL;
}

int main(int argc, char **argv) {
    const char *out = argc > 1 ? argv[1] : "/tmp/venc_out.h264";
    int dev = argc > 2 ? atoi(argv[2]) : 0;
    int use_h265 = (argc > 3 && strcmp(argv[3], "h265") == 0);
    int nframes = argc > 4 ? atoi(argv[4]) : FRAMES;
    int pace = (argc > 5 && strcmp(argv[5], "nopace") == 0) ? 0 : 1;
    if (argc > 6) W = atoi(argv[6]);
    if (argc > 7) H = atoi(argv[7]);
    hi_s32 ret;

    // ---- 初始化链 ----
    if (aclInit(NULL) != ACL_SUCCESS) return 9;
    aclrtSetDevice(dev);
    aclrtCreateContext(&g_ctx, dev);
    aclrtSetCurrentContext(g_ctx);
    if (hi_mpi_sys_init() != 0) { printf("sys_init failed\n"); return 9; }

    hi_venc_chn_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.venc_attr.type = use_h265 ? HI_PT_H265 : HI_PT_H264;
    attr.venc_attr.max_pic_width = W;
    attr.venc_attr.max_pic_height = H;
    attr.venc_attr.pic_width = W;
    attr.venc_attr.pic_height = H;
    attr.venc_attr.buf_size = 2 * 1024 * 1024;
    attr.venc_attr.is_by_frame = HI_TRUE;
    attr.rc_attr.rc_mode = use_h265 ? HI_VENC_RC_MODE_H265_CBR : HI_VENC_RC_MODE_H264_CBR;
    if (use_h265) {
        attr.rc_attr.h265_cbr.gop = FPS;
        attr.rc_attr.h265_cbr.stats_time = 1;
        attr.rc_attr.h265_cbr.src_frame_rate = FPS;
        attr.rc_attr.h265_cbr.dst_frame_rate = FPS;
        attr.rc_attr.h265_cbr.bit_rate = 1000;
        attr.venc_attr.h265_attr.rcn_ref_share_buf_en = HI_FALSE;
    } else {
        attr.rc_attr.h264_cbr.gop = FPS;
        attr.rc_attr.h264_cbr.stats_time = 1;
        attr.rc_attr.h264_cbr.src_frame_rate = FPS;
        attr.rc_attr.h264_cbr.dst_frame_rate = FPS;
        attr.rc_attr.h264_cbr.bit_rate = 1000;
        attr.venc_attr.h264_attr.rcn_ref_share_buf_en = HI_FALSE;
    }
    attr.gop_attr.gop_mode = HI_VENC_GOP_MODE_NORMAL_P;
    attr.gop_attr.normal_p.ip_qp_delta = 2;

    ret = hi_mpi_venc_create_chn(CHN, &attr);
    printf("create_chn ret=0x%x\n", ret);
    if (ret != 0) return 1;
    hi_venc_start_param sp;
    sp.recv_pic_num = -1;
    ret = hi_mpi_venc_start_chn(CHN, &sp);
    printf("start_chn ret=0x%x\n", ret);

    hi_u64 y_size = (hi_u64)W * H, frame_size = y_size * 3 / 2;
    // 多缓冲轮换：send 之后驱动接管所有权，不能立刻复用同一块
    #define NB 8
    hi_void *dbv[NB] = {NULL};
    for (int i = 0; i < NB; i++) hi_mpi_dvpp_malloc(0, &dbv[i], frame_size);
    hi_void *db = dbv[0];
    unsigned char *host = (unsigned char *)malloc(frame_size);
    g_xfer = (unsigned char *)malloc(4 * 1024 * 1024);
    g_fp = fopen(out, "wb");

    // ---- 启动取流线程 ----
    pthread_t th;
    pthread_create(&th, NULL, get_thread, NULL);

    // ---- 主线程：按 25fps 节奏喂帧，失败就退避重投同一帧（pts 不变） ----
    int sent = 0, retries = 0;
    double t0 = (double)clock() / CLOCKS_PER_SEC;
    for (int f = 0; f < nframes; f++) {
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                host[y * W + x] = (unsigned char)((x + y + f * 8) & 0xFF);
        memset(host + y_size, 128, y_size / 2);
        db = dbv[f % NB];
        aclrtMemcpy(db, frame_size, host, frame_size, ACL_MEMCPY_HOST_TO_DEVICE);

        hi_video_frame_info fi;
        memset(&fi, 0, sizeof(fi));
        fi.v_frame.width = W;
        fi.v_frame.height = H;
        fi.v_frame.field = HI_VIDEO_FIELD_FRAME;
        fi.v_frame.pixel_format = HI_PIXEL_FORMAT_YUV_SEMIPLANAR_420;
        fi.v_frame.video_format = HI_VIDEO_FORMAT_LINEAR;
        // 对齐官方样例 sample_encoder.cpp：这几个字段必须显式给，且不设 phys_addr
        fi.v_frame.dynamic_range = HI_DYNAMIC_RANGE_SDR8;
        fi.v_frame.compress_mode = HI_COMPRESS_MODE_NONE;
        fi.v_frame.color_gamut = HI_COLOR_GAMUT_BT709;
        fi.v_frame.virt_addr[0] = db;
        fi.v_frame.width_stride[0] = W;
        fi.v_frame.height_stride[0] = H;
        fi.v_frame.virt_addr[1] = (unsigned char *)db + y_size;
        fi.v_frame.width_stride[1] = W;
        fi.v_frame.height_stride[1] = H / 2;
        // ★ time_ref 必须是偶数（官方样例注释：timeref must be even）——
        //   传奇数会被 send_frame 判为非法参数（0xA0088003），表现为「隔帧失败」。
        fi.v_frame.time_ref = (hi_u32)(f * 2);
        fi.v_frame.pts = (hi_u64)f * (1000000 / FPS);

        hi_user_frame_info ufi;
        memset(&ufi, 0, sizeof(ufi));
        ufi.user_frame = fi;
        ufi.user_rc_info.frame_type = VENC_FRAME_TYPE_NONE;

        int ok = 0;
        for (int t = 0; t < 200 && !ok; t++) { // 最多等 2s
            ret = hi_mpi_venc_send_frame_ex(CHN, &ufi, 0);
            if (ret == 0) { ok = 1; }
            else { retries++; usleep(5 * 1000); }
        }
        if (!ok) printf("frame %d 最终失败 ret=0x%x（重试 %d 次）\n", f, ret, retries);
        else sent++;

        if (pace) usleep(1000000 / FPS);
    }

    // 等取流线程把尾巴排空
    for (int i = 0; i < 100 && g_frames_out <= 0; i++) usleep(10 * 1000);
    usleep(200 * 1000);
    g_stop = 1;
    pthread_join(th, NULL);

    fclose(g_fp);
    hi_mpi_venc_stop_chn(CHN);
    hi_mpi_venc_destroy_chn(CHN);
    double dt = (double)clock() / CLOCKS_PER_SEC - t0;
    printf("DONE sent=%d retries=%d frames_out=%d bytes=%ld  用时%.2fs  %.1f fps  file=%s\n",
           sent, retries, g_frames_out, g_bytes, dt, dt > 0 ? g_frames_out / dt : 0, out);
    return (g_frames_out > 0) ? 0 : 5;
}
