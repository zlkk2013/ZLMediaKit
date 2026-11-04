/*
 * AscendVdec.h —— 昇腾 310P DVPP 硬解码封装（H.264 / H.265）
 *
 * 设计目标与 AscendVenc 一致：
 *   1. 对外只暴露「喂 Annex-B 码流 → 拿到 NV12 帧」，调用方不必看见 CANN 头文件；
 *   2. 与 ZLMediaKit 解耦，可独立验证（tools/ascend/ascend_vdec_selftest.cpp）；
 *   3. 编译上完全可选（ENABLE_ASCEND_VENC 开关，默认 OFF）。
 *
 * 线程模型：
 *   - 全同步。decode() 在调用者线程里送包，并在同一线程里把已就绪的帧取出来回调；
 *   - 回调发生在 decode() 的调用线程上（ZLM 侧解码任务本身就跑在 decoder thread）；
 *   - 每次调用都会重新绑定 ACL context（context 按线程生效）。
 *
 * 实测约束（改代码前先读）：
 *   - 送进去的必须是 **设备内存**：host 缓冲要 aclrtMemcpy(H2D) 过去，不能直接给 host 指针；
 *   - 一帧必须**从帧起点开始**发送（首帧要带 SPS/PPS/SEI），否则解出来 width/height=0；
 *   - 取回的 `v_frame.virt_addr[i]` 是设备地址，要 aclrtMemcpy(D2H) 拷回 host；
 *   - 校验 frame_buf_size 用 `hi_vdec_get_pic_buf_size()`，它返回的是**尺寸不是错误码**
 *     （1080p 约 0x305800 字节，别当错误码处理）。
 */
#ifndef MEDIAKIT_ASCEND_VDEC_H
#define MEDIAKIT_ASCEND_VDEC_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace mediakit {

class AscendVdec {
public:
    using Ptr = std::shared_ptr<AscendVdec>;

    /// 解码帧回调：data 是**紧凑排列的 NV12**（stride==width，Y 后紧跟 UV），只在该回调内有效
    using OnFrame = std::function<void(const uint8_t *nv12, int width, int height, uint32_t pts_ms)>;

    struct Config {
        bool h265 = false;
        int width = 0;   ///< 码流最大宽度（用于分配缓冲，可给上界）
        int height = 0;  ///< 码流最大高度
        int device = 0;
        int channel = 0;      ///< VDEC 通道号：**必须用 acquireChannel() 拿**
        int ref_frames = 8;   ///< 参考帧数，[0,16]
        int display_frames = 2;
    };

    /// 本机是否具备可用 VDEC（会真正走一次初始化，结果不缓存）
    static bool available(int device = 0);

    /// VDEC 通道池：号空间 32，用完必须归还，否则后续流申请不到号
    /// （通道号在 **VDEC 侧是整机共享**的，与 VENC 的按进程隔离不同，见文档）
    static int acquireChannel();
    static void releaseChannel(int channel);

    ~AscendVdec();
    AscendVdec(); // 定义放 cpp：构造期异常清理需要完整类型

    /// 打开解码通道。失败时 err 带驱动返回码。
    bool open(const Config &cfg, OnFrame cb, std::string &err);

    /// 送一帧 Annex-B ES，并把当前已就绪的帧全部取出来回调
    bool decode(const uint8_t *annexb, size_t len, uint32_t pts_ms);

    /// 送 EOS 并排空剩余帧
    void flush();

    void close();

    uint32_t inFrames() const { return _in; }
    uint32_t outFrames() const { return _out; }
    /// 取到句柄但拿不到有效图像的次数（宽高为 0 / 平面地址为空）。运维判据：该值持续增长=解码异常
    uint32_t droppedFrames() const;
    /// 占用的 DVPP VDEC 通道号（-1 = 未打开）
    int channel() const;
    bool opened() const { return _opened; }

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
    bool _opened = false;
    uint32_t _in = 0;
    uint32_t _out = 0;
};

} // namespace mediakit

#endif // MEDIAKIT_ASCEND_VDEC_H
