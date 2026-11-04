/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#if defined(ENABLE_FFMPEG)
#if !defined(_WIN32)
#include <dlfcn.h>
#endif
#include "Util/File.h"
#include "Util/uv_errno.h"
#include "Transcode.h"
#ifdef ENABLE_ASCEND_VENC
#include "Transcode/AscendVdec.h"
#endif
#include "Common/config.h"

#define MAX_DELAY_SECOND 3

using namespace std;
using namespace toolkit;

namespace mediakit {

static string ffmpeg_err(int errnum) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(errnum, errbuf, AV_ERROR_MAX_STRING_SIZE);
    return errbuf;
}

std::unique_ptr<AVPacket, void (*)(AVPacket *)> alloc_av_packet() {
    return std::unique_ptr<AVPacket, void (*)(AVPacket *)>(av_packet_alloc(), [](AVPacket *pkt) { av_packet_free(&pkt); });
}

//////////////////////////////////////////////////////////////////////////////////////////
static void on_ffmpeg_log(void *ctx, int level, const char *fmt, va_list args) {
    GET_CONFIG(bool, enable_ffmpeg_log, General::kEnableFFmpegLog);
    if (!enable_ffmpeg_log) {
        return;
    }
    LogLevel lev;
    switch (level) {
        case AV_LOG_FATAL: lev = LError; break;
        case AV_LOG_ERROR: lev = LError; break;
        case AV_LOG_WARNING: lev = LWarn; break;
        case AV_LOG_INFO: lev = LInfo; break;
        case AV_LOG_VERBOSE: lev = LDebug; break;
        case AV_LOG_DEBUG: lev = LDebug; break;
        case AV_LOG_TRACE: lev = LTrace; break;
        default: lev = LTrace; break;
    }
    LoggerWrapper::printLogV(::toolkit::getLogger(), lev, __FILE__, ctx ? av_default_item_name(ctx) : "NULL", level, fmt, args);
}

static bool setupFFmpeg_l() {
    av_log_set_level(AV_LOG_TRACE);
    av_log_set_flags(AV_LOG_PRINT_LEVEL);
    av_log_set_callback(on_ffmpeg_log);
#if (LIBAVCODEC_VERSION_MAJOR < 58)
    avcodec_register_all();
#endif
    return true;
}

static void setupFFmpeg() {
    static auto flag = setupFFmpeg_l();
}

static bool checkIfSupportedNvidia_l() {
#if !defined(_WIN32)
    GET_CONFIG(bool, check_nvidia_dev, General::kCheckNvidiaDev);
    if (!check_nvidia_dev) {
        return false;
    }
    auto so = dlopen("libnvcuvid.so.1", RTLD_LAZY);
    if (!so) {
        WarnL << "libnvcuvid.so.1加载失败:" << get_uv_errmsg();
        return false;
    }
    dlclose(so);

    bool find_driver = false;
    File::scanDir("/dev", [&](const string &path, bool is_dir) {
        if (!is_dir && start_with(path, "/dev/nvidia")) {
            // 找到nvidia的驱动  [AUTO-TRANSLATED:5b87bf81]
            // Find the Nvidia driver
            find_driver = true;
            return false;
        }
        return true;
    }, false);

    if (!find_driver) {
        WarnL << "英伟达硬件编解码器驱动文件 /dev/nvidia* 不存在";
    }
    return find_driver;
#else
    return false;
#endif
}

static bool checkIfSupportedNvidia() {
    static auto ret = checkIfSupportedNvidia_l();
    return ret;
}

/**
 * Intel QSV 是否可用。
 *
 * 上游的候选列表把 h264_qsv/hevc_qsv 排在软件解码之前，却**不检查设备是否存在**。
 * 只要 ffmpeg 编译时带了 QSV（conda-forge、Ubuntu 的 ffmpeg 都带），在没有 Intel
 * 核显的机器上也会选中 qsv：`avcodec_open2` 返回成功，但每个包都
 * `avcodec_send_packet failed: Unknown error`，整条流一个帧都出不来，拉流端超时。
 * 实测（Ubuntu 22.04 + NVIDIA 独显）就撞上了这个。
 *
 * Whether Intel QSV is actually available. Upstream puts h264_qsv/hevc_qsv ahead of
 * software decoders without checking the device: on a machine with no Intel iGPU but an
 * ffmpeg built with QSV support, open2 "succeeds" and then every packet fails with
 * `avcodec_send_packet failed: Unknown error` — no frames at all, viewers time out.
 */
static bool checkIfSupportedQsv_l() {
#if defined(__linux__)
    // 不能只看 /dev/dri 是否存在：WSL2 的 GPU 直通同样会建 card0/renderD128，
    // 但那是 NVIDIA 卡，qsv 照样打不开（实测：open2 成功、每个包 send 失败）。
    // 唯一可靠判据是 sysfs 里的 PCI vendor —— 只有 0x8086 才是 Intel。
    //
    // Do not rely on /dev/dri existing: WSL2's GPU passthrough also creates
    // card0/renderD128 for an NVIDIA card, and qsv still fails there. Only the PCI
    // vendor id in sysfs (0x8086) positively identifies an Intel GPU.
    bool intel = false;
    for (int i = 0; i < 8; ++i) {
        auto path = "/sys/class/drm/card" + std::to_string(i) + "/device/vendor";
        std::ifstream ifs(path);
        if (!ifs.good()) {
            continue;
        }
        string vendor;
        ifs >> vendor;
        if (vendor.find("0x8086") != string::npos) {
            intel = true;
            break;
        }
    }
    if (!intel) {
        WarnL << "未检测到 Intel 显卡（sysfs vendor 0x8086），跳过 qsv 解码器";
    }
    return intel;
#else
    // 其它平台维持上游行为，不做额外假设
    return true;
#endif
}

static bool checkIfSupportedQsv() {
    static auto ret = checkIfSupportedQsv_l();
    return ret;
}
/** CodecName 是否指向 qsv 硬件解码器（用于「设备不存在时跳过」） */
static inline bool isQsvCodecName(const char *name) {
    return name && strstr(name, "_qsv");
}

//////////////////////////////////////////////////////////////////////////////////////////

bool TaskManager::addEncodeTask(function<void()> task) {
    {
        lock_guard<mutex> lck(_task_mtx);
        _task.emplace_back(std::move(task));
        if (_task.size() > _max_task) {
            WarnL << "encoder thread task is too more, now drop frame!";
            _task.pop_front();
        }
    }
    _sem.post();
    return true;
}

bool TaskManager::addDecodeTask(bool key_frame, function<void()> task) {
    {
        lock_guard<mutex> lck(_task_mtx);
        if (_decode_drop_start) {
            if (!key_frame) {
                TraceL << "decode thread drop frame";
                return false;
            }
            _decode_drop_start = false;
            InfoL << "decode thread stop drop frame";
        }

        _task.emplace_back(std::move(task));
        if (_task.size() > _max_task) {
            _decode_drop_start = true;
            WarnL << "decode thread start drop frame";
        }
    }
    _sem.post();
    return true;
}

void TaskManager::setMaxTaskSize(size_t size) {
    CHECK(size >= 3 && size <= 1000, "async task size limited to 3 ~ 1000, now size is:", size);
    _max_task = size;
}

void TaskManager::startThread(const string &name) {
    _thread.reset(new thread([this, name]() {
        onThreadRun(name);
    }), [](thread *ptr) {
        if (ptr->joinable()) {
            ptr->join();
        }
        delete ptr;
    });
}

void TaskManager::stopThread(bool drop_task) {
    TimeTicker();
    if (!_thread) {
        return;
    }
    {
        lock_guard<mutex> lck(_task_mtx);
        if (drop_task) {
            _exit = true;
            _task.clear();
        }
        _task.emplace_back([]() {
            throw ThreadExitException();
        });
    }
    _sem.post(10);
    _thread = nullptr;
}

TaskManager::~TaskManager() {
    stopThread(true);
}

bool TaskManager::isEnabled() const {
    return _thread.operator bool();
}

void TaskManager::onThreadRun(const string &name) {
    setThreadName(name.data());
    function<void()> task;
    _exit = false;
    while (!_exit) {
        _sem.wait();
        {
            unique_lock<mutex> lck(_task_mtx);
            if (_task.empty()) {
                continue;
            }
            task = _task.front();
            _task.pop_front();
        }

        try {
            TimeTicker2(50, TraceL);
            task();
            task = nullptr;
        } catch (ThreadExitException &ex) {
            break;
        } catch (std::exception &ex) {
            WarnL << ex.what();
            continue;
        } catch (...) {
            WarnL << "catch one unknown exception";
            throw;
        }
    }
    InfoL << name << " exited!";
}

//////////////////////////////////////////////////////////////////////////////////////////

FFmpegFrame::FFmpegFrame(std::shared_ptr<AVFrame> frame) {
    if (frame) {
        _frame = std::move(frame);
    } else {
        _frame.reset(av_frame_alloc(), [](AVFrame *ptr) {
            av_frame_free(&ptr);
        });
    }
}

FFmpegFrame::~FFmpegFrame() {
}

AVFrame *FFmpegFrame::get() const {
    return _frame.get();
}

void FFmpegFrame::fillPicture(AVPixelFormat target_format, int target_width, int target_height) {
    auto buffer_size = av_image_get_buffer_size(target_format, target_width, target_height, 32);
    _data = std::unique_ptr<char[]>(new char[buffer_size]);
    av_image_fill_arrays(_frame->data, _frame->linesize, (uint8_t *)_data.get(), target_format, target_width, target_height, 32);
}

int FFmpegFrame::getChannels() const {
    if (!_frame) return 0;
#if LIBAVCODEC_VERSION_INT >= FF_CODEC_VER_7_1
    return _frame->ch_layout.nb_channels;
#else
    return _frame->channels;
#endif
}

// 资源池复用前调用
void FFmpegFrame::reset() {
    _data.reset();
    if (_frame) {
        av_frame_unref(_frame.get()); // 清理AVFrame数据引用
    }
}

///////////////////////////////////////////////////////////////////////////

template<bool decoder = true>
static inline const AVCodec *getCodec_l(const char *name) {
    auto codec = decoder ? avcodec_find_decoder_by_name(name) : avcodec_find_encoder_by_name(name);
    if (codec) {
        InfoL << (decoder ? "got decoder:" : "got encoder:") << name;
    } else {
        TraceL << (decoder ? "decoder:" : "encoder:") << name << " not found";
    }
    return codec;
}

template<bool decoder = true>
static inline const AVCodec *getCodec_l(enum AVCodecID id) {
    auto codec = decoder ? avcodec_find_decoder(id) : avcodec_find_encoder(id);
    if (codec) {
        InfoL << (decoder ? "got decoder:" : "got encoder:") << avcodec_get_name(id);
    } else {
        TraceL << (decoder ? "decoder:" : "encoder:") << avcodec_get_name(id) << " not found";
    }
    return codec;
}

class CodecName {
public:
    CodecName(string name) : _codec_name(std::move(name)) {}
    CodecName(enum AVCodecID id) : _id(id) {}

    template <bool decoder>
    const AVCodec *getCodec() const {
        if (!_codec_name.empty()) {
            return getCodec_l<decoder>(_codec_name.data());
        }
        return getCodec_l<decoder>(_id);
    }

    /** 是否指向 qsv 解码器（无 Intel 设备时要跳过，见 checkIfSupportedQsv_l） */
    bool isQsv() const { return !_codec_name.empty() && _codec_name.find("_qsv") != string::npos; }

private:
    string _codec_name;
    enum AVCodecID _id;
};

/** 同 getCodec，但没有 Intel 设备时跳过 qsv 候选（否则会选到打不开的实现） */
template<bool decoder = true>
static inline const AVCodec *getCodecFiltered(const std::initializer_list<CodecName> &codec_list) {
    const AVCodec *ret = nullptr;
    bool skip_qsv = !checkIfSupportedQsv();
    for (int i = codec_list.size(); i >= 1; --i) {
        const auto &item = codec_list.begin()[i - 1];
        if (skip_qsv && item.isQsv()) {
            continue;
        }
        ret = item.getCodec<decoder>();
        if (ret) {
            return ret;
        }
    }
    return ret;
}

template<bool decoder = true>
static inline const AVCodec *getCodec(const std::initializer_list<CodecName> &codec_list) {
    const AVCodec *ret = nullptr;
    for (int i = codec_list.size(); i >= 1; --i) {
        ret = codec_list.begin()[i - 1].getCodec<decoder>();
        if (ret) {
            return ret;
        }
    }
    return ret;
}

template<bool decoder = true>
static inline const AVCodec *getCodecByName(const std::vector<std::string> &codec_list) {
    const AVCodec *ret = nullptr;
    for (auto &codec : codec_list) {
        ret = getCodec_l<decoder>(codec.data());
        if (ret) {
            return ret;
        }
    }
    return ret;
}

/**
 * 硬件解码的像素格式协商回调。
 *
 * 必须自己选硬件格式：默认的 avcodec_default_get_format 会挑第一个软件格式，
 * 于是「挂了 hw_device_ctx 也照样软解」——这是最容易踩的坑，日志上完全看不出。
 *
 * Pick the hardware pixel format ourselves: the default callback returns the first
 * software format, so hw device attached but decoding still on CPU, silently.
 */
static enum AVPixelFormat hwDecodeGetFormat(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts) {
    auto self = (FFmpegDecoder *) ctx->opaque;
    auto want = self ? self->_hw_pix_fmt : AV_PIX_FMT_NONE;
    for (auto p = pix_fmts; p && *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == want) {
            return *p;
        }
    }
    WarnL << "hw decode: codec 未提供期望的硬件格式，回落软件";
    return avcodec_default_get_format(ctx, pix_fmts);
}

FFmpegDecoder::FFmpegDecoder(const Track::Ptr &track, int thread_num,
                             const std::vector<std::string> &codec_name, bool hw_decode) {
    setupFFmpeg();
#ifdef ENABLE_ASCEND_VENC
    _src_track = track;   // 昇腾硬解要用它取真实宽高
#endif
    _frame_pool.setSize(AV_NUM_DATA_POINTERS);
    const AVCodec *codec = nullptr;
    const AVCodec *codec_default = nullptr;
    if (!codec_name.empty()) {
        codec = getCodecByName(codec_name);
    }
    switch (track->getCodecId()) {
        case CodecH264:
            codec_default = getCodec({AV_CODEC_ID_H264});
            if (codec && codec->id == AV_CODEC_ID_H264) {
                break;
            }
            if (checkIfSupportedNvidia()) {
                codec = getCodecFiltered({{"libopenh264"}, {AV_CODEC_ID_H264}, {"h264_qsv"}, {"h264_videotoolbox"}, {"h264_cuvid"}, {"h264_nvmpi"}});
            } else {
                codec = getCodecFiltered({{"libopenh264"}, {AV_CODEC_ID_H264}, {"h264_qsv"}, {"h264_videotoolbox"}, {"h264_nvmpi"}});
            }
            break;
        case CodecH265:
            codec_default = getCodec({AV_CODEC_ID_HEVC});
            if (codec && codec->id == AV_CODEC_ID_HEVC) {
                break;
            }
            if (checkIfSupportedNvidia()) {
                codec = getCodecFiltered({{AV_CODEC_ID_HEVC}, {"hevc_qsv"}, {"hevc_videotoolbox"}, {"hevc_cuvid"}, {"hevc_nvmpi"}});
            } else {
                codec = getCodecFiltered({{AV_CODEC_ID_HEVC}, {"hevc_qsv"}, {"hevc_videotoolbox"}, {"hevc_nvmpi"}});
            }
            break;
        case CodecAAC:
            if (codec && codec->id == AV_CODEC_ID_AAC) {
                break;
            }
            codec = getCodec({AV_CODEC_ID_AAC});
            break;
        case CodecG711A:
            if (codec && codec->id == AV_CODEC_ID_PCM_ALAW) {
                break;
            }
            codec = getCodec({AV_CODEC_ID_PCM_ALAW});
            break;
        case CodecG711U:
            if (codec && codec->id == AV_CODEC_ID_PCM_MULAW) {
                break;
            }
            codec = getCodec({AV_CODEC_ID_PCM_MULAW});
            break;
        case CodecOpus:
            if (codec && codec->id == AV_CODEC_ID_OPUS) {
                break;
            }
            codec = getCodec({AV_CODEC_ID_OPUS});
            break;
        case CodecJPEG:
            if (codec && codec->id == AV_CODEC_ID_MJPEG) {
                break;
            }
            codec = getCodec({AV_CODEC_ID_MJPEG});
            break;
        case CodecVP8:
            if (codec && codec->id == AV_CODEC_ID_VP8) {
                break;
            }
            codec = getCodec({AV_CODEC_ID_VP8});
            break;
        case CodecVP9:
            if (codec && codec->id == AV_CODEC_ID_VP9) {
                break;
            }
            codec = getCodec({AV_CODEC_ID_VP9});
            break;
        // 以下三种上游没有映射：源流是 MP3/G722/L16 时 setupTrack 直接抛
        // 「未找到解码器」，整条轨回落到透传（配了 target codec 也不生效）。
        // 实测：MP3 源 -> opus 完全不转码，观众端仍是 mp3。
        // These three were unmapped upstream: an MP3/G722/L16 source threw
        // "no decoder found" during setupTrack and the whole track fell back to
        // passthrough, so a configured target codec had no effect.
        case CodecMP3:
            if (codec && codec->id == AV_CODEC_ID_MP3) {
                break;
            }
            codec = getCodec({AV_CODEC_ID_MP3});
            break;
        case CodecG722:
            if (codec && codec->id == AV_CODEC_ID_ADPCM_G722) {
                break;
            }
            codec = getCodec({AV_CODEC_ID_ADPCM_G722});
            break;
        case CodecL16:
            if (codec && codec->id == AV_CODEC_ID_PCM_S16BE) {
                break;
            }
            codec = getCodec({AV_CODEC_ID_PCM_S16BE});
            break;
        default: codec = nullptr; break;
    }

    codec = codec ? codec : codec_default;
    if (!codec) {
        throw std::runtime_error("未找到解码器");
    }

    while (true) {
        _context.reset(avcodec_alloc_context3(codec), [](AVCodecContext *ctx) {
            avcodec_free_context(&ctx);
        });

        if (!_context) {
            throw std::runtime_error("创建解码器失败");
        }

        // 保存AVFrame的引用  [AUTO-TRANSLATED:2df53d07]
        // Save the AVFrame reference
#ifdef FF_API_OLD_ENCDEC
        _context->refcounted_frames = 1;
#endif
        _context->flags |= AV_CODEC_FLAG_LOW_DELAY;
        _context->flags2 |= AV_CODEC_FLAG2_FAST;
        if (track->getTrackType() == TrackVideo) {
            _context->width = static_pointer_cast<VideoTrack>(track)->getVideoWidth();
            _context->height = static_pointer_cast<VideoTrack>(track)->getVideoHeight();
            InfoL << "media source :" << _context->width << " X " << _context->height;
        }

        switch (track->getCodecId()) {
            case CodecG711A:
            case CodecG711U: {
                AudioTrack::Ptr audio = static_pointer_cast<AudioTrack>(track);

#if LIBAVCODEC_VERSION_INT >= FF_CODEC_VER_7_1
                av_channel_layout_default(&_context->ch_layout, audio->getAudioChannel());
#else
                _context->channels = audio->getAudioChannel();
                _context->channel_layout = av_get_default_channel_layout(_context->channels);
#endif

                _context->sample_rate = audio->getAudioSampleRate();
                break;
            }
            default:
                break;
        }
        AVDictionary *dict = nullptr;
        if (thread_num <= 0) {
            av_dict_set(&dict, "threads", "auto", 0);
        } else {
            av_dict_set(&dict, "threads", to_string(MIN((unsigned int)thread_num, thread::hardware_concurrency())).data(), 0);
        }
        av_dict_set(&dict, "zerolatency", "1", 0);
        av_dict_set(&dict, "strict", "-2", 0);

#ifdef AV_CODEC_CAP_TRUNCATED
        if (codec->capabilities & AV_CODEC_CAP_TRUNCATED) {
            /* we do not send complete frames */
            _context->flags |= AV_CODEC_FLAG_TRUNCATED;
            _do_merger = false;
        } else {
            // 此时业务层应该需要合帧  [AUTO-TRANSLATED:8dea0fff]
            // The business layer should need to merge frames at this time
            _do_merger = true;
        }
#endif

        // 硬件解码：建设备 + 挂 get_format。只对视频、且只是 H264/H265 有意义
        if (hw_decode && track->getTrackType() == TrackVideo
            && (track->getCodecId() == CodecH264 || track->getCodecId() == CodecH265)) {
            AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;
            AVPixelFormat hw_fmt = AV_PIX_FMT_NONE;
#if defined(__APPLE__)
            type = AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
            hw_fmt = AV_PIX_FMT_VIDEOTOOLBOX;
#elif defined(_WIN32) || defined(__linux__)
            type = AV_HWDEVICE_TYPE_CUDA;
            hw_fmt = AV_PIX_FMT_CUDA;
#endif
            if (type != AV_HWDEVICE_TYPE_NONE) {
                if (av_hwdevice_ctx_create(&_hw_device, type, nullptr, nullptr, 0) >= 0) {
                    _hw_pix_fmt = hw_fmt;
                    _context->hw_device_ctx = av_buffer_ref(_hw_device);
                    _context->opaque = this;
                    _context->get_format = hwDecodeGetFormat;
                    InfoL << "hw decode enabled: " << av_hwdevice_get_type_name(type);
                } else {
                    WarnL << "hw decode: 创建硬件设备失败，回落软件解码";
                    _hw_pix_fmt = AV_PIX_FMT_NONE;
                }
            }
        }

#ifdef ENABLE_ASCEND_VENC
        // 昇腾 DVPP 硬解接管：成功就完全不走 libavcodec（连 avcodec_open2 都不需要），
        // 所以必须放在这里 return，而不是"先开 avcodec 再替换"。
        if (hw_decode && (track->getCodecId() == CodecH264 || track->getCodecId() == CodecH265)
            && AscendVdec::available()) {
            auto dec = std::make_shared<AscendVdec>();
            AscendVdec::Config acfg;
            acfg.h265 = (track->getCodecId() == CodecH265);
            // 源轨在这一刻可能还没解析出宽高（目标轨是空构造）——给一个上界即可，
            // VDEC 的 attr 里这两个字段就是"最大宽高"，实际尺寸由码流决定。
            auto vtrack = static_pointer_cast<VideoTrack>(track);
            acfg.width = MAX((int) vtrack->getVideoWidth(), 1920);
            acfg.height = MAX((int) vtrack->getVideoHeight(), 1080);
            acfg.channel = AscendVdec::acquireChannel();
            std::string err;
            auto self = this;
            bool ok = false;
            if (acfg.channel >= 0) {
                ok = dec->open(acfg, [self](const uint8_t *nv12, int w, int h, uint32_t pts_ms) {
                    // 把 DVPP 吐出来的 NV12 包成 AVFrame 交给下游：
                    // 下游（sws / 昇腾硬编）本来就吃 NV12，这样硬解→硬编可以省掉一次颜色转换。
                    auto av = av_frame_alloc();
                    if (!av) {
                        return;
                    }
                    av->format = AV_PIX_FMT_NV12;
                    av->width = w;
                    av->height = h;
                    if (av_frame_get_buffer(av, 32) < 0) {
                        av_frame_free(&av);
                        return;
                    }
                    memcpy(av->data[0], nv12, (size_t) w * h);
                    memcpy(av->data[1], nv12 + (size_t) w * h, (size_t) w * h / 2);
                    av->pts = av->pkt_dts = pts_ms;
                    auto holder = std::shared_ptr<AVFrame>(av, [](AVFrame *f) { av_frame_free(&f); });
                    self->onDecode(std::make_shared<FFmpegFrame>(holder));
                }, err);
            }
            if (ok) {
                _ascend_dec = dec;
                // 零拷贝：上层若设置了设备回调，VDEC 就跳过 D2H 直接交设备地址；
                // 没设置（或 UV 布局不符）时 VDEC 自动回落主机路径，不影响功能。
                auto self_dev = this;
                dec->setOnDevFrame([self_dev](const AscendVdec::DevFrame &f) -> bool {
                    return self_dev->_on_decode_dev
                               ? self_dev->_on_decode_dev(f.data, f.width, f.height, f.stride,
                                                          f.pts_ms)
                               : false;
                });
                InfoL << "hw decode: 使用昇腾 DVPP VDEC (channel=" << acfg.channel
                      << ", max " << acfg.width << "x" << acfg.height << ")";
                return; // 不再打开 libavcodec
            }
            if (acfg.channel >= 0) {
                AscendVdec::releaseChannel(acfg.channel);
            }
            WarnL << "昇腾 DVPP 硬解不可用，回落 libavcodec: " << err;
        }
#endif

        int ret = avcodec_open2(_context.get(), codec, &dict);
        av_dict_free(&dict);
        if (ret >= 0) {
            // 成功  [AUTO-TRANSLATED:7d878ca9]
            // Success
            InfoL << "打开解码器成功:" << codec->name;
            break;
        }

        if (codec_default && codec_default != codec) {
            // 硬件编解码器打开失败，尝试软件的  [AUTO-TRANSLATED:060200f4]
            // Hardware codec failed to open, try software codec
            WarnL << "打开解码器" << codec->name << "失败，原因是:" << ffmpeg_err(ret) << ", 再尝试打开解码器" << codec_default->name;
            codec = codec_default;
            continue;
        }
        throw std::runtime_error(StrPrinter << "打开解码器" << codec->name << "失败:" << ffmpeg_err(ret));
    }
}

FFmpegDecoder::~FFmpegDecoder() {
    stopThread(true);
#ifdef ENABLE_ASCEND_VENC
    if (_ascend_dec) {
        _ascend_dec->close();
        _ascend_dec.reset();
    }
#endif
    if (_do_merger) {
        _merger.flush();
    }
    flush();
    // 先释放 codec context（它持有 hw_device_ctx 的引用），再放设备，顺序不能反
    // Release the codec context (which references hw_device_ctx) before the device.
    _context.reset();
    if (_hw_device) {
        av_buffer_unref(&_hw_device);
    }
}

void FFmpegDecoder::flush() {
    while (true) {
        auto out_frame = _frame_pool.obtain2();
        auto ret = avcodec_receive_frame(_context.get(), out_frame->get());
        if (ret == AVERROR(EAGAIN)) {
            avcodec_send_packet(_context.get(), nullptr);
            continue;
        }
        if (ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            WarnL << "avcodec_receive_frame failed:" << ffmpeg_err(ret);
            break;
        }
        onDecode(out_frame);
    }
}

const AVCodecContext *FFmpegDecoder::getContext() const {
    return _context.get();
}

bool FFmpegDecoder::inputFrame_l(const Frame::Ptr &frame, bool live, bool enable_merge) {
    if (_do_merger && enable_merge) {
        return _merger.inputFrame(frame, [this, live](uint64_t dts, uint64_t pts, const Buffer::Ptr &buffer, bool have_idr) {
            decodeFrame(buffer->data(), buffer->size(), dts, pts, live, have_idr);
        });
    }

    return decodeFrame(frame->data(), frame->size(), frame->dts(), frame->pts(), live, frame->keyFrame());
}

bool FFmpegDecoder::inputFrame(const Frame::Ptr &frame, bool live, bool async, bool enable_merge) {
    if (async && !TaskManager::isEnabled() && getContext()->codec_type == AVMEDIA_TYPE_VIDEO) {
        // 开启异步编码，且为视频，尝试启动异步解码线程  [AUTO-TRANSLATED:17a68fc6]
        // Enable asynchronous encoding, and it is video, try to start asynchronous decoding thread
        startThread("decoder thread");
    }

    if (!async || !TaskManager::isEnabled()) {
        return inputFrame_l(frame, live, enable_merge);
    }

    auto frame_cache = Frame::getCacheAbleFrame(frame);
    return addDecodeTask(frame->keyFrame(), [this, live, frame_cache, enable_merge]() {
        inputFrame_l(frame_cache, live, enable_merge);
        // 此处模拟解码太慢导致的主动丢帧  [AUTO-TRANSLATED:fc8bea8a]
        // Here simulates decoding too slow, resulting in active frame dropping
        // usleep(100 * 1000);
    });
}

bool FFmpegDecoder::decodeFrame(const char *data, size_t size, uint64_t dts, uint64_t pts, bool live, bool key_frame) {
    TimeTicker2(30, TraceL);

#ifdef ENABLE_ASCEND_VENC
    if (_ascend_dec) {
        // 通道是按最大尺寸开的，这里把源轨已解析出的真实尺寸告诉它：
        // 不改的话 VDEC 按上界输出（720p 流解成 1920x1080），下游要再缩一次，
        // 而且尺寸对不上会让零拷贝直通失效。
        if (_src_track) {
            auto vtrack = static_pointer_cast<VideoTrack>(_src_track);
            int tw = vtrack ? vtrack->getVideoWidth() : 0;
            int th = vtrack ? vtrack->getVideoHeight() : 0;
            // 只在本路还没送过包时设置一次：中途改输出尺寸要驱动重配缓冲，
            // 没必要冒这个险（同一路流的尺寸本来也不会变）
            if (tw > 0 && th > 0 && _ascend_dec->inFrames() == 0) {
                _ascend_dec->setOutputSize(tw, th);
            }
        }
        // 硬解路径：ZLM 给的 data 已经是整帧 Annex-B，直接送 VDEC（FRAME 模式）
        return _ascend_dec->decode((const uint8_t *) data, size, (uint32_t) pts);
    }
#endif
    auto pkt = alloc_av_packet();
    pkt->data = (uint8_t *)data;
    pkt->size = size;
    pkt->dts = dts;
    pkt->pts = pts;
    if (key_frame) {
        pkt->flags |= AV_PKT_FLAG_KEY;
    }

    auto ret = avcodec_send_packet(_context.get(), pkt.get());
    if (ret < 0) {
        if (ret != AVERROR_INVALIDDATA) {
            WarnL << "avcodec_send_packet failed:" << ffmpeg_err(ret);
        }
        return false;
    }

    for (;;) {
        auto out_frame = _frame_pool.obtain2();
        ret = avcodec_receive_frame(_context.get(), out_frame->get());
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            WarnL << "avcodec_receive_frame failed:" << ffmpeg_err(ret);
            break;
        }
        // 硬件解码器（如 hevc_videotoolbox）输出的是**硬件帧**（pix_fmt 带
        // AV_PIX_FMT_FLAG_HWACCEL），下游的 sws/滤镜都只吃系统内存帧，必须显式下载。
        // 这是「硬件解码」与「全 GPU 零拷贝」的分界：要真正零拷贝，就得让滤镜链也能
        // 直接消费硬件帧，而 drawtext/overlay 这类 GPU 滤镜不是各平台都齐。
        //
        // HW decoders (e.g. hevc_videotoolbox) return hardware frames; sws/filters only
        // accept system memory, so download explicitly. This is exactly the boundary
        // between "hardware decode" and true zero-copy.
        auto frame_fmt = (enum AVPixelFormat) out_frame->get()->format;
        auto desc = av_pix_fmt_desc_get(frame_fmt);
        if (desc && (desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
            auto sw_frame = _frame_pool.obtain2();
            if (av_hwframe_transfer_data(sw_frame->get(), out_frame->get(), 0) < 0) {
                WarnL << "av_hwframe_transfer_data failed, drop frame";
                continue;
            }
            sw_frame->get()->pts = out_frame->get()->pts;
            sw_frame->get()->pkt_dts = out_frame->get()->pkt_dts;
            sw_frame->get()->pict_type = out_frame->get()->pict_type;
            if (++_hw_frames == 1) {
                InfoL << "hw decode: 第一帧已从硬件帧下载到内存, pix_fmt=" << (int) frame_fmt;
            }
            onDecode(sw_frame);
            continue;
        }
        if (live && pts - out_frame->get()->pts > MAX_DELAY_SECOND * 1000 && _ticker.createdTime() > 10 * 1000) {
            // 后面的帧才忽略,防止Track无法ready  [AUTO-TRANSLATED:23f1a7c9]
            // The following frames are ignored to prevent the Track from being ready
            WarnL << "解码时，忽略" << MAX_DELAY_SECOND << "秒前的数据:" << pts << " " << out_frame->get()->pts;
            continue;
        }
        onDecode(out_frame);
    }
    return true;
}

void FFmpegDecoder::setOnDecode(FFmpegDecoder::onDec cb) {
    _cb = std::move(cb);
}

#ifdef ENABLE_ASCEND_VENC
uint32_t FFmpegDecoder::ascendDecodeIn() const {
    return _ascend_dec ? _ascend_dec->inFrames() : 0;
}
uint32_t FFmpegDecoder::ascendDecodeOut() const {
    return _ascend_dec ? _ascend_dec->outFrames() : 0;
}
uint32_t FFmpegDecoder::ascendDecodeDropped() const {
    return _ascend_dec ? _ascend_dec->droppedFrames() : 0;
}
int FFmpegDecoder::ascendDecodeChannel() const {
    return _ascend_dec ? _ascend_dec->channel() : -1;
}
#endif

void FFmpegDecoder::onDecode(const FFmpegFrame::Ptr &frame) {
    if (_cb) {
        _cb(frame);
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////
#if LIBAVCODEC_VERSION_INT >= FF_CODEC_VER_7_1
FFmpegSwr::FFmpegSwr(AVSampleFormat output, AVChannelLayout *ch_layout, int samplerate) {
    _target_format = output;
    av_channel_layout_copy(&_target_ch_layout, ch_layout);
    _target_samplerate = samplerate;
}
#else
FFmpegSwr::FFmpegSwr(AVSampleFormat output, int channel, int channel_layout, int samplerate) {
    _target_format = output;
    _target_channels = channel;
    _target_channel_layout = channel_layout;
    _target_samplerate = samplerate;

    _swr_frame_pool.setSize(AV_NUM_DATA_POINTERS);
}
#endif

FFmpegSwr::~FFmpegSwr() {
    if (_ctx) {
        swr_free(&_ctx);
    }
#if LIBAVCODEC_VERSION_INT >= FF_CODEC_VER_7_1
    av_channel_layout_uninit(&_target_ch_layout);
#endif
}

FFmpegFrame::Ptr FFmpegSwr::inputFrame(const FFmpegFrame::Ptr &frame) {
    if (frame->get()->format == _target_format &&

#if LIBAVCODEC_VERSION_INT >= FF_CODEC_VER_7_1
        !av_channel_layout_compare(&(frame->get()->ch_layout), &_target_ch_layout) &&
#else
        frame->get()->channels == _target_channels && frame->get()->channel_layout == (uint64_t)_target_channel_layout &&
#endif

        frame->get()->sample_rate == _target_samplerate) {
        // 不转格式  [AUTO-TRANSLATED:31dc6ae1]
        // Do not convert format
        return frame;
    }
    if (!_ctx) {

#if LIBAVCODEC_VERSION_INT >= FF_CODEC_VER_7_1
        _ctx = swr_alloc();
        swr_alloc_set_opts2(&_ctx, 
                    &_target_ch_layout, _target_format, _target_samplerate, 
                    &frame->get()->ch_layout, (AVSampleFormat)frame->get()->format, frame->get()->sample_rate,
                     0, nullptr);
#else
        _ctx = swr_alloc_set_opts(nullptr, _target_channel_layout, _target_format, _target_samplerate,
                                  frame->get()->channel_layout, (AVSampleFormat) frame->get()->format,
                                  frame->get()->sample_rate, 0, nullptr);
#endif

        InfoL << "swr_alloc_set_opts:" << av_get_sample_fmt_name((enum AVSampleFormat) frame->get()->format) << " -> "
              << av_get_sample_fmt_name(_target_format);
    }
    if (_ctx) {
        auto out = _swr_frame_pool.obtain2();
        out->get()->format = _target_format;

#if LIBAVCODEC_VERSION_INT >= FF_CODEC_VER_7_1
        out->get()->ch_layout = _target_ch_layout;
        av_channel_layout_copy(&(out->get()->ch_layout), &_target_ch_layout);
#else
        out->get()->channel_layout = _target_channel_layout;
        out->get()->channels = _target_channels;
#endif

        out->get()->sample_rate = _target_samplerate;
        out->get()->pkt_dts = frame->get()->pkt_dts;
        out->get()->pts = frame->get()->pts;

        int ret = 0;
        if (0 != (ret = swr_convert_frame(_ctx, out->get(), frame->get()))) {
            WarnL << "swr_convert_frame failed:" << ffmpeg_err(ret);
            return nullptr;
        }
        return out;
    }

    return nullptr;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////

FFmpegSws::FFmpegSws(AVPixelFormat output, int width, int height) {
    _target_format = output;
    _target_width = width;
    _target_height = height;

    _sws_frame_pool.setSize(AV_NUM_DATA_POINTERS);
}

FFmpegSws::~FFmpegSws() {
    if (_ctx) {
        sws_freeContext(_ctx);
        _ctx = nullptr;
    }
}

int FFmpegSws::inputFrame(const FFmpegFrame::Ptr &frame, uint8_t *data) {
    int ret;
    inputFrame(frame, ret, data);
    return ret;
}

FFmpegFrame::Ptr FFmpegSws::inputFrame(const FFmpegFrame::Ptr &frame) {
    int ret;
    return inputFrame(frame, ret, nullptr);
}

FFmpegFrame::Ptr FFmpegSws::inputFrame(const FFmpegFrame::Ptr &frame, int &ret, uint8_t *data) {
    ret = -1;
    TimeTicker2(30, TraceL);
    auto target_width = _target_width ? _target_width : frame->get()->width;
    auto target_height = _target_height ? _target_height : frame->get()->height;
    if (frame->get()->format == _target_format && frame->get()->width == target_width && frame->get()->height == target_height) {
        // 不转格式  [AUTO-TRANSLATED:31dc6ae1]
        // Do not convert format
        return frame;
    }
    if (_ctx && (_src_width != frame->get()->width || _src_height != frame->get()->height || _src_format != (enum AVPixelFormat)frame->get()->format)) {
        // 输入分辨率发生变化了  [AUTO-TRANSLATED:0e4ea2e8]
        // Input resolution has changed
        sws_freeContext(_ctx);
        _ctx = nullptr;
    }
    if (!_ctx) {
        _src_format = (enum AVPixelFormat) frame->get()->format;
        _src_width = frame->get()->width;
        _src_height = frame->get()->height;
        _ctx = sws_getContext(frame->get()->width, frame->get()->height, (enum AVPixelFormat) frame->get()->format, target_width, target_height, _target_format, SWS_FAST_BILINEAR, NULL, NULL, NULL);
        InfoL << "sws_getContext:" << av_get_pix_fmt_name((enum AVPixelFormat) frame->get()->format) << " -> " << av_get_pix_fmt_name(_target_format);
    }
    if (_ctx) {
        auto out = _sws_frame_pool.obtain2();
        out->reset(); // 清理旧数据和帧引用
        if (!out->get()->data[0]) {
            if (data) {
                av_image_fill_arrays(out->get()->data, out->get()->linesize, data, _target_format, target_width, target_height, 32);
            } else {
                out->fillPicture(_target_format, target_width, target_height);
            }
        }
        if (0 >= (ret = sws_scale(_ctx, frame->get()->data, frame->get()->linesize, 0, frame->get()->height, out->get()->data, out->get()->linesize))) {
            WarnL << "sws_scale failed:" << ffmpeg_err(ret);
            return nullptr;
        }

        out->get()->format = _target_format;
        out->get()->width = target_width;
        out->get()->height = target_height;
        out->get()->pkt_dts = frame->get()->pkt_dts;
        out->get()->pts = frame->get()->pts;
        return out;
    }
    return nullptr;
}

std::tuple<bool, std::string> FFmpegUtils::saveFrame(const FFmpegFrame::Ptr &frame, const char *filename, AVPixelFormat fmt, int w, int h, const char *font_path) {
    std::shared_ptr<AVFilterGraph> _filter_graph;
    AVFilterContext *buffersrc_ctx = nullptr;
    AVFilterContext *buffersink_ctx = nullptr;
    const AVFilter *buffersrc = nullptr;
    const AVFilter *buffersink = nullptr;
    // kServerName
    const string mark = "ZLMediaKit"; 
    char drawtext_args1[512];
    _StrPrinter ss;

    std::unique_ptr<FILE, void (*)(FILE *)> tmp_save_file_jpg(File::create_file(filename, "wb"), [](FILE *fp) {
        if (fp) {
            fclose(fp);
        }
    });

    if (!tmp_save_file_jpg) {
        ss << "Could not open the file " << filename;
        DebugL << ss;
        return make_tuple<bool, std::string>(false, ss.data());
    }

    std::string fontfile("");
    if (font_path && File::fileExist(font_path)) {
        fontfile = font_path;
    } else {
        // Fallback to common default
        fontfile = exeDir() + "/DejaVuSans.ttf";
    }

    snprintf(drawtext_args1, sizeof(drawtext_args1), "text='%s':fontfile='%s':fontcolor=white@0.1:fontsize=h/50:x=w*0.02:y=h-th-h*0.02", mark.data(), fontfile.c_str());

    const AVCodec *jpeg_codec = avcodec_find_encoder(fmt == AV_PIX_FMT_YUVJ420P ? AV_CODEC_ID_MJPEG : AV_CODEC_ID_PNG);
    std::unique_ptr<AVCodecContext, void (*)(AVCodecContext *)> jpeg_codec_ctx(
        jpeg_codec ? avcodec_alloc_context3(jpeg_codec) : nullptr, [](AVCodecContext *ctx) { avcodec_free_context(&ctx); });

    if (!jpeg_codec_ctx) {
        ss << "Could not allocate JPEG/PNG codec context";
        DebugL << ss;
        return make_tuple<bool, std::string>(false, ss.data());
    }

    jpeg_codec_ctx->width = (w > 0 && w < 8192) ? w : frame->get()->width;
    jpeg_codec_ctx->height = (h > 0 && h < 4320) ? h : frame->get()->height;
    jpeg_codec_ctx->pix_fmt = fmt;
    jpeg_codec_ctx->time_base = { 1, 1 };

    auto ret = avcodec_open2(jpeg_codec_ctx.get(), jpeg_codec, NULL);
    if (ret < 0) {
        ss << "Could not open JPEG/PNG codec, " << ffmpeg_err(ret);
        DebugL << ss;
        return make_tuple<bool, std::string>(false, ss.data());
    }

    FFmpegSws sws(fmt, jpeg_codec_ctx->width, jpeg_codec_ctx->height);
    auto new_frame = sws.inputFrame(frame);
    if (!new_frame) {
        ss << "Could not scale the frame";
        DebugL << ss;
        return make_tuple<bool, std::string>(false, ss.data());
    }

    _filter_graph.reset(avfilter_graph_alloc(), [](AVFilterGraph *ctx) { avfilter_graph_free(&ctx); });
    if (!_filter_graph) {
        ss << "avfilter_graph_alloc failed";
        DebugL << ss;
        return make_tuple<bool, std::string>(false, ss.data());
    }

    char args[512];
    snprintf(
        args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d", jpeg_codec_ctx->width, jpeg_codec_ctx->height,
        jpeg_codec_ctx->pix_fmt, jpeg_codec_ctx->time_base.num, jpeg_codec_ctx->time_base.den, jpeg_codec_ctx->sample_aspect_ratio.num,
        jpeg_codec_ctx->sample_aspect_ratio.den);

    buffersrc = avfilter_get_by_name("buffer");

    if ((ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in", args, NULL, _filter_graph.get())) < 0) {
        ss << "avfilter_graph_create_filter buffersrc failed: " << ret << " " << ffmpeg_err(ret);
        DebugL << ss;
        return make_tuple<bool, std::string>(false, ss.data());
    }

    buffersink = avfilter_get_by_name("buffersink");
    if ((ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out", NULL, NULL, _filter_graph.get())) < 0) {
        ss << "avfilter_graph_create_filter buffersink failed: " << ret << " " << ffmpeg_err(ret);
        return make_tuple<bool, std::string>(false, ss.data());
    }

    AVFilterContext *drawtext_ctx1 = nullptr;

    const AVFilter *drawtext_filter = avfilter_get_by_name("drawtext");
    if ((ret = avfilter_graph_create_filter(&drawtext_ctx1, drawtext_filter, "drawtext", drawtext_args1, NULL, _filter_graph.get())) < 0) {
        ss << "avfilter_graph_create_filter drawtext_filter failed: " << ret << " " << ffmpeg_err(ret);
        return make_tuple<bool, std::string>(false, ss.data());
    }

    if ((ret = avfilter_link(buffersrc_ctx, 0, drawtext_ctx1, 0) < 0 || avfilter_link(drawtext_ctx1, 0, buffersink_ctx, 0))< 0) {
        ss << "avfilter_link: " << ret << " " << ffmpeg_err(ret);
        return make_tuple<bool, std::string>(false, ss.data());
    }

    if ((ret = avfilter_graph_config(_filter_graph.get(), NULL)) < 0) {
        ss << "avfilter_graph_config failed: " << ret << " " << ffmpeg_err(ret);
        return make_tuple<bool, std::string>(false, ss.data());
    }

    if ((ret = av_buffersrc_add_frame_flags(buffersrc_ctx, new_frame->get(), 0)) < 0) {
        ss << "av_buffersink_get_frame failed: " << ret << " " << ffmpeg_err(ret);
        return make_tuple<bool, std::string>(false, ss.data());
    }

    auto pkt = alloc_av_packet();
    while (av_buffersink_get_frame(buffersink_ctx, new_frame->get()) >= 0) {
        if (avcodec_send_frame(jpeg_codec_ctx.get(), new_frame->get()) == 0) {
            while (avcodec_receive_packet(jpeg_codec_ctx.get(), pkt.get()) == 0) {
                fwrite(pkt.get()->data, pkt.get()->size, 1, tmp_save_file_jpg.get());
            }
        }
    }
    return make_tuple<bool, std::string>(true, "");
}

} // namespace mediakit
#endif // ENABLE_FFMPEG
