# 文档索引

## 交付给别人看（现场/集成方/领导）

| 文档 | 内容 | 谁看 |
|---|---|---|
| `DELIVERY_CONFIG_REFERENCE.md` | 全部配置键、API 参数、错误码、可观测性矩阵、已知边界 | 集成方、运维 |
| `DELIVERY_OPS_RUNBOOK.md` | 现象→查什么→原因→处理速查表、现场 8 步验收、长跑定性 | 现场运维 |
| `DELIVERY_DEPLOY.md` | 依赖、构建、systemd、硬件能力自查、交付边界、许可证 | 部署实施 |

## 开发与设计记录

| 文档 | 内容 |
|---|---|
| `TRANSCODE_PLAN.md` / `TRANSCODE_PLAN_PHASE2.md` | 最初的分阶段计划 |
| `TRANSCODE_PHASE3_INTEGRATION.md` / `TRANSCODE_PHASE3_P8.md` | 第一/二阶段接入与 API 化的设计与验收 |
| `TRANSCODE_API_STATUS.md` | API 与状态接口设计（回显、applied 语义） |
| `TRANSCODE_VIDEO_PARAMS.md` | 分辨率/帧率/码率参数与缩放 |
| `TRANSCODE_CODEC_MATRIX.md` | 支持的源→目标 codec 矩阵与投递约束 |
| `TRANSCODE_AUDIO_ROBUSTNESS.md` | 音频健壮性（分帧、静音兜底、SDP 时钟） |
| `TRANSCODE_AUDIO_FRAMING_FIX.md` | 6.4 倍放大缺陷的定位过程与九轮否定的结案记录 |
| `TRANSCODE_HW_ENCODE.md` | 硬件编码（VideoToolbox/NVENC，回落与 warmup） |
| `TRANSCODE_HW_DECODE.md` | 硬件解码（hwaccel 路线与「不省 CPU」的实测） |
| `TRANSCODE_MULTI_GPU.md` | 多卡分配与平台能力差异 |
| `TRANSCODE_SOAK.md` | 长跑判据与「池扩张 vs 泄漏」的定性方法 |
| `MAINTAINABILITY_UPSTREAM_DIFF.md` | 上游改动量逐文件必要性（rebase 可维护性） |
| `TRANSCODE_NVIDIA_VERIFICATION.md` | NVIDIA 真机（RTX 5060 Ti / WSL2）验证明细、修掉的 4 个缺陷、容量口径问题、WSL 环境坑 |

## 验收脚本（`scripts/`）

一键入口：`./scripts/final_regression.sh`（15 套，约 30 分钟；`--fast` / `--list` / `<名字>`）

| 脚本 | 覆盖 | 断言数 |
|---|---|---|
| `camera_scenario_test.py` | 现场拓扑：RTSP 推流 → 转码 → 观众拉流 | 15 |
| `rtsp_reconnect_test.py` | 断连续推（不再 406）、转码仍生效 | 18 |
| `codec_reverse_test.py` | 反向方向（H265→H264、G711A/MP3 作源） | 23 |
| `audio_matrix_test.py` | 5 种源 × 5 目标 codec 采样率矩阵 | 125 |
| `scale_param_test.py` | 分辨率倍数 / 显式宽高 | 11 |
| `filter_test.py` | 滤镜链、OSD、logo 角标 | 19 |
| `adapt_fps_test.py` | 过载自适应降帧 | 13 |
| `hw_encode_test.py` | 硬件编解码（日志/正确性/CPU/可观测） | 21 |
| `multi_gpu_test.py` | 多卡参数面与平台诚实性 | 10 |
| `api_params_test.py` | API 参数解析/拒绝/行为级 | 18 |
| `joint_accept.py` | 音视频联合验收 | 38 |
| `source_release_check.py` | 媒体源释放（引用环） | 6 |
| `teardown_liveness_test.py` | 析构存活性（自死锁） | 20 |
| `vp8_source_timeline_test.py` | VP8/VP9 作源的时间轴 | — |
| `av1_fmp4_stability.py` | AV1+fmp4 偶发崩溃复现 | — |
| `soak_test.py` | 长跑稳定性（默认 12 分钟，可传分钟数） | 7 |
| `multi_gpu_test.py` | 多卡参数面 + 按实际卡数分配（有 NVENC 时验真分配） | 10 |
| `capacity_test.py` | 容量（逐档加路，拉流端实测帧率；**口径仍在完善，数字暂不对外**） | — |
| `_platshim.py` | 平台差异收口（macOS/Linux/WSL 的进程、句柄、GPU 探测） | — |

素材由 `scripts/test_assets.py` 幂等生成（一秒钟一个关键帧，否则中途接入的用例会假红）。

## 最近一次全量结果

15 套全部通过，337 项断言 / 0 失败 / 30 分 41 秒
（日志：`logs/regression/20261101_203258/`）。
