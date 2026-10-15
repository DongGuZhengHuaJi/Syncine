//
// Created by donggu on 2026/9/8.
//

#include "Session.h"
#include "Config.h"
#include "Log.h"
#include "RandomGen.h"
#include "LogicSystem.h"

#include <nlohmann/json.hpp>

Session::Session(asio::ip::tcp::socket &&socket)
    : m_id(RandomGen::generateUUID()) {
    m_ws = std::make_unique<websocket::stream<asio::ip::tcp::socket>>(std::move(socket));
}

Session::~Session() {
    stop();
}

void Session::start() {
    // 每条连接都会记录，故用 Debug；房间级进出由 LogicSystem 以 Info 记录
    LOG_DEBUG("Session") << "会话开始: clientId=" << m_id;

    // 限制单条消息大小。Beast 默认 16 MB，而本协议任何合法消息都远小于此
    // （最大字段是 title / url）；超限的消息会被当成协议错误断连，不必先
    // 把它整个读进内存再解析。
    m_ws->read_message_max(Config::MAX_MESSAGE_BYTES);

    // 先完成 WebSocket 握手，再发 welcome 并开始收消息
    m_ws->async_accept([this, self = shared_from_this()](beast::error_code ec) {
        if (ec) {
            LOG_WARN("Session") << "WebSocket 握手失败: clientId=" << m_id
                                << " " << ec.message();
            onDisconnected();
            return;
        }

        nlohmann::json welcome;
        welcome["type"] = "welcome";
        welcome["clientId"] = m_id;
        send(welcome.dump());

        do_read();
    });
}

void Session::stop() {
    // 关闭连接；挂起的 async_read 会以错误码完成，在那里触发 onDisconnected
    beast::error_code ec;
    m_ws->close(websocket::close_code::normal, ec);
    if (ec) {
        LOG_DEBUG("Session") << "关闭连接返回错误（通常为对端已断开）: clientId=" << m_id
                             << " " << ec.message();
    }
}

std::string Session::id() const {
    return m_id;
}

void Session::setOnClose(std::function<void(std::shared_ptr<Session>)> callback) {
    m_onClose = std::move(callback);
}

void Session::send(const std::string &message) {
    // 写入必须串行；全部投递到 io_context 线程执行，
    // 这样 worker 线程调用 send 也不会与 async_write 并发
    auto self = shared_from_this();
    asio::post(m_ws->get_executor(), [this, self, message]() {
        m_writeQueue.push_back(message);
        if (m_writing)
            return;
        m_writing = true;
        do_next_write();
    });
}

void Session::do_next_write() {
    if (m_writeQueue.empty()) {
        m_writing = false;
        return;
    }

    // async_write 不拷贝载荷：它把调用方的缓冲区直接交给底层 sendmsg，
    // 所以缓冲区必须活到完成回调为止。对端读得慢（发送缓冲写满）时写操作会
    // 挂起、函数早已返回 —— 若缓冲区是这里的局部变量，那一刻它已经析构，
    // 内核恢复写入时读的就是已释放的堆内存（ASan 实测 heap-use-after-free；
    // 不开 ASan 则表现为把堆上残留数据写给对端，即内存信息泄漏）。
    //
    // 所以先把数据移进 shared_ptr，再由该 shared_ptr 建 buffer、并捕获进
    // lambda。注意不能写成把 message 移进 lambda 再 asio::buffer(message) ——
    // 那样 buffer 指向的是已被移空的 string。
    auto payload = std::make_shared<std::string>(std::move(m_writeQueue.front()));
    m_writeQueue.pop_front();

    auto self = shared_from_this();
    m_ws->async_write(asio::buffer(*payload),
                      [this, self, payload](beast::error_code ec, std::size_t) {
        if (ec) {
            LOG_WARN("Session") << "写入失败: clientId=" << m_id << " " << ec.message();
            onDisconnected();
            return;
        }
        do_next_write();
    });
}

void Session::do_read() {
    auto self = shared_from_this();
    m_ws->async_read(m_buffer,
                     [this, self](beast::error_code ec, std::size_t bytes) {
        if (ec) {
            // 对端正常关闭、EOF、主动取消都不算异常
            if (ec != websocket::error::closed
                && ec != asio::error::eof
                && ec != asio::error::operation_aborted) {
                LOG_WARN("Session") << "读取失败: clientId=" << m_id << " " << ec.message();
            }
            onDisconnected();
            return;
        }

        // 交给逻辑线程处理
        LogicSystem::getInstance().enqueueMessage(
            self, beast::buffers_to_string(m_buffer.data()));

        // 清空缓冲区，继续读下一条
        m_buffer.consume(bytes);
        do_read();
    });
}

void Session::onDisconnected() {
    if (m_closed)
        return;
    m_closed = true;
    LOG_DEBUG("Session") << "会话结束: clientId=" << m_id
                         << " 未发送消息=" << m_writeQueue.size();
    if (m_onClose)
        m_onClose(shared_from_this());
}
