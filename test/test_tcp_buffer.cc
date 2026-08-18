#include <iostream>
#include <string>
#include <vector>

#include "tcp/tcp_buffer.h"

namespace {

bool require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "tcp buffer check failed: " << message << '\n';
        return false;
    }
    return true;
}

bool hasContents(talon::TcpBuffer& buffer, const std::string& expected) {
    std::vector<char> actual;
    buffer.readFromBuffer(actual, static_cast<int>(expected.size()));
    return std::string(actual.begin(), actual.end()) == expected;
}

}  // namespace

int main() {
    talon::TcpBuffer buffer(8);
    if (!require(buffer.readAble() == 0, "new buffers are empty") ||
        !require(buffer.writeAble() == 8, "new buffers expose capacity")) {
        return 1;
    }

    const std::string first = "hello";
    buffer.writeToBuffer(first.data(), static_cast<int>(first.size()));
    if (!require(buffer.readAble() == 5, "write advances readable bytes") ||
        !require(buffer.writeIndex() == 5, "write advances write index") ||
        !require(hasContents(buffer, first), "read returns bytes in order")) {
        return 1;
    }

    const std::string sequence = "abcdef";
    buffer.writeToBuffer(sequence.data(), static_cast<int>(sequence.size()));
    std::vector<char> prefix;
    buffer.readFromBuffer(prefix, 2);
    if (!require(std::string(prefix.begin(), prefix.end()) == "ab",
                 "partial reads preserve the prefix") ||
        !require(buffer.readAble() == 4, "partial reads leave remaining bytes") ||
        !require(hasContents(buffer, "cdef"), "remaining bytes stay ordered")) {
        return 1;
    }

    const std::string growth = "0123456789abcdef";
    buffer.writeToBuffer(growth.data(), static_cast<int>(growth.size()));
    if (!require(buffer.readAble() == static_cast<int>(growth.size()),
                 "writes grow storage when capacity is insufficient") ||
        !require(buffer.m_buffer.size() >= growth.size(),
                 "grown storage fits the complete write")) {
        return 1;
    }
    buffer.resizeBuffer(32);
    if (!require(buffer.readAble() == static_cast<int>(growth.size()),
                 "resize preserves readable bytes") ||
        !require(hasContents(buffer, growth), "resized buffer preserves order")) {
        return 1;
    }

    const std::string reusable = "reuse";
    buffer.writeToBuffer(reusable.data(), static_cast<int>(reusable.size()));
    buffer.moveReadIndex(static_cast<int>(reusable.size()));
    if (!require(buffer.readAble() == 0, "consuming all bytes empties buffer") ||
        !require(buffer.readIndex() == buffer.writeIndex(),
                 "read and write indices meet after consumption")) {
        return 1;
    }

    const std::string final_write = "again";
    buffer.writeToBuffer(final_write.data(), static_cast<int>(final_write.size()));
    if (!require(hasContents(buffer, final_write), "buffer can be reused")) {
        return 1;
    }

    return 0;
}
