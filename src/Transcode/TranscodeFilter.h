/**
 * 单输入滤镜图（libavfilter 封装）。
 *
 * 用途：转码链里「解码 → 缩放 → **滤镜** → 编码」，支持任意单输入滤镜描述串，
 * 例如 `drawtext=...`（OSD 文字）、`negate`、`hflip`、`boxblur` 等。
 *
 * 为什么要自己做一层而不是直接用 avfilter API：
 *   - 滤镜图的初始化（buffersrc args 拼装、buffersink 取帧、flush、内存回收）容易写错，
 *     集中到一处以后，编码器那边只看到 filter()/flush() 两个调用；
 *   - 尺寸/像素格式必须与编码器一致，配置错了在 init 阶段就报错，而不是等 send_frame
 *     返回 EINVAL（那个报错信息看不出是滤镜导致尺寸变了）。
 *
 * 约束：
 *   - **滤镜的输出尺寸/像素格式必须与输入一致**（本类只服务「不改变编码器参数」的场景）。
 *     需要改尺寸请用转码的 video_scale / video_width，不要用 crop 之类的滤镜 ——
 *     否则编码器配置的宽高对不上，avcodec_send_frame 会返回 EINVAL。
 *   - 只支持单输入滤镜：`overlay`（需要第二路输入，如 logo 图片）暂不支持。
 *
 * Single-input filter graph wrapper. Output size/pixel format must match the input,
 * because the encoder was configured with those parameters; use video_scale or
 * video_width for resizing. Multi-input filters (overlay with a logo image) are not
 * supported yet.
 */
#pragma once

#include <memory>
#include <string>

struct AVFrame;
struct AVFilterGraph;
struct AVFilterContext;
struct AVPixFmtDescriptor;

namespace mediakit {

class TranscodeFilter {
public:
    using Ptr = std::shared_ptr<TranscodeFilter>;

    ~TranscodeFilter();

    /**
     * 建图。
     * @param desc 滤镜描述串，如 "drawtext=text='CAM-01':x=10:y=10:fontsize=24:fontcolor=white"
     * @param width/height/fmt 输入帧的尺寸与像素格式（必须与编码器一致）
     * @param time_base 输入帧的时间基（用编码器的，保证 pts 连续）
     * @return 成功返回 true；失败会打 ErrorL 并给出可读原因
     */
    bool init(const std::string &desc, int width, int height, int fmt, int num, int den);

    /**
     * 设置 logo 角标（需要在 init 之前调用）。
     *
     * 用独立配置项而不是让用户在滤镜串里写 `[1:v]overlay`，是因为滤镜串里的多输入
     * 语法要求第二路来自用户自己的输入，而我们的第二路固定是「解码出来的一张图片」，
     * 交给调用方拼字符串只会制造困惑。
     *
     * @param path 图片路径（png/jpg 等 ffmpeg 能解的格式）
     * @param x/y 叠加位置；x<0 表示「距右边缘 |x| 像素」，y<0 同理距下边缘
     * @param width 缩放后的 logo 宽度，0 = 用原始尺寸
     *
     * Logo watermark. Configured via dedicated keys instead of `[1:v]overlay` inside the
     * filter string: the second input here is always a decoded image, so asking callers
     * to compose the multi-input syntax would only confuse. Negative x/y mean
     * "distance from the right/bottom edge".
     */
    void setLogo(const std::string &path, int x, int y, int width) {
        _logo_path = path;
        _logo_x = x;
        _logo_y = y;
        _logo_width = width;
    }

    /**
     * 喂一帧。
     * @return 1 产出一帧（*out 有效，调用方用 av_frame_free 释放）
     *         0 还需要更多输入（*out 置空）
     *        -1 出错（调用方应关闭滤镜、按原样直通，别让整条流挂掉）
     */
    int filter(AVFrame *in, AVFrame **out);

    /** 冲刷：取出滤镜内部缓存的帧。返回 1 有帧 / 0 结束 / -1 出错 */
    int flush(AVFrame **out);

private:
    void close();

    bool initLogoGraph(int width, int height, int fmt, int num, int den);
    AVFrame *loadLogo(int width, int height, int fmt);

    AVFilterGraph *_graph = nullptr;
    AVFilterContext *_src = nullptr;
    AVFilterContext *_sink = nullptr;
    /** logo 输入（仅当配置了 logo 时存在） */
    AVFilterContext *_logo_src = nullptr;
    /** 常驻的 logo 帧（每帧复制一份喂给滤镜图） */
    AVFrame *_logo_frame = nullptr;
    std::string _logo_path;
    int _logo_x = 10;
    int _logo_y = 10;
    int _logo_width = 0;
    std::string _desc;
    int _width = 0;
    int _height = 0;
    bool _failed = false;
};

} // namespace mediakit
