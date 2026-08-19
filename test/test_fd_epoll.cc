#include <dirent.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <condition_variable>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "config.h"
#include "eventloop.h"
#include "fd_event.h"
#include "log.h"
#include "tcp/net_addr.h"
#include "tcp/tcp_connection.h"

namespace {

bool require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "fd/epoll check failed: " << message << '\n';
        return false;
    }
    return true;
}

class SocketPair {
public:
    SocketPair() {
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, m_fds) != 0) {
            return;
        }
        for (int fd : m_fds) {
            int flags = fcntl(fd, F_GETFL, 0);
            if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
                close(m_fds[0]);
                close(m_fds[1]);
                m_fds[0] = -1;
                m_fds[1] = -1;
                return;
            }
        }
    }

    SocketPair(const SocketPair&) = delete;
    SocketPair& operator=(const SocketPair&) = delete;

    ~SocketPair() {
        for (int fd : m_fds) {
            if (fd >= 0) {
                close(fd);
            }
        }
    }

    bool valid() const { return m_fds[0] >= 0 && m_fds[1] >= 0; }
    int local() const { return m_fds[0]; }
    int peer() const { return m_fds[1]; }
    int releaseLocal() {
        int fd = m_fds[0];
        m_fds[0] = -1;
        return fd;
    }
    int releasePeer() {
        int fd = m_fds[1];
        m_fds[1] = -1;
        return fd;
    }

private:
    int m_fds[2]{-1, -1};
};

std::unique_ptr<talon::TcpConnection> makeConnection(int fd) {
    auto address = std::make_shared<talon::IPNetAddr>("127.0.0.1", 1);
    auto connection = std::make_unique<talon::TcpConnection>(
        talon::Eventloop::GetCurrentEventLoop(), fd, 128, address, address,
        talon::TcpConnectionByServer);
    connection->setState(talon::Connected);
    return connection;
}

bool isClosed(int fd) {
    errno = 0;
    return fcntl(fd, F_GETFD) == -1 && errno == EBADF;
}

int openFdCount() {
    DIR* directory = opendir("/proc/self/fd");
    if (directory == nullptr) {
        return -1;
    }

    int count = 0;
    while (dirent* entry = readdir(directory)) {
        if (entry->d_name[0] != '.') {
            ++count;
        }
    }
    closedir(directory);
    return count;
}

bool testClose() {
    SocketPair sockets;
    if (!require(sockets.valid(), "socketpair creation failed")) {
        return false;
    }

    int owned_fd = sockets.releaseLocal();
    auto connection = makeConnection(owned_fd);
    connection->clear();

    char byte = 0;
    ssize_t received = recv(sockets.peer(), &byte, sizeof(byte), 0);
    return require(isClosed(owned_fd),
                   "fd still valid after final connection cleanup") &&
           require(received == 0,
                   "peer did not observe EOF after connection cleanup");
}

bool testIdempotent() {
    SocketPair sockets;
    if (!require(sockets.valid(), "socketpair creation failed")) {
        return false;
    }

    int old_fd = sockets.releaseLocal();
    auto connection = makeConnection(old_fd);
    connection->clear();
    if (!require(isClosed(old_fd), "first cleanup did not close the fd")) {
        return false;
    }

    SocketPair replacement;
    if (!require(replacement.valid(), "replacement socketpair creation failed")) {
        return false;
    }
    connection->clear();

    return require(!isClosed(replacement.local()),
                   "repeated cleanup closed an unrelated replacement fd");
}

bool testRepeat() {
    talon::Eventloop* loop = talon::Eventloop::GetCurrentEventLoop();
    (void)loop;
    const int before = openFdCount();
    if (!require(before >= 0, "could not inspect /proc/self/fd")) {
        return false;
    }
    for (int iteration = 0; iteration < 100; ++iteration) {
        SocketPair sockets;
        if (!require(sockets.valid(), "socketpair creation failed during repeat")) {
            return false;
        }
        int owned_fd = sockets.releaseLocal();
        auto connection = makeConnection(owned_fd);
        connection->clear();
        if (!require(isClosed(owned_fd),
                     "repeated lifecycle left an owned fd open")) {
            return false;
        }
    }
    const int after = openFdCount();
    return require(after == before,
                   "repeated lifecycle changed the open fd count");
}

struct LoopResult {
    int read_count{0};
    int write_count{0};
    int error_count{0};
    std::string received;
    bool timed_out{false};
};

LoopResult runEventScenario(
    int watched_fd, const std::function<void(talon::Eventloop*, talon::Fd_Event*,
                                               LoopResult*)>& configure,
    const std::function<void()>& trigger) {
    LoopResult result;
    std::mutex mutex;
    std::condition_variable condition;
    bool ready = false;
    bool finished = false;
    talon::Eventloop* loop = nullptr;

    std::thread worker([&]() {
        talon::Eventloop local_event_loop;
        auto* local_loop = &local_event_loop;
        auto event = std::make_unique<talon::Fd_Event>(watched_fd);
        loop = local_loop;
        configure(local_loop, event.get(), &result);
        local_loop->addEpollEvent(event.get());
        {
            std::lock_guard<std::mutex> lock(mutex);
            ready = true;
        }
        condition.notify_one();
        local_loop->loop();
        {
            std::lock_guard<std::mutex> lock(mutex);
            finished = true;
        }
        condition.notify_one();
    });

    {
        std::unique_lock<std::mutex> lock(mutex);
        if (!condition.wait_for(lock, std::chrono::milliseconds(1000),
                               [&]() { return ready; })) {
            result.timed_out = true;
        }
    }
    if (!result.timed_out) {
        trigger();
    }
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (!condition.wait_for(lock, std::chrono::milliseconds(1000),
                               [&]() { return finished; })) {
            result.timed_out = true;
        }
    }
    if (!finished && loop != nullptr) {
        loop->stop();
    }
    worker.join();
    return result;
}

bool testErrorDispatch() {
    int pipe_fds[2] = {-1, -1};
    if (!require(pipe(pipe_fds) == 0, "pipe creation failed")) {
        return false;
    }
    close(pipe_fds[0]);
    LoopResult result = runEventScenario(
        pipe_fds[1],
        [](talon::Eventloop* loop, talon::Fd_Event* event, LoopResult* result) {
            event->listen(
                talon::Fd_Event::OUT_EVENT,
                [loop, result]() {
                    ++result->write_count;
                    loop->stop();
                },
                [loop, result]() {
                    ++result->error_count;
                    loop->stop();
                });
        },
        []() {});
    close(pipe_fds[1]);
    return require(!result.timed_out, "EPOLLERR scenario timed out") &&
           require(result.error_count > 0,
                   "EPOLLERR did not invoke the error callback") &&
           require(result.write_count == 0,
                   "EPOLLERR incorrectly invoked the write callback") &&
           require(result.read_count == 0,
                   "EPOLLERR incorrectly invoked the read callback");
}

bool testHup() {
    SocketPair sockets;
    if (!require(sockets.valid(), "socketpair creation failed")) {
        return false;
    }
    int peer_fd = sockets.releasePeer();
    LoopResult result = runEventScenario(
        sockets.local(),
        [](talon::Eventloop* loop, talon::Fd_Event* event, LoopResult* result) {
            event->listen(
                talon::Fd_Event::IN_EVENT,
                [loop, result]() {
                    ++result->read_count;
                },
                [loop, result]() {
                    ++result->error_count;
                    loop->stop();
                });
        },
        [peer_fd]() { close(peer_fd); });
    return require(!result.timed_out, "EPOLLHUP scenario timed out") &&
           require(result.error_count > 0,
                   "EPOLLHUP did not reach the terminal callback");
}

bool testRdhup(bool with_data) {
    SocketPair sockets;
    if (!require(sockets.valid(), "socketpair creation failed")) {
        return false;
    }
    const std::string payload = "abc";
    if (with_data &&
        !require(send(sockets.peer(), payload.data(), payload.size(), 0) ==
                     static_cast<ssize_t>(payload.size()),
                 "failed to send pre-RDHUP payload")) {
        return false;
    }

    int peer_fd = sockets.peer();
    const int local_fd = sockets.local();
    LoopResult result = runEventScenario(
        sockets.local(),
        [local_fd](talon::Eventloop* loop, talon::Fd_Event* event,
                   LoopResult* result) {
            event->listen(
                talon::Fd_Event::IN_EVENT,
                [result, local_fd]() {
                    ++result->read_count;
                    char buffer[32];
                    while (true) {
                        ssize_t bytes = recv(local_fd, buffer, sizeof(buffer), 0);
                        if (bytes > 0) {
                            result->received.append(buffer,
                                                    static_cast<size_t>(bytes));
                            continue;
                        }
                        break;
                    }
                },
                [loop, result]() {
                    ++result->error_count;
                    loop->stop();
                });
        },
        [peer_fd]() {
            shutdown(peer_fd, SHUT_WR);
        });
    return require(!result.timed_out, "EPOLLRDHUP scenario timed out") &&
           require(result.error_count > 0,
                   "EPOLLRDHUP did not reach the terminal callback") &&
           (!with_data || require(result.received == payload,
                                  "IN plus RDHUP lost readable data"));
}

}  // namespace

int main(int argc, char** argv) {
    talon::Config::SetGlobalConfig(nullptr);
    talon::Config::GetGlobalConfig()->m_log_level = "ERROR";
    talon::Logger::InitGlobalLogger(0);

    if (argc != 2) {
        std::cerr << "usage: fd_epoll_test <close|idempotent|repeat|error_dispatch|hup|rdhup|rdhup_with_data>\n";
        return 2;
    }

    const std::string scenario = argv[1];
    if (scenario == "close") return testClose() ? 0 : 1;
    if (scenario == "idempotent") return testIdempotent() ? 0 : 1;
    if (scenario == "repeat") return testRepeat() ? 0 : 1;
    if (scenario == "error_dispatch") return testErrorDispatch() ? 0 : 1;
    if (scenario == "hup") return testHup() ? 0 : 1;
    if (scenario == "rdhup") return testRdhup(false) ? 0 : 1;
    if (scenario == "rdhup_with_data") return testRdhup(true) ? 0 : 1;

    std::cerr << "unknown fd/epoll scenario: " << scenario << '\n';
    return 2;
}
