//
// 系统声音采集的平台工厂。
//

#include "SystemAudioCapture.h"

#if defined(Q_OS_LINUX) && defined(SYNCINE_HAVE_PULSE)
#include "PulseMonitorCapture.h"
#elif defined(Q_OS_WIN)
#include "WasapiLoopbackCapture.h"
#endif

bool SystemAudioCapture::isSupported()
{
#if (defined(Q_OS_LINUX) && defined(SYNCINE_HAVE_PULSE)) || defined(Q_OS_WIN)
    return true;
#else
    return false;
#endif
}

std::unique_ptr<SystemAudioCapture> SystemAudioCapture::create()
{
#if defined(Q_OS_LINUX) && defined(SYNCINE_HAVE_PULSE)
    return std::make_unique<PulseMonitorCapture>();
#elif defined(Q_OS_WIN)
    return std::make_unique<WasapiLoopbackCapture>();
#else
    return nullptr;
#endif
}
