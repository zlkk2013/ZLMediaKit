/*
 * AscendEnv.cpp —— CANN/ACL 初始化的一次性守卫实现（VENC / VDEC 共用）
 *
 * 详见 AscendEnv.h 顶部的说明；核心约束是 aclInit / hi_mpi_sys_init 进程内只能成功一次。
 */
#include "AscendEnv.h"

#ifdef ENABLE_ASCEND_VENC

#include <cstdio>
#include <mutex>

#include "acl/acl.h"
#include "acl/dvpp/hi_mpi_sys.h"

using namespace std;

namespace mediakit {

namespace {
mutex g_mtx;
int g_ref = 0;
} // namespace

bool DvppEnv::acquire(int dev, const char *&why) {
    lock_guard<mutex> lck(g_mtx);
    if (inited) {
        if (dev != device) {
            why = "同一进程内已用其它 device 初始化过 DVPP";
            return false;
        }
        // 已初始化：只需为当前线程重绑上下文（runtime 参数校验依赖它）
        if (aclrtSetCurrentContext((aclrtContext) ctx) != ACL_SUCCESS) {
            why = "aclrtSetCurrentContext 失败";
            return false;
        }
        g_ref++;
        return true;
    }
    // aclInit 在「进程里别人已经初始化过」时会返回失败 —— 这不是错误。
    // 同一进程里 VDEC 封装 / 推理后端都可能先调过，因此这里只当警告，继续尝试建上下文。
    if (aclInit(nullptr) != ACL_SUCCESS) {
        // 不直接失败：后面的 setDevice 才是真正的判据
    }
    aclrtContext c = nullptr;
    if (aclrtSetDevice(dev) != ACL_SUCCESS) {
        // 设备已被别的模块 set 过也可能返回失败，再看能否建上下文
        if (aclrtCreateContext(&c, dev) != ACL_SUCCESS) {
            why = "aclrtSetDevice/aclrtCreateContext 均失败";
            return false;
        }
    }
    if (!c && aclrtCreateContext(&c, dev) != ACL_SUCCESS) {
        why = "aclrtCreateContext 失败";
        return false;
    }
    if (aclrtSetCurrentContext(c) != ACL_SUCCESS) {
        why = "aclrtSetCurrentContext 失败";
        return false;
    }
    auto ret = hi_mpi_sys_init();
    if (ret != 0) {
        static thread_local char buf[64];
        snprintf(buf, sizeof(buf), "hi_mpi_sys_init 失败 0x%x", ret);
        why = buf;
        return false;
    }
    ctx = c;
    device = dev;
    g_ref = 1;
    inited = true;
    return true;
}

void DvppEnv::release() {
    lock_guard<mutex> lck(g_mtx);
    if (g_ref > 0) {
        // 仅减计数：不主动 destroy context / reset device / aclFinalize。
        // 真机上这些动作有崩溃风险，且后续编码器/解码器还要复用同一套初始化。
        --g_ref;
    }
}

DvppEnv &ascendEnv() {
    static DvppEnv e;
    return e;
}

} // namespace mediakit

#endif // ENABLE_ASCEND_VENC
