#include <dirent.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <thread>

#include "config.h"
#include "iothread.h"
#include "iothreadgroup.h"
#include "fd_event_group.h"
#include "log.h"
#include "tcp/net_addr.h"
#include "tcp/tcp_connection.h"
#include "tcp/tcp_server.h"
#include "timer_event.h"

namespace {

bool require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "lifetime/iothread check failed: " << message << '\n';
        return false;
    }
    return true;
}

int taskCount() {
    DIR* directory = opendir("/proc/self/task");
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

int fdCount() {
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

bool waitForLoop(talon::IOThread& thread) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < deadline) {
        if (thread.getEventLoop()->isLooping()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

bool waitForCount(int (*counter)(), int expected) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < deadline) {
        if (counter() == expected) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return counter() == expected;
}

int runChild(const std::function<int()>& body, const std::string& failure) {
    pid_t pid = fork();
    if (pid < 0) {
        std::perror("fork");
        return false;
    }
    if (pid == 0) {
        _exit(body());
    }

    for (int attempt = 0; attempt < 1000; ++attempt) {
        int status = 0;
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            return require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                           failure);
        }
        if (result < 0) {
            return require(false, failure + ": waitpid failed");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    return require(false, failure + ": timed out");
}

int startStopChild() {
    talon::IOThread thread;
    thread.start();
    if (!waitForLoop(thread)) {
        return 1;
    }
    thread.stop();
    return 0;
}

int destructorChild() {
    { talon::IOThread thread; }
    return 0;
}

int idempotentStopChild() {
    talon::IOThread thread;
    thread.start();
    if (!waitForLoop(thread)) {
        return 1;
    }
    thread.stop();
    thread.stop();
    return 0;
}

int groupShutdownChild() {
    const int before = taskCount();
    if (before < 0) {
        return 1;
    }
    {
        talon::IOThreadGroup group(2);
        group.start();
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(1);
        while (std::chrono::steady_clock::now() < deadline) {
            bool running = true;
            for (auto* thread : group.getIOThread_group()) {
                running = running && thread->getEventLoop()->isLooping();
            }
            if (running) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    if (!waitForCount(taskCount, before)) {
        std::cerr << "group shutdown task count before=" << before
                  << " after=" << taskCount() << '\n';
        return 1;
    }
    return 0;
}

int repeatChild() {
    const int tasks_before = taskCount();
    const int fds_before = fdCount();
    if (tasks_before < 0 || fds_before < 0) {
        return 1;
    }
    for (int iteration = 0; iteration < 100; ++iteration) {
        talon::IOThread thread;
        thread.start();
        if (!waitForLoop(thread)) {
            return 1;
        }
        thread.stop();
    }
    if (!waitForCount(taskCount, tasks_before) ||
        !waitForCount(fdCount, fds_before)) {
        const int final_tasks = taskCount();
        const int final_fds = fdCount();
        std::cerr << "repeat counts before tasks=" << tasks_before
                  << " fds=" << fds_before << " after tasks=" << final_tasks
                  << " fds=" << final_fds << '\n';
        return 1;
    }
    return 0;
}

int deferredConnectionCallbackChild() {
    int sockets[2]{-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        return 1;
    }

    talon::Eventloop loop;
    auto address = std::make_shared<talon::IPNetAddr>("127.0.0.1", 1);
    auto connection = std::make_shared<talon::TcpConnection>(
        &loop, sockets[0], 128, address, address,
        talon::TcpConnectionByServer);
    connection->setState(talon::Connected);
    connection->listenRead();

    auto* event = talon::FdEventGroup::GetFdEventGroup()->getFdEvent(sockets[0]);
    loop.addTask(event->handler(talon::Fd_Event::IN_EVENT));
    loop.addTask([&loop]() { loop.stop(); });
    connection.reset();

    loop.loop();
    close(sockets[1]);
    return 0;
}

int timerCancellationChild() {
    talon::Eventloop loop;
    bool fired = false;
    auto timer = std::make_shared<talon::TimerEvent>(
        1, false, [&fired]() { fired = true; });
    loop.addTimerEvent(timer);
    loop.deleteTimerEvent(timer);
    loop.addTask([&loop]() { loop.stop(); });
    loop.loop();
    return fired ? 1 : 0;
}

int serverShutdownChild() {
    talon::Eventloop::GetCurrentEventLoop();
    const int tasks_before = taskCount();
    const int fds_before = fdCount();
    if (tasks_before < 0 || fds_before < 0) {
        return 1;
    }

    talon::Config::GetGlobalConfig()->m_io_threads = 2;
    auto address = std::make_shared<talon::IPNetAddr>("127.0.0.1", 0);
    { talon::TcpServer server(address); }

    const int tasks_after = taskCount();
    const int fds_after = fdCount();
    if (tasks_after != tasks_before || fds_after != fds_before) {
        std::cerr << "server shutdown counts before tasks=" << tasks_before
                  << " fds=" << fds_before << " after tasks=" << tasks_after
                  << " fds=" << fds_after << '\n';
        return 1;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    talon::Config::SetGlobalConfig(nullptr);
    talon::Config::GetGlobalConfig()->m_log_level = "ERROR";
    talon::Logger::InitGlobalLogger(0);

    if (argc != 2) {
        std::cerr << "usage: lifetime_iothread_test <start_stop|destructor|"
                     "idempotent_stop|group_shutdown|repeat|deferred_callback|"
                     "timer_cancel|server_shutdown>\n";
        return 2;
    }

    const std::string scenario = argv[1];
    if (scenario == "start_stop") {
        return runChild(startStopChild, "IOThread start/stop failed") ? 0 : 1;
    }
    if (scenario == "destructor") {
        return runChild(destructorChild,
                        "IOThread destructor did not complete without start")
                       ? 0
                       : 1;
    }
    if (scenario == "idempotent_stop") {
        return runChild(idempotentStopChild,
                        "IOThread stop was not idempotent")
                       ? 0
                       : 1;
    }
    if (scenario == "group_shutdown") {
        return runChild(groupShutdownChild,
                        "IOThreadGroup destructor left worker threads alive")
                       ? 0
                       : 1;
    }
    if (scenario == "repeat") {
        return runChild(repeatChild,
                        "repeated IOThread lifecycle leaked threads or fds")
                       ? 0
                       : 1;
    }
    if (scenario == "deferred_callback") {
        return runChild(deferredConnectionCallbackChild,
                        "deferred connection callback outlived its owner")
                       ? 0
                       : 1;
    }
    if (scenario == "timer_cancel") {
        return runChild(timerCancellationChild,
                        "cancelled timer callback was invoked")
                       ? 0
                       : 1;
    }
    if (scenario == "server_shutdown") {
        return runChild(serverShutdownChild,
                        "TcpServer teardown left threads or fds alive")
                       ? 0
                       : 1;
    }

    std::cerr << "unknown lifetime/iothread scenario: " << scenario << '\n';
    return 2;
}
