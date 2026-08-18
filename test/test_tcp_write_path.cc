#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "coder/tinypb_coder.h"
#include "coder/tinypb_protocol.h"
#include "config.h"
#include "eventloop.h"
#include "log.h"
#include "tcp/net_addr.h"
#include "tcp/tcp_buffer.h"
#include "tcp/tcp_connection.h"

namespace {

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
    int writer() const { return m_fds[0]; }
    int reader() const { return m_fds[1]; }

private:
    int m_fds[2]{-1, -1};
};

bool require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "tcp write-path check failed: " << message << '\n';
        return false;
    }
    return true;
}

std::shared_ptr<talon::TinyPBProtocol> makeMessage(size_t body_size) {
    auto message = std::make_shared<talon::TinyPBProtocol>();
    message->m_msg_id = "stage3-write-path";
    message->m_method_name = "Stage3.write";
    message->m_pb_data.resize(body_size);
    for (size_t i = 0; i < body_size; ++i) {
        message->m_pb_data[i] = static_cast<char>('A' + (i % 23));
    }
    return message;
}

std::vector<char> encodeMessage(
    const std::shared_ptr<talon::TinyPBProtocol>& message) {
    talon::TinyPBCoder coder;
    auto buffer = std::make_shared<talon::TcpBuffer>(128);
    std::vector<talon::AbstractProtocol::s_ptr> messages{message};
    coder.encode(messages, buffer);
    return std::vector<char>(buffer->m_buffer.begin(),
                             buffer->m_buffer.begin() + buffer->writeIndex());
}

bool readAvailable(int fd, std::vector<char>& output) {
    char buffer[16384];
    while (true) {
        ssize_t bytes = recv(fd, buffer, sizeof(buffer), 0);
        if (bytes > 0) {
            output.insert(output.end(), buffer, buffer + bytes);
            continue;
        }
        if (bytes < 0 && errno == EINTR) {
            continue;
        }
        return bytes == 0 || (bytes < 0 &&
                              (errno == EAGAIN || errno == EWOULDBLOCK));
    }
}

bool hasReadableData(int fd) {
    pollfd descriptor{fd, POLLIN, 0};
    int result = poll(&descriptor, 1, 100);
    return result > 0 && (descriptor.revents & POLLIN) != 0;
}

std::unique_ptr<talon::TcpConnection> makeConnection(int fd) {
    auto address = std::make_shared<talon::IPNetAddr>("127.0.0.1", 1);
    auto connection = std::make_unique<talon::TcpConnection>(
        talon::Eventloop::GetCurrentEventLoop(), fd, 128, address, address,
        talon::TcpConnectionByServer);
    connection->setState(talon::Connected);
    return connection;
}

void queueMessage(talon::TcpConnection& connection,
                  const std::shared_ptr<talon::TinyPBProtocol>& message) {
    std::vector<talon::AbstractProtocol::s_ptr> messages{message};
    connection.reply(messages);
}

bool testCompleteWrite() {
    SocketPair sockets;
    if (!require(sockets.valid(), "socketpair creation failed")) {
        return false;
    }

    auto message = makeMessage(32);
    const std::vector<char> expected = encodeMessage(message);
    auto connection = makeConnection(sockets.writer());
    queueMessage(*connection, message);

    connection->onWrite();
    std::vector<char> first_receive;
    if (!require(readAvailable(sockets.reader(), first_receive),
                 "failed to read the first transmission") ||
        !require(first_receive == expected,
                 "complete write did not preserve the encoded packet")) {
        return false;
    }

    connection->onWrite();
    return require(!hasReadableData(sockets.reader()),
                   "a repeated writable callback resent completed bytes");
}

bool testPartialWrite() {
    SocketPair sockets;
    if (!require(sockets.valid(), "socketpair creation failed")) {
        return false;
    }

    int send_buffer_size = 4096;
    if (!require(setsockopt(sockets.writer(), SOL_SOCKET, SO_SNDBUF,
                            &send_buffer_size, sizeof(send_buffer_size)) == 0,
                 "could not reduce socket send-buffer size")) {
        return false;
    }

    auto message = makeMessage(256 * 1024);
    const std::vector<char> expected = encodeMessage(message);
    auto connection = makeConnection(sockets.writer());
    queueMessage(*connection, message);

    connection->onWrite();
    std::vector<char> received;
    if (!require(readAvailable(sockets.reader(), received),
                 "failed to read the first partial transmission") ||
        !require(!received.empty() && received.size() < expected.size(),
                 "kernel did not produce a deterministic partial write") ||
        !require(std::equal(received.begin(), received.end(), expected.begin()),
                 "first partial transmission was not the payload prefix")) {
        return false;
    }

    for (int attempt = 0; attempt < 1024 && received.size() < expected.size();
         ++attempt) {
        connection->onWrite();
        if (!require(readAvailable(sockets.reader(), received),
                     "failed while draining a partial transmission") ||
            !require(received.size() <= expected.size(),
                     "partial writes transmitted duplicate bytes") ||
            !require(std::equal(received.begin(), received.end(),
                                expected.begin()),
                     "partial writes changed byte ordering")) {
            return false;
        }
    }

    if (!require(received == expected,
                 "partial writes did not deliver the complete payload")) {
        return false;
    }

    connection->onWrite();
    return require(!hasReadableData(sockets.reader()),
                   "post-completion callback duplicated partial-write data");
}

bool fillSendBuffer(int fd, size_t& bytes_written) {
    const std::string filler(4096, '#');
    bytes_written = 0;
    for (int attempt = 0; attempt < 10000; ++attempt) {
        ssize_t result = send(fd, filler.data(), filler.size(), MSG_NOSIGNAL);
        if (result > 0) {
            bytes_written += static_cast<size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        return result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
    }
    return false;
}

bool testEagain() {
    SocketPair sockets;
    if (!require(sockets.valid(), "socketpair creation failed")) {
        return false;
    }

    int send_buffer_size = 4096;
    setsockopt(sockets.writer(), SOL_SOCKET, SO_SNDBUF, &send_buffer_size,
               sizeof(send_buffer_size));
    size_t filler_size = 0;
    if (!require(fillSendBuffer(sockets.writer(), filler_size),
                 "socket send buffer did not reach EAGAIN")) {
        return false;
    }

    auto message = makeMessage(32);
    const std::vector<char> expected = encodeMessage(message);
    auto connection = makeConnection(sockets.writer());
    queueMessage(*connection, message);
    connection->onWrite();

    std::vector<char> filler;
    if (!require(readAvailable(sockets.reader(), filler),
                 "failed to drain send-buffer filler") ||
        !require(filler.size() == filler_size,
                 "EAGAIN write added or removed application bytes") ||
        !require(std::all_of(filler.begin(), filler.end(),
                             [](char value) { return value == '#'; }),
                 "application data was consumed during EAGAIN")) {
        return false;
    }

    connection->onWrite();
    std::vector<char> received;
    return require(readAvailable(sockets.reader(), received),
                   "failed to read data after EAGAIN retry") &&
           require(received == expected,
                   "EAGAIN retry did not preserve queued application data");
}

}  // namespace

int main(int argc, char** argv) {
    talon::Config::SetGlobalConfig(nullptr);
    talon::Logger::InitGlobalLogger(0);

    if (argc != 2) {
        std::cerr << "usage: tcp_write_path_test <complete|partial|eagain>\n";
        return 2;
    }

    const std::string scenario = argv[1];
    if (scenario == "complete") {
        return testCompleteWrite() ? 0 : 1;
    }
    if (scenario == "partial") {
        return testPartialWrite() ? 0 : 1;
    }
    if (scenario == "eagain") {
        return testEagain() ? 0 : 1;
    }

    std::cerr << "unknown tcp write-path scenario: " << scenario << '\n';
    return 2;
}
