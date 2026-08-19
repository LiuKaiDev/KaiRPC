//
// Created for KaiRPC on 23-9-28.
//

#include "fd_event.h"
#include "fcntl.h"
#include "unistd.h"
#include <utility>
#include "log.h"
#include "cstring"

namespace  talon
{

    Fd_Event::Fd_Event(int fd) : m_fd(fd) {
        reset();
    }


    Fd_Event::Fd_Event() {
        reset();
    }



    Fd_Event::~Fd_Event() {

    }


    std::function<void()> Fd_Event::handler(TriggerEvent event) {
        if (event == TriggerEvent::IN_EVENT) {
            return m_read_callback;
        } else if (event == TriggerEvent::OUT_EVENT) {
            return m_write_callback;
        } else if (event == TriggerEvent::ERROR_EVENT) {
            return m_error_callback;
        }
        return nullptr;
    }


    void Fd_Event::listen(TriggerEvent event_type, const std::function<void()>& callback, std::function<void()> error_callback /*= nullptr*/) {
        if (event_type == TriggerEvent::IN_EVENT) {
            m_listen_event.events |= EPOLLIN | EPOLLRDHUP;
            m_read_callback = callback;
        } else if (event_type == TriggerEvent::OUT_EVENT) {
            m_listen_event.events |= EPOLLOUT;
            m_write_callback = callback;
        } else {
            m_error_callback = callback;
        }

        if (error_callback != nullptr) {
            m_error_callback = std::move(error_callback);
        }

        m_listen_event.data.ptr = this;
    }


    void Fd_Event::cancle(TriggerEvent event_type) {
        if (event_type == TriggerEvent::IN_EVENT) {
            m_listen_event.events &= ~(EPOLLIN | EPOLLRDHUP);
        } else if (event_type == TriggerEvent::OUT_EVENT) {
            m_listen_event.events &= (~EPOLLOUT);
        }
    }

    void Fd_Event::reset() {
        memset(&m_listen_event, 0, sizeof(m_listen_event));
        m_listen_event.data.ptr = this;
        m_read_callback = nullptr;
        m_write_callback = nullptr;
        m_error_callback = nullptr;
    }


    void Fd_Event::setNonBlock() const {

        int flag = fcntl(m_fd, F_GETFL, 0);
        if (flag < 0) {
            return;
        }
        if (flag & O_NONBLOCK) {
            return;
        }

        fcntl(m_fd, F_SETFL, flag | O_NONBLOCK);
    }

}

