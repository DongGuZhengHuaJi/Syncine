#include "Server.h"
#include "Config.h"
#include "Log.h"

int main() {
    // 日志最先打开:后面每个对象构造时打的日志都要落进文件。
    // 默认输出到工作目录下的 logs/,
    // 级别用 SYNCINE_LOG_LEVEL 调(trace/debug/info/warn/error),
    // 目录用 SYNCINE_LOG_DIR 覆盖。
    Log::init("server");

    asio::io_context ioc;
    Server server(ioc, Config::SERVER_ADDRESS, Config::SERVER_PORT);
    server.start();
    ioc.run();

    Log::shutdown();
    return 0;
}
