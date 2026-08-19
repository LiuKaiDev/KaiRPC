//
// Created for KaiRPC on 23-10-13.
//

#include "rpc_controller.h"

#include <utility>

namespace talon {
    void RpcController::Reset() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_error_code = 0;
        m_error_info = "";
        m_msg_id = "";
        m_is_failed = false;
        m_is_cancled = false;
        m_is_finished = false;
        m_local_addr = nullptr;
        m_peer_addr = nullptr;
        m_timeout = 1000;   // ms
        m_cancel_closure = nullptr;
        m_cancel_callback = nullptr;
    }

    bool RpcController::Failed() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_is_failed;
    }

    std::string RpcController::ErrorText() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_error_info;
    }

    void RpcController::StartCancel() {
        google::protobuf::Closure* closure = nullptr;
        std::function<void()> callback;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_is_cancled || m_is_finished) {
                return;
            }
            m_is_cancled = true;
            closure = m_cancel_closure;
            callback = std::move(m_cancel_callback);
            m_cancel_closure = nullptr;
        }
        if (closure != nullptr) {
            closure->Run();
        }
        if (callback) {
            callback();
        }
    }

    void RpcController::SetFailed(const std::string& reason) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_error_info = reason;
        m_is_failed = true;
    }

    bool RpcController::IsCanceled() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_is_cancled;
    }

    void RpcController::NotifyOnCancel(google::protobuf::Closure* callback) {
        bool cancelled = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            cancelled = m_is_cancled;
            if (!cancelled) {
                m_cancel_closure = callback;
            }
        }
        if (cancelled && callback != nullptr) {
            callback->Run();
        }
    }


    void RpcController::SetError(int32_t error_code, const std::string& error_info) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_error_code = error_code;
        m_error_info = error_info;
        m_is_failed = true;
    }

    int32_t RpcController::GetErrorCode() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_error_code;
    }

    std::string RpcController::GetErrorInfo() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_error_info;
    }

    void RpcController::SetMsgId(const std::string& msg_id) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_msg_id = msg_id;
    }

    std::string RpcController::GetMsgId() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_msg_id;
    }

    void RpcController::SetLocalAddr(NetAddr::s_ptr addr) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_local_addr = std::move(addr);
    }

    void RpcController::SetPeerAddr(NetAddr::s_ptr addr) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_peer_addr = std::move(addr);
    }

    NetAddr::s_ptr RpcController::GetLocalAddr() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_local_addr;
    }

    NetAddr::s_ptr RpcController::GetPeerAddr() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_peer_addr;
    }

    void RpcController::SetTimeout(int timeout) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_timeout = timeout;
    }

    int RpcController::GetTimeout() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_timeout;
    }

    bool RpcController::Finished() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_is_finished;
    }

    void RpcController::SetFinished(bool value) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_is_finished = value;
    }

    void RpcController::SetCancelCallback(std::function<void()> callback) {
        bool cancelled = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            cancelled = m_is_cancled;
            if (!cancelled) {
                m_cancel_callback = std::move(callback);
            }
        }
        if (cancelled && callback) {
            callback();
        }
    }

    void RpcController::ClearCancelCallback() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_cancel_callback = nullptr;
    }
} // talon
