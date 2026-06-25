#include "nrpc/logger.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <thread>

namespace nrpc {
namespace {

constexpr std::string_view level_name(LogLevel level) {
    switch (level) {
        case LogLevel::error: return "ERROR";
        case LogLevel::warn: return "WARN";
        case LogLevel::info: return "INFO";
        case LogLevel::debug: return "DEBUG";
    }
    return "UNKNOWN";
}

}  // namespace

Logger::Logger(std::ostream& output, LogLevel minimum, bool packet_dump_enabled)
    : outputs_{&output},
      minimum_(minimum),
      packet_dump_enabled_(packet_dump_enabled) {}

Logger::Logger(
    std::initializer_list<std::reference_wrapper<std::ostream>> outputs,
    LogLevel minimum, bool packet_dump_enabled)
    : minimum_(minimum), packet_dump_enabled_(packet_dump_enabled) {
    outputs_.reserve(outputs.size());
    for (auto output : outputs) {
        outputs_.push_back(&output.get());
    }
}

void Logger::log(LogLevel level, std::string_view message) {
    if (level > minimum_) {
        return;
    }
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::system_clock::to_time_t(now);
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  now.time_since_epoch()) %
                              1000;
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    std::ostringstream line;
    line << std::put_time(&local, "%Y-%m-%dT%H:%M:%S") << '.'
         << std::setfill('0') << std::setw(3) << milliseconds.count()
         << " [" << level_name(level) << "] [thread "
         << std::this_thread::get_id() << "] " << message << '\n';
    std::scoped_lock lock(mutex_);
    for (auto* output : outputs_) {
        *output << line.str();
        output->flush();
    }
}

void Logger::packet(LogLevel level, std::string_view event,
                    std::span<const std::byte> bytes, std::string_view fields) {
    if (!packet_dump_enabled_) {
        return;
    }
    std::ostringstream dump;
    dump << event << ' ' << fields << " bytes=";
    for (const auto byte : bytes) {
        dump << std::hex << std::setfill('0') << std::setw(2)
             << std::to_integer<unsigned int>(byte);
    }
    log(level, dump.str());
}

}  // namespace nrpc
