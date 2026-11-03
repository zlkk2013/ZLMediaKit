// 昇腾 310P DVPP 硬编码验证 v2：NV12 → H.264 码流（落盘可被 ffprobe 识别）
// v1 结论：初始化链 + create_chn + 首帧编码都成功；两个坑已定位：
//   ① dvpp_malloc 是设备内存，host 直写 = SIGSEGV
//   ② 输出 pack 的 addr 也是设备内存，必须 aclrtMemcpy D2H 才能落盘
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "acl/acl.h"
#include "acl/dvpp/hi_mpi_sys.h"
#include "acl/dvpp/hi_media_common.h"
#include "acl/dvpp/hi_dvpp_venc.h"

#define W 640
#define H 360
#define FRAMES 10
#define CHN 0
#define NBUF 8 /* 多缓冲：驱动可能仍持有上一帧 */

static aclrtContext g_ctx = NULL;

static int dvpp_init(int dev) {
    if (aclInit(NULL) != ACL_SUCCESS) { printf("aclInit failed\n"); return 1; }
    if (aclrtSetDevice(dev) != ACL_SUCCESS) { printf("setDevice failed\n"); return 1; }
    if (aclrtCreateContext(&g_ctx, dev) != ACL_SUCCESS) { printf("createContext failed\n"); return 1; }
    if (aclrtSetCurrentContext(g_ctx) != ACL_SUCCESS) { printf("setCurrentContext failed\n"); return 1; }
    hi_s32 r = hi_mpi_sys_init();
    printf("hi_mpi_sys_init ret=0x%x\n", r);
    return r == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
    const char *out = argc > 1 ? argv[1] : "/tmp/venc_out.h264";
    int dev = argc > 2 ? atoi(argv[2]) : 0;
    hi_s32 ret;

    if (dvpp_init(dev) != 0) return 9;

    hi_venc_chn_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.venc_attr.type = HI_PT_H264;
    attr.venc_attr.max_pic_width = W;
    attr.venc_attr.max_pic_height = H;
    attr.venc_attr.pic_width = W;
    attr.venc_attr.pic_height = H;
    attr.venc_attr.buf_size = 2 * 1024 * 1024;
    attr.venc_attr.profile = 0;
    attr.venc_attr.is_by_frame = HI_TRUE;
    attr.rc_attr.rc_mode = HI_VENC_RC_MODE_H264_CBR;
    attr.rc_attr.h264_cbr.gop = 25;
    attr.rc_attr.h264_cbr.stats_time = 1;
    attr.rc_attr.h264_cbr.src_frame_rate = 25;
    attr.rc_attr.h264_cbr.dst_frame_rate = 25;
    attr.rc_attr.h264_cbr.bit_rate = 1000;
    attr.gop_attr.gop_mode = HI_VENC_GOP_MODE_NORMAL_P;
    attr.gop_attr.normal_p.ip_qp_delta = 2;

    ret = hi_mpi_venc_create_chn(CHN, &attr);
    printf("create_chn ret=0x%x\n", ret);
    if (ret != 0) return 1;

    hi_venc_start_param sp;
    sp.recv_pic_num = -1;
    ret = hi_mpi_venc_start_chn(CHN, &sp);
    printf("start_chn ret=0x%x\n", ret);

    hi_u64 y_size = (hi_u64)W * H;
    hi_u64 frame_size = y_size * 3 / 2;

    // 设备侧帧缓冲（多份轮转）
    hi_void *dev_buf[NBUF] = {NULL};
    unsigned char *host = (unsigned char *)malloc(frame_size);
    for (int i = 0; i < NBUF; i++) {
        ret = hi_mpi_dvpp_malloc(0, &dev_buf[i], frame_size);
        if (ret != 0) { printf("dvpp_malloc[%d] ret=0x%x\n", i, ret); return 3; }
    }
    for (int i = 0; i < NBUF; i++)
        printf("  buf[%d] = %p (mod4096=%llu)\n", i, dev_buf[i],
               (unsigned long long)((size_t)dev_buf[i] % 4096));
    printf("dvpp_malloc x%d ok, frame_size=%llu\n", NBUF, (unsigned long long)frame_size);

    FILE *fp = fopen(out, "wb");
    if (!fp) { printf("open %s failed\n", out); return 4; }

    int got = 0, sent = 0;
    unsigned char *xfer = (unsigned char *)malloc(4 * 1024 * 1024);

    for (int f = 0; f < FRAMES; f++) {
        // ---- 填 NV12 ----
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                host[y * W + x] = (unsigned char)((x + y + f * 8) & 0xFF);
        memset(host + y_size, 128, y_size / 2);

        hi_void *db = dev_buf[0]; /* 实验：固定用第一个缓冲，验证是不是缓冲索引导致 */
        aclError cp = aclrtMemcpy(db, frame_size, host, frame_size,
                                  ACL_MEMCPY_HOST_TO_DEVICE);
        if (cp != ACL_SUCCESS) { printf("H2D ret=%d\n", (int)cp); break; }

        hi_video_frame_info fi;
        memset(&fi, 0, sizeof(fi));
        fi.v_frame.width = W;
        fi.v_frame.height = H;
        fi.v_frame.field = HI_VIDEO_FIELD_FRAME;
        fi.v_frame.pixel_format = HI_PIXEL_FORMAT_YUV_SEMIPLANAR_420;
        fi.v_frame.video_format = HI_VIDEO_FORMAT_LINEAR;
        fi.v_frame.virt_addr[0] = db;
        fi.v_frame.phys_addr[0] = (hi_u64)(size_t)db;
        fi.v_frame.width_stride[0] = W;
        fi.v_frame.height_stride[0] = H;
        fi.v_frame.virt_addr[1] = (unsigned char *)db + y_size;
        fi.v_frame.phys_addr[1] = (hi_u64)(size_t)((unsigned char *)db + y_size);
        fi.v_frame.width_stride[1] = W;
        fi.v_frame.height_stride[1] = H / 2;
        fi.v_frame.time_ref = 0;
        fi.v_frame.pts = 0; /* 实验：pts 恒定，验证是否为 pts 语义导致 */

        hi_user_frame_info ufi;
        memset(&ufi, 0, sizeof(ufi));
        ufi.user_frame = fi;
        ufi.user_rc_info.frame_type = VENC_FRAME_TYPE_NONE;
        ret = hi_mpi_venc_send_frame_ex(CHN, &ufi, -1);
        if (ret != 0) {
            printf("send_frame_ex %d ret=0x%x\n", f, ret);
            hi_venc_chn_status st;
            memset(&st, 0, sizeof(st));
            if (hi_mpi_venc_query_status(CHN, &st) == 0)
                printf("  query: left_pics=%u cur_packs=%u\n",
                       st.left_pics, st.cur_packs);
            continue;
        }
        sent++;

    }
    // 连发完毕，一次性排空（实验：验证 get_stream 与 send 穿插是否为失败根因）
    for (int k = 0; k < 60; k++) {
        hi_venc_pack packs[8];
        hi_venc_stream stream;
        memset(&stream, 0, sizeof(stream));
        memset(packs, 0, sizeof(packs));
        stream.pack = packs;
        stream.pack_cnt = 8;

        ret = hi_mpi_venc_get_stream(CHN, &stream, 100);
        if (ret != 0) { printf("drain 结束 ret=0x%x（已取 %d 次）\n", ret, k); break; }

        for (hi_u32 i = 0; i < stream.pack_cnt; i++) {
            size_t len = packs[i].len;
            if (!packs[i].addr || len == 0) continue;
            aclError g = aclrtMemcpy(xfer, len, packs[i].addr, len,
                                     ACL_MEMCPY_DEVICE_TO_HOST);
            if (g != ACL_SUCCESS) { printf("  D2H pack%d ret=%d len=%zu\n", i, (int)g, len); continue; }
            got += (int)fwrite(xfer, 1, len, fp);
        }
        hi_mpi_venc_release_stream(CHN, &stream);
    }
    fclose(fp);

    hi_mpi_venc_stop_chn(CHN);
    hi_mpi_venc_destroy_chn(CHN);
    printf("DONE sent=%d bytes=%d file=%s\n", sent, got, out);
    return got > 0 ? 0 : 5;
}
