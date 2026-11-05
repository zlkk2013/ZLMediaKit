/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */
#include "S3/S3Vod.h"

#include "S3/MP4FileS3.h"
#include "S3/S3Client.h"
#include "S3/S3Config.h"

#include "Util/logger.h"

#if defined(ENABLE_MP4)
#include "Record/MP4Reader.h"
#endif

using namespace std;
using namespace toolkit;

namespace mediakit {

const char *S3Vod::APP_NAME = "s3vod";

#if !defined(ENABLE_MP4)
// 未编译 mp4 支持时，云点播直接不可用（明确返回失败，不做半残功能）
MediaSource::Ptr S3Vod::createFromS3(const string &, const string &, const string &, const string &) {
    return nullptr;
}
#else

MediaSource::Ptr S3Vod::createFromS3(const string &schema, const string &vhost,
                                     const string &app, const string &stream) {
    if (app != APP_NAME) {
        return nullptr;
    }
    // 云点播只对 mp4 有意义；HLS 走 /s3/<dir>/hls.m3u8 直连（切片本来就是对象）
    if (!end_with(stream, ".mp4")) {
        return nullptr;
    }

    auto cli = S3ConfigManager::instance().client();
    if (!cli) {
        WarnL << "云点播请求 " << stream << " 失败：云存储未启用";
        return nullptr;
    }

    S3Object info;
    string err;
    if (!cli->headObject(stream, info, err)) {
        // 对象不存在是正常情况（客户端拼错/已过期），不打错误日志刷屏
        InfoL << "云点播对象不存在: " << stream << " (" << err << ")";
        return nullptr;
    }

    MediaTuple tuple = {vhost, app, stream, ""};
    try {
        auto file = std::make_shared<MP4FileS3>(cli, stream, info.size);
        auto reader = std::make_shared<MP4Reader>(tuple, file);
        reader->startReadMP4();
        auto src = MediaSource::find(schema, vhost, app, stream);
        InfoL << "云点播开始: " << schema << "://" << vhost << "/" << app << "/" << stream
              << " size=" << info.size;
        return src;
    } catch (std::exception &ex) {
        WarnL << "云点播创建失败: " << stream << " err=" << ex.what();
        return nullptr;
    }
}

#endif // ENABLE_MP4

} // namespace mediakit
