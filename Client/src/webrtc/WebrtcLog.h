//
// 把 WebRTC 自己的日志（RTC_LOG）接进应用日志文件。
//
// 为什么必须有这一步：WebRTC 的内部故障几乎都是**静默**的 —— 声卡放音起不来、
// IAudioClient::Initialize() 失败、InitSpeaker() 失败，都只在 RTC_LOG 里留一行，
// 且默认写 stderr。Windows 上双击启动根本没有控制台，这些行等于不存在。
// 本项目已经踩过一次：远端语音播到了 Steam 虚拟声卡上，全程没有一行报错。
//
// 与 SYNCINE_WEBRTC_LOG 的关系：
//   未设置   → 只接管警告及以上（内部故障都在这一级），文件里多出的行很少
//   设置了   → 连同 INFO 一起接管，并保留原有的 stderr 输出，供现场排障
// 级别过滤交给 Log 那一层做（见 Log::write），此处不做二次过滤。
//

#ifndef SYNCINE_WEBRTCLOG_H
#define SYNCINE_WEBRTCLOG_H

namespace WebrtcLog {

void install(bool verbose);

} // namespace WebrtcLog

#endif //SYNCINE_WEBRTCLOG_H
