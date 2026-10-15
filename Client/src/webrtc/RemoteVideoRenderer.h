//
// 远端视频渲染：接收侧，VideoTrackSource 的镜像。
//
//   发送侧:  QVideoFrame ──转换──→ webrtc::VideoFrame → 编码器
//   接收侧:  解码器 → webrtc::VideoFrame ──转换──→ QVideoFrame → QVideoSink
//
// 需要适配器的原因：WebRTC 的 VideoSinkInterface 与 Qt 的 QVideoSink 是两套不同的
// 接口，且 WebRTC 提供的是 I420 平面数据（带行距填充），Qt 的 QVideoFrame 需要自身的
// 格式描述，转换只能在此完成。
//
// 线程：帧由 WebRTC 信令线程到达，而 QVideoSink 归属 GUI 线程，故 OnFrame 中只入队，
// 转换与投递均在 GUI 线程执行。
//

#ifndef SYNCINE_REMOTEVIDEORENDERER_H
#define SYNCINE_REMOTEVIDEORENDERER_H

#include <mutex>
#include <optional>

#include <QObject>

#include "api/media_stream_interface.h"
#include "api/video/video_frame.h"
#include "api/video/video_sink_interface.h"

class QVideoSink;

class RemoteVideoRenderer : public QObject,
                            public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
    Q_OBJECT

public:
    explicit RemoteVideoRenderer(QObject *parent = nullptr);
    ~RemoteVideoRenderer() override;

    // 画面往哪个 sink 送。由 main.cpp 装配时设成
    // PlaybackController 的 remoteVideoSink。
    void setTargetSink(QVideoSink *sink);

    // 是否往 sink 写帧。由 main.cpp 装配时设成
    void setWritingEnabled(bool enabled);

    // ---- VideoSinkInterface ----
    // 在 WebRTC 信令线程被调用
    void OnFrame(const webrtc::VideoFrame &frame) override;
    void OnDiscardedFrame() override;

    int frameCount() const {
        return m_frameCount;
    }

private:
    // 真正干活的地方，在 GUI 线程执行（取出暂存的最新帧并渲染）
    void deliverPendingFrame();

    int m_frameCount = 0;
    int m_discardedCount = 0;

    // ---- 帧暂存区：只保留"最新的一帧" ----
    std::mutex m_frameMutex;
    std::optional<webrtc::VideoFrame> m_pendingFrame;

    // 是否已经有一个投递任务在排队 —— 避免对同一帧重复排队
    bool m_deliveryPosted = false;

    // 是否往 sink 写帧)。由 setWritingEnabled 切换。
    bool m_writingEnabled = true;

    // QVideoSink 的生命周期由 PlaybackController 掌管，这里只借用
    QVideoSink *m_targetSink = nullptr;
};

#endif //SYNCINE_REMOTEVIDEORENDERER_H
