#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <string_view>

#include "nrpc/dispatcher.hpp"

namespace nrpc {

namespace gpio_function {
inline constexpr std::uint8_t open = 48;
inline constexpr std::uint8_t read = 49;
inline constexpr std::uint8_t write = 50;
inline constexpr std::uint8_t close = 51;
}  // namespace gpio_function

namespace gpio_error {
inline constexpr std::uint32_t invalid_arguments = 4001;
inline constexpr std::uint32_t library_unavailable = 4002;
inline constexpr std::uint32_t chip_open_failed = 4003;
inline constexpr std::uint32_t line_request_failed = 4004;
inline constexpr std::uint32_t invalid_handle = 4005;
inline constexpr std::uint32_t read_failed = 4006;
inline constexpr std::uint32_t write_failed = 4007;
}  // namespace gpio_error

enum class GpioDirection : std::uint8_t {
    input = 0,
    output = 1,
};

class GpioLine {
public:
    virtual ~GpioLine() = default;
    GpioLine(const GpioLine&) = delete;
    GpioLine& operator=(const GpioLine&) = delete;

    virtual std::expected<bool, BackendError> read() = 0;
    virtual std::expected<void, BackendError> write(bool value) = 0;
    virtual void close() noexcept = 0;

protected:
    GpioLine() = default;
};

class GpioDriver {
public:
    virtual ~GpioDriver() = default;
    virtual std::expected<std::unique_ptr<GpioLine>, BackendError> open(
        std::string_view chip, std::uint32_t offset, GpioDirection direction,
        bool initial_value, bool active_low, std::string_view consumer) = 0;
};

std::shared_ptr<GpioDriver> create_gpio_driver();
void register_gpio_backend(
    Dispatcher& dispatcher,
    std::shared_ptr<GpioDriver> driver = create_gpio_driver());

}  // namespace nrpc
