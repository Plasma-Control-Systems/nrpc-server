#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string_view>

#include <libserialport.h>

#include "nrpc/dispatcher.hpp"
#include "nrpc/error_codes.hpp"

namespace nrpc {

namespace serial_function {
inline constexpr std::uint8_t open = 32;
inline constexpr std::uint8_t write = 33;
inline constexpr std::uint8_t read = 34;
inline constexpr std::uint8_t close = 35;
}  // namespace serial_function

namespace serial_error {
inline constexpr std::uint32_t invalid_arguments = 3001;
inline constexpr std::uint32_t open_failed = 3002;
inline constexpr std::uint32_t configuration_failed = 3003;
inline constexpr std::uint32_t invalid_handle = 3004;
inline constexpr std::uint32_t write_failed = 3005;
inline constexpr std::uint32_t read_failed = 3006;
inline constexpr std::uint32_t operation_timeout = 3007;
inline constexpr std::uint32_t unsupported = 3008;
}  // namespace serial_error

enum class SerialParity : std::uint8_t {
    none = 0,
    odd = 1,
    even = 2,
    mark = 3,
    space = 4,
};

enum class SerialFlowControl : std::uint8_t {
    none = 0,
    software = 1,
    rts_cts = 2,
    dtr_dsr = 3,
};

struct SerialConfig {
    std::uint32_t baud_rate;
    std::uint8_t data_bits;
    SerialParity parity;
    std::uint8_t stop_bits;
    SerialFlowControl flow_control;
};

class SerialPort {
public:
    ~SerialPort();
    SerialPort(const SerialPort&) = delete;
    SerialPort& operator=(const SerialPort&) = delete;
    SerialPort(SerialPort&&) = delete;
    SerialPort& operator=(SerialPort&&) = delete;

    static std::expected<std::unique_ptr<SerialPort>, BackendError> open(
        std::string_view name, const SerialConfig& config);
    std::expected<std::size_t, BackendError> write(
        std::span<const std::byte> data, std::uint32_t timeout_ms);
    std::expected<std::size_t, BackendError> read(
        std::span<std::byte> output, std::uint32_t timeout_ms);
    void close() noexcept;

private:
    explicit SerialPort(sp_port* port) noexcept : port_(port) {}
    sp_port* port_{nullptr};
    bool opened_{false};
};

void register_serial_backend(Dispatcher& dispatcher);

}  // namespace nrpc
