/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifdef ENABLE_MP4
#include <ctime>
#include <sys/stat.h>
#include <chrono>

#include "Util/File.h"
#include "S3/S3Config.h"
#include "S3/S3Storage.h"
#include "Common/config.h"
#include "MP4Recorder.h"
#include "Thread/WorkThreadPool.h"
#include "MP4Muxer.h"

using namespace std;
using namespace toolkit;

namespace mediakit {

MP4Recorder::MP4Recorder(const MediaTuple &tuple, const string &path, size_t max_second) {
    // ///record 业务逻辑//////  [AUTO-TRANSLATED:2e78931a]
    // ///record Business Logic//////
    static_cast<MediaTuple &>(_info) = tuple;
    _info.folder = path;
    GET_CONFIG(uint32_t, s_max_second, Protocol::kMP4MaxSecond);
    _max_second = max_second ? max_second : s_max_second;
}

MP4Recorder::~MP4Recorder() {
    try {
        flush();
        closeFile();
    } catch (std::exception &ex) {
        WarnL << ex.what();
    }
}

void MP4Recorder::createFile() {
    closeFile();
    auto date = getTimeStr("%Y-%m-%d");
    auto file_name = date + "-" + getTimeStr("%H-%M-%S") + "-" + std::to_string(_file_index++) + ".mp4";
    auto full_path = _info.folder + date + "/" + file_name;
    auto full_path_tmp = _info.folder + date + "/." + file_name;

    // ///record 业务逻辑//////  [AUTO-TRANSLATED:2e78931a]
    // ///record Business Logic//////
    _info.start_time = ::time(NULL);
    _info.file_name = file_name;
    _info.file_path = full_path;
    GET_CONFIG(string, appName, Record::kAppName);
    _info.url = appName + "/" + _info.app + "/" + _info.stream + "/" + date + "/" + file_name;

    try {
        _muxer = std::make_shared<MP4Muxer>();
        if (S3Storage::enabledForMp4()) {
            // 云存储：整段录在内存里（MP4 需要 seek 回填 moov/mdat 大小），
            // 关闭时一次上传——全程不产生任何磁盘文件。
            _s3_key = S3Storage::makeKey(_info.app, _info.stream, date, file_name);
            _s3_file = std::make_shared<MP4FileMemory>();
            _muxer->openMP4(_s3_file);
            _full_path_tmp.clear();
            InfoL << "录制写云存储: " << _s3_key;
        } else {
            TraceL << "Open tmp mp4 file: " << full_path_tmp;
            _muxer->openMP4(full_path_tmp);
            _full_path_tmp = full_path_tmp;
        }
        for (auto &track :_tracks) {
            // 添加track  [AUTO-TRANSLATED:80ae762a]
            // Add track
            _muxer->addTrack(track);
        }
    } catch (std::exception &ex) {
        WarnL << ex.what();
    }
}

void MP4Recorder::asyncClose() {
    auto muxer = _muxer;
    auto full_path_tmp = _full_path_tmp;
    auto info = _info;
    auto s3_file = _s3_file;
    auto s3_key = _s3_key;
    _s3_file = nullptr;
    _s3_key.clear();
    TraceL << "Start close tmp mp4 file: " << full_path_tmp;
    WorkThreadPool::Instance().getExecutor()->async([muxer, full_path_tmp, info, s3_file,
                                                     s3_key]() mutable {
        info.time_len = muxer->getDuration() / 1000.0f;
        if (s3_file) {
            // 云存储路径：关闭 muxer（把 moov 写完）→ 整块内存上传 → 触发录制事件
            muxer->closeMP4();
            std::string data = s3_file->getAndClearMemory();
            if (data.size() < 1024) {
                WarnL << "云存储录制片段过小，丢弃: " << s3_key << " size=" << data.size();
                return;
            }
            std::string err;
            auto t0 = std::chrono::steady_clock::now();
            if (!S3Storage::instance().upload(s3_key, data, err)) {
                // 存储故障不能让录制线程崩：只记错误，这一段报失败
                WarnL << "云存储上传失败: " << s3_key << " err=" << err;
                return;
            }
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - t0).count();
            info.file_size = data.size();
            auto opt = S3ConfigManager::instance().get();
            info.file_path = "s3://" + opt.bucket + "/" + s3_key;
            InfoL << "云存储录制完成: " << info.file_path << " size=" << data.size()
                  << " 用时=" << ms << "ms";
            NOTICE_EMIT(BroadcastRecordMP4Args, Broadcast::kBroadcastRecordMP4, info);
            return;
        }
        // 关闭mp4可能非常耗时，所以要放在后台线程执行  [AUTO-TRANSLATED:a7378a11]
        // Closing mp4 can be very time-consuming, so it should be executed in the background thread
        TraceL << "Closing tmp mp4 file: " << full_path_tmp;
        muxer->closeMP4();
        TraceL << "Closed tmp mp4 file: " << full_path_tmp;
        if (!full_path_tmp.empty()) {
            // 获取文件大小  [AUTO-TRANSLATED:7b90eb41]
            // Get file size
            info.file_size = File::fileSize(full_path_tmp);
            if (info.file_size < 1024) {
                // 录像文件太小，删除之  [AUTO-TRANSLATED:923d27c3]
                // The recording file is too small, delete it
                File::delete_file(full_path_tmp);
                return;
            }
            // 临时文件名改成正式文件名，防止mp4未完成时被访问  [AUTO-TRANSLATED:541a6f00]
            // Change the temporary file name to the official file name to prevent access to the mp4 before it is completed
            rename(full_path_tmp.data(), info.file_path.data());
        }
        TraceL << "Emit mp4 record event: " << info.file_path;
        // 触发mp4录制切片生成事件  [AUTO-TRANSLATED:9959dcd4]
        // Trigger mp4 recording slice generation event
        NOTICE_EMIT(BroadcastRecordMP4Args, Broadcast::kBroadcastRecordMP4, info);
    });
}

void MP4Recorder::closeFile() {
    if (_muxer) {
        asyncClose();
        _muxer = nullptr;
    }
}

void MP4Recorder::flush() {
    if (_muxer) {
        _muxer->flush();
    }
}

bool MP4Recorder::inputFrame(const Frame::Ptr &frame) {
    auto stamp_inc = _delta_stamp[frame->getTrackType()].relativeStamp(frame->pts(), false);
    if (!_muxer || (stamp_inc > int64_t(_max_second) * 1000 && (!_have_video || frame->keyFrame()))) {
        // 成立条件  [AUTO-TRANSLATED:8c9c6083]
        // Conditions for establishment
        // 1、_muxer为空  [AUTO-TRANSLATED:fa236097]
        // 1. _muxer is empty
        // 2、到了切片时间，并且只有音频  [AUTO-TRANSLATED:212e9d23]
        // 2. It's time to slice, and there is only audio
        // 3、到了切片时间，有视频并且遇到视频的关键帧  [AUTO-TRANSLATED:fa4a71ad]
        // 3. It's time to slice, there is video and a video keyframe is encountered
        createFile();
        for (auto &ref : _delta_stamp) {
            ref.reset();
        }
    }

    if (_muxer) {
        // 生成mp4文件  [AUTO-TRANSLATED:76a8d77c]
        // Generate mp4 file
        return _muxer->inputFrame(frame);
    }
    return false;
}

bool MP4Recorder::addTrack(const Track::Ptr &track) {
    // 保存所有的track，为创建MP4MuxerFile做准备  [AUTO-TRANSLATED:815c2486]
    // Save all tracks in preparation for creating MP4MuxerFile
    _tracks.emplace_back(track);
    if (track->getTrackType() == TrackVideo) {
        _have_video = true;
    }
    return true;
}

void MP4Recorder::resetTracks() {
    closeFile();
    _tracks.clear();
    _have_video = false;
}

} /* namespace mediakit */

#endif //ENABLE_MP4
