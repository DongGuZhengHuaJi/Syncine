#include "Server.h"
#include "Config.h"
#include "Log.h"
#include "TurnCredentials.h"

int main() {
    // 日志最先打开:后面每个对象构造时打的日志都要落进文件。
    // 默认输出到工作目录下的 logs/,
    // 级别用 SYNCINE_LOG_LEVEL 调(trace/debug/info/warn/error),
    // 目录用 SYNCINE_LOG_DIR 覆盖。
    Log::init("server");

    // TURN 凭据那条线程先拉起来。第一次兑换是异步的(一次 TLS 握手 + 一个来回),
    // 越早开始,第一个建房/进房的人越可能拿到真凭据 —— 晚了就只能先给 STUN 兜底。
    TurnCredentials::getInstance().start();

    asio::io_context ioc;
    Server server(ioc, Config::SERVER_ADDRESS, Config::SERVER_PORT);
    server.start();
    ioc.run();

    // ioc.run() 返回 = 要收摊了。先停凭据线程(内部 join),再关日志:
    // 反过来的话,那条线程的收尾日志会写进已经关掉的文件。
    TurnCredentials::getInstance().stop();

    Log::shutdown();
    return 0;
}
