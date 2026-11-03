/*
 * AscendVenc.cpp —— 昇腾 310P DVPP 硬编码实现
 *
 * 整个文件只在 ENABLE_ASCEND_VENC=ON 时才参与编译（见 CMakeLists.txt），
 * 因此没有昇腾环境的构建完全不受影响。
 */
#include "AscendVenc.h"

#ifdef ENABLE_ASCEND_VENC

#include <pthread.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <vector>

#include "acl/acl.h"
#include "acl/dvpp/hi_dvpp_venc.h"
#include "acl/dvpp/hi_media_common.h"
#include "acl/dvpp/hi_mpi_sys.h"

using namespace std;

namespace mediakit {

namespace {

/// 进程级 DVPP 初始化（引用计数，避免多路编码器重复 aclInit / sys_init）
struct DvppEnv {
    mutex mtx;
    int ref = 0;
    int device = -1;
    aclrtContext ctx = nullptr;
    bool ok = false;
    bool inited = false;  ///< aclInit/sys_init 只能做一次（重复调用返回失败）

    bool acquire(int dev, const char *&why) {
        lock_guard<mutex> lck(mtx);
        if (inited) {
            if (dev != device) {
                why = "同一进程内已用其它 device 初始化过 DVPP";
                return false;
            }
            // 已初始化：只需为当前线程重绑上下文（runtime 参数校验依赖它）
            if (aclrtSetCurrentContext(ctx) != ACL_SUCCESS) {
                why = "aclrtSetCurrentContext 失败";
                return false;
            }
            ref++;
            return true;
        }
        // aclInit 在"进程里别人已经初始化过"时会返回失败——这不是错误。
        // 同一进程里 VDEC 封装/推理后端都可能先调过，因此这里只当警告，继续尝试建上下文。
        if (aclInit(nullptr) != ACL_SUCCESS) {
            // 不直接失败：后面的 setDevice 才是真正的判据
        }
        if (aclrtSetDevice(dev) != ACL_SUCCESS) {
            // 设备已被别的模块 set 过也可能返回失败，再看能否建上下文
            if (aclrtCreateContext(&ctx, dev) != ACL_SUCCESS) {
                why = "aclrtSetDevice/aclrtCreateContext 均失败";
                return false;
            }
        }
        if (!ctx && aclrtCreateContext(&ctx, dev) != ACL_SUCCESS) {
            why = "aclrtCreateContext 失败";
            return false;
        }
        if (aclrtSetCurrentContext(ctx) != ACL_SUCCESS) { why = "aclrtSetCurrentContext 失败"; return false; }
        auto ret = hi_mpi_sys_init();
        if (ret != 0) {
            char buf[64];
            snprintf(buf, sizeof(buf), "hi_mpi_sys_init 失败 0x%x", ret);
            why = buf;
            return false;
        }
        device = dev;
        ref = 1;
        ok = true;
        inited = true;
        return true;
    }

    void release() {
        lock_guard<mutex> lck(mtx);
        if (ref > 0) {
            // 仅减计数：不主动 destroy context / reset device / aclFinalize。
            // 真机上这些动作有崩溃风险，且后续编码器还要复用同一套初始化
            // （VDEC 封装里踩过同样的坑）。进程退出时由系统回收。
            --ref;
        }
    }
};

DvppEnv &env() {
    static DvppEnv e;
    return e;
}

/// Annex-B 码流里判断是否关键帧。
/// 必须扫描**全部** NAL：一个访问单元常以 SPS/PPS/SEI 打头，IDR 切片在后面
/// （只看首 NAL 会把所有关键帧判成非关键帧，实测 keys=0）。
bool isKeyFrame(const uint8_t *d, size_t n, bool h265) {
    size_t i = 0;
    while (i + 3 < n) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            const uint8_t *nal = d + i + 3;
            if (i + 4 < n) {
                if (h265) {
                    // 16~21: BLA_W_LP / BLA_W_RADL / BLA_N_LP / IDR_W_RADL / IDR_N_LP / CRA
                    int t = (nal[0] >> 1) & 0x3F;
                    if (t >= 16 && t <= 21) return true;
                    if (t >= 32 && t <= 34) return true; // VPS/SPS/PPS 只在 IRAP 前后出现，保守算作关键帧
                } else {
                    int t = nal[0] & 0x1F;
                    if (t == 5) return true;
                    if (t == 7 || t == 8) return true;   // SPS/PPS 随关键帧一起出现
                }
            }
            i += 3;
            continue;
        }
        i++;
    }
    return false;
}

} // namespace

struct AscendVenc::Impl {
    Config cfg;
    OnPacket cb;
    bool h265 = false;

    hi_u32 frame_size = 0;
    vector<void *> dev_buf;
    vector<uint8_t *> host_buf;      ///< 与 dev_buf 一一对应的 host 暂存
    vector<uint8_t> xfer;            ///< 取流时 D2H 的中转缓冲
    vector<char> au;                 ///< 拼接同一个访问单元的所有 pack

    pthread_t th{};
    atomic<bool> stop{false};
    atomic<bool> running{false};
    mutex send_mtx;
    uint32_t idx = 0;                ///< 帧序号（time_ref 用 idx*2）

    // 统计
    uint32_t local_out = 0;

    // ---- 取流线程 ----
    static void *threadEntry(void *p) {
        auto self = static_cast<Impl *>(p);
        self->loop();
        return nullptr;
    }

    /// 取一次码流（非阻塞），把拿到的包全部回调出去。返回本次取到的 pack 数。
    int drainOnce() {
        static constexpr int kMaxPacks = 64;
        hi_venc_pack packs[kMaxPacks];
        hi_venc_stream stream;
        memset(&stream, 0, sizeof(stream));
        memset(packs, 0, sizeof(packs));
        stream.pack = packs;
        stream.pack_cnt = kMaxPacks;

        // 非阻塞(0)这条路径在本进程内会卡住不返回，必须给正超时。
        auto ret = hi_mpi_venc_get_stream(cfg.channel, &stream, 5);
        if (ret != 0) {
            return 0;
        }
        // 一次 get_stream 返回的所有 pack 属于**同一个访问单元**（IDR 会是
        // AUD/VPS/SPS/PPS/SEI/切片…）。必须拼成一帧再回调：按 pack 逐个回调等于把
        // 一帧拆成多帧，实测入口 27fps 却输出 38fps，下游还会拿到半帧。
        // All packs of one get_stream() belong to the SAME access unit, so they must be
        // concatenated and emitted once. Emitting per pack splits a frame into several.
        au.clear();
        uint64_t au_pts = 0;
        for (hi_u32 i = 0; i < stream.pack_cnt && i < (hi_u32) kMaxPacks; i++) {
            size_t len = packs[i].len;
            if (!packs[i].addr || len == 0 || len > (32u << 20)) {
                continue;
            }
            if (xfer.size() < len) {
                xfer.resize(len);
            }
            if (aclrtMemcpy(xfer.data(), len, packs[i].addr, len,
                            ACL_MEMCPY_DEVICE_TO_HOST) != ACL_SUCCESS) {
                continue;
            }
            if (au.empty()) {
                au_pts = packs[i].pts;
            }
            au.insert(au.end(), (const char *) xfer.data(), (const char *) xfer.data() + len);
        }
        hi_mpi_venc_release_stream(cfg.channel, &stream);
        if (au.empty()) {
            return 0;
        }
        local_out++;
        if (cb) {
            cb(au.data(), au.size(), au_pts,
               isKeyFrame((const uint8_t *) au.data(), au.size(), h265));
        }
        return 1;
    }

    void loop() {
        // 线程内必须重绑上下文，否则 runtime 参数校验失败
        {
            auto dev_rc = aclrtSetDevice(cfg.device);
            auto ctx_rc = aclrtSetCurrentContext(env().ctx);
            fprintf(stderr, "[ascend venc] get-thread bind: setDevice=%d setCtx=%d ctx=%p dev=%d\n",
                    (int) dev_rc, (int) ctx_rc, (void *) env().ctx, cfg.device);
        }
        while (!stop.load()) {
            // 一个访问单元可能吐出多个 pack（IDR = AUD/VPS/SPS/PPS/SEI/切片…）。
            // 数组小了驱动会写越界，之后读到野指针长度 → aclrtMemcpy 段错误（实测）。
            static constexpr int kMaxPacks = 64;
            hi_venc_pack packs[kMaxPacks];
            hi_venc_stream stream;
            memset(&stream, 0, sizeof(stream));
            memset(packs, 0, sizeof(packs));
            stream.pack = packs;
            stream.pack_cnt = kMaxPacks;

            auto ret = hi_mpi_venc_get_stream(cfg.channel, &stream, 0);
            if (ret != 0) {
                // 头 200 次打印返回码：区分「暂时没数据」和「通道不对/上下文没绑」
                static int dbg = 0;
                if (dbg < 200 && (dbg++ % 40) == 0) {
                    fprintf(stderr, "[ascend venc] get_stream ch=%d ret=0x%x ctx=%p\n",
                            cfg.channel, ret, (void *) env().ctx);
                }
                usleep(500); // 非阻塞轮询
                continue;
            }
            if (stream.pack_cnt > (hi_u32) kMaxPacks) {
                fprintf(stderr, "[ascend venc] pack_cnt=%u exceeds %d\n",
                        (unsigned) stream.pack_cnt, kMaxPacks);
            }
            for (hi_u32 i = 0; i < stream.pack_cnt && i < (hi_u32) kMaxPacks; i++) {
                size_t len = packs[i].len;
                if (!packs[i].addr || len == 0 || len > (32u << 20)) {
                    continue;
                }
                if (xfer.size() < len) {
                    xfer.resize(len);
                }
                if (aclrtMemcpy(xfer.data(), len, packs[i].addr, len,
                                ACL_MEMCPY_DEVICE_TO_HOST) != ACL_SUCCESS) {
                    continue;
                }
                local_out++;
                if (cb) {
                    cb((const char *) xfer.data(), len, packs[i].pts,
                       isKeyFrame(xfer.data(), len, h265));
                }
            }
            hi_mpi_venc_release_stream(cfg.channel, &stream);
        }
    }
};

namespace {
constexpr int kMaxVencChannels = 16;
std::mutex g_chan_mtx;
bool g_chan_used[kMaxVencChannels] = {};
} // namespace

int AscendVenc::acquireChannel() {
    lock_guard<mutex> lck(g_chan_mtx);
    for (int i = 0; i < kMaxVencChannels; i++) {
        if (!g_chan_used[i]) {
            g_chan_used[i] = true;
            return i;
        }
    }
    return -1;
}

void AscendVenc::releaseChannel(int channel) {
    if (channel < 0 || channel >= kMaxVencChannels) {
        return;
    }
    lock_guard<mutex> lck(g_chan_mtx);
    g_chan_used[channel] = false;
}

int AscendVenc::defaultBitrateKbps(int width, int height) {
    long px = (long) width * height;
    if (px >= 1920L * 1080) return 6000;
    if (px >= 1280L * 720) return 4000;
    if (px >= 640L * 480) return 1500;
    return 800;
}

bool AscendVenc::available(int device) {
    const char *why = nullptr;
    if (!env().acquire(device, why)) {
        return false;
    }
    env().release();
    return true;
}

AscendVenc::AscendVenc() = default;

AscendVenc::~AscendVenc() {
    close();
}

bool AscendVenc::open(const Config &cfg_in, OnPacket cb, std::string &err) {
    if (_opened) {
        err = "已经打开";
        return false;
    }
    if (cfg_in.width <= 0 || cfg_in.height <= 0) {
        err = "宽高非法";
        return false;
    }
    Config cfg = cfg_in;
    if (cfg.fps <= 0) cfg.fps = 25;
    if (cfg.gop <= 0) cfg.gop = cfg.fps;
    if (cfg.bitrate_kbps <= 0) cfg.bitrate_kbps = defaultBitrateKbps(cfg.width, cfg.height);
    if (cfg.bufs <= 0) cfg.bufs = 8;

    const char *why = nullptr;
    if (!env().acquire(cfg.device, why)) {
        err = string("DVPP 初始化失败：") + (why ? why : "unknown");
        return false;
    }

    // C++11 兼容（ZLM 用 -std=c++11，没有 make_unique）
    std::unique_ptr<Impl> impl(new Impl());
    impl->cfg = cfg;
    // 包一层：统计出帧数（取流回调发生在内部线程上，atomic 自增安全）
    impl->cb = [this, cb](const char *d, size_t n, uint64_t pts, bool key) {
        _out++;
        if (cb) {
            cb(d, n, pts, key);
        }
    };
    impl->h265 = cfg.h265;
    impl->frame_size = (hi_u32) cfg.width * cfg.height * 3 / 2;

    hi_venc_chn_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.venc_attr.type = cfg.h265 ? HI_PT_H265 : HI_PT_H264;
    attr.venc_attr.max_pic_width = cfg.width;
    attr.venc_attr.max_pic_height = cfg.height;
    attr.venc_attr.pic_width = cfg.width;
    attr.venc_attr.pic_height = cfg.height;
    attr.venc_attr.buf_size = cfg.h265 ? (2 * 1024 * 1024) : (4 * 1024 * 1024);
    attr.venc_attr.profile = 0;
    attr.venc_attr.is_by_frame = HI_TRUE;

    attr.rc_attr.rc_mode = cfg.h265 ? HI_VENC_RC_MODE_H265_CBR : HI_VENC_RC_MODE_H264_CBR;
    if (cfg.h265) {
        attr.venc_attr.h265_attr.rcn_ref_share_buf_en = HI_FALSE;
        attr.rc_attr.h265_cbr.gop = cfg.gop;
        attr.rc_attr.h265_cbr.stats_time = 1;
        attr.rc_attr.h265_cbr.src_frame_rate = cfg.fps;
        attr.rc_attr.h265_cbr.dst_frame_rate = cfg.fps;
        attr.rc_attr.h265_cbr.bit_rate = cfg.bitrate_kbps;
    } else {
        attr.venc_attr.h264_attr.rcn_ref_share_buf_en = HI_FALSE;
        attr.rc_attr.h264_cbr.gop = cfg.gop;
        attr.rc_attr.h264_cbr.stats_time = 1;
        attr.rc_attr.h264_cbr.src_frame_rate = cfg.fps;
        attr.rc_attr.h264_cbr.dst_frame_rate = cfg.fps;
        attr.rc_attr.h264_cbr.bit_rate = cfg.bitrate_kbps;
    }
    attr.gop_attr.gop_mode = HI_VENC_GOP_MODE_NORMAL_P;
    attr.gop_attr.normal_p.ip_qp_delta = 2;

    auto ret = hi_mpi_venc_create_chn(cfg.channel, &attr);
    if (ret != 0) {
        char buf[96];
        snprintf(buf, sizeof(buf), "hi_mpi_venc_create_chn 失败 0x%x（通道 %d 可能被占用）",
                 ret, cfg.channel);
        err = buf;
        env().release();
        return false;
    }
    // 官方样例 create_chn 之后还有两步，缺了不影响「能建通道」但可能让码控丢帧：
    //   1) set_scene_mode(SCENE_0)：声明画面内容特征（静止/规律运动）
    //   2) get_rc_param → set_rc_param 往返：把驱动默认码控参数显式下发一次
    {
        auto sm = hi_mpi_venc_set_scene_mode(cfg.channel, HI_VENC_SCENE_0);
        hi_venc_rc_param rcp;
        memset(&rcp, 0, sizeof(rcp));
        auto gr = hi_mpi_venc_get_rc_param(cfg.channel, &rcp);
        auto sr = (gr == 0) ? hi_mpi_venc_set_rc_param(cfg.channel, &rcp) : gr;
        fprintf(stderr, "[ascend venc] scene_mode=0x%x rc_roundtrip get=0x%x set=0x%x\n",
                sm, gr, sr);
    }

    fprintf(stderr, "[ascend venc] before start_chn\n");
    hi_venc_start_param sp;
    sp.recv_pic_num = -1;
    ret = hi_mpi_venc_start_chn(cfg.channel, &sp);
    fprintf(stderr, "[ascend venc] start_chn ret=0x%x\n", ret);
    if (ret != 0) {
        char buf[96];
        snprintf(buf, sizeof(buf), "hi_mpi_venc_start_chn 失败 0x%x", ret);
        err = buf;
        hi_mpi_venc_destroy_chn(cfg.channel);
        env().release();
        return false;
    }

    // 设备帧缓冲 + host 暂存
    impl->dev_buf.resize(cfg.bufs, nullptr);
    impl->host_buf.resize(cfg.bufs, nullptr);
    for (int i = 0; i < cfg.bufs; i++) {
        ret = hi_mpi_dvpp_malloc(0, &impl->dev_buf[i], impl->frame_size);
        if (ret != 0) {
            char buf[96];
            snprintf(buf, sizeof(buf), "hi_mpi_dvpp_malloc[%d] 失败 0x%x", i, ret);
            err = buf;
            impl->dev_buf.resize(i);
            goto fail;
        }
        impl->host_buf[i] = (uint8_t *) malloc(impl->frame_size);
    }
    impl->xfer.resize(4 * 1024 * 1024);
    fprintf(stderr, "[ascend venc] buffers ready (%d x %u bytes)\n", cfg.bufs,
            (unsigned) impl->frame_size);

    // 不启动独立取流线程：本进程里该线程恒拿不到数据（见 drainOnce 注释），
    // 且与内联排空并发访问同一通道会段错误。
    impl->stop.store(false);

    _impl = std::move(impl);
    _opened = true;

    return true;

fail:
    for (auto p : impl->dev_buf) {
        if (p) hi_mpi_dvpp_free(p);
    }
    for (auto p : impl->host_buf) {
        if (p) free(p);
    }
    hi_mpi_venc_stop_chn(cfg.channel);
    hi_mpi_venc_destroy_chn(cfg.channel);
    env().release();
    return false;
}

bool AscendVenc::encode(const uint8_t *nv12, uint64_t pts_us) {
    if (!_opened || !_impl || !nv12) {
        return false;
    }
    auto &im = *_impl;
    lock_guard<mutex> lck(im.send_mtx);

    // 上下文是**按线程**生效的：调用方可能在任意线程喂帧（多路并发时每路一个线程）。
    // 不重绑的话 aclrtMemcpy/send 会因参数校验失败而直接返回错误（多路测试实测 300/300 全失败）。
    if (aclrtSetCurrentContext(env().ctx) != ACL_SUCCESS) {
        return false;
    }

    uint32_t slot = im.idx % (uint32_t) im.dev_buf.size();
    if (aclrtMemcpy(im.dev_buf[slot], im.frame_size, nv12, im.frame_size,
                    ACL_MEMCPY_HOST_TO_DEVICE) != ACL_SUCCESS) {
        return false;
    }

    hi_video_frame_info fi;
    memset(&fi, 0, sizeof(fi));
    fi.v_frame.width = im.cfg.width;
    fi.v_frame.height = im.cfg.height;
    fi.v_frame.field = HI_VIDEO_FIELD_FRAME;
    fi.v_frame.pixel_format = HI_PIXEL_FORMAT_YUV_SEMIPLANAR_420;
    fi.v_frame.video_format = HI_VIDEO_FORMAT_LINEAR;
    fi.v_frame.dynamic_range = HI_DYNAMIC_RANGE_SDR8;
    fi.v_frame.compress_mode = HI_COMPRESS_MODE_NONE;
    fi.v_frame.color_gamut = HI_COLOR_GAMUT_BT709;
    fi.v_frame.virt_addr[0] = im.dev_buf[slot];
    fi.v_frame.width_stride[0] = im.cfg.width;
    fi.v_frame.height_stride[0] = im.cfg.height;
    fi.v_frame.virt_addr[1] = (uint8_t *) im.dev_buf[slot] + (size_t) im.cfg.width * im.cfg.height;
    fi.v_frame.width_stride[1] = im.cfg.width;
    fi.v_frame.height_stride[1] = im.cfg.height / 2;
    // ★ time_ref 必须偶数，否则驱动判非法（0xA0088003）
    fi.v_frame.time_ref = im.idx * 2;
    fi.v_frame.pts = pts_us;

    hi_user_frame_info ufi;
    memset(&ufi, 0, sizeof(ufi));
    ufi.user_frame = fi;
    // ★ 必须显式设置，留 0 时一帧都不收
    ufi.user_rc_info.frame_type = VENC_FRAME_TYPE_NONE;

    // 编码器内部队列很浅，偶发需要退避重投（同一帧重投时 pts/time_ref 不能变）
    bool ok = false;
    for (int t = 0; t < 40; t++) {
        auto ret = hi_mpi_venc_send_frame_ex(im.cfg.channel, &ufi, 0);
        if (ret == 0) {
            ok = true;
            break;
        }
        _retries++;
        usleep(2 * 1000);
    }
    if (ok) {
        im.idx++;
        _sent++;
        // 同线程排空：实测「另起取流线程」在本进程里拿不到数据（恒 BUF_EMPTY），
        // 而同线程同步取流稳定拿得到（INLINE 探针 send=1/got_packs=1）。
        // 编码器出包有几毫秒延迟，所以要带等待重试，不能取一次空就放弃。
        // 等出包：编码器出包有几毫秒延迟，一旦不等就会让「发送↔取流」错拍，
        // 下游解码队列积压到丢帧（实测只剩预热那一帧能编出来）。
        for (int w = 0; w < 25; w++) {
            if (im.drainOnce() > 0) {
                break;
            }
            usleep(1000);
        }
        // 把同一时刻可取的包取干净，但必须有上界：
        // 若驱动在 release 后仍报告有包，无界循环会在这里死住（实测卡死不返回）
        for (int k = 0; k < 16 && im.drainOnce() > 0; k++) {
        }
    } else {
        // 只在头几帧打印：偶发失败有重试兜底，持续性失败才需要人看
        if (_sent.load() < 5) {
            fprintf(stderr, "[ascend venc] send failed after retries, ch=%d size=%dx%d\n",
                    im.cfg.channel, im.cfg.width, im.cfg.height);
        }
    }
    return ok;
}

void AscendVenc::flush() {
    if (!_opened || !_impl) {
        return;
    }
    // 给取流线程一点时间把残余码流取干净
    for (int i = 0; i < 50 && _out.load() < _sent.load(); i++) {
        usleep(2 * 1000);
    }
}

void AscendVenc::close() {
    if (!_opened || !_impl) {
        return;
    }
    auto &im = *_impl;
    flush();
    im.stop.store(true);
    if (im.running.load()) {
        pthread_join(im.th, nullptr);
        im.running.store(false);
    }
    hi_mpi_venc_stop_chn(im.cfg.channel);
    hi_mpi_venc_destroy_chn(im.cfg.channel);
    // 通道必须归还，否则后续流申请不到号（池耗尽时会明确报错并回落软编）
    releaseChannel(im.cfg.channel);
    for (auto p : im.dev_buf) {
        if (p) hi_mpi_dvpp_free(p);
    }
    for (auto p : im.host_buf) {
        if (p) free(p);
    }
    im.dev_buf.clear();
    im.host_buf.clear();
    env().release();
    _impl.reset();
    _opened = false;
}

} // namespace mediakit

#else // !ENABLE_ASCEND_VENC

namespace mediakit {
int AscendVenc_unused_symbol_guard = 0;
}

#endif // ENABLE_ASCEND_VENC
