#ifndef KAIRPC_STAGE2_INTEGRATION_TEST_SUPPORT_H
#define KAIRPC_STAGE2_INTEGRATION_TEST_SUPPORT_H

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <utility>

namespace kairpc_stage2 {

class ScopedFd {
public:
    explicit ScopedFd(int fd = -1) : m_fd(fd) {}
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
    ~ScopedFd() {
        if (m_fd >= 0) {
            close(m_fd);
        }
    }

    bool valid() const { return m_fd >= 0; }

private:
    int m_fd;
};

class ChildProcess {
public:
    ChildProcess(std::string binary, std::string label)
        : m_binary(std::move(binary)),
          m_log_path("/tmp/kairpc-stage2-" + std::to_string(getpid()) +
                     "-" + std::move(label) + ".log") {}

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    ~ChildProcess() {
        terminate();
        std::remove(m_log_path.c_str());
    }

    bool start() {
        m_pid = fork();
        if (m_pid < 0) {
            std::perror("fork");
            return false;
        }
        if (m_pid == 0) {
            setpgid(0, 0);
            int log_fd = open(m_log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC,
                              0600);
            if (log_fd >= 0) {
                dup2(log_fd, STDOUT_FILENO);
                dup2(log_fd, STDERR_FILENO);
                close(log_fd);
            }
            execl(m_binary.c_str(), m_binary.c_str(),
                  static_cast<char*>(nullptr));
            std::perror("exec");
            _exit(127);
        }

        if (setpgid(m_pid, m_pid) < 0 && errno != EACCES && errno != ESRCH) {
            std::perror("setpgid");
        }
        return true;
    }

    bool running() const {
        return m_pid > 0 && (kill(m_pid, 0) == 0 || errno == EPERM);
    }

    bool waitForExit(int timeout_ms, int* status = nullptr) {
        if (m_pid <= 0) {
            if (status != nullptr) {
                *status = m_status;
            }
            return true;
        }

        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeout_ms);
        while (true) {
            int observed_status = 0;
            pid_t result = waitpid(m_pid, &observed_status, WNOHANG);
            if (result == m_pid) {
                m_status = observed_status;
                m_pid = -1;
                if (status != nullptr) {
                    *status = m_status;
                }
                return true;
            }
            if (result < 0 && errno != EINTR) {
                if (errno == ECHILD) {
                    m_pid = -1;
                    return true;
                }
                return false;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    void terminate() {
        if (m_pid <= 0) {
            return;
        }

        if (kill(-m_pid, SIGTERM) < 0 && errno == ESRCH) {
            kill(m_pid, SIGTERM);
        }
        if (!waitForExit(1000)) {
            if (kill(-m_pid, SIGKILL) < 0 && errno == ESRCH) {
                kill(m_pid, SIGKILL);
            }
            waitForExit(1000);
        }
    }

    std::string log() const {
        std::ifstream stream(m_log_path);
        if (!stream.is_open()) {
            return "<no child log available>";
        }
        return std::string((std::istreambuf_iterator<char>(stream)),
                           std::istreambuf_iterator<char>());
    }

    const std::string& logPath() const { return m_log_path; }

private:
    std::string m_binary;
    std::string m_log_path;
    pid_t m_pid{-1};
    int m_status{0};
};

inline int connectTo(int port, int timeout_ms = 1000) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);

    timeval timeout{};
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1 ||
        connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) <
            0) {
        close(fd);
        return -1;
    }
    return fd;
}

inline bool sendAll(int fd, const std::string& payload) {
    size_t offset = 0;
    while (offset < payload.size()) {
        ssize_t written = send(fd, payload.data() + offset,
                               payload.size() - offset, MSG_NOSIGNAL);
        if (written > 0) {
            offset += static_cast<size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

inline std::string request(int port, const std::string& payload,
                           int timeout_ms = 2000) {
    int fd = connectTo(port, timeout_ms);
    if (fd < 0 || !sendAll(fd, payload)) {
        if (fd >= 0) {
            close(fd);
        }
        return {};
    }
    shutdown(fd, SHUT_WR);

    std::string response;
    char buffer[4096];
    while (true) {
        pollfd descriptor{fd, POLLIN, 0};
        int ready = poll(&descriptor, 1, timeout_ms);
        if (ready <= 0) {
            break;
        }
        ssize_t bytes = recv(fd, buffer, sizeof(buffer), 0);
        if (bytes > 0) {
            response.append(buffer, static_cast<size_t>(bytes));
            continue;
        }
        if (bytes < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    close(fd);
    return response;
}

inline int waitForPortOpen(ChildProcess& process, int port, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        int fd = connectTo(port, 200);
        if (fd >= 0) {
            return fd;
        }
        if (!process.running()) {
            return -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return -1;
}

inline bool waitForResponse(ChildProcess& process, int port,
                            const std::string& payload,
                            const std::string& expected, int timeout_ms,
                            std::string& last_response) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        last_response = request(port, payload);
        if (last_response.find(expected) != std::string::npos) {
            return true;
        }
        if (!process.running()) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

inline bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace kairpc_stage2

#endif
