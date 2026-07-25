#pragma once

#include <chrono>
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
                    bool packet_dump_enabled = false,
                    bool rpc_timing_enabled = false);
    Logger(std::initializer_list<std::reference_wrapper<std::ostream>> outputs,
           LogLevel minimum = LogLevel::info, bool packet_dump_enabled = false,
           bool rpc_timing_enabled = false);

    void log(LogLevel level, std::string_view message);
    void packet(LogLevel level, std::string_view event, std::span<const std::byte> bytes,
                std::string_view fields);
    void rpc_timing(std::string_view fields,
                    std::chrono::microseconds duration);
    [[nodiscard]] bool packet_dump_enabled() const noexcept;
    [[nodiscard]] bool rpc_timing_enabled() const noexcept;

private:
    std::vector<std::ostream*> outputs_;
    LogLevel minimum_;
    bool packet_dump_enabled_;
    bool rpc_timing_enabled_;
    std::mutex mutex_;
};

}  // namespace nrpc
