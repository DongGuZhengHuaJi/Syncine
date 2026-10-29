//
// 只为让内存探针链接 PlaybackController.cpp 而存在。
//
// 探针不涉及远端渲染（setRemoteRenderer 从未调用，m_webrtcManager 恒为空），
// 这两个方法不会被执行；但 PlaybackController.cpp 里的调用点需要符号，
// 而链接真实的 WebrtcManager.cpp 会把整份 libwebrtc.a 拖进来。
//

#include "webrtc/WebrtcManager.h"

void WebrtcManager::setRemoteVideoSink(QVideoSink *)
{
}

void WebrtcManager::setRemoteVideoEnabled(bool)
{
}
