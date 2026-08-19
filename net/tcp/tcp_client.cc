//
// Created for KaiRPC on 23-10-3.
//

#include "tcp_client.h"

#include <utility>

#include "cstring"
#include "err_code.h"
#include "eventloop.h"
#include "fd_event_group.h"
#include "log.h"
#include "memory"
#include "sys/socket.h"
#include "unistd.h"
namespace talon{
    TcpClient::TcpClient(const NetAddr::s_ptr& peer_addr) : m_peer_addr(peer_addr) {
        m_event_loop = Eventloop::GetCurrentEventLoop();
        if(m_event_loop->m_stop_flag){
            /* 修复了在同一*/
            m_event_loop->m_stop_flag = false;
            m_event_loop->m_is_looping = false;
        }
        m_fd = socket(peer_addr->getFamily(), SOCK_STREAM, 0);

        if (m_fd < 0) {
            ERRORLOG("TcpClient::TcpClient() error, failed to create fd");
            return;
        }

        m_fd_event = FdEventGroup::GetFdEventGroup()->getFdEvent(m_fd);
        m_fd_event->setNonBlock();

        m_connection = std::make_shared<TcpConnection>(m_event_loop, m_fd, 128, peer_addr, nullptr, TcpConnectionByClient);
        m_connection->setConnectionType(TcpConnectionByClient);

    }

    TcpClient::~TcpClient() {
        DEBUGLOG("TcpClient::~TcpClient()");
        if (m_connection != nullptr) {
            m_connection->clear();
        }
        m_fd = -1;
        m_fd_event = nullptr;
    }

// 异步的进行 conenct
// 如果connect 成功，done 会被执行
    void TcpClient::connect(const std::function<void()>& done) {
        int rt = ::connect(m_fd, m_peer_addr->getSockAddr(), m_peer_addr->getSockLen());
        if (rt == 0) {
            DEBUGLOG("connect [%s] sussess", m_peer_addr->toString().c_str());
            m_connection->setState(Connected);
            initLocalAddr();
            if (done) {
                done();
            }
        } else if (rt == -1) {
            if (errno == EINPROGRESS) {
                // epoll 监听可写事件，然后判断错误码
                std::weak_ptr<TcpClient> weak_self = weak_from_this();
                auto connect_callback = [weak_self, done]() {
                    auto self = weak_self.lock();
                    if (self == nullptr) {
                        return;
                    }
                    self->m_fd_event->cancle(Fd_Event::OUT_EVENT);
                    self->m_event_loop->deleteEpollEvent(self->m_fd_event);
                    int rt = ::connect(self->m_fd, self->m_peer_addr->getSockAddr(),
                                       self->m_peer_addr->getSockLen());
                    if ((rt < 0 && errno == EISCONN) || (rt == 0)) {
                        DEBUGLOG("connect [%s] sussess", self->m_peer_addr->toString().c_str());
                        self->initLocalAddr();
                        self->m_connection->setState(Connected);
                    } else {
                        if (errno == ECONNREFUSED) {
                            self->m_connect_error_code = ERROR_PEER_CLOSED;
                            self->m_connect_error_info = "connect refused, sys error = " + std::string(strerror(errno));
                        } else {
                            self->m_connect_error_code = ERROR_FAILED_CONNECT;
                            self->m_connect_error_info = "connect unkonwn error, sys error = " + std::string(strerror(errno));
                        }
                        ERRORLOG("connect errror, errno=%d, error=%s", errno,
                                 strerror(errno));
                        self->m_connection->setDisconnectCallback(nullptr);
                        self->m_connection->clear();
                        self->m_fd = -1;
                        self->m_fd_event = nullptr;
                    }

                    DEBUGLOG("now begin to done");
                    // 如果连接完成，才会执行回调函数
                    if (done) {
                        done();
                    }
                };
                m_fd_event->listen(Fd_Event::OUT_EVENT, connect_callback,
                                   connect_callback);
                m_event_loop->addEpollEvent(m_fd_event);
               if (!m_event_loop->isLooping()) {
                    m_event_loop->loop();
                }
            } else {
                ERRORLOG("connect errror, errno=%d, error=%s", errno, strerror(errno));
                m_connect_error_code = ERROR_FAILED_CONNECT;
                m_connect_error_info = "connect error, sys error = " + std::string(strerror(errno));
                if (done) {
                    done();
                }
            }
        }

    }


    void TcpClient::stop() {
        if (m_event_loop->isLooping()) {
            m_event_loop->stop();
        }
    }

// 异步的发送 message
// 如果发送 message 成功，会调用 done 函数， 函数的入参就是 message 对象
    void TcpClient::writeMessage(const AbstractProtocol::s_ptr& message, std::function<void(AbstractProtocol::s_ptr)> done) {
        // 1. 把 message 对象写入到 Connection 的 buffer, done 也写入
        // 2. 启动 connection 可写事件
        m_connection->pushSendMessage(message, std::move(done));
        m_connection->listenWrite();

    }

// 异步的读取 message
// 如果读取 message 成功，会调用 done 函数， 函数的入参就是 message 对象
    void TcpClient::readMessage(const std::string& msg_id, std::function<void(AbstractProtocol::s_ptr)> done) {
        // 1. 监听可读事件
        // 2. 从 buffer 里 decode 得到 message 对象, 判断是否 msg_id 相等，相等则读成功，执行其回调
        m_connection->pushReadMessage(msg_id, done);
        m_connection->listenRead();
    }

    void TcpClient::removeReadMessage(const std::string& msg_id) {
        if (m_connection != nullptr) {
            m_connection->removeReadMessage(msg_id);
        }
    }

    void TcpClient::setDisconnectCallback(std::function<void()> callback) {
        if (m_connection != nullptr) {
            m_connection->setDisconnectCallback(std::move(callback));
        }
    }

    void TcpClient::disconnect() {
        if (m_connection != nullptr) {
            m_connection->setDisconnectCallback(nullptr);
            m_connection->clear();
        }
        m_fd = -1;
        m_fd_event = nullptr;
    }

    int TcpClient::getConnectErrorCode() const {
        return m_connect_error_code;
    }

    std::string TcpClient::getConnectErrorInfo() {
        return m_connect_error_info;

    }

    NetAddr::s_ptr TcpClient::getPeerAddr() {
        return m_peer_addr;
    }

    NetAddr::s_ptr TcpClient::getLocalAddr() {
        return m_local_addr;
    }

    void TcpClient::initLocalAddr() {
        sockaddr_in local_addr{};
        socklen_t len = sizeof(local_addr);

        int ret = getsockname(m_fd, reinterpret_cast<sockaddr*>(&local_addr), &len);
        if (ret != 0) {
            ERRORLOG("initLocalAddr error, getsockname error. errno=%d, error=%s", errno, strerror(errno));
            return;
        }

        m_local_addr = std::make_shared<IPNetAddr>(local_addr);

    }


    void TcpClient::addTimerEvent(const TimerEvent::s_ptr& timer_event) {
        m_event_loop->addTimerEvent(timer_event);
    }

    void TcpClient::deleteTimerEvent(const TimerEvent::s_ptr& timer_event) {
        if (m_event_loop != nullptr) {
            m_event_loop->deleteTimerEvent(timer_event);
        }
    }

    void TcpClient::addTask(const std::function<void()>& task) {
        if (m_event_loop != nullptr) {
            m_event_loop->addTask(task, true);
        }
    }
}
