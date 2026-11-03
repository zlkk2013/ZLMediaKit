/*
 * AscendVenc.h —— 昇腾 310P DVPP 硬编码封装（H.264 / H.265）
 *
 * 设计目标：
 *   1. 对外只暴露「喂 NV12 帧 → 拿到 Annex-B 码流」两件事，调用方不需要看见任何 CANN 头文件；
 *   2. 与 ZLMediaKit 的 Transcoder 解耦：先作为独立编码后端验证，再接进转码链路；
 *   3. 编译上完全可选（CMake 里默认关闭），不开时对上游构建零影响。
 *
 * 线程模型：
 *   - encode() 可以在调用者线程里同步调用（内部有锁 + 失败退避重投）；
 *   - 内部起一个取流线程持续 hi_mpi_venc_get_stream，拿到码流后回调 on_packet。
 *     回调发生**在取流线程**上，调用方需自行保证线程安全（ZLM 侧已有输出锁）。
 *
 * 实测约束（真机踩出来的，改代码前先读）：
 *   - v_frame.time_ref 必须是偶数，否则奇数帧被驱动判非法（0xA0088003）；
 *   - pts 必须严格递增；
 *   - user_rc_info.frame_type 必须显式设 VENC_FRAME_TYPE_NONE，留 0 一帧都不收；
 *   - 设备内存不能 host 直写，进出都要 aclrtMemcpy。
 */
#ifndef MEDIAKIT_ASCEND_VENC_H
#define MEDIAKIT_ASCEND_VENC_H

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace mediakit {

class AscendVenc {
public:
    using Ptr = std::shared_ptr<AscendVenc>;

    /// 码流回调：data 指向 Annex-B 码流（回调返回后失效），key 表示是否为关键帧
    using OnPacket = std::function<void(const char *data, size_t len, uint64_t pts_us, bool key)>;

    struct Config {
        int width = 0;
        int height = 0;
        bool h265 = false;
        int fps = 25;
        int bitrate_kbps = 0; ///< 0 = 按分辨率自动估
        int gop = 0;          ///< 0 = 1 秒一个 I 帧
        int device = 0;       ///< NPU 设备号
        int channel = 0;      ///< VENC 通道号（整机共享号空间，多路要错开）
        int bufs = 8;         ///< 设备帧缓冲个数（轮换，避免驱动持有期冲突）
    };

    /// 本机是否具备可用 VENC（初始化一次，结果缓存）
    static bool available(int device = 0);

    /// 按分辨率给默认码率（kbps），与 ffmpeg 硬编路径保持同一套口径
    static int defaultBitrateKbps(int width, int height);

    ~AscendVenc();
    AscendVenc(); // 定义放 cpp：构造函数的异常清理会实例化 unique_ptr<Impl> 析构，需要完整类型

    /// 打开编码通道。失败时 err 带具体原因（含驱动返回码）。
    bool open(const Config &cfg, OnPacket cb, std::string &err);
    void close();

    /// 喂一帧 NV12（width*height*3/2）。pts_us 必须严格递增。
    bool encode(const uint8_t *nv12, uint64_t pts_us);

    /// 排空编码器内部残余帧（不销毁通道）
    void flush();

    bool opened() const { return _opened; }
    uint32_t sentFrames() const { return _sent; }
    uint32_t outFrames() const { return _out; }
    uint32_t sendRetries() const { return _retries; }

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
    bool _opened = false;
    std::atomic<uint32_t> _sent{0};
    std::atomic<uint32_t> _out{0};
    std::atomic<uint32_t> _retries{0};
};

} // namespace mediakit

#endif // MEDIAKIT_ASCEND_VENC_H
