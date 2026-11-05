/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef ZLMEDIAKIT_S3_VOD_H
#define ZLMEDIAKIT_S3_VOD_H

#include <string>

#include "Common/MediaSource.h"

namespace mediakit {

/**
 * 云点播：把桶里的一个 mp4 对象变成一个媒体源，于是 rtsp/rtmp/http-flv/webrtc 都能放。
 *
 * 寻址约定（和本地「文件即流」的思路一致，只是把文件路径换成对象名）：
 *
 *   rtsp://host/s3vod/<对象名>
 *   rtsp://host/s3vod/record/live/cam01/2026-11-05/2026-11-05-10-00-00-0.mp4
 *
 * 解析规则：app 固定为 **s3vod**，其余整段（可以带 `/`）都是对象名 ——
 * ZLM 的 MediaInfo 只把第一段当 app、后面全归 stream，所以对象名里的斜杠天然可用。
 *
 * 为什么走 app 前缀而不是复用 `record`：`record` 已经是本地 mp4 点播的应用名，
 * 两者混在一起时「同一路径到底读本地还是读云」会变成隐式行为，排障很不友好。
 *
 * 行为：第一次有人播放 → HEAD 对象拿大小 → 建媒体源 → 定时器按需 Range 读；
 * 所有观看者都走人后由 MediaSourceEvent 回收（与本地 mp4 点播一致）。
 */
class S3Vod {
public:
    /// 云点播的应用名（URL 第一段）
    static const char *APP_NAME;

    /**
     * 尝试为一次「流不存在」的播放请求创建云点播源。
     * @return 成功返回媒体源；app 不匹配 / 未启用云存储 / 对象不存在都返回 nullptr
     */
    static MediaSource::Ptr createFromS3(const std::string &schema, const std::string &vhost,
                                        const std::string &app, const std::string &stream);
};

} // namespace mediakit

#endif // ZLMEDIAKIT_S3_VOD_H
