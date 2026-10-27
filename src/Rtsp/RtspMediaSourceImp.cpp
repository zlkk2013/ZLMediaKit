#include "RtspMediaSourceImp.h"
#include "RtspDemuxer.h"
#include "Common/config.h"
#if defined(ENABLE_FFMPEG)
#include "Transcode/TranscodeConfig.h"
#endif
namespace mediakit {
void RtspMediaSource::setSdp(const std::string &sdp) {
    SdpParser sdp_parser(sdp);
    _have_video = static_cast<bool>(sdp_parser.getTrack(TrackVideo));
    _sdp = sdp_parser.toString();
    if (_ring) {
        regist();
    }
}

void RtspMediaSource::updateSdpCache(const std::string &sdp) {
    SdpParser sdp_parser(sdp);
    _have_video = static_cast<bool>(sdp_parser.getTrack(TrackVideo));
    _sdp = sdp_parser.toString();
}

void RtspMediaSource::onWrite(RtpPacket::Ptr rtp, bool keyPos) {
    _speed[rtp->type] += rtp->size();
    assert(rtp->type >= 0 && rtp->type < TrackMax);
    auto stamp = rtp->getStampMS();
    if (!_ring) {
        std::weak_ptr<RtspMediaSource> weakSelf = std::static_pointer_cast<RtspMediaSource>(shared_from_this());
        auto lam = [weakSelf](int size) {
            auto strongSelf = weakSelf.lock();
            if (!strongSelf) {
                return;
            }
            strongSelf->onReaderChanged(size);
        };
        // GOP默认缓冲512组RTP包，每组RTP包时间戳相同(如果开启合并写了，那么每组为合并写时间内的RTP包),  [AUTO-TRANSLATED:dc09b92e]
        // GOP defaults to buffering 512 groups of RTP packets, each group of RTP packets has the same timestamp (if merge writing is enabled, then each group is the RTP packet within the merge writing time),
        // 每次遇到关键帧第一个RTP包，则会清空GOP缓存(因为有新的关键帧了，同样可以实现秒开)  [AUTO-TRANSLATED:db44dc72]
        // Every time a key frame's first RTP packet is encountered, the GOP cache will be cleared (because there is a new key frame, which can also achieve instant playback)
        _ring = std::make_shared<RingType>(_ring_size, std::move(lam));
        if (!_sdp.empty()) {
            regist();
        }
    }
    auto is_video = rtp->type == TrackVideo;
    PacketCache<RtpPacket>::inputPacket(stamp, is_video, std::move(rtp), keyPos);
}

RtspMediaSourceImp::RtspMediaSourceImp(const MediaTuple& tuple, int ringSize): RtspMediaSource(tuple, ringSize)
{
    _demuxer = std::make_shared<RtspDemuxer>();
    _demuxer->setTrackListener(this);
}

void RtspMediaSourceImp::setSdp(const std::string &strSdp)
{
    if (!getSdp().empty()) {
        return;
    }
    _demuxer->loadSdp(strSdp);
    RtspMediaSource::setSdp(strSdp);
}

void RtspMediaSourceImp::onWrite(RtpPacket::Ptr rtp, bool key_pos)
{
    if (_all_track_ready && !_muxer->isEnabled()) {
        // 获取到所有Track后，并且未开启转协议，那么不需要解复用rtp  [AUTO-TRANSLATED:31cbc558]
        // After getting all Tracks and not enabling protocol conversion, there is no need to demultiplex rtp
        // 在关闭rtp解复用后，无法知道是否为关键帧，这样会导致无法秒开，或者开播花屏  [AUTO-TRANSLATED:279f1332]
        // After closing rtp demultiplexing, it is impossible to know whether it is a key frame, which will lead to the inability to achieve instant playback or the screen will be garbled when playing
        key_pos = rtp->type == TrackVideo;
    } else {
        // 需要解复用rtp  [AUTO-TRANSLATED:0deaf9f1]
        // Need to demultiplex rtp
        key_pos = _demuxer->inputRtp(rtp);
    }
    GET_CONFIG(bool, directProxy, Rtsp::kDirectProxy);
    if (directProxy && !_transcode_enabled) {
        // 直接代理模式才直接使用原始rtp  [AUTO-TRANSLATED:afd4ae3b]
        // Only the direct proxy mode directly uses the original rtp
        // 启用转码时不能走这里：转码换轨在 MultiMediaSourceMuxer 内部完成，直接把源
        // RTP 透传出去会让观众看到源编码（H265 源配 H264 目标时观众仍拿到 H265）。
        // 解复用后帧会经 muxer 里的 TranscodeSink 换轨，再由 muxer 自己的
        // RtspMediaSource 重新打成 RTP 发给观众。
        //
        // Not allowed while transcoding: the track swap happens inside the muxer,
        // so forwarding the source RTP as-is shows viewers the source codec.
        RtspMediaSource::onWrite(std::move(rtp), key_pos);
    }
}

bool RtspMediaSourceImp::resolveTranscodeEnable() {
#if defined(ENABLE_FFMPEG)
    std::string source;
    auto option = TranscodeRuntime::instance().resolve(getMediaTuple(), &source);
    _transcode_enabled = option.enable;
    if (_transcode_enabled) {
        // 转码流必须走解复用+muxer 路径，日志里显式说明一句，方便对账
        // Transcoded streams must go the demux+muxer way; say so in the log
        InfoL << "transcode enabled, direct-proxy disabled for " << getUrl()
              << " (source=" << source << ")";
    }
#else
    _transcode_enabled = false;
#endif
    return _transcode_enabled;
}

void RtspMediaSourceImp::setProtocolOption(const ProtocolOption &option)
{
    GET_CONFIG(bool, direct_proxy, Rtsp::kDirectProxy);
    // 开启直接代理模式时，rtsp直接代理，不重复产生；但是有些rtsp推流端，由于sdp中已有sps pps，rtp中就不再包括sps pps,  [AUTO-TRANSLATED:1bbc0e31]
    // When direct proxy mode is enabled, rtsp is directly proxied and not duplicated; however, some rtsp push stream ends, because there are already sps pps in the sdp, rtp no longer includes sps pps,
    // 导致rtc无法播放，所以在rtsp推流rtc播放时，建议关闭直接代理模式  [AUTO-TRANSLATED:2c705dec]
    // This leads to the inability of rtc to play, so it is recommended to turn off direct proxy mode when rtsp pushes the stream and rtc plays
    _option = option;
    // 启用转码时同样必须让 muxer 生成 RTSP 输出源：转码后的轨只有 muxer 里才有，
    // 观众必须连到 muxer 的 RtspMediaSource（SDP 带 a=control:trackID=）而不是
    // 推流端的 RtspMediaSourceImp（SDP 是推流端原始 SDP，带 streamid=）。
    //
    // Transcoding also requires the muxer to own the RTSP output source, otherwise
    // viewers keep reading the publisher's RtspMediaSourceImp and its raw SDP.
    _transcode_enabled = resolveTranscodeEnable();
    _option.enable_rtsp = direct_proxy ? _transcode_enabled : true;
    _muxer = std::make_shared<MultiMediaSourceMuxer>(_tuple, _demuxer->getDuration(), _option);
    _muxer->setMediaListener(getListener());
    _muxer->setTrackListener(std::static_pointer_cast<RtspMediaSourceImp>(shared_from_this()));
    // 让_muxer对象拦截一部分事件(比如说录像相关事件)  [AUTO-TRANSLATED:32320477]
    // Let the _muxer object intercept some events (such as recording related events)
    MediaSource::setListener(_muxer);

    for (auto &track : _demuxer->getTracks(false)) {
        _muxer->addTrack(track);
        track->addDelegate(_muxer);
    }
}

RtspMediaSource::Ptr RtspMediaSourceImp::clone(const std::string &stream) {
    auto tuple = _tuple;
    tuple.stream = stream;
    auto src_imp = std::make_shared<RtspMediaSourceImp>(tuple);
    src_imp->setSdp(getSdp());
    src_imp->setProtocolOption(getProtocolOption());
    return src_imp;
}

}

