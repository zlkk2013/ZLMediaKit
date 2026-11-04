/*
 * ascend_zero_copy_probe.cpp —— 昇腾 VDEC → VENC 零拷贝可行性探针
 *
 * 对比两条路径（同一份 ES、同一个编码器、同一组参数）：
 *   --copy  VDEC 输出 → aclrtMemcpy(D2H) → host NV12 → aclrtMemcpy(H2D) → VENC  （生产现状）
 *   --zero  VDEC 输出的设备地址 → 直接交给 VENC（不做任何主机往返）
 *
 * 判据（缺一不可）：
 *   1) 两条路径都能编出可解码的 H.265，帧数接近；
 *   2) 画面与源一致（PSNR 与源比对，>=30dB 视为正常）；
 *   3) --zero 每帧耗时明显低于 --copy，否则「省了拷贝」没意义。
 *
 * 用法：./zc_probe in.h264 out.h265 --zero|--copy [帧数]
 *
 * 踩过的坑（每条都是实测）：
 *   - VDEC 必须由调用方给输出缓冲（pic.vir_addr），给 0 是 BAD_ADDR(0xA0058011)；
 *     vir_addr 与 buffer_size 都不给是 BAD_PARAM(0xA0058003)。
 *   - VENC create 之后必须 set_scene_mode + get/set_rc_param 往返，缺了出不了有效码流。
 *   - VDEC 回填的 v_frame.width/height 可能是 0，必须按配置自己构造帧描述。
 *   - 送帧后要等编码器出包再读，紧接着读会拿到没写完的包缓冲（一堆 32 字节描述符，
 *     不是 00 00 01 开头的 NAL）。
 *   - 零拷贝时必须等编码器消费完才能 hi_mpi_vdec_release_frame，否则缓冲被覆盖。
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <time.h>
#include <unistd.h>

#include "acl/acl.h"
#include "acl/dvpp/hi_dvpp_vdec.h"
#include "acl/dvpp/hi_dvpp_venc.h"
#include "acl/dvpp/hi_media_common.h"
#include "acl/dvpp/hi_mpi_sys.h"

using namespace std;

namespace {

constexpr int kDev = 0;
constexpr int kChnVdec = 0;
constexpr int kChnVenc = 0;   // 与生产一致：VENC 从 0 开始（和 VDEC 是两个模块，通道号不冲突）
constexpr int kW = 1280;
constexpr int kH = 720;
constexpr size_t kFrameBytes = (size_t) kW * kH * 3 / 2;

aclrtContext g_ctx = nullptr;
// VDEC 要留参考帧：只给一块输出缓冲会被它自己覆写（实测画面和源对不上），
// 必须按 frame_buf_cnt 给一组缓冲轮转使用。
constexpr int kOutBufs = 8;
hi_u8 *g_out_bufs[kOutBufs] = {nullptr};
hi_u8 *g_out_buf = nullptr;     ///< 最近一次用的那块（兜底用）
hi_u32 g_out_buf_size = 0;
int g_out_idx = 0;

bool initAcl() {
    aclInit(nullptr);
    if (aclrtSetDevice(kDev) != ACL_SUCCESS) {
        return false;
    }
    if (aclrtCreateContext(&g_ctx, kDev) != ACL_SUCCESS) {
        return false;
    }
    if (aclrtSetCurrentContext(g_ctx) != ACL_SUCCESS) {
        return false;
    }
    return hi_mpi_sys_init() == 0;
}

bool allocVdecOutBuf() {
    hi_pic_buf_attr pba;
    memset(&pba, 0, sizeof(pba));
    pba.width = kW;
    pba.height = kH;
    pba.bit_width = HI_DATA_BIT_WIDTH_8;
    pba.pixel_format = HI_PIXEL_FORMAT_YUV_SEMIPLANAR_420;
    pba.compress_mode = HI_COMPRESS_MODE_NONE;
    g_out_buf_size = hi_vdec_get_pic_buf_size(HI_PT_H264, &pba);
    for (int i = 0; i < kOutBufs; i++) {
        if (hi_mpi_dvpp_malloc(kDev, (void **) &g_out_bufs[i], g_out_buf_size) != 0) {
            return false;
        }
    }
    g_out_buf = g_out_bufs[0];
    fprintf(stderr, "[probe] vdec out_bufs=%p..%p x%d size=%u\n", (void *) g_out_bufs[0],
            (void *) g_out_bufs[kOutBufs - 1], kOutBufs, g_out_buf_size);
    return true;
}

bool openVdec() {
    hi_vdec_chn_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.type = HI_PT_H264;
    attr.mode = HI_VDEC_SEND_MODE_FRAME;
    attr.pic_width = kW;
    attr.pic_height = kH;
    attr.stream_buf_size = kW * kH * 3;
    attr.frame_buf_cnt = 8 + 2 + 1;
    attr.frame_buf_size = g_out_buf_size;
    attr.video_attr.ref_frame_num = 8;
    attr.video_attr.tmv_buf_size = kW * kH * 3 / 2;
    if (hi_mpi_vdec_create_chn(kChnVdec, &attr) != 0) {
        return false;
    }
    return hi_mpi_vdec_start_recv_stream(kChnVdec) == 0;
}

bool openVenc() {
    hi_venc_chn_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.venc_attr.type = HI_PT_H265;
    attr.venc_attr.max_pic_width = kW;
    attr.venc_attr.max_pic_height = kH;
    attr.venc_attr.pic_width = kW;
    attr.venc_attr.pic_height = kH;
    attr.venc_attr.buf_size = 2 * 1024 * 1024;
    attr.venc_attr.profile = 0;
    attr.venc_attr.is_by_frame = HI_TRUE;
    attr.venc_attr.h265_attr.rcn_ref_share_buf_en = HI_FALSE;
    attr.rc_attr.rc_mode = HI_VENC_RC_MODE_H265_CBR;
    attr.rc_attr.h265_cbr.gop = 25;
    attr.rc_attr.h265_cbr.stats_time = 1;
    attr.rc_attr.h265_cbr.src_frame_rate = 25;
    attr.rc_attr.h265_cbr.dst_frame_rate = 25;
    attr.rc_attr.h265_cbr.bit_rate = 4000;
    attr.gop_attr.gop_mode = HI_VENC_GOP_MODE_NORMAL_P;
    attr.gop_attr.normal_p.ip_qp_delta = 2;

    if (hi_mpi_venc_create_chn(kChnVenc, &attr) != 0) {
        return false;
    }
    hi_mpi_venc_set_scene_mode(kChnVenc, HI_VENC_SCENE_0);
    hi_venc_rc_param rcp;
    memset(&rcp, 0, sizeof(rcp));
    if (hi_mpi_venc_get_rc_param(kChnVenc, &rcp) == 0) {
        hi_mpi_venc_set_rc_param(kChnVenc, &rcp);
    }
    hi_venc_start_param sp;
    sp.recv_pic_num = -1;
    return hi_mpi_venc_start_chn(kChnVenc, &sp) == 0;
}

int collectOnce(FILE *out, int *bytes) {
    hi_venc_pack packs[64];
    hi_venc_stream st;
    memset(&st, 0, sizeof(st));
    memset(packs, 0, sizeof(packs));
    st.pack = packs;
    st.pack_cnt = 64;
    if (hi_mpi_venc_get_stream(kChnVenc, &st, 5) != 0) {
        return 0;
    }
    int wrote = 0;
    for (hi_u32 i = 0; i < st.pack_cnt && i < 64; i++) {
        if (!packs[i].addr || !packs[i].len || packs[i].len > (32u << 20)) {
            continue;
        }
        vector<uint8_t> buf(packs[i].len);
        if (aclrtMemcpy(buf.data(), buf.size(), packs[i].addr, packs[i].len,
                        ACL_MEMCPY_DEVICE_TO_HOST) == ACL_SUCCESS) {
            fwrite(buf.data(), 1, buf.size(), out);
            *bytes += (int) buf.size();
            wrote += (int) buf.size();
        }
    }
    hi_mpi_venc_release_stream(kChnVenc, &st);
    return wrote;
}

int drainVenc(FILE *out, int *bytes) {
    int total = 0;
    for (int w = 0; w < 20; w++) {
        total += collectOnce(out, bytes);
        if (total > 0) {
            break;
        }
        usleep(1000);
    }
    for (int k = 0; k < 3; k++) {
        int n = collectOnce(out, bytes);
        if (n == 0) {
            break;
        }
        total += n;
    }
    return total;
}

bool isVclH264(const uint8_t *d, size_t n) {
    if (n < 1) {
        return false;
    }
    int t = d[0] & 0x1F;
    return t == 1 || t == 5;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "用法: %s in.h264 out.h265 --zero|--copy [帧数]\n", argv[0]);
        return 2;
    }
    const char *in_path = argv[1];
    const char *out_path = argv[2];
    bool zero = strcmp(argv[3], "--zero") == 0;
    int max_frames = (argc > 4) ? atoi(argv[4]) : 100;

    FILE *fp = fopen(in_path, "rb");
    if (!fp) {
        fprintf(stderr, "打不开 %s\n", in_path);
        return 2;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    vector<uint8_t> es((size_t) (sz > 0 ? sz : 0));
    size_t rd = fread(es.data(), 1, es.size(), fp);
    fclose(fp);
    es.resize(rd);
    if (es.size() < 1024) {
        fprintf(stderr, "输入太小\n");
        return 2;
    }

    if (!initAcl()) {
        fprintf(stderr, "ACL 初始化失败\n");
        return 1;
    }
    if (!allocVdecOutBuf()) {
        fprintf(stderr, "dvpp_malloc(vdec out) 失败\n");
        return 1;
    }
    if (!openVdec()) {
        fprintf(stderr, "VDEC 打开失败\n");
        return 1;
    }
    if (!openVenc()) {
        fprintf(stderr, "VENC 打开失败\n");
        return 1;
    }

    hi_u8 *dev_in = nullptr;
    hi_u32 dev_in_size = kW * kH * 3;
    if (hi_mpi_dvpp_malloc(kDev, (void **) &dev_in, dev_in_size) != 0) {
        fprintf(stderr, "dvpp_malloc(dev_in) 失败\n");
        return 1;
    }
    vector<uint8_t> host_nv12(kFrameBytes);
    hi_u8 *dev_nv12 = nullptr;
    if (!zero && hi_mpi_dvpp_malloc(kDev, (void **) &dev_nv12, kFrameBytes) != 0) {
        fprintf(stderr, "dvpp_malloc(dev_nv12) 失败\n");
        return 1;
    }

    FILE *out = fopen(out_path, "wb");
    if (!out) {
        return 2;
    }

    int sent = 0, decoded = 0, encoded = 0, out_bytes = 0;
    uint64_t pts = 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    auto feed = [&](const uint8_t *data, size_t len) {
        if (len > dev_in_size) {
            return;
        }
        aclrtMemcpy(dev_in, dev_in_size, data, len, ACL_MEMCPY_HOST_TO_DEVICE);
        hi_vdec_stream st;
        memset(&st, 0, sizeof(st));
        st.addr = dev_in;
        st.len = (hi_u32) len;
        st.pts = pts;
        st.end_of_frame = HI_TRUE;
        st.need_display = HI_TRUE;
        hi_vdec_pic_info pic;
        memset(&pic, 0, sizeof(pic));
        pic.width = kW;
        pic.height = kH;
        pic.width_stride = kW;
        pic.height_stride = kH;
        pic.pixel_format = HI_PIXEL_FORMAT_YUV_SEMIPLANAR_420;
        pic.buffer_size = g_out_buf_size;
        hi_u8 *cur_buf = g_out_bufs[g_out_idx % kOutBufs];
        g_out_idx++;
        pic.vir_addr = (hi_u64) cur_buf;
        if (hi_mpi_vdec_send_stream(kChnVdec, &st, &pic, 0) != 0) {
            return;
        }
        sent++;

        for (;;) {
            hi_video_frame_info fi;
            hi_vdec_supplement_info supp;
            hi_vdec_stream ost;
            memset(&fi, 0, sizeof(fi));
            memset(&supp, 0, sizeof(supp));
            memset(&ost, 0, sizeof(ost));
            if (hi_mpi_vdec_get_frame(kChnVdec, &fi, &supp, &ost, 0) != 0) {
                break;
            }
            decoded++;

            // VDEC 回填的 width/height 可能是 0，自己按配置构造
            hi_video_frame_info vfi;
            memset(&vfi, 0, sizeof(vfi));
            vfi.v_frame.width = kW;
            vfi.v_frame.height = kH;
            vfi.v_frame.field = HI_VIDEO_FIELD_FRAME;
            vfi.v_frame.pixel_format = HI_PIXEL_FORMAT_YUV_SEMIPLANAR_420;
            vfi.v_frame.video_format = HI_VIDEO_FORMAT_LINEAR;
            vfi.v_frame.dynamic_range = HI_DYNAMIC_RANGE_SDR8;
            vfi.v_frame.compress_mode = HI_COMPRESS_MODE_NONE;
            vfi.v_frame.color_gamut = HI_COLOR_GAMUT_BT709;
            vfi.v_frame.width_stride[0] = kW;
            vfi.v_frame.height_stride[0] = kH;
            vfi.v_frame.width_stride[1] = kW;
            vfi.v_frame.height_stride[1] = kH / 2;
            vfi.v_frame.time_ref = (hi_u64) (decoded * 2);
            vfi.v_frame.pts = (hi_u64) fi.v_frame.pts;

            hi_u8 *y_dev = (hi_u8 *) fi.v_frame.virt_addr[0];
            if (!y_dev) {
                y_dev = cur_buf;
            }
            if (zero) {
                vfi.v_frame.virt_addr[0] = y_dev;
                vfi.v_frame.virt_addr[1] = y_dev + (size_t) kW * kH;
            } else {
                hi_u8 *u_dev = (hi_u8 *) fi.v_frame.virt_addr[1];
                if (!u_dev) {
                    u_dev = y_dev + (size_t) kW * kH;
                }
                aclrtMemcpy(host_nv12.data(), (size_t) kW * kH, y_dev, (size_t) kW * kH,
                            ACL_MEMCPY_DEVICE_TO_HOST);
                aclrtMemcpy(host_nv12.data() + (size_t) kW * kH, (size_t) kW * kH / 2, u_dev,
                            (size_t) kW * kH / 2, ACL_MEMCPY_DEVICE_TO_HOST);
                aclrtMemcpy(dev_nv12, kFrameBytes, host_nv12.data(), kFrameBytes,
                            ACL_MEMCPY_HOST_TO_DEVICE);
                vfi.v_frame.virt_addr[0] = dev_nv12;
                vfi.v_frame.virt_addr[1] = dev_nv12 + (size_t) kW * kH;
            }

            hi_user_frame_info ufi;
            memset(&ufi, 0, sizeof(ufi));
            ufi.user_frame = vfi;
            ufi.user_rc_info.frame_type = VENC_FRAME_TYPE_NONE;
            if (hi_mpi_venc_send_frame_ex(kChnVenc, &ufi, 0) == 0) {
                encoded++;
            }
            usleep(2000);
            drainVenc(out, &out_bytes);
            hi_mpi_vdec_release_frame(kChnVdec, &fi);
            if (encoded >= max_frames) {
                return;
            }
        }
    };

    vector<uint8_t> au;
    size_t pos = 0;
    while (pos + 4 < es.size() && encoded < max_frames) {
        size_t start = pos;
        while (pos + 4 < es.size()) {
            if (es[pos] == 0 && es[pos + 1] == 0 && es[pos + 2] == 1) {
                break;
            }
            if (es[pos] == 0 && es[pos + 1] == 0 && es[pos + 2] == 0 && es[pos + 3] == 1) {
                break;
            }
            pos++;
        }
        size_t sc = (es[pos + 2] == 0) ? 4 : 3;
        size_t ns = pos + sc;
        size_t j = ns;
        while (j + 4 < es.size()) {
            if (es[j] == 0 && es[j + 1] == 0 && es[j + 2] == 1) {
                break;
            }
            if (es[j] == 0 && es[j + 1] == 0 && es[j + 2] == 0 && es[j + 3] == 1) {
                break;
            }
            j++;
        }
        if (j <= ns) {
            pos = ns;
            continue;
        }
        bool vcl = isVclH264(&es[ns], j - ns);
        au.insert(au.end(), &es[start], &es[j]);
        if (vcl) {
            feed(au.data(), au.size());
            au.clear();
        }
        pos = j;
    }
    drainVenc(out, &out_bytes);
    fclose(out);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double sec = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("%s: 送包=%d 解码=%d 编码=%d 输出=%d 字节 用时=%.2fs → %.1f fps, 每帧 %.2f ms\n",
           zero ? "零拷贝" : "拷贝  ", sent, decoded, encoded, out_bytes, sec,
           sec > 0 ? encoded / sec : 0, encoded ? sec * 1000 / encoded : 0);

    hi_mpi_venc_stop_chn(kChnVenc);
    hi_mpi_venc_destroy_chn(kChnVenc);
    hi_mpi_vdec_stop_recv_stream(kChnVdec);
    hi_mpi_vdec_destroy_chn(kChnVdec);
    if (dev_in) hi_mpi_dvpp_free(dev_in);
    if (dev_nv12) hi_mpi_dvpp_free(dev_nv12);
    for (int i = 0; i < kOutBufs; i++) {
        if (g_out_bufs[i]) hi_mpi_dvpp_free(g_out_bufs[i]);
    }
    return encoded > 0 ? 0 : 1;
}
