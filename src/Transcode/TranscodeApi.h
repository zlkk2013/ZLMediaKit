/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef ZLMEDIAKIT_TRANSCODE_API_H
#define ZLMEDIAKIT_TRANSCODE_API_H

#include "Common/macros.h"

#if defined(ENABLE_FFMPEG)

#include <map>
#include <string>
#include "Common/Parser.h"
#include "json/json.h"

namespace mediakit {

/** addTranscode / getTranscode 的入参（url + body + header 已合并，key 不区分大小写） */
using TranscodeApiArgs = std::map<std::string, std::string, StrCaseCompare>;

/**
 * /index/api/addTranscode —— 为指定流设置/覆盖转码参数。
 *
 * 返回值自带 `code` / `msg`（取值与 server 层 `API::ApiErr` 一致），不抛异常；
 * 因此 server/WebApi.cpp 里只需要一行 `val = addTranscodeApi(args)`。
 *
 * 覆盖只存进程内存，重启回到 ini 配置，返回体的 `persistent=false` 明示这一点。
 *
 * Returns the full response body including `code`/`msg` (same values as the
 * server's `API::ApiErr`) and never throws, so the registration hook in
 * WebApi.cpp stays a one-liner. Overrides are memory-only; the body reports
 * `persistent=false`.
 */
Json::Value addTranscodeApi(const TranscodeApiArgs &args);

/**
 * /index/api/getTranscode —— 查询指定流当前的转码配置与实测状态（含逐轨明细）。
 * 同样以 `code` 表达「流不存在」，不抛异常。
 */
Json::Value getTranscodeApi(const TranscodeApiArgs &args);

}//namespace mediakit
#endif//ENABLE_FFMPEG
#endif//ZLMEDIAKIT_TRANSCODE_API_H
