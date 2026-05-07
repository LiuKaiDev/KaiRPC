//
// Created for KaiRPC on 23-10-20.
//
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <unordered_map>

#include "config_reader.h"

namespace {

constexpr int DEFAULT_SERVICE_TTL_MS = 10000;

constexpr int MAX_EVENTS = 100;
int SERVER_PORT = 8080;
int CONTROL_PORT = 9090;
int SERVICE_TTL_MS = DEFAULT_SERVICE_TTL_MS;

struct ServiceNode {
    std::string addr;
    int64_t last_seen_ms{0};
};

std::map<std::string, ServiceNode> service_map;

int64_t nowMs() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

bool isExpired(const ServiceNode& node) {
    return nowMs() - node.last_seen_ms > SERVICE_TTL_MS;
}

void pruneExpiredServices() {
    for (auto it = service_map.begin(); it != service_map.end();) {
        if (isExpired(it->second)) {
            printf("service expired, name=%s, addr=%s\n",
                   it->first.c_str(),
                   it->second.addr.c_str());
            it = service_map.erase(it);
        } else {
            ++it;
        }
    }
}

int readIntConfig(const std::unordered_map<std::string, std::string>& config,
                  const std::string& key,
                  int default_value) {
    auto it = config.find(key);
    if (it == config.end() || it->second.empty()) {
        return default_value;
    }

    int value = std::atoi(it->second.c_str());
    return value > 0 ? value : default_value;
}

bool parseServiceAndAddr(const std::string& content,
                         std::string& service_name,
                         std::string& service_addr) {
    size_t split = content.find_first_of(' ');
    if (split == std::string::npos) {
        return false;
    }

    service_name = content.substr(0, split);
    service_addr = content.substr(split + 1);
    return !service_name.empty() && !service_addr.empty();
}

void writeMessage(int fd, const std::string& message) {
    if (write(fd, message.c_str(), message.length()) < 0) {
        perror("write error");
    }
}

}  // namespace

int main() {
    ConfigReader readr("./conf/service_center.conf");
    auto config = readr.getMap();
    SERVER_PORT = readIntConfig(config, "query_port", SERVER_PORT);
    CONTROL_PORT = readIntConfig(config, "control_port", CONTROL_PORT);
    SERVICE_TTL_MS =
        readIntConfig(config, "service_ttl_ms", DEFAULT_SERVICE_TTL_MS);
    printf("service center start, query port:%d, control port:%d, ttl:%dms\n",
           SERVER_PORT,
           CONTROL_PORT,
           SERVICE_TTL_MS);

    int serverSocket, controlSocket, clientSocket;
    struct sockaddr_in serverAddr {
    }, controlAddr{}, clientAddr{};
    socklen_t serverAddrLen = sizeof(clientAddr);
    socklen_t controlAddrLen = sizeof(clientAddr);
    socklen_t clientAddrLen = sizeof(clientAddr);
    char buffer[1024];
    serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket == -1) {
        std::cerr << "Error creating server socket" << std::endl;
        return 1;
    }
    controlSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (controlSocket == -1) {
        std::cerr << "Error creating control socket" << std::endl;
        close(serverSocket);
        return 1;
    }
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(SERVER_PORT);
    serverAddr.sin_addr.s_addr = INADDR_ANY;

    controlAddr.sin_family = AF_INET;
    controlAddr.sin_port = htons(CONTROL_PORT);
    controlAddr.sin_addr.s_addr = INADDR_ANY;

    int val = 1;
    if (setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)) !=
        0) {
        printf("setsockopt error\n");
    }
    int val2 = 1;
    if (setsockopt(controlSocket, SOL_SOCKET, SO_REUSEADDR, &val2,
                   sizeof(val2)) != 0) {
        printf("setsockopt error\n");
    }
    if (bind(serverSocket, reinterpret_cast<const sockaddr *>(&serverAddr),
             serverAddrLen) == -1) {
        std::cerr << "Error binding to the server socket" << std::endl;
        close(serverSocket);
        close(controlSocket);
        return 1;
    }

    if (bind(controlSocket, reinterpret_cast<const sockaddr *>(&controlAddr),
             controlAddrLen) == -1) {
        std::cerr << "Error binding to the control socket" << std::endl;
        close(serverSocket);
        close(controlSocket);
        return 1;
    }

    if (listen(serverSocket, 5) == -1) {
        std::cerr << "Error listening on the server socket" << std::endl;
        close(serverSocket);
        close(controlSocket);
        return 1;
    }

    if (listen(controlSocket, 5) == -1) {
        std::cerr << "Error listening on the control socket" << std::endl;
        close(serverSocket);
        close(controlSocket);
        return 1;
    }

    int epollFd = epoll_create1(0);
    if (epollFd == -1) {
        std::cerr << "Error creating epoll" << std::endl;
        close(serverSocket);
        close(controlSocket);
        return 1;
    }

    struct epoll_event event {};
    event.data.fd = serverSocket;
    event.events = EPOLLIN;
    if (epoll_ctl(epollFd, EPOLL_CTL_ADD, serverSocket, &event) < 0) {
        std::cerr << "Error adding server socket to epoll" << std::endl;
        close(serverSocket);
        close(controlSocket);
        close(epollFd);
        return 1;
    }

    event.data.fd = controlSocket;
    if (epoll_ctl(epollFd, EPOLL_CTL_ADD, controlSocket, &event) == -1) {
        std::cerr << "Error adding control socket to epoll" << std::endl;
        close(serverSocket);
        close(controlSocket);
        close(epollFd);
        return 1;
    }

    while (true) {
        struct epoll_event events[MAX_EVENTS];
        int numEvents = epoll_wait(epollFd, events, MAX_EVENTS, -1);
        for (int i = 0; i < numEvents; ++i) {
            if (events[i].data.fd == serverSocket) {
                clientSocket = ::accept(
                    serverSocket, reinterpret_cast<sockaddr *>(&controlAddr),
                    &serverAddrLen);
                printf("c = %d \n", clientSocket);
                if (clientSocket < 0) {
                    std::cerr << "Error accepting client connection"
                              << std::endl;
                    continue;
                }
                event.data.fd = clientSocket;
                event.events = EPOLLIN;
                if (epoll_ctl(epollFd, EPOLL_CTL_ADD, clientSocket, &event) <
                    0) {
                    std::cerr << "Error adding client socket to epoll"
                              << std::endl;
                    close(clientSocket);
                }
            } else if (events[i].data.fd == controlSocket) {
                clientSocket =
                    accept(controlSocket, (struct sockaddr *)&clientAddr,
                           &clientAddrLen);
                if (clientSocket == -1) {
                    std::cerr << "Error accepting control connection"
                              << std::endl;
                    continue;
                }
                size_t bytesRead =
                    recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
                if (bytesRead <= 0) {
                    // Connection closed or error.
                } else {
                    buffer[bytesRead] = '\0';
                    std::string command = buffer;
                    size_t op_end = command.find_first_of(' ');
                    std::string operator_c =
                        op_end == std::string::npos
                            ? command
                            : command.substr(0, op_end);
                    std::string content =
                        op_end == std::string::npos
                            ? ""
                            : command.substr(op_end + 1);
                    std::string back_message;
                    printf("operator_c = %s\n", operator_c.c_str());
                    printf("content = %s\n", content.c_str());

                    if (operator_c == "add" || operator_c == "heartbeat") {
                        std::string service_name;
                        std::string service_addr;
                        if (!parseServiceAndAddr(
                                content, service_name, service_addr)) {
                            back_message = "invalid service command";
                        } else {
                            service_map[service_name] =
                                ServiceNode{service_addr, nowMs()};
                            back_message = operator_c + " service success";
                            printf("service_name = %s\n",
                                   service_name.c_str());
                            printf("service_addr = %s\n",
                                   service_addr.c_str());
                        }
                        writeMessage(clientSocket, back_message);
                    } else if (operator_c == "delete") {
                        std::string service_name = content;
                        if (service_map.find(service_name) ==
                            service_map.end()) {
                            back_message = "service not exist";
                        } else {
                            service_map.erase(service_name);
                            back_message = "delete service success";
                        }
                        writeMessage(clientSocket, back_message);
                    } else if (operator_c == "lookup") {
                        pruneExpiredServices();
                        if (service_map.empty()) {
                            back_message = "service map is empty";
                        } else {
                            for (auto& item : service_map) {
                                back_message += item.first + " " +
                                                item.second.addr + "\n";
                            }
                        }
                        writeMessage(clientSocket, back_message);
                    } else if (operator_c == "modify") {
                        std::string service_name;
                        std::string service_addr;
                        if (!parseServiceAndAddr(
                                content, service_name, service_addr)) {
                            back_message = "invalid service command";
                        } else if (service_map.find(service_name) ==
                                   service_map.end()) {
                            back_message = "service not exist";
                        } else {
                            service_map[service_name] =
                                ServiceNode{service_addr, nowMs()};
                            back_message = "modify service success";
                        }
                        writeMessage(clientSocket, back_message);
                    } else {
                        back_message = "unkown command";
                        writeMessage(clientSocket, back_message);
                    }
                }
                close(clientSocket);
            } else {
                size_t byteRead =
                    recv(events[i].data.fd, buffer, sizeof(buffer) - 1,
                         0);  // 为'\0'留下一个位置

                if (byteRead <= 0) {
                    close(events[i].data.fd);
                } else {
                    buffer[byteRead] = '\0';
                    std::string server_name = buffer;
                    std::string service_addr;
                    auto it = service_map.find(server_name);
                    if (it == service_map.end() || isExpired(it->second)) {
                        if (it != service_map.end()) {
                            printf("service expired on query, name=%s, addr=%s\n",
                                   it->first.c_str(),
                                   it->second.addr.c_str());
                            service_map.erase(it);
                        }
                        service_addr = "unknown host";
                    } else {
                        service_addr = it->second.addr;
                    }
                    printf("get service name = %s, return addr = %s\n",
                           server_name.c_str(), service_addr.c_str());
                    ssize_t ret = write(events[i].data.fd,
                                        service_addr.c_str(),
                                        service_addr.length());
                    if (ret < 0) {
                        perror("write error");
                    } else if (static_cast<size_t>(ret) !=
                               service_addr.length()) {
                        std::cerr << "Not all bytes written. Expected: "
                                  << service_addr.length()
                                  << ", Written: " << ret << std::endl;
                    }
                }
            }
        }
    }
}
