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

#include "Transcoder.h"
#include "Common/config.h"
#include "Transcode/TranscodeFilter.h"
#include "Util/util.h"
#include <algorithm>
#include <chrono>
#include <cmath>

// ── 声道布局的跨版本兼容 ────────────────────────────────────────────────
// ZLM 原本就区分 FFmpeg 7.1 前后两套 API（见 Transcode.h 里 FFmpegSwr 的两个构造函数）；
// 本文件新增的转码代码当初直接用了 ch_layout，于是 ffmpeg < 7.1（含 Ubuntu 22.04 的 4.4）
// 编不过：`AVFrame has no member named 'ch_layout'`。下面这组 helper 把差异收在一处，
// 分支开关与仓库既有约定保持一致（FF_CODEC_VER_7_1）。
//
// Channel-layout compatibility. ZLM already distinguishes the pre/post-7.1 APIs
// (see FFmpegSwr's two constructors); our new transcode code used ch_layout
// unconditionally, so anything below 7.1 (including Ubuntu 22.04's 4.4) failed to
// build. Keep the difference here, using the repo's existing switch.
#if LIBAVCODEC_VERSION_INT >= FF_CODEC_VER_7_1
static inline int frameChannels(const AVFrame *f) { return f ? f->ch_layout.nb_channels : 0; }
static inline int ctxChannels(const AVCodecContext *c) { return c ? c->ch_layout.nb_channels : 0; }
/** 把上下文的声道布局拷给 frame（老 API 是 channels + channel_layout 两个字段） */
static inline void copyCtxLayoutToFrame(AVFrame *f, const AVCodecContext *c) {
    av_channel_layout_copy(&f->ch_layout, &c->ch_layout);
}
#else
static inline int frameChannels(const AVFrame *f) { return f ? f->channels : 0; }
static inline int ctxChannels(const AVCodecContext *c) { return c ? c->channels : 0; }
static inline void copyCtxLayoutToFrame(AVFrame *f, const AVCodecContext *c) {
    f->channels = c->channels;
    f->channel_layout = c->channel_layout;
}
#endif


using namespace std;
using namespace toolkit;


/**
 * 探测 Annex-B 起始码长度
 * Detect the Annex-B start code length.
 *
 * ffmpeg 的 H264/H265 编码器输出 Annex-B 格式，但起始码可能是 4 字节
 * (0x00 0x00 0x00 0x01) 也可能是 3 字节 (0x00 0x00 0x01)，取决于首个 NAL
 * 前的填充。ZLM 的 Track::inputFrame 必须知道这个长度才能正确读出 NAL 头。
 * ffmpeg's H264/H265 encoders emit Annex-B, but the start code may be 4 bytes
 * (0x00 0x00 0x00 0x01) or 3 bytes (0x00 0x00 0x01). Track::inputFrame must
 * know this length to read the NAL header correctly.
 *
 * @return 3 或 4；无法识别时返回 0
 */
/**
 * 把 Annex-B 整包拆成单 NAL 帧
 * Split an Annex-B packet into single-NAL frames.
 *
 * ZLM 的 RTP/RTMP 打包器只按第一个 NAL 的类型分发：拿到含 VPS+SPS+PPS+IDR 的整包时，
 * 它会把这个包当作 config 存起来就返回，IDR 被整体丢弃，观众端收不到画面
 * （ffmpeg 报 "Illegal temporal ID in RTP/HEVC packet"）。
 * 所以编码输出必须拆成单 NAL 再往下传，与 ZLM 内部 H264/H265 的流转形态一致。
 * ZLM's RTP/RTMP packers dispatch on the first NAL type only: given a packet carrying
 * VPS+SPS+PPS+IDR they store it as config and return, dropping the IDR entirely so
 * viewers get no picture (ffmpeg reports "Illegal temporal ID in RTP/HEVC packet").
 * Encoder output therefore has to be split into single-NAL frames, matching how
 * H264/H265 flow inside ZLM.
 *
 * @param ptr 整包起始（含起始码）
 * @param len 整包长度
 * @param prefix 首个 NAL 的起始码长度
 * @param cb 每个 NAL 的回调（ptr 为含起始码的子包起点，len 为子包长度，prefix 为该子包的起始码长度）
 */
static void splitAnnexb(const char *ptr, size_t len, size_t prefix,
                        const std::function<void(const char *, size_t, size_t)> &cb) {
    if (!ptr || len == 0 || prefix == 0) {
        return;
    }
    const uint8_t *data = (const uint8_t *) ptr;
    // 先扫出所有起始码的位置和长度，再按相邻位置切段。
    // ffmpeg 的输出会混用 4 字节(00 00 00 01)和 3 字节(00 00 01)起始码，
    // 必须逐个记录而不能假定统一长度。
    // First collect every start code position and length, then cut segments between
    // neighbours. ffmpeg's output mixes 4-byte (00 00 00 01) and 3-byte (00 00 01)
    // start codes, so each must be recorded instead of assuming a uniform length.
    std::vector<std::pair<size_t, size_t>> codes;
    size_t i = 0;
    while (i + 3 <= len) {
        if (data[i] == 0 && data[i + 1] == 0) {
            if (data[i + 2] == 1) {
                codes.emplace_back(i, 3);
                i += 3;
                continue;
            }
            if (i + 4 <= len && data[i + 2] == 0 && data[i + 3] == 1) {
                codes.emplace_back(i, 4);
                i += 4;
                continue;
            }
        }
        ++i;
    }
    // 统一输出 4 字节起始码：下游 H265RtpEncoder 用 data() + prefixSize() 定位
    // 裸 NAL 头，而 prefixSize 只有一个值。若各 NAL 的起始码长度不一（ffmpeg 会混用
    // 3/4 字节），3 字节起始码的那一帧就会多留一个 0x00 在 NAL 头前面，
    // RTP 打包时被当成 NAL 头解析 → 观众端报 Illegal temporal ID / Multi-layer HEVC，
    // 更严重的是整包再被当 NAL 做二次 FU 封装（出现 b0=0x62 这种 NAL type 49 的包）。
    // 因此这里为每个 NAL 复制一份、前置 4 字节起始码，保证 prefixSize 恒为 4。
    // Emit a uniform 4-byte start code: downstream H265RtpEncoder locates the raw NAL
    // header via data() + prefixSize(), and prefixSize is a single value. When start
    // codes vary in length (ffmpeg mixes 3- and 4-byte), every 3-byte one leaves an extra
    // 0x00 in front of the NAL header, so the RTP packer parses garbage -> the viewer
    // reports "Illegal temporal ID" / "Multi-layer HEVC", and worse, whole packets get
    // FU-wrapped a second time (packets with b0=0x62, NAL type 49). So copy each NAL with
    // a 4-byte start code prepended to keep prefixSize constant at 4.
    for (size_t k = 0; k < codes.size(); ++k) {
        size_t pos = codes[k].first;
        size_t pre = codes[k].second;
        size_t end = (k + 1 < codes.size()) ? codes[k + 1].first : len;
        if (end <= pos + pre) {
            continue;
        }
        size_t nal_len = end - pos - pre;
        // ZLM 侧已经没有统一的起始码长度，下游只认 prefixSize 一个值，
        // 所以把裸 NAL 交给调用方、由它用固定的 4 字节起始码组装帧。
        // Downstream accepts a single prefixSize, so hand the bare NAL to the caller
        // and let it assemble the frame with a fixed 4-byte start code.
        cb(ptr + pos + pre, nal_len, pre);
    }
}

static size_t probeAnnexbStartCodeLen(const uint8_t *data, size_t size) {
    if (!data || size < 3) {
        return 0;
    }
    if (size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) {
        return 4;
    }
    if (data[0] == 0 && data[1] == 0 && data[2] == 1) {
        return 3;
    }
    return 0;
}

namespace mediakit {

/**
 * 转码输出的单 NAL 帧
 *
 * 必须自己报告 keyFrame()/configFrame()。用 `FrameFromBuffer<FrameFromPtr>` 直接构造时
 * 这两个都是 false（`FrameFromPtr::_is_key` 默认 false，`configFrame()` 在 FrameFromPtr 里
 * 恒返回 false），下游后果是三处一起坏：
 *  1. `MultiMediaSourceMuxer` 的 GOP 缓存只按 `frame->keyFrame() || configFrame()` 标记
 *     GOP 起点。全 false 时缓存永不重定位，观众**中途接入**拿到的第一个视频帧不是 IDR，
 *     解码器立刻报 "Could not find ref with POC"（实测复现）。
 *  2. `H265Track::inputFrame_l` 在关键帧前补插 config 帧的逻辑失效，新观众可能先收到
 *     切片后收到 SPS。
 *  3. RTP 打包不标 key 位（`CommonRtpEncoder::inputFrame` 的 is_key），丢包重传/秒开都受影响。
 *
 * 判定语义与 ext-codec 的 H265FrameHelper/H264FrameHelper 保持一致（单切片判定也照抄：
 * 多切片时只有第一片算帧开始，否则 GOP 缓存会从中间一片起，同样解不开）。
 * 这里不复用它们是为了不让 Transcoder 依赖 ext-codec 的具体类型。
 *
 * Frames emitted by the encoder must report keyFrame()/configFrame() themselves.
 * Constructed as `FrameFromBuffer<FrameFromPtr>` both are always false
 * (`_is_key` defaults false, `configFrame()` is hardcoded false in FrameFromPtr),
 * which breaks three things at once: the muxer's GOP cache never restarts at a
 * keyframe (a viewer joining mid-stream starts decoding at a non-IDR frame and
 * immediately reports "Could not find ref with POC" — reproduced), H265Track stops
 * inserting config frames before keyframes, and RTP never sets the key bit. The
 * classification matches ext-codec's H265FrameHelper/H264FrameHelper, including the
 * first-slice-only rule (multi-slice frames must not all count as a picture start).
 */
class TranscodedFrame : public FrameFromBuffer<FrameFromPtr> {
public:
    TranscodedFrame(CodecId codec, toolkit::Buffer::Ptr buf, uint64_t dts, uint64_t pts, size_t prefix)
        : FrameFromBuffer<FrameFromPtr>(codec, buf, dts, pts, prefix) {
    }

    /**
     * 裸帧 codec（VP8/VP9/AV1）的关键帧只能由编码器包的 AV_PKT_FLAG_KEY 给出：
     * 它们没有 NAL 头可以解析。走 NAL 判定的默认分支会按码流首字节瞎猜，
     * 结果 GOP 缓存不从关键帧起、观众中途接入解不出来。
     * For raw-packet codecs (VP8/VP9/AV1) the keyframe flag can only come from the
     * encoder's AV_PKT_FLAG_KEY — they have no NAL header to parse. The NAL-based
     * default would guess from the first byte of the bitstream, so the GOP cache
     * would not restart at a keyframe and mid-stream viewers could not decode.
     */
    void setKeyFrameFromPacket(bool key) { _key_from_packet = key; }

    bool keyFrame() const override {
        if (isRawPacketCodec()) {
            return _key_from_packet;
        }
        auto ptr = nalHeader();
        if (!ptr) {
            return false;
        }
        if (getCodecId() == CodecH264) {
            // IDR=5；多切片时 first_mb_in_slice 为 0 才是帧开始
            // IDR=5; with multiple slices only the one with first_mb_in_slice==0 starts the frame
            return (ptr[0] & 0x1F) == 5 && (ptr[1] & 0x80);
        }
        // H265：IRAP 覆盖 [BLA_W_LP(16), RSV_IRAP_VCL23(23)]；多切片时看
        // first_slice_segment_in_pic_flag
        // H265: IRAP spans [BLA_W_LP(16), RSV_IRAP_VCL23(23)]; for multiple slices check
        // first_slice_segment_in_pic_flag
        int type = (ptr[0] >> 1) & 0x3F;
        return type >= 16 && type <= 23 && (ptr[2] & 0x80);
    }

    bool configFrame() const override {
        // 裸帧 codec 没有独立的 config 帧（sequence header 就带在关键帧里），
        // 当成 config 会让下游跳过这一帧的画面
        // Raw-packet codecs have no standalone config frame (the sequence header
        // rides along with the keyframe); claiming otherwise makes downstream skip
        // the picture entirely
        if (isRawPacketCodec()) {
            return false;
        }
        auto ptr = nalHeader();
        if (!ptr) {
            return false;
        }
        if (getCodecId() == CodecH264) {
            int type = ptr[0] & 0x1F;
            return type == 7 || type == 8;   // SPS / PPS
        }
        int type = (ptr[0] >> 1) & 0x3F;
        return type == 32 || type == 33 || type == 34;   // VPS / SPS / PPS
    }

private:
    bool isRawPacketCodec() const {
        auto codec = getCodecId();
        return codec == CodecVP8 || codec == CodecVP9 || codec == CodecAV1;
    }

    bool _key_from_packet = false;
    const uint8_t *nalHeader() const {
        // size 太短时保守当非关键帧：调用方（emit）保证至少 4 字节起始码 + 1 字节 NAL 头，
        // 但热身/异常路径下也不能越界读。
        // Be conservative on short buffers: emit guarantees 4-byte start code + NAL header,
        // but warmup and error paths must not read out of bounds either.
        if (size() <= prefixSize() + 2) {
            return nullptr;
        }
        return (const uint8_t *) data() + prefixSize();
    }
};


static string ffmpeg_err_str(int errnum) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(errnum, errbuf, AV_ERROR_MAX_STRING_SIZE);
    return errbuf;
}

static AVPacket *alloc_packet() {
    return av_packet_alloc();
}

static void free_packet(AVPacket **pkt) {
    if (pkt && *pkt) {
        av_packet_free(pkt);
    }
}

//////////////////////////////////////////////////////////////////////////////////////////
// TranscodeAudioFifo

TranscodeAudioFifo::~TranscodeAudioFifo() {
    if (_fifo) {
        av_audio_fifo_free(_fifo);
        _fifo = nullptr;
    }
}

bool TranscodeAudioFifo::write(const AVFrame *frame) {
    if (!frame || !frame->nb_samples) {
        return false;
    }
    if (!_fifo) {
        _channels = frameChannels(frame);
        _samplerate = frame->sample_rate;
        _format = (AVSampleFormat) frame->format;
        if (_channels <= 0 || _samplerate <= 0) {
            WarnL << "fifo init with invalid audio params, channels=" << _channels << ", samplerate=" << _samplerate;
            return false;
        }
        _fifo = av_audio_fifo_alloc(_format, _channels, frame->nb_samples * 4);
        if (!_fifo) {
            WarnL << "av_audio_fifo_alloc failed";
            return false;
        }
        TraceL << "audio fifo init, " << _channels << "ch " << _samplerate << "Hz "
               << av_get_sample_fmt_name(_format);
    } else if (_channels != frameChannels(frame)
               || _samplerate != frame->sample_rate
               || _format != (AVSampleFormat) frame->format) {
        // 参数变了，必须重建，否则会写入错位数据
        // Parameters changed, must rebuild, otherwise misaligned data will be written
        WarnL << "audio params changed, rebuilding fifo"
              << ", " << _channels << "ch/" << _samplerate << "Hz/" << av_get_sample_fmt_name(_format)
              << " -> " << frameChannels(frame) << "ch/" << frame->sample_rate
              << "Hz/" << av_get_sample_fmt_name((AVSampleFormat) frame->format);
        // 关键：先把成员更新成新参数再分配。旧代码用旧的 _format/_channels 重建，
        // 再用新格式去 av_audio_fifo_write，格式与 FIFO 不符 → 写入失败/数据错位，
        // 表现为换轨（分辨率/声道变更）后整条音频变成静音或爆音。
        // Critical: update the members to the new parameters BEFORE allocating. The old
        // code rebuilt with the OLD _format/_channels then wrote new-format data into it,
        // so the format never matched the FIFO -> write failure/misaligned samples,
        // i.e. audio went silent or garbled after a mid-stream parameter change.
        _channels = frameChannels(frame);
        _samplerate = frame->sample_rate;
        _format = (AVSampleFormat) frame->format;
        av_audio_fifo_free(_fifo);
        _fifo = av_audio_fifo_alloc(_format, _channels, frame->nb_samples * 4);
        if (!_fifo) {
            WarnL << "av_audio_fifo_alloc failed on rebuild";
            return false;
        }
    }
    auto ret = av_audio_fifo_write(_fifo, (void **) frame->data, frame->nb_samples);
    if (ret < 0) {
        WarnL << "av_audio_fifo_write failed:" << ffmpeg_err_str(ret);
        return false;
    }
    return ret == frame->nb_samples;
}

bool TranscodeAudioFifo::read(AVFrame *frame, int sample_size) {
    if (!_fifo || sample_size <= 0) {
        return false;
    }
    if (av_audio_fifo_size(_fifo) < sample_size) {
        return false;
    }
    auto ret = av_audio_fifo_read(_fifo, (void **) frame->data, sample_size);
    if (ret < 0) {
        WarnL << "av_audio_fifo_read failed:" << ffmpeg_err_str(ret);
        return false;
    }
    frame->nb_samples = ret;
    return ret == sample_size;
}

int TranscodeAudioFifo::size() const {
    return _fifo ? av_audio_fifo_size(_fifo) : 0;
}

//////////////////////////////////////////////////////////////////////////////////////////
// FFmpegEncoder

/** 判断一个 AVCodec 是不是硬件编码器（用于回落与日志） */
static bool isHwEncoderCodec(const AVCodec *codec) {
    if (!codec || !codec->name) {
        return false;
    }
    static const char *kMarks[] = {"videotoolbox", "nvenc", "vaapi", "qsv", "amf", "v4l2m2m", "mediacodec"};
    for (auto mark : kMarks) {
        if (strstr(codec->name, mark)) {
            return true;
        }
    }
    return false;
}

/**
 * 该 codec 的平台首选硬件编码器名。
 *
 * macOS 用 VideoToolbox；Linux/Windows 首选 NVENC（无 N 卡时再由 findEncoder 回落到
 * 软件编码）。不硬编没有对应实现的 codec（VP8/VP9/AV1 暂不列）—— 它们会拿到 nullptr
 * 从而直接走软件路径。
 *
 * Preferred hardware encoder per platform. macOS uses VideoToolbox; Linux/Windows prefer
 * NVENC. Codecs without a hardware implementation return nullptr and stay software.
 */
static const char *hwEncoderName(CodecId codec_id) {
#if defined(__APPLE__)
    if (codec_id == CodecH264) return "h264_videotoolbox";
    if (codec_id == CodecH265) return "hevc_videotoolbox";
#elif defined(_WIN32) || defined(__linux__)
    if (codec_id == CodecH264) return "h264_nvenc";
    if (codec_id == CodecH265) return "hevc_nvenc";
#endif
    return nullptr;
}

/** 按 track codec 选编码器，返回 nullptr 表示不支持 */
static const AVCodec *findEncoder(CodecId codec_id, bool prefer_hw = false) {
    auto find = [](const char *name, AVCodecID id) -> const AVCodec * {
        if (name) {
            auto codec = avcodec_find_encoder_by_name(name);
            if (codec) {
                InfoL << "got encoder by name:" << name;
                return codec;
            }
        }
        auto codec = avcodec_find_encoder(id);
        if (codec) {
            InfoL << "got encoder:" << avcodec_get_name(id);
        } else {
            WarnL << "encoder not found:" << avcodec_get_name(id);
        }
        return codec;
    };

    // 硬件优先：拿到就用，拿不到继续走下面的软件路径（不报错、不影响出流）
    // Hardware first: if unavailable, fall through to software silently.
    if (prefer_hw) {
        if (auto hw_name = hwEncoderName(codec_id)) {
            if (auto hw = avcodec_find_encoder_by_name(hw_name)) {
                InfoL << "got hardware encoder:" << hw_name;
                return hw;
            }
            WarnL << "hardware encoder unavailable, using software:" << hw_name;
        }
    }

    switch (codec_id) {
        case CodecH264: return find("libx264", AV_CODEC_ID_H264);
        case CodecH265: return find("libx265", AV_CODEC_ID_HEVC);
        case CodecAAC:  return find(nullptr, AV_CODEC_ID_AAC);
        case CodecOpus: return find("libopus", AV_CODEC_ID_OPUS);
        case CodecG711A:return find(nullptr, AV_CODEC_ID_PCM_ALAW);
        case CodecG711U:return find(nullptr, AV_CODEC_ID_PCM_MULAW);
        // G722 在 ffmpeg 里是 adpcm_g722（16kHz 单声道，但样本按 8kHz 计时，
        // 这是 G722 的历史包袱：RTP 时钟 16000、帧长 20ms = 320 样本）
        // G722 lives in ffmpeg as adpcm_g722 (16kHz mono; its RTP clock is 16000
        // with 20ms frames = 320 samples)
        case CodecG722: return find(nullptr, AV_CODEC_ID_ADPCM_G722);
        // MP3 必须显式找 libmp3lame：裸 AV_CODEC_ID_MP3 在部分构建里只注册了解码器，
        // 旧代码根本没有这个 case，配 audio_codec=MP3 会直接抛「未找到编码器」。
        // MP3 must resolve libmp3lame explicitly: a bare AV_CODEC_ID_MP3 build may only
        // register the decoder. The old code had no case at all, so audio_codec=MP3 threw
        // "encoder not found" and silently fell back to passthrough.
        case CodecMP3:  return find("libmp3lame", AV_CODEC_ID_MP3);
        case CodecVP8:  return find("libvpx", AV_CODEC_ID_VP8);
        case CodecVP9:  return find("libvpx-vp9", AV_CODEC_ID_VP9);
        // AV1 优先 libaom-av1 而不是更快的 libsvtav1：libaom 在 lag-in-frames=0 下
        // 第一帧就出包（warmup 才能拿到关键帧、目标轨才能 ready），而 SVT-AV1 有
        // 内部 lookahead，首帧一个包都不吐（实测 warmup produced no packet），
        // 目标轨因此永远 unready，整条视频轨被 muxer 丢弃。
        // AV1 prefers libaom-av1 over the faster libsvtav1: with lag-in-frames=0 libaom
        // emits a packet on the very first frame (what warmup needs to make the target
        // track ready), while SVT-AV1 buffers internally and emits nothing for the first
        // frame (measured: "warmup produced no packet"), leaving the track forever
        // unready so every muxer drops the whole video track.
        case CodecAV1:  return find("libaom-av1", AV_CODEC_ID_AV1);
#if LIBAVCODEC_VERSION_MAJOR >= 60
        // H266/VVC：ffmpeg 侧只有解码器（vvc dec），编码器要靠外部 libvvenc，
        // 主流发行版/conda 构建都没带。这里显式说明，而不是落到 default 的
        // 「未找到编码器」——否则看起来像我们没接。
        // H266/VVC: ffmpeg ships a decoder only; encoding needs the external libvvenc,
        // which neither distro nor conda builds enable. Fail with a reason instead of
        // the generic "encoder not found".
        case CodecH266: return find("libvvenc", AV_CODEC_ID_VVC);
#endif
        default:        return nullptr;
    }
}

FFmpegEncoder::FFmpegEncoder(const Track::Ptr &track, int thread_num, float src_fps,
                             const Track::Ptr &src_video, const VideoTargetParams &video_params) {
    if (!track) {
        throw std::runtime_error("encoder created with null track");
    }
    // _video_fps 是「输出」帧率，_src_fps 是「源」帧率：
    // 降帧抽帧要用两者之比，时间轴推进要用输出帧率。
    // _video_fps is the OUTPUT frame rate, _src_fps the SOURCE one: decimation
    // needs both, timeline advancement needs the output rate.
    _src_fps = src_fps;
    _video_params = video_params;
    _video_fps = (_video_params.fps > 0) ? _video_params.fps : src_fps;
    _codec_id = track->getCodecId();
    // 源帧率是否需要实测：裸帧 codec（VP8/VP9/AV1）的码流与 SDP 都不带帧率，
    // 源轨的 getVideoFps() 只是 VideoTrackImp 的默认 30；任何 codec 只要源轨
    // 没给出帧率（<=0）同样只能实测。有 SPS/VUI 帧率的 H264/H265 不走这条路。
    // Whether the source rate must be measured: raw-packet codecs (VP8/VP9/AV1)
    // carry no frame rate in the bitstream or SDP, so the track's getVideoFps() is
    // just VideoTrackImp's default 30; any codec whose track reports no rate (<=0)
    // needs measuring too. H264/H265 (rate present in SPS/VUI) never take this path.
    _src_fps_measure = track->getTrackType() == TrackVideo && src_video &&
                       (src_fps <= 0.5f || src_video->getCodecId() == CodecVP8 ||
                        src_video->getCodecId() == CodecVP9 || src_video->getCodecId() == CodecAV1);
#ifdef ENABLE_ASCEND_VENC
    // 昇腾硬编：只在「视频 + 要求硬编 + H264/H265 + 本机 DVPP 可用」时接管。
    // 注意这条路径不需要 libavcodec 编码器（_context 全程为空），
    // 所以下面的 findEncoder 失败不算错误。
    if (_video_params.hw_encode && track->getTrackType() == TrackVideo &&
        (_codec_id == CodecH264 || _codec_id == CodecH265) && AscendVenc::available(0)) {
        _ascend_wanted = true;
        InfoL << "transcode: video will be encoded by Ascend DVPP VENC";
    }
#endif
    _codec = findEncoder(_codec_id,
                         _video_params.hw_encode && track->getTrackType() == TrackVideo);
#ifdef ENABLE_ASCEND_VENC
    // 昇腾路径不需要 libavcodec 编码器，这里不能因为找不到就抛异常
    const bool ascend_ready = _ascend_wanted;
#else
    const bool ascend_ready = false;
#endif
    if (!_codec && !ascend_ready) {
        throw std::runtime_error("未找到编码器: " + string(::mediakit::getCodecName(_codec_id)));
    }

    _track_index = track->getIndex();
    int bitrate = (int) track->getBitRate();
    if (track->getTrackType() == TrackVideo) {
        auto video = static_pointer_cast<VideoTrack>(track);
        int width = video->getVideoWidth();
        int height = video->getVideoHeight();
        // 目标轨（H265Track 等）是空构造，宽高为 0，要等 inputFrame 解析 SPS 才补齐。
        // 但编码器在 open2 时就必须知道尺寸，所以优先用源轨的宽高。
        // The target track (H265Track etc.) is default-constructed with width/height 0
        // and only fills them after inputFrame parses the SPS. The encoder needs the
        // size at open2 time, so prefer the source track's dimensions.
        int src_w = width, src_h = height;
        if ((src_w <= 0 || src_h <= 0) && src_video) {
            if (auto src = static_pointer_cast<VideoTrack>(src_video)) {
                src_w = src->getVideoWidth();
                src_h = src->getVideoHeight();
            }
        }
        // 目标尺寸：显式配置优先，否则跟随源。注意不能只改一半（宽给高不给），
        // 那会输出极端长宽比的画面，所以必须成对生效。
        // Target size: explicit config wins, otherwise follow the source. A half-set
        // pair (width without height) would produce an extreme aspect ratio, so the
        // two must be applied together.
        if (_video_params.width > 0 && _video_params.height > 0) {
            width = _video_params.width;
            height = _video_params.height;
            // 缩放有硬件/编码器约束：H264/H265 的宏块对齐是按 2 的倍数，
            // 奇数尺寸会被编码器拒绝或裁掉最后一行像素，这里向上取偶数。
            // Scaling has encoder constraints: H264/H265 macroblocks align to 2, so
            // odd dimensions are rejected or crop the last row; round up to even.
            width += width & 1;
            height += height & 1;
        } else {
            width = src_w;
            height = src_h;
        }
        _out_width = width;
        _out_height = height;
        if (_video_params.bitrate > 0) {
            bitrate = _video_params.bitrate;
        } else if (_video_params.hw_encode && bitrate <= 0) {
            // 硬件编码器必须给明确码率：VideoToolbox/NVENC 在 bit_rate 未设时没有
            // 可预期的默认质量（实测会按极低码率出画面）。软编则允许留空由编码器决定。
            // Hardware encoders need an explicit bitrate: with bit_rate unset VideoToolbox /
            // NVENC have no predictable default and produced a very low-quality picture.
            long px = (long) width * height;
            bitrate = px >= 1920L * 1080 ? 6000000 : px >= 1280L * 720 ? 4000000
                      : px >= 640L * 480 ? 1500000 : 800000;
            InfoL << "hw encoder without explicit bitrate, defaulting to " << bitrate << "bps";
        }
        bool ret;
#ifdef ENABLE_ASCEND_VENC
        if (_ascend_wanted) {
            ret = openAscendVideo(width, height, bitrate);
        } else
#endif
        {
            ret = openVideoCodec(width, height, bitrate, _codec);
        }
        if (!ret && _video_params.hw_encode && isHwEncoderCodec(_codec)) {
            // 硬件编码器存在但打不开（驱动异常、并发会话数上限、分辨率超出硬件能力）
            // 必须回落软件编码：宁可用 CPU 也不能让这条流起不来。
            // The hardware encoder exists but refuses to open (driver issue, session limit,
            // resolution beyond capability). Fall back to software: burning CPU is better
            // than refusing to start the stream.
            WarnL << "hardware encoder open failed, falling back to software: " << _codec->name;
            if (auto soft = findEncoder(_codec_id, false)) {
                _codec = soft;
                ret = openVideoCodec(width, height, bitrate, _codec);
            }
        }
        if (!ret) {
            throw std::runtime_error(string("打开视频编码器失败: ") + _codec->name);
        }
        InfoL << "transcode video encoder: " << _codec->name
              << (isHwEncoderCodec(_codec) ? " (hardware)" : " (software)")
              << " " << width << "x" << height << " @" << bitrate << "bps";
    } else {
        auto audio = static_pointer_cast<AudioTrack>(track);
        auto sample_rate = audio->getAudioSampleRate();
        auto channel = audio->getAudioChannel();
        // PCMA/PCMU 的 RTP 载荷规格是死的：8000Hz 单声道（见 Rtsp.h 的 payload 表
        // XX(PCMA, TrackAudio, 8, 8000, 1, ...)）。编码器必须按这个规格打开，
        // 否则输出时间戳的时基与轨道声明不一致——实测 8kHz 源打成 PCMA 时，
        // 5 秒音频被塞进 795~1594 个包（正确值 250），观众端把 8k 内容当更高速率播，
        // 5 秒声音被拉成 30 秒。强制 8k 单声道后由 swr 负责重采样与下混。
        //
        // PCMA/PCMU have a fixed RTP payload format: 8000Hz mono (see the payload
        // table in Rtsp.h). Opening the encoder at the source rate makes the output
        // timestamp base disagree with the declared track: measured 795..1594 packets
        // for 5s of 8kHz audio (correct is 250), and the receiver played 5s as ~30s.
        // Forcing 8k mono lets swr do the resample and downmix.
        // 载荷规格写死的 codec（G711/MP3）必须按规格打开编码器，与目标轨的定义共用
        // forcedAudioSpec()，避免两处各算一套
        // Codecs with a fixed RTP payload (G711/MP3) must be opened at that spec,
        // sharing forcedAudioSpec() with the target-track creation
        forcedAudioSpec(_codec_id, sample_rate, channel);
        auto ret = openAudioCodec(sample_rate, channel, bitrate, _codec, track);
        if (!ret) {
            throw std::runtime_error(string("打开音频编码器失败: ") + _codec->name);
        }
    }

    if (thread_num > 0 && track->getTrackType() == TrackVideo) {
        // 视频才开异步线程；音频必须严格保序  [AUTO-TRANSLATED:55e1c5b0]
        // Only video starts an async thread; audio must strictly preserve order
        startThread("encoder thread");
    }
    InfoL << "encoder ready:" << _codec->name
          << ", codec=" << ::mediakit::getCodecName(_codec_id)
          << ", bitrate=" << (_context ? _context->bit_rate : bitrate)
          << ", out_size=" << _out_width << "x" << _out_height
          << ", out_fps=" << _video_fps << ", src_fps=" << _src_fps;
}

FFmpegEncoder::~FFmpegEncoder() {
    stopThread(true);
    flush();
#ifdef ENABLE_ASCEND_VENC
    if (_ascend) {
        _ascend->close();
        _ascend.reset();
    }
    if (_ascend_sws) {
        sws_freeContext(_ascend_sws);
        _ascend_sws = nullptr;
    }
#endif
    if (_context) {
        _context.reset();
    }
}

/**
 * 估算自动码率
 *
 * bpp（bit per pixel）按编码器压缩效率取值：H265 比 H264 省约 30% 码率，
 * 用同一个 bpp 会让 H265 输出明显偏大。
 * 实测（320x240@15 噪声内容）：H264 bpp≈0.09、H265 bpp≈0.06 时画质与码率平衡良好。
 * Estimate the automatic bitrate. The bits-per-pixel factor depends on codec
 * efficiency: H265 needs about 30% less than H264, so a shared factor would make
 * H265 output noticeably oversized.
 */
int FFmpegEncoder::estimateBitrate(int width, int height, int fps) const {
    // 亚像素尺寸没有意义，先夹到最小 16x16
    // Sub-pixel sizes are meaningless; clamp to at least 16x16
    int w = width > 16 ? width : 16;
    int h = height > 16 ? height : 16;
    int f = fps > 0 ? fps : 25;
    double bpp = (_codec_id == CodecH265 || _codec_id == CodecH266) ? 0.06 : 0.09;
    auto bps = (int64_t) ((double) w * (double) h * (double) f * bpp);
    if (bps < 32 * 1024) {
        // 下限只保护自动估算：显式配置的码率必须被尊重，不能被悄悄改写
        // The floor only protects the auto-estimate; an explicit bitrate must be
        // honoured, never silently rewritten
        bps = 32 * 1024;
    }
    return (int) bps;
}

/**
 * 降帧判断
 *
 * 只在「显式配置的输出帧率 < 源帧率」时抽帧，其余情况一律保留（不插帧：转码
 * 不能凭空造帧，上游来多少帧是这个数）。用整数累加避免浮点漂移：
 * 第 n 个源帧在目标时间轴上的位置是 n*out/src，取整变化时才输出。
 * Decide whether to keep a source frame. Frames are dropped only when the
 * explicitly configured output fps is below the source fps; otherwise everything
 * is kept (frames are never synthesised — transcoding cannot invent what upstream
 * did not send). Integer accumulation avoids float drift: the n-th source frame
 * sits at n*out/src on the target timeline, and is emitted when that integer steps.
 */
bool FFmpegEncoder::shouldKeepFrame(const AVFrame *frame) {
    // 目标帧率：配置的目标与「过载降帧后的帧率」取小者
    float target = _video_fps;
    if (_adapt_fps && _adapt_fps_now > 0.5f && (_video_fps <= 0.5f || _adapt_fps_now < _video_fps)) {
        target = _adapt_fps_now;
    }
    if (_src_fps <= 0.5f || target <= 0.5f || target >= _src_fps - 0.01f) {
        // 不降帧：不计数，避免计数器无意义增长
        // No decimation: skip the counters entirely
        return true;
    }
    // 目标帧率按整数比处理（如 30→15 是 1/2，30→12 是 2/5）
    // Treat the rates as an integer ratio (30->15 is 1/2, 30->12 is 2/5)
    int64_t out = (int64_t) lroundf(target * 100.0f);
    int64_t src = (int64_t) lroundf(_src_fps * 100.0f);
    if (out <= 0 || src <= 0) {
        return true;
    }
    _drop_index++;
    // 第 n 帧（从 0 计）应落在 floor(n*out/src)；位置前进就保留
    // Frame n (0-based) lands on floor(n*out/src); keep it when that advances
    int64_t should_have = (_drop_index - 1) * out / src;
    bool keep = should_have >= _keep_index;
    if (keep) {
        _keep_index = should_have + 1;
    }
    // 关键帧无条件保留，否则下游拿不到 GOP 起点，观众端就是「花屏/黑块」。
    // 过载降帧时这一条尤其重要：降得越狠，越容易正好抽到 I 帧。
    // Keyframes are always kept: dropping one leaves downstream without a GOP start,
    // which shows up as corruption/black blocks on the viewer side. This matters most
    // exactly when we drop aggressively.
    if (frame && frame->pict_type == AV_PICTURE_TYPE_I) {
        return true;
    }
    return keep;
}

/**
 * 实测源帧间隔
 *
 * VP8/VP9/AV1 的源没有任何帧率元数据，只能看解码帧的时间戳间隔。做法：
 * 保留最近 kSrcFpsWindow 个正向间隔（<=1000ms，排除断流/跳变），去掉一个
 * 最小值和最大值后取平均 —— 丢帧造成的 2 倍间隔、以及个别抖动都被剔除，
 * 而 CFR 源在 90000/15 = 6000 tick/帧这种除不尽的情况下间隔是 66/67 交替，
 * 平均正好回到 66.67ms（误差 <0.5%，长期不漂）。
 *
 * Measure the source frame interval. VP8/VP9/AV1 sources carry no frame rate at
 * all, so the only evidence is the spacing of decoded frame timestamps. Keep the
 * last kSrcFpsWindow positive deltas (<=1000ms; drops/stalls excluded), trim the
 * min and max, and average the rest: a missed frame (2x interval) and one-off
 * jitter are trimmed while a CFR source whose interval does not divide evenly
 * (90000/15 = 6000 ticks) alternates 66/67 and averages back to 66.67ms
 * (<0.5% error, no long-term drift).
 */
void FFmpegEncoder::updateSrcFpsEstimate(int64_t src_ms) {
    if (src_ms == (int64_t) AV_NOPTS_VALUE || src_ms < 0) {
        return;
    }
    if (_src_fps_last_ms != (int64_t) AV_NOPTS_VALUE) {
        int64_t delta = src_ms - _src_fps_last_ms;
        // 只收正常帧间隔：<=0 是乱序/重复，>1000ms 是断流或时间戳跳变，
        // 都不代表帧率，收进去会把估算拉偏。
        // Only normal intervals: <=0 means reorder/duplicate, >1000ms means a stall
        // or a timestamp jump; feeding either would skew the estimate.
        if (delta > 0 && delta <= 1000) {
            _src_fps_deltas[_src_fps_delta_pos] = delta;
            _src_fps_delta_pos = (_src_fps_delta_pos + 1) % kSrcFpsWindow;
            if (_src_fps_delta_count < kSrcFpsWindow) {
                ++_src_fps_delta_count;
            }
        }
    }
    _src_fps_last_ms = src_ms;

    // 两个样本才有一个间隔，样本不足时保持上一次的结果（首帧就是 0）
    // One interval needs two samples; keep the previous value while starved
    // (it is 0 on the very first frame, so the caller falls back to the
    // declared rate for the first couple of frames).
    if (_src_fps_delta_count < 2) {
        return;
    }

    int64_t sorted[kSrcFpsWindow];
    for (int i = 0; i < _src_fps_delta_count; ++i) {
        sorted[i] = _src_fps_deltas[i];
    }
    std::sort(sorted, sorted + _src_fps_delta_count);
    // 样本够多时去掉一个最小/最大值；样本很少时不敢丢，直接平均
    // Trim one min/max once there are enough samples; with very few samples
    // trimming could throw away the only real evidence.
    int trim = _src_fps_delta_count >= 5 ? 1 : 0;
    int64_t sum = 0;
    int count = _src_fps_delta_count - trim * 2;
    for (int i = trim; i < trim + count; ++i) {
        sum += sorted[i];
    }
    double avg_ms = (double) sum / count;
    if (avg_ms < 8.0) {
        // 120fps 以上不可能是视频，多半是时间戳错了，别把步进压到 1ms
        // Above 120fps is not video; the timestamps are wrong, so never squeeze
        // the step toward 1ms.
        avg_ms = 8.0;
    } else if (avg_ms > 1000.0) {
        avg_ms = 1000.0;
    }
    float fps = (float) (1000.0 / avg_ms);
    // 只在有明显变化时更新：±1% 以内不动，避免时间轴被 jitter 来回拉扯
    // Only update on a material change: within ±1% keep the current value so
    // jitter cannot wobble the timeline.
    if (_src_fps_measured > 0.5f && fabs(fps - _src_fps_measured) < _src_fps_measured * 0.01f) {
        return;
    }
    InfoL << "source fps measured: " << fps << " (interval=" << avg_ms
          << "ms, samples=" << _src_fps_delta_count << ", declared=" << _src_fps
          << "), using it for the output timeline";
    _src_fps_measured = fps;
    // 写回 _src_fps：降帧判断（shouldKeepFrame）也要用真实源帧率，否则
    // 「15fps 源 → 15fps 目标」会被当成 30→15 而丢掉一半的帧。
    // Write it back to _src_fps: decimation needs the real rate too, otherwise
    // "15fps source -> 15fps target" looks like 30->15 and loses half the frames.
    _src_fps = fps;
}

/**
 * 负载自适应降帧的控制律。
 *
 * 判据是「单帧编码耗时 / 当前帧间隔」的比值 load：
 *   - load > 0.85 且连续 15 帧 → 认为跟不上，帧率乘 0.8（下限 _adapt_min_fps）
 *   - load < 0.55 且连续 60 帧 → 认为有余量，帧率乘 1.25（上限基准帧率）
 *   - 中间区抖动不做动作，避免来回抖
 *
 * 为什么不用 CPU 占用率：多核机器上 30% 占用也可能已经喂不饱这条流，而别的进程
 * 抢占 CPU 又会让我们误判。直接量「这一帧编了多久」才是可归因的指标。
 *
 * Control law: ratio of per-frame encode cost to the current frame interval. Rising
 * load drops the rate by 20% (floored at min_fps), falling load restores by 25%.
 * CPU usage is deliberately not used: on multi-core boxes it neither reflects whether
 * this stream keeps up nor is attributable when another process steals CPU.
 */
/** 自测钩子：从环境变量读「人为放大的编码耗时」，生产环境不会设置。
 *  用环境变量而不是配置项/API 参数，是为了不把「会让转码变慢」的开关暴露给运维。 */
static int adaptSelftestDelayFromEnv() {
    static int value = []() {
        const char *env = ::getenv("ZLM_ADAPT_SELFTEST_DELAY_MS");
        int v = env ? ::atoi(env) : 0;
        if (v < 0) {
            v = 0;
        }
        return v;
    }();
    return value;
}

void FFmpegEncoder::updateAdaptiveFps(double cost_ms) {
    float base = _video_params.fps > 0.5f ? _video_params.fps
                 : (_src_fps > 0.5f ? _src_fps : _video_fps);
    if (base <= 0.5f) {
        return;
    }
    if (_adapt_fps_now <= 0.5f) {
        _adapt_fps_now = base;
    }
    // 自测延迟优先取环境变量（生产不设置即 0），配置项保留给离线调试
    int selftest = adaptSelftestDelayFromEnv();
    cost_ms += selftest > 0 ? selftest : _adapt_selftest_delay_ms;
    _enc_cost_ewma_ms = _enc_cost_ewma_ms > 0 ? _enc_cost_ewma_ms * 0.9 + cost_ms * 0.1 : cost_ms;
    double interval = 1000.0 / _adapt_fps_now;
    double load = _enc_cost_ewma_ms / interval;

    if (load > 0.85) {
        _idle_frames = 0;
        if (++_overload_frames >= 15) {
            _overload_frames = 0;
            float next = _adapt_fps_now * 0.8f;
            if (next < _adapt_min_fps) {
                next = _adapt_min_fps;
            }
            if (next < _adapt_fps_now - 0.01f) {
                InfoL << "adapt: 编码跟不上(load=" << load << "), 降帧 "
                      << _adapt_fps_now << " -> " << next << " fps, 累计丢 " << _adapt_dropped;
                _adapt_fps_now = next;
            }
        }
    } else if (load < 0.55) {
        _overload_frames = 0;
        if (++_idle_frames >= 60) {
            _idle_frames = 0;
            float next = _adapt_fps_now * 1.25f;
            if (next > base) {
                next = base;
            }
            if (next > _adapt_fps_now + 0.01f) {
                InfoL << "adapt: 有余量(load=" << load << "), 恢复帧率 "
                      << _adapt_fps_now << " -> " << next;
                _adapt_fps_now = next;
            }
        }
    } else {
        _overload_frames = 0;
        _idle_frames = 0;
    }
}

std::string hwDecoderName(CodecId codec_id) {
#if defined(__APPLE__)
    if (codec_id == CodecH264) return "h264_videotoolbox";
    if (codec_id == CodecH265) return "hevc_videotoolbox";
#elif defined(_WIN32) || defined(__linux__)
    // NVIDIA 的解码器叫 cuvid；AMD/Intel 走各自实现，这里按最常见的一档给
    if (codec_id == CodecH264) return "h264_cuvid";
    if (codec_id == CodecH265) return "hevc_cuvid";
#endif
    return "";
}

bool hwDeviceSelectionSupported() {
#if defined(_WIN32) || defined(__linux__)
    return true;
#else
    return false;
#endif
}

bool FFmpegEncoder::isHardwareEncoder() const {
    return ::mediakit::isHwEncoderCodec(_codec);
}

double FFmpegEncoder::videoStepMs() const {
    // 基准帧率：显式配置的输出帧率优先，其次实测源帧率
    // Base rate: an explicitly configured output rate wins, then the measured source rate
    float base = _video_params.fps > 0.5f ? _video_params.fps
                 : (_src_fps > 0.5f ? _src_fps : _video_fps);
    // 过载降帧时，时间轴步进必须跟着降帧后的帧率走。
    // 否则「每秒少编了帧、每帧还按原间隔推进」= 慢放：实测 15fps 降到 7.5fps 后
    // 5 秒拉流只推进 2.5 秒内容，音画直接错位。
    // When adaptive dropping lowers the frame rate, the timeline step must follow it.
    // Otherwise fewer frames per second each still advance the old interval => slow
    // motion (15 -> 7.5fps turned 5s of pull into 2.5s of content, desyncing audio).
    if (_adapt_fps && _adapt_fps_now > 0.5f && _adapt_fps_now < base) {
        base = _adapt_fps_now;
    }
    if (base > 0.5f) {
        return 1000.0 / base;
    }
    // 实测的源帧率优先于轨上声明的帧率：裸帧 codec 只有前者是真的
    // The measured source rate wins over the declared one: for raw-packet codecs
    // only the former is real.
    if (_src_fps > 0.5f) {
        return 1000.0 / _src_fps;
    }
    // 两者都没有（首帧、样本未收敛）时的保守回退
    // Conservative fallback when neither is known yet (first frames).
    return _video_fps > 0.5f ? 1000.0 / _video_fps : 40.0;
}

int64_t FFmpegEncoder::nextVideoStamp(double &acc) {
    int64_t stamp = (int64_t) llround(acc);
    acc += videoStepMs();
    return stamp;
}

bool FFmpegEncoder::openVideoCodec(int width, int height, int bitrate, const AVCodec *codec) {
    if (width <= 0 || height <= 0) {
        WarnL << "invalid video size:" << width << "x" << height;
        return false;
    }
    _context.reset(avcodec_alloc_context3(codec), [](AVCodecContext *ctx) { avcodec_free_context(&ctx); });
    if (!_context) {
        return false;
    }
    _context->width = width;
    _context->height = height;
    _context->time_base = AVRational{1, 1000};

    // 帧率必须跟随源轨（或显式配置的输出帧率）。硬编码 25fps 时，源为 15fps
    // 的流会按 25fps 打时间戳，导致下游播放速度错误、音画不同步。
    // The frame rate must follow the source track (or the explicitly configured
    // output rate). A hardcoded 25fps stamps a 15fps source at 25fps, producing
    // wrong playback speed and A/V desync.
    int fps = (_video_fps > 0.5f) ? (int) lroundf(_video_fps) : 0;
    if (fps <= 0) {
        fps = 25;   // 源轨无帧率信息时的保守回退
    }
    _context->framerate = AVRational{fps, 1};
    // GOP 设为 1 秒（而不是固定 25 帧），与帧率解耦，保证关键帧间隔稳定
    // Set GOP to one second (not a fixed 25 frames) so the keyframe interval
    // stays stable regardless of frame rate
    _context->gop_size = fps;

    bool explicit_bitrate = bitrate > 0;
    _context->bit_rate = explicit_bitrate ? bitrate : estimateBitrate(width, height, fps);
    if (_context->bit_rate < 8192) {
        // 低于 8kbps 的流任何编码器都无法成形；这只是给显式配置的兜底
        // Below 8kbps nothing is watchable; this is only a backstop for explicit values
        _context->bit_rate = 8192;
    }
    // 只设 bit_rate 时 x264/x265 走 ABR，实测会超出目标 10%~30%（x265 尤甚）。
    // 补上 VBV 约束（maxrate=bufsize=目标码率，即 1 秒缓冲）后实测收敛到 1.03~1.05 倍，
    // 且码率上限可控 —— 直播场景要的就是这个确定性。
    // With only bit_rate set, x264/x265 run ABR and measured 10-30% over target
    // (x265 worst). Adding VBV constraints (maxrate=bufsize=target, i.e. a 1-second
    // buffer) brought it to 1.03-1.05x and bounds the peak — the determinism a live
    // stream needs.
    _context->rc_max_rate = _context->bit_rate;
    _context->rc_buffer_size = _context->bit_rate;

    _context->max_b_frames = 0;   // 低延迟，直播不要 B 帧
    // 像素格式：优先 yuv420p。codec->pix_fmts[0] 对 x265 可能是 yuv420p10le 等
    // 高位深格式，浏览器/部分播放器不支持，且 sws 转换成本更高。
    // Pixel format: prefer yuv420p. For x265, codec->pix_fmts[0] may be a
    // high-bit-depth format such as yuv420p10le, which browsers and some
    // players reject and which costs more to convert.
    _context->pix_fmt = AV_PIX_FMT_YUV420P;
    if (codec->pix_fmts) {
        bool supported = false;
        for (int i = 0; codec->pix_fmts[i] != AV_PIX_FMT_NONE; ++i) {
            if (codec->pix_fmts[i] == AV_PIX_FMT_YUV420P) {
                supported = true;
                break;
            }
        }
        if (!supported) {
            _context->pix_fmt = codec->pix_fmts[0];
        }
    }

    if (codec->id == AV_CODEC_ID_H264) {
        // 多卡：只有 NVENC 有 gpu 选项。其它硬件编码器（VAAPI 走 render node、
        // VideoToolbox 无设备索引）会忽略这个值，状态接口里会如实报 -1。
        // Only NVENC has a `gpu` option; VAAPI (render nodes) and VideoToolbox ignore it.
        if (_video_params.device >= 0 && strstr(codec->name, "nvenc")) {
            if (av_opt_set(_context->priv_data, "gpu", std::to_string(_video_params.device).data(), 0) < 0) {
                WarnL << "nvenc: 设置 gpu=" << _video_params.device << " 失败，使用默认设备";
            } else {
                InfoL << "nvenc: 绑定 GPU " << _video_params.device;
            }
        }
        av_opt_set(_context->priv_data, "preset", "veryfast", 0);
        av_opt_set(_context->priv_data, "tune", "zerolatency", 0);
        // 关掉场景切换强制 IDR：直播里源内容抖动（噪声/闪白）会频繁触发，
        // 实测 150 帧里出现 53 个 IDR，码率随之失控。关键帧只按 gop_size 出。
        // Disable scene-cut forced IDR: jittery source content (noise/flashes) triggers
        // it constantly — measured 53 IDRs in 150 frames, blowing up the bitrate.
        // Keyframes then come only from gop_size.
        av_opt_set_int(_context->priv_data, "sc_threshold", 0, 0);
    } else if (codec->id == AV_CODEC_ID_HEVC) {
        // x265 低延迟参数：直播场景不要 lookahead、不要 B 帧
        // x265 low-latency params: no lookahead, no B frames for live
        av_opt_set(_context->priv_data, "preset", "ultrafast", 0);
        av_opt_set(_context->priv_data, "tune", "zerolatency", 0);
        // 显式关闭场景切换 IDR，理由同 H264
        // Explicitly disable scene-cut IDR, same reason as H264
        std::string params = "log-level=error:scenecut=0";
        av_opt_set(_context->priv_data, "x265-params", params.c_str(), 0);
    } else if (codec->id == AV_CODEC_ID_VP8 || codec->id == AV_CODEC_ID_VP9) {
        // libvpx 默认是「离线高质量」档：deadline=good + lag-in-frames=25（前向参考）。
        // 直播必须显式切实时模式，否则两个后果都实测到了：
        //   1. 第一帧不出包 —— warmup 拿不到关键帧，目标轨永远 unready，
        //      RtspMuxer 直接 "VP9 unready!" 丢掉整条轨（日志 "warmup produced no packet"）
        //   2. 每帧编码耗时远超实时，延迟迅速累积
        // libvpx defaults to its offline/high-quality profile: deadline=good with
        // lag-in-frames=25. Live needs realtime mode explicitly — both failure modes
        // were measured: (1) no packet for the first frame, so warmup never gets a
        // keyframe and the target track stays unready ("VP9 unready!", track dropped);
        // (2) per-frame encode cost far above realtime.
        av_opt_set(_context->priv_data, "deadline", "realtime", 0);
        // cpu-used 8 = 实时档的速度/质量折衷（VP8/VP9 实时档最大值都是 8）
        // cpu-used 8 is the realtime-profile sweet spot (max is 8 for both)
        av_opt_set_int(_context->priv_data, "cpu-used", 8, 0);
        av_opt_set_int(_context->priv_data, "lag-in-frames", 0, 0);
        // auto-alt-ref 依赖 lookahead，lag=0 时必须一起关掉，否则 open2 失败
        // auto-alt-ref needs lookahead; must be off together with lag=0 or open2 fails
        av_opt_set_int(_context->priv_data, "auto-alt-ref", 0, 0);
        // 直播不重传：开错误恢复，单包丢失不至于整帧花掉
        // Live has no retransmission: enable error resilience so one lost packet
        // does not wreck the whole frame
        av_opt_set_int(_context->priv_data, "error-resilient", 1, 0);
    } else if (codec->id == AV_CODEC_ID_AV1) {
        // AV1 的 lookahead 同样必须关：libaom 默认 lag-in-frames=25，
        // 首帧不出包会让 warmup 失败（与 libvpx 同一个坑）
        // AV1 lookahead must be off for the same reason as libvpx: the default
        // lag-in-frames=25 emits nothing on the first frame and warmup fails
        if (std::string(codec->name) == "libaom-av1") {
            av_opt_set_int(_context->priv_data, "lag-in-frames", 0, 0);
            av_opt_set_int(_context->priv_data, "cpu-used", 8, 0);
            // 不能用 usage=realtime：实测这条 nonrd 快路径在 libaom 3.9.1 上会踩坏堆
            // （崩溃栈 aom_memalign <- av1_alloc_pmc <- av1_nonrd_use_partition，
            // 6 次里 1 次）。默认 usage=good + cpu-used=8 已足够实时，且稳定。
            // Do NOT use usage=realtime: on libaom 3.9.1 this nonrd fast path corrupts
            // the heap (crash stack aom_memalign <- av1_alloc_pmc <-
            // av1_nonrd_use_partition, 1 in 6 runs). Default usage=good + cpu-used=8
            // is fast enough for realtime and stable.
            _context->thread_count = 1;
        } else {
            // libsvtav1 等：preset 越大越快（0..13）
            av_opt_set_int(_context->priv_data, "preset", 10, 0);
        }
    }
    auto ret = avcodec_open2(_context.get(), codec, nullptr);
    if (ret < 0) {
        WarnL << "avcodec_open2 video failed:" << ffmpeg_err_str(ret);
        _context.reset();
        return false;
    }
    InfoL << "video encoder opened: " << codec->name
          << ", size=" << _context->width << "x" << _context->height
          << ", fps=" << fps << ", gop=" << _context->gop_size
          << ", bitrate=" << _context->bit_rate << "bps (" << (_context->bit_rate / 1024) << "kbps)"
          << ", maxrate=" << _context->rc_max_rate
          << ", explicit_bitrate=" << explicit_bitrate;
    return true;
}

bool FFmpegEncoder::openAudioCodec(int samplerate, int channel, int bitrate, const AVCodec *codec,
                                   const Track::Ptr &target) {
    _context.reset(avcodec_alloc_context3(codec), [](AVCodecContext *ctx) { avcodec_free_context(&ctx); });
    if (!_context) {
        return false;
    }

    // 编码器有自己的采样率/声道/格式约束，必须按它来，否则 open2 失败  [AUTO-TRANSLATED:a6b0a0a6]
    // The encoder constrains samplerate/channels/format; follow it or open2 fails
    _context->sample_rate = samplerate > 0 ? samplerate : 44100;
    if (codec->supported_samplerates) {
        bool found = false;
        for (auto p = codec->supported_samplerates; *p; ++p) {
            if (*p == _context->sample_rate) {
                found = true;
                break;
            }
        }
        if (!found) {
            // 取"最接近"的支持采样率，而不是 [0]。libopus 的 supported_samplerates 是
            // 48000/24000/16000/12000/8000，[0] 恰好是 48000 所以旧代码没暴露问题；
            // 但换个编码器（列表降序）就会把 8kHz 的源上采样到 96kHz，既废 CPU 又改变音色。
            // Pick the NEAREST supported rate, not [0]. libopus lists 48000 first so the
            // old code looked fine; another encoder listing high rates first would upsample
            // an 8kHz source to 96kHz, wasting CPU and altering the timbre.
            int best = codec->supported_samplerates[0];
            int best_diff = abs(best - _context->sample_rate);
            for (auto p = codec->supported_samplerates; *p; ++p) {
                int diff = abs(*p - _context->sample_rate);
                if (diff < best_diff) {
                    best = *p;
                    best_diff = diff;
                }
            }
            WarnL << "samplerate " << _context->sample_rate << " not supported by " << codec->name
                  << ", fallback to nearest " << best;
            _context->sample_rate = best;
        }
    }

    int want_channels = channel > 0 ? channel : 1;
#if LIBAVCODEC_VERSION_INT >= FF_CODEC_VER_7_1
    av_channel_layout_default(&_context->ch_layout, want_channels);
    if (codec->ch_layouts) {
        // 必须优先挑与请求声道数一致的布局。旧代码无条件取 ch_layouts[0]：
        // libmp3lame 的列表是 {1ch, 2ch}，立体声源会被静默降成单声道，丢一半声道，
        // 观众端音量掉 3dB 且左右声道合并。
        // Prefer a layout matching the requested channel count. The old code blindly took
        // ch_layouts[0]; libmp3lame lists {1ch, 2ch}, so a stereo source was silently
        // downmixed to mono — one channel dropped, ~3dB quieter, L/R summed.
        const AVChannelLayout *pick = &codec->ch_layouts[0];
        bool matched = false;
        for (auto p = codec->ch_layouts; p->nb_channels; ++p) {
            if (p->nb_channels == want_channels) {
                pick = p;
                matched = true;
                break;
            }
        }
        if (!matched) {
            WarnL << "codec " << codec->name << " does not support " << want_channels
                  << " channel(s), fallback to " << codec->ch_layouts[0].nb_channels;
        }
        av_channel_layout_copy(&_context->ch_layout, pick);
        want_channels = _context->ch_layout.nb_channels;
    }
#else
    // ffmpeg < 7.1：声道布局是 channels + channel_layout 两个字段。
    // 同样优先挑与请求声道数一致的布局（libmp3lame 的列表是 {1ch, 2ch}，
    // 无条件取 [0] 会把立体声静默降成单声道）。
    _context->channels = want_channels;
    _context->channel_layout = av_get_default_channel_layout(want_channels);
    if (codec->channel_layouts) {
        uint64_t pick = codec->channel_layouts[0];
        bool matched = false;
        for (auto p = codec->channel_layouts; *p; ++p) {
            if ((int) av_get_channel_layout_nb_channels(*p) == want_channels) {
                pick = *p;
                matched = true;
                break;
            }
        }
        if (!matched) {
            WarnL << "codec " << codec->name << " does not support " << want_channels
                  << " channel(s), fallback to "
                  << av_get_channel_layout_nb_channels(codec->channel_layouts[0]);
        }
        _context->channel_layout = pick;
        _context->channels = (int) av_get_channel_layout_nb_channels(pick);
        want_channels = _context->channels;
    }
#endif

    _context->sample_fmt = codec->sample_fmts ? codec->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
    _context->bit_rate = bitrate > 0 ? bitrate : 64000;
    _context->time_base = AVRational{1, _context->sample_rate};

    auto ret = avcodec_open2(_context.get(), codec, nullptr);
    if (ret < 0) {
        WarnL << "avcodec_open2 audio failed:" << ffmpeg_err_str(ret);
        _context.reset();
        return false;
    }

    // 必须在 open2 之后判断：opus 的 frame_size 由 open2 填成 960，
    // open2 之前是 0，会让固定帧长的编码器被误判成可变帧长，
    // 直接把 1024 采样的帧送进去，libopus 会返回 EINVAL。
    // Must be decided after open2: opus gets frame_size=960 from open2;
    // before open2 it is 0, which misclassifies a fixed-frame encoder as
    // variable, sending 1024-sample frames straight in, and libopus returns EINVAL.
    _variable_frame_size = (_context->frame_size <= 1);

    // G711/其他"变长帧"编码器 frame_size 恒为 0，此时一帧的时长由我们自己定：
    // 固定 20ms（与 RTP 打包粒度、实时性权衡一致），换算成采样点数即为 rate/50。
    // 这里算一次存起来，静音兜底和 pts 推进都用同一个值，避免两条路径各算一套。
    // Encoders like G711 always report frame_size == 0; we then choose 20ms per frame
    // (matches the RTP packing granularity) = rate/50 samples. Compute it once so the
    // silence fallback and the pts clock agree instead of each deriving its own value.
    if (_variable_frame_size) {
        _silence_samples = _context->sample_rate / 50;
        if (_silence_samples <= 0) {
            _silence_samples = 160;
        }
    } else {
        _silence_samples = _context->frame_size;
    }
    // 一帧的毫秒时长，静音补帧的节奏与 pts 推进都以它为准
    // Per-frame duration in ms; silence pacing and pts advancement both use it
    _audio_frame_ms = (int64_t) _silence_samples * 1000LL / _context->sample_rate;
    if (_audio_frame_ms <= 0) {
        _audio_frame_ms = 20;
    }
    // 音频也写进 _frame_interval_ms：onEncode 的单调时钟与兜底推进都读它。
    // G711 的 frame_size 是 0，旧代码在 onEncode 里落到 `next += 40` 的硬编码分支，
    // 而 G711 一帧是 20ms → 静音时间轴按 2 倍速前进，观众端静音段被拉长一倍。
    // 视频轨的 _frame_interval_ms 在 inputFrame_l 视频分支里另行赋值，不受影响。
    // Also store it in _frame_interval_ms, which onEncode's monotonic clock and the
    // fallback advance both read. G711 reports frame_size 0, so the old code fell into
    // the hardcoded `next += 40` branch while a G711 frame is 20ms — silence advanced at
    // 2x, so viewers saw the silent tail stretched to double length. The video track
    // assigns _frame_interval_ms separately in inputFrame_l's video branch, unaffected.
    _frame_interval_ms = _audio_frame_ms;

    InfoL << "audio encoder:" << codec->name
          << ", samplerate=" << _context->sample_rate
          << ", channels=" << ctxChannels(_context.get())
          << ", sample_fmt=" << av_get_sample_fmt_name(_context->sample_fmt)
          << ", frame_size=" << _context->frame_size
          << ", variable_frame=" << _variable_frame_size
          << ", frame_ms=" << _audio_frame_ms;

    // AAC 目标轨必须在 addTrack 之前 ready，否则 RtspMuxer/MP4Muxer 直接把它丢掉
    // （实测日志：`mpeg4-generic unready!` → RTSP 观众端完全没有音频轨）。
    // AACTrack::ready() 只取决于 _cfg（AudioSpecificConfig）是否为空，而 _cfg 只能由
    // setExtraData 填入，转码路径此前从不调用它 → 目标轨永远 unready。
    // 好消息是 aac 编码器在 avcodec_open2 里就把 extradata（=AudioSpecificConfig）
    // 生成好了，这里直接回填即可，无需预热编码。
    // The AAC target track must be ready before addTrack, otherwise RtspMuxer/MP4Muxer
    // drop it outright (measured: "mpeg4-generic unready!" -> RTP viewers got no audio
    // track at all). AACTrack::ready() only depends on _cfg (AudioSpecificConfig) being
    // non-empty, and _cfg is only ever set through setExtraData, which the transcode path
    // never called — so the target track stayed unready forever. The aac encoder already
    // fills extradata (=AudioSpecificConfig) inside avcodec_open2, so backfill it here;
    // no warmup encode needed.
    if (target && (_context->codec_id == AV_CODEC_ID_AAC || _context->codec_id == AV_CODEC_ID_OPUS)
        && _context->extradata && _context->extradata_size > 0) {
        target->setExtraData(_context->extradata, (size_t) _context->extradata_size);
        InfoL << "audio target track extra data set, size=" << _context->extradata_size
              << ", ready=" << target->ready();
    }
    return true;
}

FFmpegFrame::Ptr FFmpegEncoder::formatFrame(const FFmpegFrame::Ptr &frame) {
    auto av_frame = frame->get();

    // 视频走 sws（像素格式/缩放），音频走 swr（采样格式/重采样）。
    // 这两条路径必须严格分开：把视频帧送进 FFmpegSwr 会让它以音频方式读取
    // ch_layout（视频的 ch_layout 是空的），在 FFmpegSwr 内部 strlen(NULL) 崩溃。
    // Video goes through sws (pixel format / scaling), audio through swr (sample
    // format / resampling). The two paths must stay separate: feeding a video frame
    // into FFmpegSwr makes it read ch_layout as if it were audio (a video frame has
    // no channel layout), which crashes inside FFmpegSwr at strlen(NULL).
    if (_context->codec_type == AVMEDIA_TYPE_VIDEO) {
        // 帧尺寸/格式可能中途变化（源切换分辨率、或首帧还没进来时拿到的帧），
        // FFmpegSws 内部只在 _ctx 已存在且源参数不匹配时才重建，这里把
        // 「目标尺寸」也纳入判断：缩放场景下目标尺寸来自配置而不是帧本身。
        // The frame size/format can change mid-stream (source resolution switch). The
        // FFmpegSws target is fixed at construction, so rebuild it whenever the encoder
        // target changes, otherwise we would emit frames at the previous size.
        if (av_frame->format == _context->pix_fmt
            && av_frame->width == _context->width
            && av_frame->height == _context->height) {
            return frame;
        }
        if (!_sws || _sws_width != _context->width || _sws_height != _context->height
            || _sws_format != (AVPixelFormat) _context->pix_fmt) {
            _sws.reset(new FFmpegSws(_context->pix_fmt, _context->width, _context->height));
            _sws_width = _context->width;
            _sws_height = _context->height;
            _sws_format = (AVPixelFormat) _context->pix_fmt;
            InfoL << "video scale path: " << av_frame->width << "x" << av_frame->height
                  << " fmt=" << av_get_pix_fmt_name((AVPixelFormat) av_frame->format)
                  << " -> " << _context->width << "x" << _context->height
                  << " fmt=" << av_get_pix_fmt_name((AVPixelFormat) _context->pix_fmt);
        }
        auto out = _sws->inputFrame(frame);
        if (!out) {
            WarnL << "FFmpegSws convert failed: "
                  << av_frame->width << "x" << av_frame->height << " fmt=" << av_frame->format
                  << " -> " << _context->width << "x" << _context->height
                  << " fmt=" << _context->pix_fmt;
            return nullptr;
        }
        // 校验缩放结果：sws 若因为目标尺寸非法而内部回退到源尺寸，
        // 编码器会以尺寸不符报 EINVAL，但那时已经很难定位，这里直接拦住。
        // Verify the scaled result: if sws silently falls back to the source size for
        // an invalid target, the encoder then fails with an unhelpful EINVAL.
        if (out->get()->width != _context->width || out->get()->height != _context->height) {
            WarnL << "scaled frame size mismatch: got "
                  << out->get()->width << "x" << out->get()->height
                  << ", encoder expects " << _context->width << "x" << _context->height;
            return nullptr;
        }
        return out;
    }

    // ---- 音频路径 ----
    if (av_frame->format == _context->sample_fmt
        && av_frame->sample_rate == _context->sample_rate
        && frameChannels(av_frame) == ctxChannels(_context.get())) {
        return frame;
    }
    if (!_swr) {
#if LIBAVCODEC_VERSION_INT >= FF_CODEC_VER_7_1
        _swr.reset(new FFmpegSwr(_context->sample_fmt, &_context->ch_layout, _context->sample_rate));
#else
        _swr.reset(new FFmpegSwr(_context->sample_fmt, _context->channels,
                                 (int) _context->channel_layout, _context->sample_rate));
#endif
    }
    return _swr->inputFrame(frame);
}

bool FFmpegEncoder::inputFrame(const FFmpegFrame::Ptr &frame, bool async) {
    if (!frame || !frame->get()) {
        return false;
    }
    if (!_context) {
#ifdef ENABLE_ASCEND_VENC
        // 昇腾路径没有 libavcodec 上下文：只有开了 AscendVenc 才继续，
        // 否则必须照样丢弃（保持原有「无上下文不能编码」的语义）。
        if (!_ascend) {
            return false;
        }
#else
        return false;
#endif
    }
    _flush_done = false;
    if (async && TaskManager::isEnabled()) {
        auto cache = frame;
        return addEncodeTask([this, cache]() {
            inputFrame_l(cache);
        });
    }
    return inputFrame_l(frame);
}

/**
 * 把一帧已符合编码器格式的 PCM 送进编码器。
 *
 * 可变帧长的编码器直接编码；固定帧长的走 FIFO 攒够 frame_size 再编。
 * 真实帧与静音帧共用这条路径，保证两者在 PCM 层面完全等价：
 * 若静音绕过 FIFO，FIFO 里残留的采样会与静音帧错位，Opus 输出虽然能解，
 * 但时间轴会出现几十毫秒的空洞。
 *
 * Feed one PCM frame already in the encoder's format. Variable-frame encoders
 * encode directly; fixed-frame encoders buffer through the FIFO. Real frames and
 * silence frames share this path so they are PCM-equivalent: bypassing the FIFO
 * for silence would misalign its leftover samples and punch a gap in the timeline.
 */
int FFmpegEncoder::audioChunkSamples() const {
    // 切帧长度的唯一来源，编码与静音补帧必须用同一个值，否则时间轴会漂。
    // frame_size > 1 的编码器（libopus 960、AAC 1024、libmp3lame 1152）用它自己的；
    // frame_size 为 0/1 的（G711/L16 这类裸 PCM）用 20ms = rate/50，
    // 与 RTP 打包粒度一致，也避开 ffmpeg 默认 1024 帧长补零的坑。
    //
    // Single source of truth for the chunk size. Fixed-frame encoders
    // (libopus 960, AAC 1024, libmp3lame 1152) use their own frame_size; the bare PCM
    // ones that report 0/1 use 20ms = rate/50, matching the RTP packing granularity and
    // avoiding ffmpeg's default 1024-sample padding.
    if (_context->frame_size > 1) {
        return _context->frame_size;
    }
    auto rate = _context->sample_rate > 0 ? _context->sample_rate : 8000;
    auto samples = rate / 50;
    return samples > 0 ? samples : 1;
}

bool FFmpegEncoder::encodeAudio_l(AVFrame *converted) {
    if (!_fifo) {
        _fifo.reset(new TranscodeAudioFifo());
    }
    if (!_fifo->write(converted)) {
        return false;
    }

    auto frame_size = audioChunkSamples();
    bool ret = true;
    while (true) {
        auto out = av_frame_alloc();
        if (!out) {
            return false;
        }
        out->format = _context->sample_fmt;
        out->sample_rate = _context->sample_rate;
        out->nb_samples = frame_size;
        copyCtxLayoutToFrame(out, _context.get());
        auto alloc_ret = av_frame_get_buffer(out, 0);
        if (alloc_ret < 0) {
            WarnL << "av_frame_get_buffer failed:" << ffmpeg_err_str(alloc_ret);
            av_frame_free(&out);
            return false;
        }
        if (!_fifo->read(out, frame_size)) {
            av_frame_free(&out);
            break;
        }
        // 送进编码器的 pts 是采样计数（time_base = 1/sample_rate），
        // 毫秒换算统一放在 onEncode 出口做，避免单位混用
        // The pts fed to the encoder counts samples (time_base = 1/sample_rate);
        // conversion to ms happens once at the onEncode exit
        out->pts = _frame_index;
        _frame_index += frame_size;
        if (!encodeFrame(out)) {
            ret = false;
        }
        av_frame_free(&out);
    }
    return ret;
}

bool FFmpegEncoder::inputSilence() {
    if (!_context || _context->codec_type != AVMEDIA_TYPE_AUDIO) {
        return false;
    }
    _flush_done = false;

    // 一帧的采样数与时长由编码器自身决定（libopus: 960 @48k = 20ms；
    // AAC: 1024 @44.1k ≈ 23.2ms；G711: frame_size=0，我们自己定 20ms = rate/50）。
    // 不能用固定值，否则时间轴会漂。
    // The frame's sample count and duration come from the encoder itself
    // (libopus: 960 @48k = 20ms; AAC: 1024 @44.1k ≈ 23.2ms; G711 reports frame_size 0 so
    // we fix it at 20ms = rate/50). Hardcoding would drift the timeline.
    int rate = _context->sample_rate > 0 ? _context->sample_rate : 48000;
    int frame_size = _silence_samples > 0 ? _silence_samples
                     : (_context->frame_size > 0 ? _context->frame_size : (rate / 50));

    // 虚拟推进源时钟：静音帧在时间轴上紧跟上一帧，不产生空洞也不压缩。
    // 注意这里推进的是"若源端恢复后真实帧应落在哪里"的参考值，实际输出时间戳
    // 由 onEncode 的单调时钟给出（见 _silence_encoding）。
    // Advance the virtual source clock: silence follows the previous frame on the
    // timeline with no hole and no compression. This only tracks where a resumed
    // real frame "should" land; the actual output timestamp comes from the
    // monotonic clock in onEncode (see _silence_encoding).
    // 用整数毫秒会累计误差（AAC 23.22ms 取整成 23ms，100 帧就差 22ms），
    // 这里按采样计数累加，静音与真实帧走同一条采样时间轴。
    // Integer ms accumulates error (AAC 23.22ms truncates to 23ms; 100 frames drift
    // 22ms), so accumulate in samples: silence and real frames share one sample clock.
    if (_last_src_ms != AV_NOPTS_VALUE) {
        _last_src_ms += (int64_t) (frame_size * 1000LL / rate);
    } else {
        _last_src_ms = 0;
    }

    auto frame = av_frame_alloc();
    if (!frame) {
        return false;
    }
    frame->format = _context->sample_fmt;
    frame->sample_rate = rate;
    frame->nb_samples = frame_size;
    copyCtxLayoutToFrame(frame, _context.get());
    auto ret = av_frame_get_buffer(frame, 0);
    if (ret < 0) {
        WarnL << "av_frame_get_buffer failed for silence:" << ffmpeg_err_str(ret);
        av_frame_free(&frame);
        return false;
    }
    // av_frame_get_buffer 不保证清零，必须显式填静音，否则"静音"里会带随机噪声
    // av_frame_get_buffer does not zero the buffer; fill explicit silence or the
    // "silence" carries random noise
    av_samples_set_silence(frame->data, 0, frame_size, frameChannels(frame),
                           (AVSampleFormat) frame->format);

    // 与真实帧走完全相同的编码路径（含 FIFO 对齐）；
    // 期间置 _silence_encoding，让 onEncode 走单调时钟而不是源时间轴
    // Same encode path as real frames (FIFO included); _silence_encoding makes
    // onEncode use the monotonic clock instead of the source timeline
    _silence_encoding = true;
    auto ok = encodeAudio_l(frame);
    _silence_encoding = false;
    av_frame_free(&frame);
    return ok;
}

bool FFmpegEncoder::inputFrame_l(const FFmpegFrame::Ptr &frame) {
    TimeTicker2(30, TraceL);

#ifdef ENABLE_ASCEND_VENC
    // 昇腾硬编自己负责格式转换与 pts 推进，不走下面的 libavcodec 路径
    if (_ascend && !_context) {
        return encodeAscend(frame->get());
    }
#endif
    if (!_context) {
        return false;
    }

    // 转码输出的时间戳必须跟随源流：下游 Stamp 已按源流时间推进，
    // 若从 0 重新起算，帧会被判为"过旧"直接丢弃（视频表现最明显）。
    // Transcoded timestamps must follow the source stream: the downstream Stamp
    // has already advanced with the source, so restarting from 0 makes frames
    // look stale and get dropped (most visible on video).
    auto src_dts = frame->get()->pkt_dts != AV_NOPTS_VALUE ? frame->get()->pkt_dts
                                                           : frame->get()->pts;
    if (src_dts != AV_NOPTS_VALUE && _src_base_ms < 0) {
        _src_base_ms = src_dts;
    }
    _last_src_ms = src_dts;

    if (_context->codec_type == AVMEDIA_TYPE_VIDEO) {
        // 裸帧 codec 的源没有帧率元数据，先按解码帧时间戳间隔实测；
        // 实测值会写进 _src_fps，降帧判断与时间轴步进都用它。
        // Raw-packet codecs carry no source frame rate: measure it from decoded
        // frame timestamp deltas. The result lands in _src_fps and drives both
        // decimation and the timeline step.
        if (_src_fps_measure) {
            updateSrcFpsEstimate(src_dts);
        }
        // 降帧：目标帧率低于源时，按「源帧序号 × 目标/源」跳帧，替代方案（按时钟
        // 累计）在源帧间隔抖动时会一次连丢/连留，这里的整数累加是确定性的。
        // Decimation: when the target fps is below the source, skip frames by
        // "source index * target/source". A wall-clock accumulator would burst-skip
        // or burst-keep when the source jitters; this integer scheme is deterministic.
        // 编码耗时用于负载判定：把 encodeFrame 包一层计时，两处出口共用
        auto encode_and_measure = [this](AVFrame *f) -> bool {
            auto t0 = std::chrono::steady_clock::now();
            bool ok = encodeFrame(f);
            if (_adapt_fps) {
                double cost = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t0).count();
                updateAdaptiveFps(cost);
            }
            return ok;
        };
        if (!shouldKeepFrame(frame->get())) {
            if (_adapt_fps) {
                ++_adapt_dropped;
            }
            return true;
        }
        auto converted = formatFrame(frame);
        if (!converted) {
            return false;
        }
        auto av_frame = converted->get();
        // 编码器的 time_base 是 1/1000，pts 单位就是毫秒，必须按帧间隔推进，
        // 不能填帧序号：填序号等于每帧只前进 1ms，15fps 的流会被压缩成 1000fps
        // 的时间轴，下游按源流时间判定时几乎每帧都"过旧"被丢，
        // 观众端表现为帧数远低于源、且参考帧缺失（Could not find ref with POC）。
        // The encoder time_base is 1/1000 so pts is in milliseconds and must advance by
        // the frame interval, never by the frame index: an index advances 1ms per frame,
        // compressing a 15fps stream onto a 1000fps timeline. Downstream then judges
        // almost every frame stale against the source clock and drops it, so the viewer
        // sees far fewer frames plus missing references ("Could not find ref with POC").
        int64_t step_ms = (int64_t) llround(videoStepMs());
        if (step_ms <= 0) {
            step_ms = 40;
        }
        // 一律按（实测/声明的）源帧率匀速推进，不跟源 pkt_dts。
        // 原因：RTMP 源的 pkt_dts 经 1/1000 时基换算后抖动很大（实测 15fps 的源
        // 算出 ~53ms/帧 = 19fps），用它做时间轴会让转码流比源流跑快 25%，
        // 下游按源流时间判定就会把 2/3 的帧当"过旧"丢弃。
        // 转码是恒帧率输出，按 1000/fps 匀速推进才是正确的时间轴；步进本身
        // 不再取常量，而是「目标帧率 > 实测源帧率 > 兜底」。
        // Always advance at the (measured/declared) source frame rate instead of
        // following the source pkt_dts: after the 1/1000 timebase conversion an RTMP
        // source's pkt_dts jitters badly (a 15fps source measured at ~53ms/frame =
        // 19fps), and using it as the timeline makes the transcoded stream run 25%
        // faster than the source, so downstream judges 2/3 of the frames stale and
        // drops them. Transcoding is a constant-frame-rate output, so advancing at
        // 1000/fps is the correct timeline; the step itself is no longer a constant
        // but "configured target rate > measured source rate > fallback".
        av_frame->pts = nextVideoStamp(_video_in_ms);
        // 让 onEncode 出口的单调守卫也用视频帧间隔，否则它退回 20ms 的默认值
        // 会把视频时间轴按 20ms 拉扯。
        // Let the onEncode monotonic guard use the video interval too; otherwise it
        // falls back to a 20ms default and drags the video timeline along at 20ms.
        _frame_interval_ms = step_ms;

        // ---- 滤镜（可选）----
        // 位置很关键：放在「缩放 + 像素格式转换」之后、编码之前。
        // 放在之前会让 drawtext 在源分辨率上绘制，缩小后文字跟着糊掉 —— OSD 文字
        // 必须按**输出分辨率**算字号才有意义。
        //
        // 滤镜出错时只关闭滤镜并打一次 Error，绝不丢弃帧：观众宁可看到没有 OSD 的画面，
        // 也不该整路断掉。
        //
        // Filters run AFTER sws scaling and BEFORE encoding: drawing OSD text at the
        // source resolution and then downscaling would blur it, so text size must be
        // relative to the OUTPUT resolution. On filter errors we disable the filter and
        // keep passing frames through — losing OSD is better than losing the stream.
        if (!_filter_desc.empty() || !_logo_path.empty()) {
            if (!_filter && !_filter_failed) {
                _filter = std::make_shared<TranscodeFilter>();
                if (!_logo_path.empty()) {
                    _filter->setLogo(_logo_path, _logo_x, _logo_y, _logo_width);
                }
                _filter_failed = true;   // 先置位：init 失败不再重试同一张图
                if (_filter->init(_filter_desc, _context->width, _context->height,
                                  _context->pix_fmt, _context->time_base.num,
                                  _context->time_base.den)) {
                    _filter_failed = false;
                } else {
                    ErrorL << "transcode filter init failed, disabled: " << _filter_desc;
                    _filter = nullptr;
                }
            }
            if (_filter) {
                AVFrame *filtered = nullptr;
                int ret = _filter->filter(av_frame, &filtered);
                if (ret < 0) {
                    ErrorL << "transcode filter 出错，已停用: " << _filter_desc;
                    _filter = nullptr;
                    _filter_failed = true;
                } else if (ret == 0) {
                    // 滤镜内部还在缓冲（如 fps/重叠类滤镜），这帧先不下发
                    return true;
                } else {
                    filtered->pts = av_frame->pts;
                    bool ok = encode_and_measure(filtered);
                    av_frame_free(&filtered);
                    return ok;
                }
            }
        }
        return encode_and_measure(av_frame);
    }

    // ---- 音频路径 ----
    // 所有音频 codec 一律走 encodeAudio_l 的 FIFO 分帧，**不要**因为 frame_size==0
    // 就直接把任意长度的帧塞进编码器。
    //
    // 原因（实测教训）：PCM_ALAW/PCM_MULAW 虽然 frame_size 报 0，但它们并没有
    // AV_CODEC_CAP_VARIABLE_FRAME_SIZE 能力，ffmpeg 会按默认帧长 1024 把输入帧补零。
    // 8kHz 源的解码帧只有 160 样本（20ms），每帧被撑成 1024 样本 → 内容放大约 6.4 倍：
    // 拉 5 秒音频解码出来是 31.88 秒、1589 个包（正确值 250 包 / 5.000 秒）。
    // 目标采样率与源相同时（8k→8k）不做重采样，这条路径才会被走到，所以 44.1k→PCMA
    // 反而正确，掩盖了问题。
    //
    // Every audio codec goes through encodeAudio_l's FIFO framing. Do NOT feed arbitrary
    // frame lengths straight into the encoder just because frame_size == 0: PCM_ALAW /
    // PCM_MULAW report frame_size 0 but lack AV_CODEC_CAP_VARIABLE_FRAME_SIZE, so ffmpeg
    // pads each frame to its default 1024 samples. An 8kHz source frame is only 160
    // samples (20ms), so each became 1024 -> content inflated ~6.4x (5s of pull decoded
    // to 31.88s / 1589 packets instead of 250 / 5.000s). Only reachable when the target
    // rate equals the source rate (no resampling), which is why 44.1k -> PCMA looked fine.
    auto converted = formatFrame(frame);
    if (!converted) {
        return false;
    }
    if (!converted->get() || converted->get()->nb_samples <= 0) {
        return false;
    }
    return encodeAudio_l(converted->get());
}

bool FFmpegEncoder::warmup(const Track::Ptr &target) {
    if (!target) {
        return false;
    }
#ifdef ENABLE_ASCEND_VENC
    // 昇腾硬编没有 _context，尺寸取输出尺寸、像素格式用 YUV420P（encodeAscend 会转 NV12）
    if (!_ascend)
#endif
    if (!_context) {
        return false;
    }
    // 预热用的尺寸必须是「编码器的输出尺寸」，不是源尺寸：缩放场景下两者不同，
    // 用源尺寸造帧会让 avcodec_send_frame 因尺寸不符直接返回 EINVAL。
    // The warmup frame must use the ENCODER's output size, not the source size:
    // they differ when scaling, and a mismatched frame makes avcodec_send_frame
    // return EINVAL immediately.
    int width = _context->width;
    int height = _context->height;
    AVPixelFormat warm_fmt = _context->pix_fmt;
#ifdef ENABLE_ASCEND_VENC
    if (_ascend) {
        width = _out_width;
        height = _out_height;
        warm_fmt = AV_PIX_FMT_YUV420P;
    }
#endif
    if (width <= 0 || height <= 0) {
        WarnL << "warmup: encoder has no valid size";
        return false;
    }
    // 目标轨只可能是视频轨
    // The target track can only be a video track
    auto video = dynamic_pointer_cast<VideoTrack>(target);
    if (!video) {
        return false;
    }
    // 已经 ready 就不用预热
    // No need to warm up if already ready
    if (video->ready()) {
        return true;
    }

    // 造一帧纯色画面（YUV420P 全灰），只用于让编码器吐出 VPS/SPS/PPS，
    // 这一帧本身的画面内容会被下游丢弃（time base 与真实流不连续、且不是关键帧序列的一部分）。
    // Build one solid-color frame (uniform gray in YUV420P) purely to make the encoder
    // emit VPS/SPS/PPS. The picture content itself is discarded downstream (its time base
    // is not continuous with the real stream and it is not part of the keyframe sequence).
    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        return false;
    }
    frame->format = warm_fmt;
    frame->width = width;
    frame->height = height;
    int ret = av_frame_get_buffer(frame, 32);
    if (ret < 0) {
        WarnL << "warmup av_frame_get_buffer failed:" << ffmpeg_err_str(ret);
        av_frame_free(&frame);
        return false;
    }
    // 填成灰色（Y=128, U/V=128），避免纯黑导致某些编码器优化掉
    // Fill with mid-gray (Y=128, U/V=128); pure black may be optimized away
    for (int i = 0; i < 3; ++i) {
        int h = (i == 0) ? height : ((height + 1) / 2);
        int w = (i == 0) ? width : ((width + 1) / 2);
        int val = (i == 0) ? 128 : 128;
        for (int y = 0; y < h; ++y) {
            memset(frame->data[i] + (size_t) y * frame->linesize[i], val, (size_t) w);
        }
    }
    frame->pts = 0;
    frame->pict_type = AV_PICTURE_TYPE_I;

    // 预热产生的包不对外输出，只用来提取 config
    // Warmup packets are not emitted; they are only used to extract the config
    auto old_cb = _cb;
    std::vector<Frame::Ptr> config_frames;
    _cb = [&config_frames](const Frame::Ptr &f) {
        if (f) {
            config_frames.push_back(f);
        }
    };
    // 注意：绝不能在这里 send nullptr 冲刷编码器。nullptr 会让编码器进入 EOF
    // 状态，之后所有真实帧的 avcodec_send_frame 都会返回 "End of file"，
    // 整条转码链路从此不再产出任何帧。
    // 所以这里只用普通编码，接收 x265 在普通编码后就会吐出的首包（含 VPS/SPS/PPS）。
    // Never send nullptr here to flush: that moves the encoder into EOF state and every
    // later avcodec_send_frame for real frames returns "End of file", killing the whole
    // transcode chain. Plain encoding already makes x265 emit its first packet with
    // VPS/SPS/PPS, which is all we need.
    // 硬件编码器（VideoToolbox/NVENC）内部有帧缓冲，**第一帧不一定吐包**：
    // 实测 hevc_videotoolbox 只喂 1 帧时一个包都不出，warmup 判定失败、目标轨永远
    // unready，整条视频轨被 muxer 丢弃（症状是转码 CPU 几乎为 0、拉流 404）。
    // 软编 x265/x264 通常首帧就出包，所以这里循环到拿到包为止。
    // Hardware encoders buffer internally: feeding a single frame made
    // hevc_videotoolbox emit nothing, so warmup failed and the target track stayed
    // unready (muxers dropped it; symptom: ~0% CPU and a 404 on pull). Loop until a
    // packet appears; software encoders emit on the first frame anyway.
    bool ok = false;
    static constexpr int kMaxWarmupFrames = 12;
    for (int i = 0; i < kMaxWarmupFrames && config_frames.empty(); ++i) {
        frame->pts = (int64_t) i * 33;
        ok = encodeFrame(frame);
        if (!ok) {
            break;
        }
#ifdef ENABLE_ASCEND_VENC
        // 昇腾链路是异步的：encodeFrame 只保证「送进编码器」，码流在取流线程里才回调。
        // 不等就会在 config_frames 还空时就恢复 _cb，预热包被当成真实帧往下游送，
        // 此时目标轨尚未 ready，muxer 取 SPS 直接断言 abort（实测段错误）。
        if (_ascend) {
            for (int w = 0; w < 40 && config_frames.empty(); w++) {
                usleep(5 * 1000);
            }
        }
#endif
    }
    _cb = old_cb;
    av_frame_free(&frame);

    if (config_frames.empty()) {
        WarnL << "warmup produced no packet";
        return false;
    }

    // 裸帧 codec（VP8/VP9/AV1）没有 Annex-B 起始码，也没有 SPS/PPS：宽高只能从
    // 关键帧自身（VP8/VP9 的帧头、AV1 的 sequence header OBU）解析。所以这里把
    // 编码器首包整帧喂给目标轨，只为让它 ready；不往后转发（同下，pts=0 与真实
    // 时间轴不连续，会让 RTP 打包器时序错乱）。
    // Raw-packet codecs (VP8/VP9/AV1) have no Annex-B start codes and no SPS/PPS: the
    // dimensions live inside the keyframe itself (VP8/VP9 frame header, AV1 sequence
    // header OBU). Feed the encoder's first packet to the target track purely to make
    // it ready; do not forward it (see below: pts=0 is discontinuous with the real
    // timeline and corrupts the RTP packer's ordering).
    if (_codec_id == CodecVP8 || _codec_id == CodecVP9 || _codec_id == CodecAV1) {
        int fed = 0;
        for (auto &f : config_frames) {
            size_t prefix = f->prefixSize();
            if (f->size() <= prefix) {
                continue;
            }
            auto buf = std::make_shared<toolkit::BufferLikeString>();
            buf->assign(f->data() + prefix, f->size() - prefix);
            auto one = std::make_shared<TranscodedFrame>(_codec_id, buf, 0, 0, 0);
            one->setKeyFrameFromPacket(true);
            video->inputFrame(one);
            ++fed;
            if (video->ready()) {
                break;
            }
        }
        InfoL << "warmup done (raw packet codec): fed=" << fed << ", ready=" << video->ready()
              << ", size=" << video->getVideoWidth() << "x" << video->getVideoHeight();
        return video->ready();
    }

    // 只把预热包作为 config 帧喂给目标轨，用于提取 VPS/SPS/PPS 让 ready() 变真。
    // 注意不要把这些包当作普通帧往后传：它们的时间戳是 0，与真实流不连续，
    // 会让 RTP 打包器的环形缓冲时序错乱（曾导致 RtspMediaSource::onWrite 崩溃）。
    // Feed the warmup packet into the target track only as a config frame so it can
    // extract VPS/SPS/PPS. Do not forward it as a normal frame: its timestamp is 0 and
    // discontinuous with the real stream, which corrupts the RTP packer's ring buffer
    // ordering (this previously crashed RtspMediaSource::onWrite).
    // 只喂 VPS/SPS/PPS 三个 NAL 给目标轨，绝不能把整包（含 IDR 切片）喂进去。
    // 原因：H265Track::inputFrame 遇到 IDR 会触发 delegate，把这一帧当作真实画面
    // 推给下游 RTP 打包器；但这个包的 pts 是 0、与真实流时间轴不连续，
    // 会让 RingBuffer 时序错乱并崩溃（RtspMediaSource::onWrite SIGSEGV）。
    // Feed only the VPS/SPS/PPS NAL units into the target track, never the whole packet
    // (which also carries IDR slices). H265Track::inputFrame fires delegates on IDR and
    // pushes it downstream to the RTP packer as a real picture, but this packet has
    // pts=0 and is discontinuous with the real timeline, which corrupts the ring buffer
    // ordering and crashes (RtspMediaSource::onWrite SIGSEGV).
    int fed_config = 0;
    for (auto &f : config_frames) {
        size_t prefix = f->prefixSize() ? f->prefixSize() : 4;
        splitAnnexb(f->data(), f->size(), prefix,
                    [&](const char *nal, size_t nal_len, size_t /*pre*/) {
                        if (nal_len == 0) {
                            return;
                        }
                        bool is_config_nal;
                        if (_codec_id == CodecH264) {
                            // H264 的 NAL 头是 1 字节、类型占低 5 位，没有 VPS：
                            // 7=SPS 8=PPS。这里不能用 H265 的 (b0>>1)&0x3F，
                            // 否则 H264 的 SPS(0x67) 会算出 51，永远匹配不上，
                            // 目标轨永远不 ready，整条视频轨被 muxer 丢弃。
                            // H264's NAL header is one byte with the type in the low 5
                            // bits and has no VPS: 7=SPS, 8=PPS. The H265 formula
                            // (b0>>1)&0x3F would compute 51 for an H264 SPS (0x67) and
                            // never match, so the target track never becomes ready and
                            // muxers drop the whole video track.
                            int h264_type = ((uint8_t) nal[0]) & 0x1F;
                            is_config_nal = (h264_type == 7 || h264_type == 8);
                        } else {
                            int nal_type = (((uint8_t) nal[0]) >> 1) & 0x3F;
                            // 32=VPS 33=SPS 34=PPS；其余（IDR/SEI）一律丢弃
                            // 32=VPS 33=SPS 34=PPS; drop everything else (IDR/SEI)
                            is_config_nal = (nal_type == 32 || nal_type == 33 || nal_type == 34);
                        }
                        if (is_config_nal) {
                            // 同样用固定 4 字节起始码，和真实帧保持一致的形态
                            // Use a fixed 4-byte start code to match the real frames
                            auto buf = std::make_shared<toolkit::BufferLikeString>();
                            buf->assign("\x00\x00\x00\x01", 4);
                            buf->append(nal, nal_len);
                            auto one = std::make_shared<TranscodedFrame>(_codec_id, buf, 0, 0, 4);
                            video->inputFrame(one);
                            fed_config++;
                        }
                    });
    }
    InfoL << "warmup done: config_nals=" << fed_config << ", ready=" << video->ready()
          << ", size=" << video->getVideoWidth() << "x" << video->getVideoHeight();
    return video->ready();
}


bool FFmpegEncoder::isVideo() const {
    if (_context) {
        return _context->codec_type == AVMEDIA_TYPE_VIDEO;
    }
#ifdef ENABLE_ASCEND_VENC
    if (_ascend || _ascend_wanted) {
        return true;
    }
#endif
    return _codec_id == CodecH264 || _codec_id == CodecH265 || _codec_id == CodecVP8 ||
           _codec_id == CodecVP9 || _codec_id == CodecAV1;
}

#ifdef ENABLE_ASCEND_VENC

bool FFmpegEncoder::openAscendVideo(int width, int height, int bitrate) {
    // 通道号是整机共享资源：必须申请/归还，不能取模复用（复用会把在跑的流打停）
    AscendVenc::Config cfg;
    cfg.width = width;
    cfg.height = height;
    cfg.h265 = (_codec_id == CodecH265);
    cfg.fps = (int) llround(_video_fps > 0.5f ? _video_fps : 25.0f);
    cfg.bitrate_kbps = bitrate > 0 ? bitrate / 1000 : 0;
    cfg.channel = AscendVenc::acquireChannel();
    if (cfg.channel < 0) {
        WarnL << "ascend venc: no free DVPP channel (max 16 concurrent), fallback to software";
        return false;
    }

    std::string err;
    auto enc = std::make_shared<AscendVenc>();
    // 回调跑在 AscendVenc 的取流线程上：组装成 AVPacket 后复用 onEncode 的
    // 整条下游逻辑（Annex-B 组装、关键帧标记、时间轴推进），与软编路径完全一致。
    auto cb = [this](const char *data, size_t len, uint64_t, bool key) {
        AVPacket *pkt = av_packet_alloc();
        if (!pkt) {
            return;
        }
        if (av_new_packet(pkt, (int) len) < 0) {
            av_packet_free(&pkt);
            return;
        }
        memcpy(pkt->data, data, len);
        if (key) {
            pkt->flags |= AV_PKT_FLAG_KEY;
        }
        onEncode(pkt);
        av_packet_free(&pkt);
    };
    if (!enc->open(cfg, cb, err)) {
        WarnL << "ascend venc open failed: " << err << " (fallback to software)";
        return false;
    }
    InfoL << "ascend venc opened: " << width << "x" << height << " "
          << (cfg.h265 ? "H265" : "H264") << " ch=" << cfg.channel
          << " fps=" << cfg.fps << " bitrate=" << cfg.bitrate_kbps << "kbps";
    _ascend = enc;
    return true;
}

bool FFmpegEncoder::encodeAscend(AVFrame *frame) {
    if (!_ascend || !frame) {
        if (!_ascend) {
            WarnL << "encodeAscend called without ascend encoder";
        }
        return false;
    }
    int w = _out_width > 0 ? _out_width : frame->width;
    int h = _out_height > 0 ? _out_height : frame->height;
    if (w <= 0 || h <= 0) {
        return false;
    }
    size_t need = (size_t) w * h * 3 / 2;
    if (_ascend_nv12.size() != need) {
        _ascend_nv12.resize(need);
    }
    _ascend_sws = sws_getCachedContext(_ascend_sws, frame->width, frame->height,
                                       (AVPixelFormat) frame->format, w, h,
                                       AV_PIX_FMT_NV12, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!_ascend_sws) {
        WarnL << "ascend venc: sws_getCachedContext failed src=" << frame->width << "x"
              << frame->height << " fmt=" << frame->format;
        return false;
    }
    uint8_t *dst[4] = {_ascend_nv12.data(), _ascend_nv12.data() + (size_t) w * h, nullptr,
                       nullptr};
    int dst_stride[4] = {w, w, 0, 0};
    sws_scale(_ascend_sws, frame->data, frame->linesize, 0, frame->height, dst, dst_stride);

    // pts 必须严格递增（设备侧实测约束），按**输出帧率匀速推进**。
    // 不要用墙上时钟：解码偶发突发时墙上时间与源时间轴脱节，会把输出时间轴
    // 压缩（实测 dts 间隔 25ms 而非 40ms），下游按 25fps 播就会变速。
    // Advance pts by the nominal frame interval instead of wall-clock time: a decode
    // burst made the output timeline ~1.6x too fast (25ms instead of 40ms spacing).
    uint64_t pts = _ascend_pts_us;
    _ascend_pts_us += (uint64_t) (1000000.0 / (_video_fps > 0.5f ? _video_fps : 25.0f));
    if (!_ascend->encode(_ascend_nv12.data(), pts)) {
        WarnL << "ascend venc encode failed";
        return false;
    }
    return true;
}
#endif // ENABLE_ASCEND_VENC

bool FFmpegEncoder::encodeFrame(AVFrame *frame) {
#ifdef ENABLE_ASCEND_VENC
    if (_ascend) {
        return encodeAscend(frame);
    }
#endif
    auto ret = avcodec_send_frame(_context.get(), frame);
    if (ret < 0) {
        if (ret != AVERROR(EAGAIN)) {
            WarnL << "avcodec_send_frame failed:" << ffmpeg_err_str(ret);
        }
        return false;
    }
    return drainEncoder();
}

bool FFmpegEncoder::drainEncoder() {
    AVPacket *pkt = alloc_packet();
    if (!pkt) {
        return false;
    }
    bool ret = true;
    while (true) {
        auto err = avcodec_receive_packet(_context.get(), pkt);
        if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) {
            break;
        }
        if (err < 0) {
            WarnL << "avcodec_receive_packet failed:" << ffmpeg_err_str(err);
            ret = false;
            break;
        }
        onEncode(pkt);
        av_packet_unref(pkt);
    }
    free_packet(&pkt);
    return ret;
}

void FFmpegEncoder::flush() {
#ifdef ENABLE_ASCEND_VENC
    if (_ascend) {
        if (_flush_done) {
            return;
        }
        _flush_done = true;
        _ascend->flush();
        return;
    }
#endif
    if (!_context || _flush_done) {
        return;
    }
    _flush_done = true;
    // 音频 FIFO 里剩下的数据用静音补齐最后一帧，否则尾部会丢
    // Pad the tail of the audio FIFO so the last frame is not lost
    if (_fifo && _fifo->inited() && _context->codec_type == AVMEDIA_TYPE_AUDIO) {
        auto frame_size = audioChunkSamples();
        auto left = _fifo->size();
        if (left > 0 && left < frame_size) {
            TraceL << "flush audio fifo with " << left << "/" << frame_size << " samples";
        }
    }
    avcodec_send_frame(_context.get(), nullptr);
    drainEncoder();
    TraceL << "encoder flushed, total frames:" << _frame_index;
}

void FFmpegEncoder::onEncode(AVPacket *packet) {
    if (!_cb || !packet || packet->size <= 0) {
        return;
    }
    // 一个 AVPacket = 一帧（视频/音频同理）。这是「真实帧数」的唯一准确位置：
    // 再往下会被拆成多个 NAL 分别回调（x265 每帧 AUD+VCL，IDR 还有 VPS/SPS/PPS），
    // 在 NAL 级计数会虚高约 2.2 倍。
    // One AVPacket is one frame. This is the only accurate place to count: further down
    // the packet is split into NALs (AUD+VCL per frame, VPS/SPS/PPS per IDR).
    ++_encoded_frames;

    // 不依赖 ffmpeg 输出的 pts/dts：编码器的 time_base 与 ZLM 的毫秒体系不同，
    // 换算后仍可能出现负值或跳变。这里用自维护的毫秒时钟，每帧按固定间隔推进一步，
    // 保证下游看到的 dts 严格单调、从 0 开始。
    // Do not trust ffmpeg's pts/dts: the encoder time_base differs from ZLM's
    // millisecond scale and conversion can still yield negative or jumping values.
    // Use our own millisecond clock advancing by a fixed step per frame so
    // downstream sees strictly monotonic dts starting at 0.
    int64_t next_ms;
    if (isVideo()) {
        // 视频时间轴完全由 videoStepMs() 决定的步进累加（见那里的说明），
        // 这里不另起时钟。
        // The video timeline is the accumulation of videoStepMs() (see there); no
        // second clock is started here.
        // 不用 packet->pts 也不用编码器 time_base：libx265 会把时基重新缩放
        // （实测输入 6600 ticks/1ms 时基，输出算出 5280ms，恰好 0.8 倍），
        // 换算结果与源帧率不符，下游按源流时间判定就会大量丢帧。
        // Neither packet->pts nor the encoder time_base can be trusted: libx265 rescales
        // the timebase (an input of 6600 ticks on a 1ms base measured out as 5280ms,
        // exactly 0.8x), so the converted timeline disagrees with the source frame rate
        // and downstream drops most frames as stale.
        next_ms = nextVideoStamp(_video_out_ms);
    } else if (!_silence_encoding && _last_src_ms != AV_NOPTS_VALUE && _src_base_ms > 0) {
        // 音频跟随源流时间轴
        // Audio follows the source timeline
        next_ms = _last_src_ms - _src_base_ms;
    } else {
        // 静音兜底帧：只跟随自家的单调时钟（见 _silence_encoding 注释）
        // Silence padding frames follow our own monotonic clock only
        next_ms = _next_pts_ms;
    }
    // 无论如何都保持严格单调，防止解码器输出乱序导致下游报 Non-monotonic DTS
    // Always keep it strictly monotonic so decoder reordering cannot produce
    // non-monotonic DTS downstream
    // 视频的 next_ms 已由包序号严格递增，不需要也不能再被这个守卫改写；
    // 只有音频才需要它兜底乱序。
    // The video next_ms is already strictly increasing from the packet index and must
    // not be rewritten by this guard; only audio needs it as a reorder backstop.
    // 昇腾硬编路径没有 libavcodec 上下文（_context 为空），这里必须判空，
    // 否则视频首包就在这里解引用空指针（实测 SIGSEGV）
    // The Ascend path has no libavcodec context, so this must be null-checked
    if ((!_context || _context->codec_type != AVMEDIA_TYPE_VIDEO) && next_ms <= _emitted_ms) {
        next_ms = _emitted_ms + (_frame_interval_ms > 0 ? _frame_interval_ms : 20);
    }
    _emitted_ms = next_ms;
    if (_frame_interval_ms > 0) {
        _next_pts_ms = next_ms + _frame_interval_ms;
    } else if (_context && _context->frame_size > 0 && _context->sample_rate > 0) {
        _next_pts_ms = next_ms + (int64_t)(_context->frame_size * 1000LL / _context->sample_rate);
    } else {
        _next_pts_ms = next_ms + 40;
    }

    // 视频必须带正确的 Annex-B 起始码长度，否则 Track::inputFrame
    // 会把起始码当 NAL 头，解析不到 SPS/VPS/PPS（宽高恒为 0、不出画）。
    // Video requires the correct Annex-B start code length, otherwise
    // Track::inputFrame treats the start code as the NAL header, SPS/VPS/PPS
    // are never parsed (width/height stay 0 and nothing renders).
    size_t prefix_size = 0;
    if (_codec_id == CodecH264 || _codec_id == CodecH265) {
        prefix_size = _nalu_prefix_size;
        if (prefix_size == 0) {
            // 首帧探测并记住，后续帧复用，避免每帧扫描
            // Probe on the first frame and remember it to avoid rescanning
            prefix_size = probeAnnexbStartCodeLen(packet->data, (size_t) packet->size);
            if (prefix_size > 0) {
                _nalu_prefix_size = prefix_size;
            }
        }
    }
    bool is_key = (packet->flags & AV_PKT_FLAG_KEY) != 0;

    // 裸帧 codec（VP8/VP9/AV1）没有 Annex-B 起始码，编码包本身就是完整帧：
    // 原样交出、prefixSize=0。若沿用下面的 emit（前置 4 字节起始码 + prefix=4），
    // 下游 RtpEncoder 会把那 4 个字节当成 VP8 的 payload descriptor /
    // AV1 的 aggregation header，观众端直接解不出来。
    // Raw-packet codecs (VP8/VP9/AV1) carry no Annex-B start code — the encoded packet
    // IS the frame. Hand it over untouched with prefixSize=0. Reusing emit() below
    // (which prepends a 4-byte start code and reports prefix=4) makes the RTP packers
    // read those 4 bytes as a VP8 payload descriptor / AV1 aggregation header and the
    // viewer cannot decode anything.
    if (_codec_id == CodecVP8 || _codec_id == CodecVP9 || _codec_id == CodecAV1) {
        auto buf = std::make_shared<toolkit::BufferLikeString>();
        buf->assign((const char *) packet->data, (size_t) packet->size);
        auto frame = std::make_shared<TranscodedFrame>(_codec_id, buf, next_ms, next_ms, 0);
        frame->setKeyFrameFromPacket(is_key);
        frame->setIndex(_track_index);
        _cb(frame);
        return;
    }

    auto emit = [&](const char *nal, size_t nal_len, size_t /*prefix*/) {
        // 统一按 4 字节起始码组装：下游 H265RtpEncoder / 各类 Muxer 只用 prefixSize
        // 一个值定位裸 NAL 头，若这一帧的起始码是 3 字节而 prefixSize 写 4（或反过来），
        // NAL 头就会错位 —— 观众端报 Illegal temporal ID / Multi-layer HEVC，
        // 整包还会被当 NAL 再 FU 封装一次。
        // Always assemble with a 4-byte start code: downstream (H265RtpEncoder and every
        // muxer) locates the bare NAL header from a single prefixSize value, so a 3-byte
        // start code with prefixSize 4 (or vice versa) shifts the NAL header — the viewer
        // reports Illegal temporal ID / Multi-layer HEVC and whole packets get FU-wrapped
        // a second time.
        if (nal_len == 0) {
            return;
        }
        auto buf = std::make_shared<toolkit::BufferLikeString>();
        buf->assign("\x00\x00\x00\x01", 4);
        buf->append(nal, nal_len);
        // 用 TranscodedFrame 而不是裸 FrameFromBuffer：后者不会报告 keyFrame/configFrame，
        // 会让 GOP 缓存不从关键帧开始、观众中途接入报 "Could not find ref with POC"。
        // Use TranscodedFrame rather than a bare FrameFromBuffer: the latter never
        // reports keyFrame/configFrame, so the GOP cache stops starting at keyframes and
        // mid-stream viewers fail with "Could not find ref with POC".
        auto frame = std::make_shared<TranscodedFrame>(_codec_id, buf, next_ms, next_ms, 4);
        frame->setIndex(_track_index);
        _cb(frame);
    };

    // H264/H265 必须把编码包拆成单 NAL 帧再往下传。
    // 原因：一个编码包通常同时含 VPS+SPS+PPS+IDR 四个 NAL，而 ZLM 的 RTP/RTMP 打包器
    // 只看第一个 NAL 的类型做分发——看到 VPS 就当成 config 存起来并 return，
    // 整个 IDR 帧被丢弃，观众端收到 config 却没有画面，并报
    // "Illegal temporal ID in RTP/HEVC packet"。
    // H264/H265 packets must be split into single-NAL frames. One encoded packet usually
    // carries VPS+SPS+PPS+IDR; ZLM's RTP/RTMP packers dispatch on the FIRST NAL type only,
    // so a packet starting with VPS is stored as config and returned, dropping the whole
    // IDR. Viewers then get config but no picture and ffmpeg reports
    // "Illegal temporal ID in RTP/HEVC packet".
    if ((_codec_id == CodecH264 || _codec_id == CodecH265) && prefix_size > 0) {
        splitAnnexb((const char *) packet->data, (size_t) packet->size, prefix_size,
                    [&](const char *ptr, size_t len, size_t prefix) { emit(ptr, len, prefix); });
        return;
    }

    emit((const char *) packet->data, (size_t) packet->size, prefix_size);
}

}//namespace mediakit
#endif//ENABLE_FFMPEG
