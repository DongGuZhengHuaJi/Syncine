#include <iostream>
#include "Server.h"
#include "Config.h"

int main() {
    asio::io_context ioc;
    Server server(ioc, Config::SERVER_ADDRESS, Config::SERVER_PORT);
    server.start();
    ioc.run();
    return 0;
}
