//
// Created for KaiRPC on 23-10-4.
//

#ifndef TALON_RPC_TINYPB_PROTOCOL_H
#define TALON_RPC_TINYPB_PROTOCOL_H

#include "abstract_protocol.h"

#include <cstdint>
#include <string>

namespace talon {
struct TinyPBProtocol : public AbstractProtocol {
   public:
    TinyPBProtocol() {}
    ~TinyPBProtocol() {}

   public:
    static constexpr int32_t MAX_PACKAGE_SIZE = 10 * 1024 * 1024;
    static constexpr int32_t MIN_PACKAGE_SIZE = 1 + 4 + 4 + 4 + 4 + 4 + 4 + 1;
    static char PB_START ;
    static char PB_END;

   public:
    int32_t m_pk_len{0};
    int32_t m_msg_id_len{0};
    // msg_id 继承父类

    int32_t m_method_name_len{0};
    std::string m_method_name;
    int32_t m_err_code{0};
    int32_t m_err_info_len{0};
    std::string m_err_info;
    std::string m_pb_data;
    int32_t m_check_sum{0};

    bool parse_success{false};
};


}  // namespace talon
#endif  // TALON_RPC_TINYPB_PROTOCOL_H
