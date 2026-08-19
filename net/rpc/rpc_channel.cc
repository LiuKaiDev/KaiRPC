//
// Created for KaiRPC on 23-10-14.
//

#include "rpc_channel.h"

#include <atomic>
#include <cstdlib>
#include <utility>

#include "coder/abstract_protocol.h"
#include "coder/tinypb_protocol.h"
#include "config.h"
#include "err_code.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/message.h"
#include "log.h"
#include "msg_id_util.h"
#include "rpc_controller.h"
#include "run_time.h"
#include "service_discovery/service_discovery.h"
#include "tcp/tcp_client.h"
#include "timer_event.h"

namespace talon {

struct RpcChannel::RequestState {
    std::shared_ptr<RpcController> controller;
    std::shared_ptr<google::protobuf::Message> response;
    std::shared_ptr<google::protobuf::Closure> closure;
    TcpClient::s_ptr client;
    TimerEvent::s_ptr timer;
    std::string msg_id;
    std::atomic_bool completed{false};
    std::weak_ptr<RpcChannel> owner;

    bool beginCompletion() {
        if (completed.exchange(true)) {
            return false;
        }

        if (controller != nullptr) {
            controller->SetFinished(true);
        }

        if (client != nullptr) {
            client->setDisconnectCallback(nullptr);
            client->removeReadMessage(msg_id);
            if (timer != nullptr) {
                client->deleteTimerEvent(timer);
            }
        }
        if (controller != nullptr) {
            controller->ClearCancelCallback();
        }
        return true;
    }

    void finish(int32_t error_code = 0, const std::string& error_info = {}) {
        if (controller != nullptr) {
            if (error_code != 0) {
                controller->SetError(error_code, error_info);
            }
            controller->SetFinished(true);
        }

        auto done = std::move(closure);
        auto active_client = client;
        timer.reset();
        if (active_client != nullptr) {
            active_client->setDisconnectCallback(nullptr);
            active_client->disconnect();
            active_client->stop();
        }
        if (done != nullptr) {
            done->Run();
        }
        client.reset();

        if (auto channel = owner.lock(); channel != nullptr &&
            channel->m_request_state.get() == this) {
            channel->m_request_state.reset();
        }
    }
};

namespace {

std::shared_ptr<RpcController> aliasController(
    const std::shared_ptr<google::protobuf::RpcController>& controller,
    google::protobuf::RpcController* raw_controller) {
    auto typed = std::dynamic_pointer_cast<RpcController>(controller);
    if (typed != nullptr) {
        return typed;
    }
    auto* raw = dynamic_cast<RpcController*>(raw_controller);
    if (raw == nullptr) {
        return nullptr;
    }
    return std::shared_ptr<RpcController>(raw, [](RpcController*) {});
}

template <typename T>
std::shared_ptr<T> aliasObject(T* object) {
    return std::shared_ptr<T>(object, [](T*) {});
}

void completeSynchronously(RpcController* controller,
                           google::protobuf::Closure* done,
                           int32_t error_code,
                           const std::string& error_info) {
    controller->SetError(error_code, error_info);
    controller->SetFinished(true);
    if (done != nullptr) {
        done->Run();
    }
}

}  // namespace

RpcChannel::RpcChannel(NetAddr::s_ptr peer_addr)
    : m_peer_addr(std::move(peer_addr)) {
    INFOLOG("RpcChannel");
}

RpcChannel::~RpcChannel() {
    INFOLOG("~RpcChannel");
}

void RpcChannel::callBack() {
    auto* controller = dynamic_cast<RpcController*>(getController());
    if (controller == nullptr || controller->Finished()) {
        return;
    }
    if (m_closure != nullptr) {
        m_closure->Run();
    }
    controller->SetFinished(true);
}

void RpcChannel::CallMethod(
    const google::protobuf::MethodDescriptor* method,
    google::protobuf::RpcController* controller,
    const google::protobuf::Message* request,
    google::protobuf::Message* response,
    google::protobuf::Closure* done) {
    auto* rpc_controller = dynamic_cast<RpcController*>(controller);
    if (rpc_controller == nullptr || request == nullptr || response == nullptr ||
        method == nullptr) {
        if (rpc_controller != nullptr) {
            completeSynchronously(rpc_controller, done, ERROR_RPC_CHANNEL_INIT,
                                  "controller, method, request or response NULL");
        }
        return;
    }

    if (rpc_controller->IsCanceled()) {
        completeSynchronously(rpc_controller, done, ERROR_RPC_CALL_CANCELLED,
                              "rpc call cancelled");
        return;
    }

    auto service_config = Config::getServiceCenterMap();
    const std::string service_center_ip = service_config["service_center_ip"];
    const int query_port = std::atoi(service_config["query_port"].c_str());
    auto server_addr = serviceDiscovery(method->full_name(), service_center_ip,
                                         query_port);
    if (server_addr == "unknown host") {
        completeSynchronously(rpc_controller, done, ERROR_RPC_PEER_ADDR,
                              "service not found");
        return;
    }
    m_peer_addr = FindAddr(server_addr);
    if (m_peer_addr == nullptr) {
        completeSynchronously(rpc_controller, done, ERROR_RPC_PEER_ADDR,
                              "peer addr nullptr");
        return;
    }

    auto req_protocol = std::make_shared<TinyPBProtocol>();
    if (rpc_controller->GetMsgId().empty()) {
        std::string msg_id = RunTime::GetRunTime()->m_msgid;
        if (msg_id.empty()) {
            msg_id = MsgIDUtil::GenMsgID();
        }
        req_protocol->m_msg_id = msg_id;
        rpc_controller->SetMsgId(msg_id);
    } else {
        req_protocol->m_msg_id = rpc_controller->GetMsgId();
    }
    req_protocol->m_method_name = method->full_name();

    if (!m_is_init) {
        completeSynchronously(rpc_controller, done, ERROR_RPC_CHANNEL_INIT,
                              "RpcChannel not call init()");
        return;
    }
    if (!request->SerializeToString(&req_protocol->m_pb_data)) {
        completeSynchronously(rpc_controller, done, ERROR_FAILED_SERIALIZE,
                              "failed to serialize request");
        return;
    }

    auto state = std::make_shared<RequestState>();
    state->controller = aliasController(m_controller, controller);
    state->response = m_response != nullptr
                          ? m_response
                          : aliasObject(response);
    state->closure = m_closure != nullptr
                         ? m_closure
                         : aliasObject(done);
    state->msg_id = req_protocol->m_msg_id;
    state->owner = weak_from_this();
    if (state->controller == nullptr) {
        completeSynchronously(rpc_controller, done, ERROR_RPC_CHANNEL_INIT,
                              "controller type is unsupported");
        return;
    }

    m_request_state = state;
    m_client = std::make_shared<TcpClient>(m_peer_addr);
    state->client = m_client;
    std::weak_ptr<RequestState> weak_state = state;
    auto client = m_client;

    state->controller->SetCancelCallback([weak_state, client]() {
        client->addTask([weak_state]() {
            auto current = weak_state.lock();
            if (current != nullptr && current->beginCompletion()) {
                current->finish(ERROR_RPC_CALL_CANCELLED, "rpc call cancelled");
            }
        });
    });
    state->timer = std::make_shared<TimerEvent>(
        state->controller->GetTimeout(), false, [weak_state]() {
            auto current = weak_state.lock();
            if (current != nullptr && current->beginCompletion()) {
                current->finish(ERROR_RPC_CALL_TIMEOUT, "rpc call timeout");
            }
        });
    m_client->addTimerEvent(state->timer);
    m_client->setDisconnectCallback([weak_state]() {
        auto current = weak_state.lock();
        if (current != nullptr && current->beginCompletion()) {
            current->finish(ERROR_RPC_TRANSPORT,
                            "rpc transport disconnected");
        }
    });

    m_client->connect([weak_state, req_protocol]() {
        auto current = weak_state.lock();
        if (current == nullptr || current->completed.load()) {
            return;
        }
        if (current->client->getConnectErrorCode() != 0) {
            if (current->beginCompletion()) {
                current->finish(current->client->getConnectErrorCode(),
                                current->client->getConnectErrorInfo());
            }
            return;
        }
        current->client->writeMessage(
            req_protocol, [weak_state, req_protocol](const AbstractProtocol::s_ptr&) {
                auto current = weak_state.lock();
                if (current == nullptr || current->completed.load()) {
                    return;
                }
                current->client->readMessage(
                    req_protocol->m_msg_id,
                    [weak_state](const AbstractProtocol::s_ptr& message) {
                        auto current = weak_state.lock();
                        if (current == nullptr || !current->beginCompletion()) {
                            return;
                        }
                        auto response =
                            std::dynamic_pointer_cast<TinyPBProtocol>(message);
                        if (response == nullptr) {
                            current->finish(ERROR_FAILED_GET_REPLY,
                                            "invalid rpc response");
                            return;
                        }
                        if (!current->response->ParseFromString(
                                response->m_pb_data)) {
                            current->finish(ERROR_FAILED_SERIALIZE,
                                            "failed to deserialize response");
                            return;
                        }
                        if (response->m_err_code != 0) {
                            current->finish(response->m_err_code,
                                            response->m_err_info);
                            return;
                        }
                        current->finish();
                    });
            });
    });
}

void RpcChannel::Init(controller_s_ptr controller, message_s_ptr req,
                      message_s_ptr res, closure_s_ptr done) {
    if (m_is_init) {
        return;
    }
    m_controller = std::move(controller);
    m_request = std::move(req);
    m_response = std::move(res);
    m_closure = std::move(done);
    m_is_init = true;
}

google::protobuf::RpcController* RpcChannel::getController() {
    return m_controller.get();
}

google::protobuf::Message* RpcChannel::getRequest() {
    return m_request.get();
}

google::protobuf::Message* RpcChannel::getResponse() {
    return m_response.get();
}

google::protobuf::Closure* RpcChannel::getClosure() {
    return m_closure.get();
}

TcpClient* RpcChannel::getTcpClient() {
    return m_client.get();
}

NetAddr::s_ptr RpcChannel::FindAddr(const std::string& str) {
    if (IPNetAddr::CheckValid(str)) {
        return std::make_shared<IPNetAddr>(str);
    }
    auto it = Config::GetGlobalConfig()->m_rpc_stubs.find(str);
    if (it != Config::GetGlobalConfig()->m_rpc_stubs.end()) {
        return it->second.addr;
    }
    return nullptr;
}

}  // namespace talon
