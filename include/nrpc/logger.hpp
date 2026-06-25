#pragma once

#include <cstddef>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <ostream>
#include <span>
#include <string_view>
#include <vector>

namespace nrpc {

enum class LogLevel { error, warn, info, debug };

class Logger {
public:
    explicit Logger(std::ostream& output, LogLevel minimum = LogLevel::info,
                    bool packet_dump_enabled = false);
    Logger(std::initializer_list<std::reference_wrapper<std::ostream>> outputs,
           LogLevel minimum = LogLevel::info, bool packet_dump_enabled = false);

    void log(LogLevel level, std::string_view message);
    void packet(LogLevel level, std::string_view event, std::span<const std::byte> bytes,
                std::string_view fields);

private:
    std::vector<std::ostream*> outputs_;
    LogLevel minimum_;
    bool packet_dump_enabled_;
    std::mutex mutex_;
};

}  // namespace nrpc
