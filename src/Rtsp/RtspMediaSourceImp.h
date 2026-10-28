/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef SRC_RTSP_RTSPTORTMPMEDIASOURCE_H_
#define SRC_RTSP_RTSPTORTMPMEDIASOURCE_H_

#include "RtspMediaSource.h"
#include "RtspDemuxer.h"
#include "Common/MultiMediaSourceMuxer.h"

namespace mediakit {
class RtspDemuxer;
class RtspMediaSourceImp final : public RtspMediaSource, private TrackListener, public MultiMediaSourceMuxer::Listener  {
public:
    using Ptr = std::shared_ptr<RtspMediaSourceImp>;

    /**
     * 构造函数
     * @param vhost 虚拟主机
     * @param app 应用名
     * @param id 流id
     * @param ringSize 环形缓存大小
     * Constructor
     * @param vhost Virtual host
     * @param app Application name
     * @param id Stream id
     * @param ringSize Ring buffer size
     
     * [AUTO-TRANSLATED:7679d212]
     */
    RtspMediaSourceImp(const MediaTuple& tuple, int ringSize = RTP_GOP_SIZE);

    /**
     * 设置sdp
     * Set sdp
     
     * [AUTO-TRANSLATED:76a533c4]
     */
    void setSdp(const std::string &strSdp) override;

    /**
     * 输入rtp并解析
     * Input rtp and parse
     
     * [AUTO-TRANSLATED:778f743f]
     */
    void onWrite(RtpPacket::Ptr rtp, bool key_pos) override;

    /**
     * 获取观看总人数，包括(hls/rtsp/rtmp)
     * Get total number of viewers, including (hls/rtsp/rtmp)
     
     * [AUTO-TRANSLATED:19a26d5a]
     */
    int totalReaderCount() override {
        return readerCount() + (_muxer ? _muxer->totalReaderCount() : 0);
    }

    /**
     * 设置协议转换选项
     * Set protocol conversion options
     
     * [AUTO-TRANSLATED:a6a9b24a]
     */
    void setProtocolOption(const ProtocolOption &option);

    const ProtocolOption &getProtocolOption() const {
        return _option;
    }

    /**
     * _demuxer触发的添加Track事件
     * _demuxer triggered add Track event
     
     * [AUTO-TRANSLATED:80dbcf16]
     */
    bool addTrack(const Track::Ptr &track) override {
        if (_muxer) {
            if (_muxer->addTrack(track)) {
                track->addDelegate(_muxer);
                return true;
            }
        }
        return false;
    }

    /**
     * _demuxer触发的Track添加完毕事件
     * _demuxer triggered Track add complete event
     
     * [AUTO-TRANSLATED:939cb312]
     */
    void addTrackCompleted() override {
        if (_muxer) {
            _muxer->addTrackCompleted();
        }
    }

    void resetTracks() override {
        if (_muxer) {
            _muxer->resetTracks();
        }
    }

    /**
     * _muxer触发的所有Track就绪的事件
     * _muxer triggered all Track ready event
     
     * [AUTO-TRANSLATED:1d34b7e0]
     */
    void onAllTrackReady() override{
        _all_track_ready = true;
    }

    /**
     * 设置事件监听器
     * @param listener 监听器
     * Set event listener
     * @param listener Listener
     
     * [AUTO-TRANSLATED:d829419b]
     */
    void setListener(const std::weak_ptr<MediaSourceEvent> &listener) override{
        if (_muxer) {
            // _muxer对象不能处理的事件再给listener处理  [AUTO-TRANSLATED:47858305]
            // _muxer object cannot handle the event, then give it to the listener
            _muxer->setMediaListener(listener);
        } else {
            // 未创建_muxer对象，事件全部给listener处理  [AUTO-TRANSLATED:eec04bc3]
            // The _muxer object is not created, all events are given to the listener
            MediaSource::setListener(listener);
        }
    }

    RtspMediaSource::Ptr clone(const std::string& stream) override;

    /**
     * 推流端对象销毁 = 这条流真的结束了（含 `continue_push_ms` 的延时注销窗口）。
     *
     * 必须在这里显式释放 muxer 的输出源：启用转码时 muxer 内部存在引用环
     * （详见 MultiMediaSourceMuxer::releaseMediaSources()），自然析构不会发生，
     * 它对外发布的 RTSP 源会一直占着媒体源池里的键，推流端重连时 ANNOUNCE
     * 永远回 406 Already publishing。
     *
     * The pusher object dying means the stream is really over (including the
     * continue_push_ms grace window). The muxer's output sources must be released
     * here: with transcoding there is a reference cycle inside the muxer (see
     * MultiMediaSourceMuxer::releaseMediaSources()), so it never destructs on its
     * own and its published RTSP source keeps the pool key forever, which makes
     * every reconnect answer 406 Already publishing.
     */
    ~RtspMediaSourceImp() override;

private:
    /**
     * 本流是否启用转码（读 [transcode] 配置 + API 覆盖）。
     * 只用于决定 onWrite 走 direct-proxy 还是解复用，所以结果缓存在
     * _transcode_enabled 里，避免每个 RTP 包都去解析一遍配置。
     *
     * Whether transcoding is enabled for this stream. Only used to pick the
     * direct-proxy vs. demux path in onWrite; the answer is cached in
     * _transcode_enabled so we do not re-parse the config for every RTP packet.
     */
    bool resolveTranscodeEnable();

    bool _all_track_ready = false;
    /**
     * 启用转码时必须关闭 direct-proxy：转码换轨发生在 MultiMediaSourceMuxer
     * 内部，而 direct-proxy 把源 RTP 原样透传给观众，两条路不交叉，转码输出
     * 永远到不了观众端（见 docs/TRANSCODE_AUDIO_FRAMING_FIX.md §10.4）。
     *
     * Direct-proxy must be off when transcoding: the track swap happens inside
     * MultiMediaSourceMuxer, while direct-proxy forwards the source RTP as-is.
     * The two paths never meet, so transcoded output would never reach viewers.
     */
    bool _transcode_enabled = false;
    ProtocolOption _option;
    RtspDemuxer::Ptr _demuxer;
    MultiMediaSourceMuxer::Ptr _muxer;
};
} /* namespace mediakit */

#endif /* SRC_RTSP_RTSPTORTMPMEDIASOURCE_H_ */
