/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef ZLMEDIAKIT_MULTIMEDIASOURCEMUXER_H
#define ZLMEDIAKIT_MULTIMEDIASOURCEMUXER_H

#include "Common/Stamp.h"
#include "Common/MediaSource.h"
#include "Common/MediaSink.h"
#include "Record/Recorder.h"
#include "Rtp/RtpSender.h"
#include "Record/HlsRecorder.h"
#include "Record/HlsMediaSource.h"
#include "Rtsp/RtspMediaSourceMuxer.h"
#include "Rtmp/RtmpMediaSourceMuxer.h"
#include "TS/TSMediaSourceMuxer.h"
#include "FMP4/FMP4MediaSourceMuxer.h"

namespace mediakit {

struct FrameInfo {
    CodecId codec_id = CodecInvalid;
    int64_t dts = 0;
    int64_t pts = 0;
    int64_t recv_stamp = 0;
    size_t frame_size = 0;

    int index = 0;
    bool key_frame = false;
    bool config_frame = false;
};

class MultiMediaSourceMuxer : public MediaSourceEventInterceptor, public MediaSink, public toolkit::noncopyable, public std::enable_shared_from_this<MultiMediaSourceMuxer>{
public:
    using Ptr = std::shared_ptr<MultiMediaSourceMuxer>;
    using RingType = toolkit::RingBuffer<Frame::Ptr>;
    using onCreateMuxer = std::function<MediaSinkInterface::Ptr()>;

    class Listener {
    public:
        virtual ~Listener() = default;
        virtual void onAllTrackReady() = 0;
    };

    MultiMediaSourceMuxer(const MediaTuple& tuple, float dur_sec = 0.0,const ProtocolOption &option = ProtocolOption());

    /**
     * 设置事件监听器
     * @param listener 监听器
     * Set event listener
     * @param listener Listener
     
     * [AUTO-TRANSLATED:d829419b]
     */
    void setMediaListener(const std::weak_ptr<MediaSourceEvent> &listener);

     /**
      * 设置Track就绪事件监听器
      * @param listener 事件监听器
      * Set Track ready event listener
      * @param listener Event listener
      
      * [AUTO-TRANSLATED:64262ac5]
     */
    void setTrackListener(const std::weak_ptr<Listener> &listener);

    /**
     * 返回总的消费者个数
     * Return the total number of consumers
     
     * [AUTO-TRANSLATED:5eaac131]
     */
    int totalReaderCount() const;

    /**
     * 判断是否生效(是否正在转其他协议)
     * Determine whether it is effective (whether it is being converted to another protocol)
     
     * [AUTO-TRANSLATED:ca92165c]
     */
    bool isEnabled();

    /**
     * 本 muxer 的 RTSP 输出源（未开启 RTSP 转协议时为空）。
     *
     * 推流端（RtspMediaSourceImp）需要拿到它，把自己登记成它的“推流源”，
     * 否则推流端断连重推时 ANNOUNCE 只找到这个 muxer 源，认不出它背后还有
     * 一个可接管的推流端，只能回 406 Already publishing。
     *
     * RTSP output source of this muxer (null when RTSP conversion is off).
     * The RTSP pusher needs it to register itself as its push source; otherwise a
     * reconnecting pusher only finds this muxer source in the pool and cannot
     * realize a takeover is possible, and answers 406 Already publishing.
     */
    MediaSource::Ptr getRtspMediaSource() const;

    /**
     * 推流端对象销毁时调用：丢弃本 muxer 的全部转协议输出源。
     *
     * 启用转码后 muxer 内部存在引用环：
     * `muxer → TranscodeSink → 编解码器 → 源 Track →(FrameDispatcher 的 delegate)→ muxer`，
     * 环不断开则 muxer 永不析构，它对外发布的源（含 RTSP 源）会一直留在媒体源池里，
     * 推流端重连时 ANNOUNCE 永远拿到 Already publishing。
     *
     * Called when the pusher object is destroyed: drop every output source.
     * With transcoding there is a reference cycle
     * `muxer -> TranscodeSink -> codec -> source Track -> (FrameDispatcher delegate) -> muxer`,
     * so without breaking it the muxer never dies and its published sources keep the
     * pool keys, making every reconnect answer "Already publishing".
     */
    void releaseMediaSources();

    /**
     * 重置track
     * Reset track
     
     * [AUTO-TRANSLATED:95dc0b4f]
     */
    void resetTracks() override;

    /////////////////////////////////MediaSourceEvent override/////////////////////////////////

    /**
     * 观看总人数
     * @param sender 事件发送者
     * @return 观看总人数
     * Total number of viewers
     * @param sender Event sender
     * @return Total number of viewers
     
     * [AUTO-TRANSLATED:f4d7146c]
     */
    int totalReaderCount(MediaSource &sender) override;

    /**
     * 设置录制状态
     * @param type 录制类型
     * @param start 开始或停止
     * @param custom_path 开启录制时，指定自定义路径
     * @return 是否设置成功
     * Set recording status
     * @param type Recording type
     * @param start Start or stop
     * @param custom_path Specify a custom path when recording is enabled
     * @return Whether the setting is successful
     
     * [AUTO-TRANSLATED:cb1fd8a9]
     */
    bool setupRecord(MediaSource &sender, Recorder::type type, bool start, const std::string &custom_path, size_t max_second);

    /**
     * 开始录制mp4
     * @param file_path mp4相对路径
     * @param back_time_ms 回溯录制时长
     * @param forward_time_ms 后续录制时长
     * @return 录制文件绝对路径
     */
    std::string startRecord(const std::string &file_path, int back_time_ms, int forward_time_ms);

    /**
     * 获取录制状态
     * @param type 录制类型
     * @return 录制状态
     * Get recording status
     * @param type Recording type
     * @return Recording status
     
     * [AUTO-TRANSLATED:798afa71]
     */
    bool isRecording(Recorder::type type);

    /**
     * 开始发送ps-rtp流
     * @param cb 启动成功或失败回调
     */
    void startSendRtp(const MediaSourceEvent::SendRtpArgs &args, const std::function<void(uint16_t, const toolkit::SockException &)> cb);

    /**
     * 停止ps-rtp发送
     * @return 是否成功
     * Stop ps-rtp sending
     * @return Whether it is successful
     
     * [AUTO-TRANSLATED:b91e2055]
     */
    bool stopSendRtp(const std::string &ssrc);

    /**
     * 获取所属线程
     * Get the thread it belongs to
     
     * [AUTO-TRANSLATED:a4dc847e]
     */
    toolkit::EventPoller::Ptr getOwnerPoller(MediaSource &sender) override;
    
    /**
     * 关闭流
     * @return 是否成功
     */
    bool close(MediaSource &sender) override;

    /**
     * 获取本对象
     * Get this object
     
     * [AUTO-TRANSLATED:5e119bb3]
     */
    std::shared_ptr<MultiMediaSourceMuxer> getMuxer(MediaSource &sender) const override;

    // 获取frame ring reader
    RingType::RingReader::Ptr getFrameReader();

    const ProtocolOption &getOption() const;
    const MediaTuple &getMediaTuple() const;
    std::string shortUrl() const;
#if defined(ENABLE_RTPPROXY)
    void forEachRtpSender(const std::function<void(const std::string &ssrc, const RtpSender &sender)> &cb) const;
#endif // ENABLE_RTPPROXY

    void addProbe(uint32_t probe_ms, const std::function<void(const std::list<FrameInfo> &info_list)> &cb);

protected:
    /////////////////////////////////MediaSink override/////////////////////////////////

    /**
    * 某track已经准备好，其ready()状态返回true，
    * 此时代表可以获取其例如sps pps等相关信息了
    * @param track
     * A certain track is ready, its ready() status returns true,
     * This means that you can get information such as sps pps, etc.
     * @param track
     
     * [AUTO-TRANSLATED:05659d48]
    */
    bool onTrackReady(const Track::Ptr & track) override;

    /**
     * 所有Track已经准备好，
     * All Tracks are ready,
     
     * [AUTO-TRANSLATED:c54d02e2]
     */
    void onAllTrackReady() override;

    /**
     * 某Track输出frame，在onAllTrackReady触发后才会调用此方法
     * @param frame
     * A certain Track outputs a frame, this method will be called after onAllTrackReady is triggered
     * @param frame
     
     * [AUTO-TRANSLATED:debbd247]
     */
    bool onTrackFrame(const Frame::Ptr &frame) override;
    bool onTrackFrame_l(const Frame::Ptr &frame);

#if defined(ENABLE_FFMPEG)
    /**
     * 转码输出帧的回灌入口（在解码/编码线程调用，不是源线程）。
     * 下游 muxer（fmp4/ts/hls 的 BufferLikeString、paced sender 等）都假定单线程，
     * 而源轨透传帧来自 RTP 线程、转码帧来自解码线程，两路并发写同一个 muxer
     * 会破坏堆内存（实测 5 秒内 malloc abort，栈停在 MP4FileMemory::onWrite）。
     * 所以这里和 onTrackFrame 用同一把递归锁串行化。
     *
     * Re-entry point for transcoded frames (called on the codec thread, not the
     * source thread). Downstream muxers assume single-threaded input; concurrent
     * writes from the pass-through path (RTP thread) and the transcoder corrupt
     * the heap, so both paths share one recursive lock.
     */
    void onTranscodeFrame(const Frame::Ptr &frame);
#endif//ENABLE_FFMPEG

private:
    void createGopCacheIfNeed();
    std::shared_ptr<MediaSinkInterface> makeRecorder(Recorder::type type);

private:
    bool _is_enable = false;
    bool _create_in_poller = false;
    bool _video_key_pos = false;
    float _dur_sec;
    std::function<void(const Frame::Ptr &frame)> _on_frame;
    std::shared_ptr<class FramePacedSender> _paced_sender;
    MediaTuple _tuple;
    ProtocolOption _option;
    toolkit::Ticker _last_check;
    std::unordered_map<int, Stamp> _stamps;
    std::weak_ptr<Listener> _track_listener;
    /**
     * 串行化下游帧分发：源轨透传帧在源线程、转码帧在解码线程，下游 muxer 不是
     * 线程安全的（见 onTranscodeFrame 说明）。递归锁，因为透传路径内部可能同步
     * 回调出转码帧。
     *
     * Serializes downstream frame distribution: pass-through frames arrive on the
     * source thread while transcoded frames arrive on codec threads. Recursive
     * because the pass-through path can synchronously emit a transcoded frame.
     */
    std::recursive_mutex _frame_mtx;
#if defined(ENABLE_RTPPROXY)
    std::unordered_multimap<std::string, std::tuple<RingType::RingReader::Ptr, std::weak_ptr<RtpSender>>> _rtp_sender;
#endif // ENABLE_RTPPROXY
    FMP4MediaSourceMuxer::Ptr _fmp4;
    RtmpMediaSourceMuxer::Ptr _rtmp;
    RtspMediaSourceMuxer::Ptr _rtsp;
    TSMediaSourceMuxer::Ptr _ts;
    MediaSinkInterface::Ptr _mp4;
    HlsRecorder::Ptr _hls;
    HlsFMP4Recorder::Ptr _hls_fmp4;
    toolkit::EventPoller::Ptr _poller;
    RingType::Ptr _ring;
    MediaSinkInterface::Ptr _delegate;
#if defined(ENABLE_FFMPEG)
    // 转码节点：为 nullptr 时行为与上游完全一致
    // Transcode node; when nullptr, behavior is identical to upstream
    std::shared_ptr<class TranscodeSink> _transcode;
#endif//ENABLE_FFMPEG
    // 对象个数统计  [AUTO-TRANSLATED:3b43e8c2]
    // Object count statistics
    toolkit::ObjectStatistic<MultiMediaSourceMuxer> _statistic;
};

}//namespace mediakit
#endif //ZLMEDIAKIT_MULTIMEDIASOURCEMUXER_H
