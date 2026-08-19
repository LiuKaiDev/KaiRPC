#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "err_code.h"
#include "log.h"
#include "order.pb.h"
#include "coder/tinypb_coder.h"
#include "rpc/rpc_channel.h"
#include "rpc/rpc_closure.h"
#include "rpc/rpc_controller.h"

namespace {

enum class PeerMode {
    Sink,
    Disconnect,
    Response,
    DelayedResponse,
    ConnectFailure
};

int makeListener(int& port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(0);
    if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(fd, 8) != 0) {
        close(fd);
        return -1;
    }
    socklen_t length = sizeof(address);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        close(fd);
        return -1;
    }
    port = ntohs(address.sin_port);
    return fd;
}

std::string makeResponsePacket(const std::string& msg_id) {
    auto message = std::make_shared<talon::TinyPBProtocol>();
    message->m_msg_id = msg_id;
    message->m_method_name = "Order.makeOrder";
    makeOrderResponse response;
    response.set_order_id("completion-test");
    response.SerializeToString(&message->m_pb_data);

    auto buffer = std::make_shared<talon::TcpBuffer>(256);
    std::vector<talon::AbstractProtocol::s_ptr> messages{message};
    talon::TinyPBCoder coder;
    coder.encode(messages, buffer);
    return std::string(buffer->m_buffer.data(), buffer->readAble());
}

int runCall(const std::string& scenario, PeerMode mode, int timeout_ms,
            bool cancel_request, int expected_error, int expected_done) {
    int discovery_port = 0;
    int peer_port = 0;
    int discovery_fd = makeListener(discovery_port);
    int peer_fd = makeListener(peer_port);
    if (discovery_fd < 0 || peer_fd < 0) {
        return 1;
    }

    std::thread discovery([discovery_fd, peer_port]() {
        int client = accept(discovery_fd, nullptr, nullptr);
        if (client >= 0) {
            char request[512];
            recv(client, request, sizeof(request), 0);
            const std::string endpoint = "127.0.0.1:" +
                                          std::to_string(peer_port);
            send(client, endpoint.data(), endpoint.size(), MSG_NOSIGNAL);
            close(client);
        }
        close(discovery_fd);
    });

    if (mode == PeerMode::ConnectFailure) {
        close(peer_fd);
        peer_fd = -1;
    }
    std::thread peer([peer_fd, mode]() {
        if (peer_fd < 0) {
            return;
        }
        int client = accept(peer_fd, nullptr, nullptr);
        if (client >= 0) {
            char raw[64 * 1024];
            ssize_t size = recv(client, raw, sizeof(raw), 0);
            std::string msg_id;
            if (size > 0) {
                auto buffer = std::make_shared<talon::TcpBuffer>(size + 16);
                buffer->writeToBuffer(raw, static_cast<int>(size));
                std::vector<talon::AbstractProtocol::s_ptr> messages;
                talon::TinyPBCoder coder;
                coder.decode(messages, buffer);
                if (!messages.empty()) {
                    msg_id = messages.front()->m_msg_id;
                }
            }
            if (mode == PeerMode::Disconnect) {
                close(client);
            } else if (mode == PeerMode::Response ||
                       mode == PeerMode::DelayedResponse) {
                if (mode == PeerMode::DelayedResponse) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                const std::string packet = makeResponsePacket(msg_id);
                send(client, packet.data(), packet.size(), MSG_NOSIGNAL);
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                close(client);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                close(client);
            }
        }
        close(peer_fd);
    });

    talon::Config::SetGlobalConfig(nullptr);
    talon::Config::service_center_map["service_center_ip"] = "127.0.0.1";
    talon::Config::service_center_map["query_port"] =
        std::to_string(discovery_port);
    talon::Config::service_center_map["control_port"] =
        std::to_string(discovery_port);

    auto channel = std::make_shared<talon::RpcChannel>(nullptr);
    auto controller = std::make_shared<talon::RpcController>();
    controller->SetTimeout(timeout_ms);
    auto request = std::make_shared<makeOrderRequest>();
    auto response = std::make_shared<makeOrderResponse>();
    request->set_price(100);

    std::atomic<int> done_count{0};
    auto closure = std::make_shared<talon::RpcClosure>(nullptr, [&]() {
        ++done_count;
        if (channel->getTcpClient() != nullptr) {
            channel->getTcpClient()->stop();
        }
    });

    std::thread canceler;
    if (cancel_request) {
        canceler = std::thread([controller]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            controller->StartCancel();
        });
    }

    channel->Init(controller, request, response, closure);
    Order_Stub stub(channel.get());
    stub.makeOrder(controller.get(), request.get(), response.get(),
                   closure.get());

    if (canceler.joinable()) {
        canceler.join();
    }
    discovery.join();
    peer.join();

    const int actual_error = controller->GetErrorCode();
    const bool error_ok = expected_error == 0
                              ? actual_error == 0
                              : (actual_error == expected_error ||
                                 (expected_error == ERROR_FAILED_CONNECT &&
                                  actual_error == ERROR_PEER_CLOSED));
    const bool response_ok = expected_error == 0
                                 ? response->order_id() == "completion-test"
                                 : true;
    if (!(done_count.load() == expected_done && controller->Finished() &&
          error_ok && response_ok)) {
        std::cerr << scenario << " failed: done=" << done_count.load()
                  << " error=" << controller->GetErrorCode()
                  << " text=" << controller->GetErrorInfo() << '\n';
        return 1;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    talon::Config::SetGlobalConfig(nullptr);
    talon::Logger::InitGlobalLogger(0);
    if (argc != 2) {
        return 2;
    }
    const std::string scenario = argv[1];
    if (scenario == "timeout") {
        return runCall(scenario, PeerMode::Sink, 40, false,
                       ERROR_RPC_CALL_TIMEOUT, 1);
    }
    if (scenario == "cancel") {
        return runCall(scenario, PeerMode::Sink, 500, true,
                       ERROR_RPC_CALL_CANCELLED, 1);
    }
    if (scenario == "disconnect") {
        return runCall(scenario, PeerMode::Disconnect, 500, false,
                       ERROR_RPC_TRANSPORT, 1);
    }
    if (scenario == "connect_failure") {
        return runCall(scenario, PeerMode::ConnectFailure, 500, false,
                       ERROR_FAILED_CONNECT, 1);
    }
    if (scenario == "response") {
        return runCall(scenario, PeerMode::Response, 500, false, 0, 1);
    }
    if (scenario == "timeout_wins") {
        return runCall(scenario, PeerMode::DelayedResponse, 40, false,
                       ERROR_RPC_CALL_TIMEOUT, 1);
    }
    std::cerr << "unknown rpc completion scenario: " << scenario << '\n';
    return 2;
}
