#include "TestHarness.h"

#include "server/Server.h"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;

namespace {

void waitUntil(const std::function<bool()> &predicate,
               std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw test::Failure("condition timed out");
        std::this_thread::sleep_for(5ms);
    }
}

ServerOptions testOptions(unsigned short port) {
    ServerOptions options;
    options.address = "127.0.0.1";
    options.port = port;
    options.logicThreads = 2;
    options.handshakeTimeout = 100ms;
    options.heartbeatInterval = 50ms;
    options.heartbeatTimeout = 100ms;
    options.acceptBackoffInitial = 10ms;
    options.acceptBackoffMax = 50ms;
    return options;
}

class RunningServer {
public:
    explicit RunningServer(ServerOptions options)
        : m_options(std::move(options)),
          m_server(m_ioc, m_options) {
        CHECK(m_server.start());
        m_thread = std::thread([this]() { m_ioc.run(); });
    }

    ~RunningServer() {
        stop();
    }

    unsigned short port() const {
        return m_server.port();
    }

    std::size_t connectionCount() const {
        return m_server.connectionCount();
    }

    void stop() {
        if (m_stopped.exchange(true))
            return;
        m_ioc.stop();
        if (m_thread.joinable())
            m_thread.join();
        m_server.stop();
    }

private:
    ServerOptions m_options;
    asio::io_context m_ioc;
    Server m_server;
    std::thread m_thread;
    std::atomic<bool> m_stopped{false};
};

class WebSocketClient {
public:
    explicit WebSocketClient(unsigned short port)
        : m_ws(m_ioc) {
        tcp::resolver resolver(m_ioc);
        const auto endpoints = resolver.resolve("127.0.0.1", std::to_string(port));
        asio::connect(m_ws.next_layer(), endpoints);
        m_ws.handshake("127.0.0.1:" + std::to_string(port), "/");

        const auto welcome = readJson();
        CHECK(welcome.at("type") == "welcome");
    }

    ~WebSocketClient() {
        abort();
    }

    void sendJson(const nlohmann::json &message) {
        m_ws.write(asio::buffer(message.dump()));
    }

    nlohmann::json readJson() {
        m_buffer.consume(m_buffer.size());
        m_ws.read(m_buffer);
        return nlohmann::json::parse(beast::buffers_to_string(m_buffer.data()));
    }

    void abort() {
        beast::error_code ec;
        m_ws.next_layer().close(ec);
    }

private:
    asio::io_context m_ioc;
    websocket::stream<tcp::socket> m_ws;
    beast::flat_buffer m_buffer;
};

class AsyncWebSocketClient {
public:
    explicit AsyncWebSocketClient(unsigned short port)
        : m_ws(m_ioc),
          m_resolver(m_ioc),
          m_deadline(m_ioc) {
        m_deadline.expires_after(2s);
        m_deadline.async_wait([this](beast::error_code ec) {
            if (!ec) {
                m_failed.store(true);
                abortTransport();
            }
        });

        m_resolver.async_resolve(
            "127.0.0.1", std::to_string(port),
            [this](beast::error_code ec,
                   tcp::resolver::results_type results) {
                if (ec) {
                    fail();
                    return;
                }
                asio::async_connect(
                    m_ws.next_layer(), results,
                    [this](beast::error_code connectError, const tcp::endpoint &) {
                        if (connectError) {
                            fail();
                            return;
                        }
                        m_ws.async_handshake(
                            "127.0.0.1", "/",
                            [this](beast::error_code handshakeError) {
                                if (handshakeError) {
                                    fail();
                                    return;
                                }
                                m_connected.store(true);
                                doRead();
                            });
                    });
            });

        m_thread = std::thread([this]() { m_ioc.run(); });
    }

    ~AsyncWebSocketClient() {
        stop();
    }

    bool connected() const {
        return m_connected.load();
    }

    bool failed() const {
        return m_failed.load();
    }

    void stop() {
        if (m_stopped.exchange(true))
            return;
        m_ioc.stop();
        if (m_thread.joinable())
            m_thread.join();
        abortTransport();
    }

private:
    void doRead() {
        m_ws.async_read(
            m_buffer,
            [this](beast::error_code ec, std::size_t bytes) {
                if (ec) {
                    if (ec != websocket::error::closed
                        && ec != asio::error::operation_aborted) {
                        m_failed.store(true);
                    }
                    return;
                }
                m_buffer.consume(bytes);
                doRead();
            });
    }

    void fail() {
        m_failed.store(true);
        abortTransport();
    }

    void abortTransport() {
        beast::error_code ec;
        m_ws.next_layer().close(ec);
    }

    asio::io_context m_ioc;
    websocket::stream<tcp::socket> m_ws;
    tcp::resolver m_resolver;
    asio::steady_timer m_deadline;
    beast::flat_buffer m_buffer;
    std::thread m_thread;
    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_failed{false};
    std::atomic<bool> m_stopped{false};
};

bool waitForSocketClose(tcp::socket &socket,
                        std::chrono::milliseconds timeout = 1s) {
    socket.non_blocking(true);
    char byte = 0;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        beast::error_code ec;
        socket.read_some(asio::buffer(&byte, 1), ec);
        if (!ec)
            return false;
        if (ec != asio::error::would_block
            && ec != asio::error::try_again) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return false;
}

} // namespace

TEST_CASE(handshake_timeout_releases_connection_slot) {
    auto options = testOptions(0);
    options.maxConnections = 1;
    RunningServer server(options);

    asio::io_context ioc;
    tcp::socket socket(ioc);
    tcp::resolver resolver(ioc);
    asio::connect(socket, resolver.resolve(
                              "127.0.0.1", std::to_string(server.port())));

    CHECK(waitForSocketClose(socket));
    waitUntil([&]() { return server.connectionCount() == 0; });

    WebSocketClient replacement(server.port());
}

TEST_CASE(connection_limit_pauses_and_resumes_accept) {
    auto options = testOptions(0);
    options.maxConnections = 1;
    options.heartbeatInterval = 0ms;
    options.heartbeatTimeout = 0ms;
    RunningServer server(options);

    WebSocketClient first(server.port());
    waitUntil([&]() { return server.connectionCount() == 1; });

    AsyncWebSocketClient second(server.port());
    std::this_thread::sleep_for(200ms);
    CHECK(!second.connected());

    first.abort();
    waitUntil([&]() { return second.connected(); });
    CHECK(!second.failed());
    second.stop();
}

TEST_CASE(messages_from_one_connection_are_processed_in_order) {
    auto options = testOptions(0);
    options.logicThreads = 8;
    RunningServer server(options);

    WebSocketClient first(server.port());
    first.sendJson({
        {"type", "create_room"},
        {"roomId", "654321"},
        {"roomName", "ordered"},
        {"nickname", "first"},
        {"mode", "local"},
    });
    first.sendJson({
        {"type", "leave_room"},
    });

    std::this_thread::sleep_for(100ms);

    WebSocketClient second(server.port());
    second.sendJson({
        {"type", "create_room"},
        {"roomId", "654321"},
        {"roomName", "replacement"},
        {"nickname", "second"},
        {"mode", "local"},
    });

    const auto response = second.readJson();
    CHECK(response.at("type") == "room_created");
}

TEST_CASE(disconnect_removes_completed_room) {
    auto options = testOptions(0);
    RunningServer server(options);

    WebSocketClient first(server.port());
    first.sendJson({
        {"type", "create_room"},
        {"roomId", "112233"},
        {"roomName", "cleanup"},
        {"nickname", "first"},
        {"mode", "local"},
    });
    CHECK(first.readJson().at("type") == "room_created");
    first.abort();

    waitUntil([&]() { return server.connectionCount() == 0; });
    std::this_thread::sleep_for(100ms);

    WebSocketClient second(server.port());
    second.sendJson({
        {"type", "create_room"},
        {"roomId", "112233"},
        {"roomName", "after-cleanup"},
        {"nickname", "second"},
        {"mode", "local"},
    });
    CHECK(second.readJson().at("type") == "room_created");
}

TEST_CASE(disconnect_discards_queued_room_creation) {
    auto options = testOptions(0);
    RunningServer server(options);

    WebSocketClient first(server.port());
    first.sendJson({
        {"type", "create_room"},
        {"roomId", "445566"},
        {"roomName", "queued-cleanup"},
        {"nickname", "first"},
        {"mode", "local"},
    });
    first.abort();

    waitUntil([&]() { return server.connectionCount() == 0; });
    std::this_thread::sleep_for(100ms);

    WebSocketClient second(server.port());
    second.sendJson({
        {"type", "create_room"},
        {"roomId", "445566"},
        {"roomName", "after-queued-cleanup"},
        {"nickname", "second"},
        {"mode", "local"},
    });
    CHECK(second.readJson().at("type") == "room_created");
}

TEST_CASE(missing_pong_closes_connection) {
    auto options = testOptions(0);
    RunningServer server(options);

    WebSocketClient client(server.port());
    waitUntil([&]() { return server.connectionCount() == 1; });
    waitUntil([&]() { return server.connectionCount() == 0; }, 1s);
}

TEST_CASE(pong_responses_keep_connection_alive) {
    auto options = testOptions(0);
    RunningServer server(options);

    AsyncWebSocketClient client(server.port());
    waitUntil([&]() { return client.connected(); });
    std::this_thread::sleep_for(250ms);

    CHECK(!client.failed());
    CHECK(server.connectionCount() == 1);
    client.stop();
}
