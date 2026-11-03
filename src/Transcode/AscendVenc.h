/*
 * AscendVenc.h —— 昇腾 310P DVPP 硬编码封装（H.264 / H.265）
 *
 * 设计目标：
 *   1. 对外只暴露「喂 NV12 帧 → 拿到 Annex-B 码流」两件事，调用方不需要看见任何 CANN 头文件；
 *   2. 与 ZLMediaKit 的 Transcoder 解耦：先作为独立编码后端验证，再接进转码链路；
 *   3. 编译上完全可选（CMake 里默认关闭），不开时对上游构建零影响。
 *
 * 线程模型（重要，别改成后台取流线程）：
 *   - 全部同步：encode() 在调用者线程里送帧，送完**就在同一线程里排空码流**并回调 on_packet；
 *   - 早期版本用独立取流线程，实测在 ZLM 进程内 hi_mpi_venc_get_stream 恒返回 BUF_EMPTY
 *     （同一线程同步取流却正常），且与内联排空并发访问同一通道会段错误 —— 故已删除该线程；
 *   - 回调发生在 encode() 的调用线程上，调用方需保证自身线程安全（ZLM 侧已有输出锁）；
 *   - 多流并发时 encode() 内部有锁，同一实例不会并发进入，但**每次调用都会重新绑定
 *     ACL context**（context 是按线程生效的）。
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
        int channel = 0;      ///< VENC 通道号：**必须用 acquireChannel() 拿**，别自己填
        int bufs = 8;         ///< 设备帧缓冲个数（轮换，避免驱动持有期冲突）
    };

    /// 本机是否具备可用 VENC（初始化一次，结果缓存）
    static bool available(int device = 0);

    /// 按分辨率给默认码率（kbps），与 ffmpeg 硬编路径保持同一套口径
    static int defaultBitrateKbps(int width, int height);

    /// 申请/归还 DVPP VENC 通道（池上限 16）。
    /// 必须「用完归还」：早期用 `计数器 % 16` 分配，第 17 路会复用通道 0/1… 把正在跑的流
    /// 打停（实测 18 路时原有两路视频在 1.8 秒后断流，音频直通却正常，极易误判）。
    /// 通道号是**按进程隔离**的（两个进程各用通道 0 互不影响，实测各自 100/100 帧），
    /// 所以超过 16 路要靠多进程分片，而不是在一个进程里抢号。
    /// Acquire a DVPP VENC channel; returns -1 when all are busy.
    static int acquireChannel();
    static void releaseChannel(int channel);

    ~AscendVenc();
    AscendVenc(); // 定义放 cpp：构造函数的异常清理会实例化 unique_ptr<Impl> 析构，需要完整类型

    /// 打开编码通道。失败时 err 带具体原因（含驱动返回码）。
    bool open(const Config &cfg, OnPacket cb, std::string &err);
    void close();

    /// 喂一帧 NV12（width*height*3/2），并在同一线程里把编好的码流排空回调出去。
    /// pts_us 必须严格递增（按 1/fps 递增，不要用墙上时钟，否则时间轴会被压缩）。
    /// 返回 false 表示这一帧最终没送进编码器（会重试若干次后放弃）。
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
