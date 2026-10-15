#include "Server.h"
#include "Config.h"
#include "Log.h"
#include "TurnCredentials.h"

int main() {
    // 日志最先打开：后续所有对象构造时产生的日志才能落进文件。
    // 默认输出到工作目录下的 logs/，级别用 SYNCINE_LOG_LEVEL 调整
    // （trace/debug/info/warn/error），目录用 SYNCINE_LOG_DIR 覆盖。
    Log::init("server");

    // 先拉起 TURN 凭据线程。首次兑换是异步的（一次 TLS 握手加一个来回），
    // 启动越早，第一个建房/进房的客户端越可能拿到真实凭据，否则只能先给 STUN 兜底。
    TurnCredentials::getInstance().start();

    int exitCode = 0;
    {
        asio::io_context ioc;
        Server server(ioc, Config::SERVER_ADDRESS, Config::SERVER_PORT);
        if (!server.start()) {
            // 监听没建立起来（多半是端口被占）。**必须退出**：旧代码在这里
            // 继续跑 accept 循环，结果是在没绑上的套接字上自旋，单核 100%。
            exitCode = 1;
        } else {
            ioc.run();
        }
        // Server 与 io_context 在内层作用域结束时就析构，它们的收尾日志
        // （"服务停止"）才写得到日志文件 —— 放到 Log::shutdown() 之后
        // 析构的话，那几行只会在控制台出现。
    }

    // 先停凭据线程（内部 join），再关闭日志；顺序颠倒会导致该线程的收尾
    // 日志写进已关闭的文件。
    TurnCredentials::getInstance().stop();

    Log::shutdown();
    return exitCode;
}
