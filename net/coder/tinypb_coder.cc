//
// Created for KaiRPC on 23-10-4.
//

#include "tinypb_coder.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "log.h"
#include "tinypb_protocol.h"
#include "util.h"

namespace talon {
namespace {

constexpr int32_t kInt32Size = 4;

bool canReadInt32(int index, int limit) {
    return index >= 0 && static_cast<int64_t>(index) + kInt32Size <= limit;
}

bool canReadField(int index, int32_t len, int limit) {
    return len >= 0 && index >= 0 && static_cast<int64_t>(index) + len <= limit;
}

void moveReadIndexIfNeeded(const TcpBuffer::s_ptr& buffer, int size) {
    if (size > 0) {
        buffer->moveReadIndex(size);
    }
}

std::string readStringField(const std::vector<char>& data, int index, int32_t len) {
    if (len == 0) {
        return "";
    }
    return std::string(&data[index], len);
}

}  // namespace

TinyPBCoder::TinyPBCoder() = default;

void TinyPBCoder::encode(std::vector<AbstractProtocol::s_ptr>& messages,
                         TcpBuffer::s_ptr out_buffer) {
    for (auto& i : messages) {
        std::shared_ptr<TinyPBProtocol> msg =
            std::dynamic_pointer_cast<TinyPBProtocol>(i);
        if (msg == nullptr) {
            ERRORLOG("encode TinyPB failed, protocol type mismatch");
            continue;
        }

        int len = 0;
        const char* buf = encodeTinyPB(msg, len);
        if (buf != nullptr && len != 0) {
            out_buffer->writeToBuffer(buf, len);
        }
        if (buf != nullptr) {
            std::free((void*)buf);
            buf = nullptr;
        }
    }
}

void TinyPBCoder::decode(std::vector<AbstractProtocol::s_ptr>& out_messages,
                         TcpBuffer::s_ptr buffer) {
    while (buffer->readAble() > 0) {
        const std::vector<char>& tmp = buffer->m_buffer;
        const int read_index = buffer->readIndex();
        const int write_index = buffer->writeIndex();

        int start_index = -1;
        for (int i = read_index; i < write_index; ++i) {
            if (tmp[i] == TinyPBProtocol::PB_START) {
                start_index = i;
                break;
            }
        }

        if (start_index < 0) {
            moveReadIndexIfNeeded(buffer, buffer->readAble());
            DEBUGLOG("decode TinyPB, discard data without start flag");
            return;
        }

        if (start_index > read_index) {
            moveReadIndexIfNeeded(buffer, start_index - read_index);
            continue;
        }

        if (!canReadInt32(start_index + 1, write_index)) {
            return;
        }

        const int32_t pk_len = getInt32FromNetByte(&tmp[start_index + 1]);
        DEBUGLOG("get pk_len = %d", pk_len);

        if (pk_len < TinyPBProtocol::MIN_PACKAGE_SIZE ||
            pk_len > TinyPBProtocol::MAX_PACKAGE_SIZE) {
            ERRORLOG("invalid TinyPB package length[%d]", pk_len);
            moveReadIndexIfNeeded(buffer, 1);
            continue;
        }

        const int64_t packet_end64 = static_cast<int64_t>(start_index) + pk_len;
        if (packet_end64 > write_index) {
            return;
        }

        const int packet_end = static_cast<int>(packet_end64);
        const int end_flag_index = packet_end - 1;
        if (tmp[end_flag_index] != TinyPBProtocol::PB_END) {
            ERRORLOG("invalid TinyPB end flag, package length[%d]", pk_len);
            moveReadIndexIfNeeded(buffer, 1);
            continue;
        }

        std::shared_ptr<TinyPBProtocol> message = std::make_shared<TinyPBProtocol>();
        message->m_pk_len = pk_len;

        const int checksum_index = end_flag_index - kInt32Size;
        int cursor = start_index + 1 + kInt32Size;
        bool parse_success = true;

        if (!canReadInt32(cursor, checksum_index)) {
            parse_success = false;
            ERRORLOG("parse TinyPB error, msg_id_len field is incomplete");
        } else {
            message->m_msg_id_len = getInt32FromNetByte(&tmp[cursor]);
            cursor += kInt32Size;
        }

        if (parse_success && !canReadField(cursor, message->m_msg_id_len, checksum_index)) {
            parse_success = false;
            ERRORLOG("parse TinyPB error, invalid msg_id length[%d]", message->m_msg_id_len);
        } else if (parse_success) {
            message->m_msg_id = readStringField(tmp, cursor, message->m_msg_id_len);
            cursor += message->m_msg_id_len;
            DEBUGLOG("parse msg_id=%s", message->m_msg_id.c_str());
        }

        if (parse_success && !canReadInt32(cursor, checksum_index)) {
            parse_success = false;
            ERRORLOG("parse TinyPB error, method_name_len field is incomplete");
        } else if (parse_success) {
            message->m_method_name_len = getInt32FromNetByte(&tmp[cursor]);
            cursor += kInt32Size;
        }

        if (parse_success && !canReadField(cursor, message->m_method_name_len, checksum_index)) {
            parse_success = false;
            ERRORLOG("parse TinyPB error, invalid method_name length[%d]", message->m_method_name_len);
        } else if (parse_success) {
            message->m_method_name =
                readStringField(tmp, cursor, message->m_method_name_len);
            cursor += message->m_method_name_len;
            DEBUGLOG("parse method_name=%s", message->m_method_name.c_str());
        }

        if (parse_success && !canReadInt32(cursor, checksum_index)) {
            parse_success = false;
            ERRORLOG("parse TinyPB error, error_code field is incomplete");
        } else if (parse_success) {
            message->m_err_code = getInt32FromNetByte(&tmp[cursor]);
            cursor += kInt32Size;
        }

        if (parse_success && !canReadInt32(cursor, checksum_index)) {
            parse_success = false;
            ERRORLOG("parse TinyPB error, error_info_len field is incomplete");
        } else if (parse_success) {
            message->m_err_info_len = getInt32FromNetByte(&tmp[cursor]);
            cursor += kInt32Size;
        }

        if (parse_success && !canReadField(cursor, message->m_err_info_len, checksum_index)) {
            parse_success = false;
            ERRORLOG("parse TinyPB error, invalid error_info length[%d]", message->m_err_info_len);
        } else if (parse_success) {
            message->m_err_info = readStringField(tmp, cursor, message->m_err_info_len);
            cursor += message->m_err_info_len;
            DEBUGLOG("parse error_info=%s", message->m_err_info.c_str());
        }

        const int32_t pb_data_len = checksum_index - cursor;
        if (parse_success && pb_data_len < 0) {
            parse_success = false;
            ERRORLOG("parse TinyPB error, invalid protobuf body length[%d]", pb_data_len);
        } else if (parse_success) {
            message->m_pb_data = readStringField(tmp, cursor, pb_data_len);
            cursor += pb_data_len;
        }

        if (parse_success && !canReadInt32(cursor, end_flag_index)) {
            parse_success = false;
            ERRORLOG("parse TinyPB error, checksum field is incomplete");
        } else if (parse_success) {
            message->m_check_sum = getInt32FromNetByte(&tmp[cursor]);
            cursor += kInt32Size;
            // TODO: Implement checksum validation. The field is preserved for
            // wire compatibility, but it is not trusted yet.
        }

        message->parse_success = parse_success;
        moveReadIndexIfNeeded(buffer, pk_len);

        if (parse_success) {
            out_messages.push_back(message);
        }
    }
}

const char* TinyPBCoder::encodeTinyPB(
    const std::shared_ptr<TinyPBProtocol>& message, int& len) {
    len = 0;
    if (message == nullptr) {
        ERRORLOG("encode TinyPB failed, null message");
        return nullptr;
    }

    if (message->m_msg_id.empty()) {
        message->m_msg_id = "123456789";
    }

    DEBUGLOG("msg_id = %s", message->m_msg_id.c_str());

    const int64_t pk_len64 =
        TinyPBProtocol::MIN_PACKAGE_SIZE +
        static_cast<int64_t>(message->m_msg_id.length()) +
        static_cast<int64_t>(message->m_method_name.length()) +
        static_cast<int64_t>(message->m_err_info.length()) +
        static_cast<int64_t>(message->m_pb_data.length());
    if (pk_len64 > TinyPBProtocol::MAX_PACKAGE_SIZE) {
        ERRORLOG("encode TinyPB failed, package length[%lld] exceeds max size[%d]",
                 static_cast<long long>(pk_len64),
                 TinyPBProtocol::MAX_PACKAGE_SIZE);
        return nullptr;
    }

    int pk_len = static_cast<int>(pk_len64);
    DEBUGLOG("pk_len = %d", pk_len);

    char* buf = reinterpret_cast<char*>(std::malloc(pk_len));
    if (buf == nullptr) {
        ERRORLOG("encode TinyPB failed, malloc package length[%d]", pk_len);
        return nullptr;
    }
    char* tmp = buf;

    *tmp = TinyPBProtocol::PB_START;
    tmp++;

    int32_t pk_len_net = htonl(pk_len);
    memcpy(tmp, &pk_len_net, sizeof(pk_len_net));
    tmp += sizeof(pk_len_net);

    int msg_id_len = static_cast<int>(message->m_msg_id.length());
    int32_t msg_id_len_net = htonl(msg_id_len);
    memcpy(tmp, &msg_id_len_net, sizeof(msg_id_len_net));
    tmp += sizeof(msg_id_len_net);

    if (!message->m_msg_id.empty()) {
        memcpy(tmp, &(message->m_msg_id[0]), msg_id_len);
        tmp += msg_id_len;
    }

    int method_name_len = static_cast<int>(message->m_method_name.length());
    int32_t method_name_len_net = htonl(method_name_len);
    memcpy(tmp, &method_name_len_net, sizeof(method_name_len_net));
    tmp += sizeof(method_name_len_net);

    if (!message->m_method_name.empty()) {
        memcpy(tmp, &(message->m_method_name[0]), method_name_len);
        tmp += method_name_len;
    }

    int32_t err_code_net = htonl(message->m_err_code);
    memcpy(tmp, &err_code_net, sizeof(err_code_net));
    tmp += sizeof(err_code_net);

    int err_info_len = static_cast<int>(message->m_err_info.length());
    int32_t err_info_len_net = htonl(err_info_len);
    memcpy(tmp, &err_info_len_net, sizeof(err_info_len_net));
    tmp += sizeof(err_info_len_net);

    if (!message->m_err_info.empty()) {
        memcpy(tmp, &(message->m_err_info[0]), err_info_len);
        tmp += err_info_len;
    }

    if (!message->m_pb_data.empty()) {
        memcpy(tmp, &(message->m_pb_data[0]), message->m_pb_data.length());
        tmp += message->m_pb_data.length();
    }

    // TODO: Implement a real checksum. For now encode writes a placeholder
    // value and decode only preserves the field.
    message->m_check_sum = 1;
    int32_t check_sum_net = htonl(message->m_check_sum);
    memcpy(tmp, &check_sum_net, sizeof(check_sum_net));
    tmp += sizeof(check_sum_net);

    *tmp = TinyPBProtocol::PB_END;

    message->m_pk_len = pk_len;
    message->m_msg_id_len = msg_id_len;
    message->m_method_name_len = method_name_len;
    message->m_err_info_len = err_info_len;
    message->parse_success = true;
    len = pk_len;

    DEBUGLOG("encode message[%s] success", message->m_msg_id.c_str());

    return buf;
}
}  // namespace talon
