#include <arpa/inet.h>
#include <cassert>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "coder/tinypb_coder.h"
#include "coder/tinypb_protocol.h"
#include "config.h"
#include "log.h"
#include "tcp/tcp_buffer.h"

namespace {

std::shared_ptr<talon::TinyPBProtocol> makeMessage(const std::string& msg_id) {
    auto message = std::make_shared<talon::TinyPBProtocol>();
    message->m_msg_id = msg_id;
    message->m_method_name = "Order.makeOrder";
    message->m_err_code = 0;
    message->m_err_info = "";
    message->m_pb_data = "serialized-body";
    return message;
}

std::vector<char> encodeMessage(const std::shared_ptr<talon::TinyPBProtocol>& message) {
    talon::TinyPBCoder coder;
    auto buffer = std::make_shared<talon::TcpBuffer>(1024);
    std::vector<talon::AbstractProtocol::s_ptr> messages;
    messages.push_back(message);

    coder.encode(messages, buffer);
    return std::vector<char>(
        buffer->m_buffer.begin(),
        buffer->m_buffer.begin() + buffer->writeIndex());
}

}  // namespace

int main() {
    talon::Config::SetGlobalConfig(nullptr);
    talon::Logger::InitGlobalLogger(0);

    talon::TinyPBCoder coder;

    auto full_packet = encodeMessage(makeMessage("msg-1"));
    auto full_buffer = std::make_shared<talon::TcpBuffer>(1024);
    full_buffer->writeToBuffer(full_packet.data(), full_packet.size());

    std::vector<talon::AbstractProtocol::s_ptr> decoded;
    coder.decode(decoded, full_buffer);
    assert(decoded.size() == 1);

    auto decoded_msg = std::dynamic_pointer_cast<talon::TinyPBProtocol>(decoded[0]);
    assert(decoded_msg != nullptr);
    assert(decoded_msg->parse_success);
    assert(decoded_msg->m_msg_id == "msg-1");
    assert(decoded_msg->m_method_name == "Order.makeOrder");
    assert(decoded_msg->m_pb_data == "serialized-body");

    auto partial_buffer = std::make_shared<talon::TcpBuffer>(1024);
    partial_buffer->writeToBuffer(full_packet.data(), 5);
    decoded.clear();
    coder.decode(decoded, partial_buffer);
    assert(decoded.empty());
    assert(partial_buffer->readIndex() == 0);

    partial_buffer->writeToBuffer(full_packet.data() + 5, full_packet.size() - 5);
    coder.decode(decoded, partial_buffer);
    assert(decoded.size() == 1);

    auto sticky_buffer = std::make_shared<talon::TcpBuffer>(2048);
    auto second_packet = encodeMessage(makeMessage("msg-2"));
    sticky_buffer->writeToBuffer(full_packet.data(), full_packet.size());
    sticky_buffer->writeToBuffer(second_packet.data(), second_packet.size());
    decoded.clear();
    coder.decode(decoded, sticky_buffer);
    assert(decoded.size() == 2);

    auto invalid_buffer = std::make_shared<talon::TcpBuffer>(32);
    char invalid_header[5] = {talon::TinyPBProtocol::PB_START};
    int32_t invalid_len = htonl(talon::TinyPBProtocol::MAX_PACKAGE_SIZE + 1);
    std::memcpy(invalid_header + 1, &invalid_len, sizeof(invalid_len));
    invalid_buffer->writeToBuffer(invalid_header, sizeof(invalid_header));
    decoded.clear();
    coder.decode(decoded, invalid_buffer);
    assert(decoded.empty());
    assert(invalid_buffer->readAble() == 0);

    return 0;
}
