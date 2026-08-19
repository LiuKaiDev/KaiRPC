//
// Created for KaiRPC on 23-9-30.
//

#include "iothread.h"
#include <cassert>
#include <cerrno>
#include "log.h"
#include "util.h"
namespace talon{
    IOThread::IOThread() {

        int rt = sem_init(&m_init_semaphore, 0, 0);
        assert(rt == 0);

        rt = sem_init(&m_start_semaphore, 0, 0);
        assert(rt == 0);

        pthread_create(&m_thread, nullptr, &IOThread::Main, this);

        // wait, 直到新线程执行完 Main 函数的前置
        sem_wait(&m_init_semaphore);

        DEBUGLOG("IOThread [%d] create success", m_thread_id);
    }

    IOThread::~IOThread() {
        stop();
        join();
        if (m_event_loop) {
            delete m_event_loop;
            m_event_loop = nullptr;
        }
        sem_destroy(&m_init_semaphore);
        sem_destroy(&m_start_semaphore);
    }


    void* IOThread::Main(void* arg) {
        auto* thread = static_cast<IOThread*> (arg);

        thread->m_event_loop = new Eventloop();
        thread->m_thread_id = get_thread_id();


        // 唤醒等待的线程
        sem_post(&thread->m_init_semaphore);

        // 让IO 线程等待，直到我们主动的启动

        DEBUGLOG("IOThread %d created, wait start semaphore", thread->m_thread_id);

        int wait_result = 0;
        do {
            wait_result = sem_wait(&thread->m_start_semaphore);
        } while (wait_result != 0 && errno == EINTR);
        if (wait_result != 0) {
            return nullptr;
        }
        DEBUGLOG("IOThread %d start loop ", thread->m_thread_id);
        thread->m_event_loop->loop();

        DEBUGLOG("IOThread %d end loop ", thread->m_thread_id);

        return nullptr;

    }


    Eventloop* IOThread::getEventLoop() {
        return m_event_loop;
    }

    void IOThread::start() {
        DEBUGLOG("Now invoke IOThread %d", m_thread_id);
        if (!m_started.exchange(true)) {
            sem_post(&m_start_semaphore);
        }
    }

    void IOThread::stop() {
        if (m_event_loop != nullptr) {
            m_event_loop->stop();
        }
        if (!m_started.exchange(true)) {
            sem_post(&m_start_semaphore);
        }
    }

    void IOThread::join() {
        if (pthread_equal(pthread_self(), m_thread)) {
            return;
        }
        bool expected = false;
        if (m_joined.compare_exchange_strong(expected, true)) {
            pthread_join(m_thread, nullptr);
        }
    }
}
