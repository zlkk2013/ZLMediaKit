/*
 * AscendVdec.cpp —— 昇腾 310P DVPP 硬解码实现
 *
 * 整个文件只在 ENABLE_ASCEND_VENC=ON 时才参与编译（见 CMakeLists.txt），
 * 没有昇腾环境的构建完全不受影响。
 *
 * 关键点（都是真机踩出来的，改之前先读）：
 *   1. 送包缓冲必须是设备内存（hi_mpi_dvpp_malloc），host → 设备要 aclrtMemcpy；
 *   2. 输出的 v_frame.virt_addr[0/1] 也是设备内存，要 aclrtMemcpy 拷回 host；
 *   3. 一帧要从帧起点开始发送（首帧含 SPS/PPS），否则解出来宽高为 0；
 *   4. hi_mpi_vdec_get_frame 的第 4 参是纯出参，别 memset 预填。
 */
#include "AscendVdec.h"

#ifdef ENABLE_ASCEND_VENC

#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "AscendEnv.h"
#include "acl/acl.h"
#include "acl/dvpp/hi_dvpp_vdec.h"
#include "acl/dvpp/hi_media_common.h"
#include "acl/dvpp/hi_mpi_sys.h"

using namespace std;

namespace mediakit {

namespace {

constexpr int kMaxVdecChannels = 32; ///< SDK 号空间上限（整机 32，不是每卡）
mutex g_chan_mtx;
bool g_chan_used[kMaxVdecChannels] = {};

/// 送包超时：0 = 不等待。解码器内部缓冲有限，用非阻塞 + 少量重试更可控。
constexpr int kSendTimeoutMs = 0;
constexpr int kSendRetries = 40;
constexpr int kGetTimeoutMs = 0;

} // namespace

int AscendVdec::acquireChannel() {
    lock_guard<mutex> lck(g_chan_mtx);
    for (int i = 0; i < kMaxVdecChannels; i++) {
        if (!g_chan_used[i]) {
            g_chan_used[i] = true;
            return i;
        }
    }
    return -1;
}

void AscendVdec::releaseChannel(int channel) {
    if (channel < 0 || channel >= kMaxVdecChannels) {
        return;
    }
    lock_guard<mutex> lck(g_chan_mtx);
    g_chan_used[channel] = false;
}

struct AscendVdec::Impl {
    Config cfg;
    OnFrame cb;

    hi_u8 *stream_buf = nullptr; ///< 送包用的设备缓冲
    hi_u32 stream_buf_size = 0;
    hi_u8 *host_buf = nullptr;   ///< 送包前的 host 暂存
    hi_u8 *out_buf = nullptr;    ///< 解码输出缓冲（设备内存，驱动直接往里写）
    hi_u32 out_buf_size = 0;
    hi_u32 host_buf_size = 0;

    vector<uint8_t> nv12;        ///< 拷回 host 后的紧凑 NV12
    bool streaming = false;

    /// 把一帧设备内存里的 NV12 拷成紧凑 NV12 并回调
    uint32_t dropped = 0; ///< 取到句柄但拿不到有效图像的次数（诊断用）

    void emit(hi_video_frame_info &fi) {
        auto &vf = fi.v_frame;
        int w = (int) vf.width;
        int h = (int) vf.height;
        if (w <= 0 || h <= 0) {
            // 宽高为 0 说明这一帧没有真正的图像头（常见于首包没带 SPS/PPS）。
            // 注意：_out 是在调用 emit() 之前自增的，所以「out 在涨」不代表真出了帧，
            // 这里单独计数，避免再去猜。
            if (++dropped <= 3) {
                fprintf(stderr, "[ascend vdec] drop: width=%d height=%d stride0=%u virt0=%p\n",
                        w, h, (unsigned) vf.width_stride[0], vf.virt_addr[0]);
            }
            return;
        }
        // 注意字段名是 width_stride（不是 stride），设备侧可能按对齐给出大于 width 的跨度
        int y_stride = (int) vf.width_stride[0] > 0 ? (int) vf.width_stride[0] : w;
        int uv_stride = (int) vf.width_stride[1] > 0 ? (int) vf.width_stride[1] : y_stride;
        size_t need = (size_t) w * h * 3 / 2;
        nv12.resize(need);

        // 平面地址兜底：调用方给了 pic.vir_addr（我们自己的 out_buf）时，
        // 有些版本不回填 v_frame.virt_addr，此时解码数据就在 out_buf 里。
        const uint8_t *base_y = (const uint8_t *) vf.virt_addr[0];
        const uint8_t *base_uv = (const uint8_t *) vf.virt_addr[1];
        if (!base_y) {
            base_y = out_buf;
            base_uv = out_buf ? out_buf + (size_t) y_stride * h : nullptr;
        }
        if (!base_y || !base_uv) {
            if (++dropped <= 3) {
                fprintf(stderr, "[ascend vdec] drop: no plane addr (virt0=%p virt1=%p out=%p)\n",
                        vf.virt_addr[0], vf.virt_addr[1], out_buf);
            }
            return;
        }

        // Y 平面逐行拷（设备侧 stride 可能大于 width，需要紧凑化）
        for (int row = 0; row < h; row++) {
            auto src = base_y + (size_t) row * y_stride;
            auto dst = nv12.data() + (size_t) row * w;
            if (aclrtMemcpy(dst, w, src, w, ACL_MEMCPY_DEVICE_TO_HOST) != ACL_SUCCESS) {
                return;
            }
        }
        // UV 平面：NV12 是高度/2 行、每行 width 字节（interleaved）
        for (int row = 0; row < h / 2; row++) {
            auto src = base_uv + (size_t) row * uv_stride;
            auto dst = nv12.data() + (size_t) w * h + (size_t) row * w;
            if (aclrtMemcpy(dst, w, src, w, ACL_MEMCPY_DEVICE_TO_HOST) != ACL_SUCCESS) {
                return;
            }
        }
        if (cb) {
            cb(nv12.data(), w, h, (uint32_t) vf.pts);
        }
    }
};

AscendVdec::AscendVdec() = default;

AscendVdec::~AscendVdec() {
    close();
}

bool AscendVdec::available(int device) {
    const char *why = nullptr;
    if (!ascendEnv().acquire(device, why)) {
        return false;
    }
    ascendEnv().release();
    return true;
}

bool AscendVdec::open(const Config &cfg, OnFrame cb, std::string &err) {
    if (_opened) {
        err = "already opened";
        return false;
    }
    const char *why = nullptr;
    if (!ascendEnv().acquire(cfg.device, why)) {
        err = why ? why : "DVPP 初始化失败";
        return false;
    }
    // 仓库统一 -std=gnu++11，用不了 std::make_unique
    std::unique_ptr<Impl> impl(new Impl());
    impl->cfg = cfg;
    impl->cb = cb;

    hi_vdec_chn_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.type = cfg.h265 ? HI_PT_H265 : HI_PT_H264;
    attr.mode = HI_VDEC_SEND_MODE_FRAME; // 按帧送：ZLM 侧本来就是按帧切好的
    attr.pic_width = (hi_u32) cfg.width;
    attr.pic_height = (hi_u32) cfg.height;
    // 送包缓冲：按最大帧的 2 倍给（一个 IDR 可能比平均帧大很多）
    attr.stream_buf_size = (hi_u32) cfg.width * cfg.height * 3 / 2 * 2;
    attr.frame_buf_cnt = (hi_u32) (cfg.ref_frames + cfg.display_frames + 1);

    hi_pic_buf_attr buf_attr;
    memset(&buf_attr, 0, sizeof(buf_attr));
    buf_attr.width = (hi_u32) cfg.width;
    buf_attr.height = (hi_u32) cfg.height;
    buf_attr.align = 0;
    buf_attr.bit_width = HI_DATA_BIT_WIDTH_8;
    buf_attr.pixel_format = HI_PIXEL_FORMAT_YUV_SEMIPLANAR_420;
    buf_attr.compress_mode = HI_COMPRESS_MODE_NONE;
    // 注意：这个函数返回的是**尺寸**不是错误码（1080p 约 0x305800）
    attr.frame_buf_size = hi_vdec_get_pic_buf_size(attr.type, &buf_attr);

    attr.video_attr.ref_frame_num = (hi_u32) cfg.ref_frames;
    attr.video_attr.temporal_mvp_en = HI_FALSE;
    attr.video_attr.tmv_buf_size = (hi_u32) cfg.width * cfg.height * 3 / 2;

    auto ret = hi_mpi_vdec_create_chn(cfg.channel, &attr);
    if (ret != 0) {
        char buf[96];
        snprintf(buf, sizeof(buf), "hi_mpi_vdec_create_chn 失败 0x%x（通道 %d）", ret, cfg.channel);
        err = buf;
        ascendEnv().release();
        return false;
    }

    // 建通道之后必须显式开始收流，否则 send_stream 一律返回 0xA0058009(NOT_PERM)。
    // create_chn 只是分配资源，通道还处于 stop 状态。
    ret = hi_mpi_vdec_start_recv_stream(cfg.channel);
    if (ret != 0) {
        char buf[96];
        snprintf(buf, sizeof(buf), "hi_mpi_vdec_start_recv_stream 失败 0x%x", ret);
        err = buf;
        hi_mpi_vdec_destroy_chn(cfg.channel);
        ascendEnv().release();
        return false;
    }
    impl->streaming = true;

    impl->stream_buf_size = attr.stream_buf_size;
    if (hi_mpi_dvpp_malloc(cfg.device, (void **) &impl->stream_buf, impl->stream_buf_size) != 0) {
        err = "hi_mpi_dvpp_malloc(stream_buf) 失败";
        hi_mpi_vdec_stop_recv_stream(cfg.channel);
        hi_mpi_vdec_destroy_chn(cfg.channel);
        ascendEnv().release();
        return false;
    }
    impl->host_buf_size = impl->stream_buf_size;
    impl->host_buf = (hi_u8 *) malloc(impl->host_buf_size);

    // 输出缓冲必须由调用方提供并告诉驱动（pic.vir_addr）：给 0 会被判 BAD_ADDR(0xA0058011)。
    impl->out_buf_size = attr.frame_buf_size;
    if (hi_mpi_dvpp_malloc(cfg.device, (void **) &impl->out_buf, impl->out_buf_size) != 0) {
        err = "hi_mpi_dvpp_malloc(out_buf) 失败";
        hi_mpi_dvpp_free(impl->stream_buf);
        hi_mpi_vdec_stop_recv_stream(cfg.channel);
        hi_mpi_vdec_destroy_chn(cfg.channel);
        free(impl->host_buf);
        ascendEnv().release();
        return false;
    }

    _impl = std::move(impl);
    _opened = true;
    return true;
}

bool AscendVdec::decode(const uint8_t *annexb, size_t len, uint32_t pts_ms) {
    if (!_opened || !_impl) {
        return false;
    }
    auto &im = *_impl;
    if (!annexb || len == 0) {
        return false;
    }
    if (len > im.stream_buf_size) {
        // 超过送包缓冲：直接丢，强行送会被驱动判非法
        return false;
    }
    // 每个调用线程都要重绑 context（ACL context 按线程生效）
    aclrtSetCurrentContext((aclrtContext) ascendEnv().ctx);

    memcpy(im.host_buf, annexb, len);
    if (aclrtMemcpy(im.stream_buf, im.stream_buf_size, im.host_buf, len,
                    ACL_MEMCPY_HOST_TO_DEVICE) != ACL_SUCCESS) {
        return false;
    }

    hi_vdec_stream st;
    memset(&st, 0, sizeof(st));
    st.addr = im.stream_buf;
    st.len = (hi_u32) len;
    st.pts = pts_ms;
    st.end_of_frame = HI_TRUE;   // ZLM 给的本来就是整帧
    st.end_of_stream = HI_FALSE;
    st.need_display = HI_TRUE;

    hi_vdec_pic_info pic;
    memset(&pic, 0, sizeof(pic));
    // 输出缓冲由驱动按 frame_buf_size 自行分配，这里只需给缓存属性
    pic.width = (hi_u32) im.cfg.width;
    pic.height = (hi_u32) im.cfg.height;
    pic.width_stride = (hi_u32) im.cfg.width;
    pic.height_stride = (hi_u32) im.cfg.height;
    pic.pixel_format = HI_PIXEL_FORMAT_YUV_SEMIPLANAR_420;
    pic.buffer_size = im.out_buf_size;
    pic.vir_addr = (hi_u64) im.out_buf;   // 必须给真实设备地址

    auto ret = 0;
    for (int t = 0; t < kSendRetries; t++) {
        ret = hi_mpi_vdec_send_stream(im.cfg.channel, &st, &pic, kSendTimeoutMs);
        if (ret == 0) {
            break;
        }
        usleep(1000); // 缓冲满时退避重投
    }
    if (ret != 0) {
        static int warn = 0;
        if (warn++ < 5) {
            fprintf(stderr, "[ascend vdec] send_stream 失败 0x%x len=%zu\n", ret, len);
        }
        return false;
    }
    _in++;
    if (_in % 500 == 0) {
        // 这版 SDK 没有 hi_mpi_vdec_get_chn_status（只有 get_chn_attr），状态只能自己计数。
        // 每 500 帧一行：既能确认硬解在跑，又不会刷屏（25fps 下约 20 秒一行）。
        fprintf(stderr, "[ascend vdec] in=%u out=%u dropped=%u\n", _in, _out, im.dropped);
    }

    // 取走当前所有已就绪的帧
    for (;;) {
        hi_video_frame_info fi;
        hi_vdec_supplement_info supp;
        hi_vdec_stream out_st;
        memset(&fi, 0, sizeof(fi));
        memset(&supp, 0, sizeof(supp));
        // 第 4 参是纯出参，官方从不预填
        auto gret = hi_mpi_vdec_get_frame(im.cfg.channel, &fi, &supp, &out_st, kGetTimeoutMs);
        if (gret != 0) {
            break;
        }
        _out++;
        im.emit(fi);
        hi_mpi_vdec_release_frame(im.cfg.channel, &fi);
    }
    return true;
}

void AscendVdec::flush() {
    if (!_opened || !_impl) {
        return;
    }
    auto &im = *_impl;
    aclrtSetCurrentContext((aclrtContext) ascendEnv().ctx);

    hi_vdec_stream st;
    memset(&st, 0, sizeof(st));
    st.addr = im.stream_buf;
    st.len = 0;
    st.end_of_frame = HI_TRUE;
    st.end_of_stream = HI_TRUE;
    st.need_display = HI_FALSE;

    hi_vdec_pic_info pic;
    memset(&pic, 0, sizeof(pic));
    hi_mpi_vdec_send_stream(im.cfg.channel, &st, &pic, kSendTimeoutMs);

    // 排空剩余帧
    for (int t = 0; t < 100; t++) {
        hi_video_frame_info fi;
        hi_vdec_supplement_info supp;
        hi_vdec_stream out_st;
        memset(&fi, 0, sizeof(fi));
        memset(&supp, 0, sizeof(supp));
        if (hi_mpi_vdec_get_frame(im.cfg.channel, &fi, &supp, &out_st, 10) != 0) {
            break;
        }
        _out++;
        im.emit(fi);
        hi_mpi_vdec_release_frame(im.cfg.channel, &fi);
    }
}

void AscendVdec::close() {
    if (!_opened || !_impl) {
        return;
    }
    auto &im = *_impl;
    if (im.streaming) {
        hi_mpi_vdec_stop_recv_stream(im.cfg.channel);
    }
    hi_mpi_vdec_destroy_chn(im.cfg.channel);
    if (im.stream_buf) {
        hi_mpi_dvpp_free(im.stream_buf);
        im.stream_buf = nullptr;
    }
    if (im.host_buf) {
        free(im.host_buf);
        im.host_buf = nullptr;
    }
    if (im.out_buf) {
        hi_mpi_dvpp_free(im.out_buf);
        im.out_buf = nullptr;
    }
    releaseChannel(im.cfg.channel);
    ascendEnv().release();
    _impl.reset();
    _opened = false;
}

} // namespace mediakit

#endif // ENABLE_ASCEND_VENC
