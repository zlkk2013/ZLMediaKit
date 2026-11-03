/*
 * AscendEnv.h —— CANN/ACL 初始化的一次性守卫（VENC / VDEC 共用）
 *
 * 为什么单独一个文件：
 *   `aclInit()` / `hi_mpi_sys_init()` 在**同一进程内只能成功一次**，重复调用会返回失败。
 *   硬编（AscendVenc）和硬解（AscendVdec）都要用这套初始化，如果各自写一份，
 *   第二个被打开的模块必然初始化失败 —— 之前 VENC 里踩过（available() 探测过之后再 open 就失败）。
 *   所以初始化链收口在这里，双方都用 refcount 的 acquire/release。
 *
 * 初始化链（顺序不能乱，缺一步就报 0xA0088015 / 0xA0058015）：
 *   aclInit → aclrtSetDevice → aclrtCreateContext → aclrtSetCurrentContext → hi_mpi_sys_init
 *
 * 线程约束：ACL context 是**按线程生效**的，调用方在自己的工作线程里必须
 *   先 `aclrtSetCurrentContext(ascendEnv().ctx)` 再调 hi_mpi_* 接口。
 *   acquire() 会为调用线程绑一次，但别的线程不会自动继承。
 */
#ifndef MEDIAKIT_ASCEND_ENV_H
#define MEDIAKIT_ASCEND_ENV_H

#include <string>

namespace mediakit {

struct DvppEnv {
    /// 引用计数 +1；已初始化过则只为当前线程重绑 context。
    /// 失败时 why 给出具体卡在哪一步（探测/日志用）。
    bool acquire(int device, const char *&why);

    /// 引用计数 -1。刻意不做 aclrtDestroyContext / aclFinalize：
    /// 真机上这些动作有崩溃风险，且后续模块还要复用同一套初始化。
    void release();

    /// 是否已成功初始化（用于廉价探测）
    bool ready() const { return inited; }

    // 下面这些字段是给调用方拼 hi_mpi_* 参数用的，与 acquire() 的返回值配套
    int device = -1;
    void *ctx = nullptr;
    bool inited = false;
};

/// 进程内唯一的 DVPP 运行环境
DvppEnv &ascendEnv();

} // namespace mediakit

#endif // MEDIAKIT_ASCEND_ENV_H
