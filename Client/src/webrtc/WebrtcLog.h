//
// 把 WebRTC 自己的日志（RTC_LOG）接进应用日志文件。
//
// 为什么必须有这一步：WebRTC 的内部故障几乎都是**静默**的 —— 声卡放音起不来、
// IAudioClient::Initialize() 失败、InitSpeaker() 失败，都只在 RTC_LOG 里留一行，
// 且默认写 stderr。Windows 上双击启动根本没有控制台，这些行等于不存在。
// 本项目已经踩过一次：远端语音播到了 Steam 虚拟声卡上，全程没有一行报错。
//
// 所有 WebRTC 消息统一记为 Trace：默认 info 级别完全不输出，不刷日志。
// 排查时把日志级别调到 trace（SYNCINE_LOG_LEVEL=trace 或设置页）即可看到。
// SYNCINE_WEBRTC_LOG=1 只决定是否连 WebRTC 的 VERBOSE 级也接进来。
// 此处不再按消息内容做过滤，避免漏掉意外错误。
//

#ifndef SYNCINE_WEBRTCLOG_H
#define SYNCINE_WEBRTCLOG_H

namespace WebrtcLog {

void install(bool verbose);

} // namespace WebrtcLog

#endif //SYNCINE_WEBRTCLOG_H
