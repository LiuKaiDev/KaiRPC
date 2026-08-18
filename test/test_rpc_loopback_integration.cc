#include <iostream>
#include <string>

#include "integration_test_support.h"

namespace {

bool check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "RPC loopback integration failed: " << message << '\n';
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "usage: rpc_loopback_integration_test <center> <server> "
                     "<client>\n";
        return 2;
    }

    kairpc_stage2::ChildProcess center(argv[1], "rpc-center");
    if (!check(center.start(), "could not launch service center")) {
        std::cerr << center.log() << '\n';
        return 1;
    }

    std::string readiness_response;
    if (!check(kairpc_stage2::waitForResponse(
                   center, 9090, "lookup", "service map is empty", 5000,
                   readiness_response),
               "service center control port did not become ready: " +
                   readiness_response)) {
        std::cerr << center.log() << '\n';
        return 1;
    }

    kairpc_stage2::ChildProcess server(argv[2], "rpc-server");
    if (!check(server.start(), "could not launch RPC server")) {
        std::cerr << server.log() << '\n';
        return 1;
    }

    std::string registration;
    if (!check(kairpc_stage2::waitForResponse(
                   center, 9090, "lookup", "Order.makeOrder", 5000,
                   registration),
               "service registration did not appear: " + registration) ||
        !check(kairpc_stage2::contains(registration, "127.0.0.1:9999"),
               "registration did not contain the server endpoint: " +
                   registration)) {
        std::cerr << server.log() << '\n' << center.log() << '\n';
        return 1;
    }

    kairpc_stage2::ScopedFd server_readiness(
        kairpc_stage2::waitForPortOpen(server, 9999, 5000));
    if (!check(server_readiness.valid(),
               "RPC server port did not become ready")) {
        std::cerr << server.log() << '\n';
        return 1;
    }

    kairpc_stage2::ChildProcess client(argv[3], "rpc-client");
    if (!check(client.start(), "could not launch RPC client")) {
        std::cerr << client.log() << '\n';
        return 1;
    }

    int status = 0;
    if (!check(client.waitForExit(15000, &status),
               "RPC client did not exit within 15 seconds")) {
        std::cerr << client.log() << '\n';
        return 1;
    }
    if (!check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
               "RPC client exit status was not zero") ||
        !check(kairpc_stage2::contains(client.log(), "call rpc success"),
               "RPC client log did not report success") ||
        !check(kairpc_stage2::contains(client.log(),
                                       "order_id: \"20231015\""),
               "RPC client log did not contain the expected order id")) {
        std::cerr << client.log() << '\n' << server.log() << '\n';
        return 1;
    }

    return 0;
}
