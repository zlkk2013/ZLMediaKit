/*
* Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
*
* This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
*
* Use of this source code is governed by MIT-like license that can be found in the
* LICENSE file in the root of the source tree. All contributing project authors
* may be found in the AUTHORS file in the root of the source tree.
*/

#ifndef ZLMEDIAKIT_PACKET_CACHE_H_
#define ZLMEDIAKIT_PACKET_CACHE_H_

#include "Common/config.h"
#include "Util/List.h"
#include <mutex>

namespace mediakit {
// / 缓存刷新策略类  [AUTO-TRANSLATED:bd941d15]
// / Cache refresh strategy class
class FlushPolicy {
public:
    bool isFlushAble(bool is_video, bool is_key, uint64_t new_stamp, size_t cache_size);

private:
    // 音视频的最后时间戳  [AUTO-TRANSLATED:957d18ed]
    // Last timestamp of audio and video
    uint64_t _last_stamp[2] = { 0, 0 };
};

// / 合并写缓存模板  [AUTO-TRANSLATED:25cde944]
// / Merge write cache template
// / \tparam packet 包类型  [AUTO-TRANSLATED:43085d9b]
// / \tparam packet Packet type
// / \tparam policy 刷新缓存策略  [AUTO-TRANSLATED:c5ac29a7]
// / \tparam policy Refresh cache strategy
// / \tparam packet_list 包缓存类型  [AUTO-TRANSLATED:a434e7fe]
// / \tparam packet_list Packet cache type
template<typename packet, typename policy = FlushPolicy, typename packet_list = toolkit::List<std::shared_ptr<packet> > >
class PacketCache {
public:
    PacketCache() { _cache = std::make_shared<packet_list>(); }

    virtual ~PacketCache() = default;

    void inputPacket(uint64_t stamp, bool is_video, std::shared_ptr<packet> pkt, bool key_pos) {
        // 合写缓存必须是线程安全的：转码场景下视频帧来自编码器线程、音频帧来自
        // 推流端线程，两条线程会同时进到同一个 PacketCache。flush() 里
        // `onFlush(std::move(_cache))` 与随后重建 _cache 之间有窗口，另一个线程
        // 在窗口里 emplace_back 就会踩到空的 _cache → SIGSEGV（实测 50~100 个包后崩）。
        // The merge-write cache must be thread safe: with transcoding, video frames come
        // from the encoder thread while audio frames come from the publisher thread, and
        // both enter the same PacketCache. flush() moves _cache out and only then
        // reassigns it; another thread appending inside that window dereferences a null
        // _cache -> SIGSEGV (measured after 50-100 packets).
        std::lock_guard<std::mutex> lck(_mtx);
        bool flag = flushImmediatelyWhenCloseMerge();
        if (!flag && _policy.isFlushAble(is_video, key_pos, stamp, _cache->size())) {
            flush_l();
        }

        // 追加数据到最后  [AUTO-TRANSLATED:e24ccfb6]
        // Append data to the end
        _cache->emplace_back(std::move(pkt));
        if (key_pos) {
            _key_pos = key_pos;
        }

        if (flag) {
            flush_l();
        }
    }

    void flush() {
        std::lock_guard<std::mutex> lck(_mtx);
        flush_l();
    }

    virtual void clearCache() {
        std::lock_guard<std::mutex> lck(_mtx);
        _cache->clear();
    }

    virtual void onFlush(std::shared_ptr<packet_list>, bool key_pos) = 0;

private:
    /// 调用方必须已持有 _mtx
    /// Caller must already hold _mtx
    void flush_l() {
        if (_cache->empty()) {
            return;
        }
        onFlush(std::move(_cache), _key_pos);
        _cache = std::make_shared<packet_list>();
        _key_pos = false;
    }

    bool flushImmediatelyWhenCloseMerge() {
        // 一般的协议关闭合并写时，立即刷新缓存，这样可以减少一帧的延时，但是rtp例外  [AUTO-TRANSLATED:54eba701]
        // Generally, when the protocol closes the merge write, the cache is refreshed immediately, which can reduce the delay of one frame, but RTP is an exception.
        // 因为rtp的包很小，一个RtpPacket包中也不是完整的一帧图像，所以在关闭合并写时，  [AUTO-TRANSLATED:b219082d]
        // Because the RTP packet is very small, and a RtpPacket does not contain a complete frame of image, so when closing the merge write,
        // 还是有必要缓冲一帧的rtp(也就是时间戳相同的rtp)再输出，这样虽然会增加一帧的延时  [AUTO-TRANSLATED:27c7ee8b]
        // It is still necessary to buffer one frame of RTP (that is, RTP with the same timestamp) before outputting. Although this will increase the delay of one frame,
        // 但是却对性能提升很大，这样做还是比较划算的  [AUTO-TRANSLATED:80eab719]
        // But it greatly improves performance, so it is still worthwhile to do so.

        GET_CONFIG(int, mergeWriteMS, General::kMergeWriteMS);
        GET_CONFIG(int, rtspLowLatency, Rtsp::kLowLatency);
        return std::is_same<packet, RtpPacket>::value ? rtspLowLatency : (mergeWriteMS <= 0);
    }

private:
    bool _key_pos = false;
    std::mutex _mtx;
    policy _policy;
    std::shared_ptr<packet_list> _cache;
};
}

#endif //ZLMEDIAKIT_PACKET_CACHE_H_
