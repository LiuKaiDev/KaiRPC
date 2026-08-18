#include <iostream>
#include <string>

#include "integration_test_support.h"

namespace {

bool check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "service discovery integration failed: " << message
                  << '\n';
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: service_discovery_integration_test <binary>\n";
        return 2;
    }

    const std::string service_name =
        "Stage2.Service." + std::to_string(static_cast<long long>(getpid()));
    const std::string endpoint =
        "127.0.0.1:" +
        std::to_string(30000 + static_cast<int>(getpid() % 1000));

    kairpc_stage2::ChildProcess center(argv[1], "service-discovery");
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

    const std::string add_response =
        kairpc_stage2::request(9090, "add " + service_name + " " + endpoint);
    if (!check(kairpc_stage2::contains(add_response, "add service success"),
               "add response was: " + add_response)) {
        std::cerr << center.log() << '\n';
        return 1;
    }

    const std::string query_response =
        kairpc_stage2::request(8080, service_name);
    if (!check(query_response == endpoint,
               "query response was: " + query_response)) {
        std::cerr << center.log() << '\n';
        return 1;
    }

    const std::string lookup_response =
        kairpc_stage2::request(9090, "lookup");
    if (!check(kairpc_stage2::contains(lookup_response, service_name + " " +
                                        endpoint),
               "lookup response was: " + lookup_response)) {
        std::cerr << center.log() << '\n';
        return 1;
    }

    const std::string delete_response =
        kairpc_stage2::request(9090, "delete " + service_name);
    if (!check(kairpc_stage2::contains(delete_response, "delete service success"),
               "delete response was: " + delete_response)) {
        std::cerr << center.log() << '\n';
        return 1;
    }

    return 0;
}
