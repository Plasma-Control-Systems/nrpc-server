#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "nrpc/dispatcher.hpp"

namespace nrpc {

namespace spi_function {
inline constexpr std::uint8_t open = 52;
inline constexpr std::uint8_t transfer = 53;
inline constexpr std::uint8_t close = 54;
}  // namespace spi_function

namespace spi_error {
inline constexpr std::uint32_t invalid_arguments = 4501;
inline constexpr std::uint32_t unavailable = 4502;
inline constexpr std::uint32_t open_failed = 4503;
inline constexpr std::uint32_t configure_failed = 4504;
inline constexpr std::uint32_t invalid_handle = 4505;
inline constexpr std::uint32_t transfer_failed = 4506;
}  // namespace spi_error

struct SpiTransferOptions {
    std::uint32_t speed_hz{};
    std::uint16_t delay_us{};
    bool cs_change{};
};

class SpiDevice {
public:
    virtual ~SpiDevice() = default;
    SpiDevice(const SpiDevice&) = delete;
    SpiDevice& operator=(const SpiDevice&) = delete;

    virtual std::expected<std::vector<std::byte>, BackendError> transfer(
        std::span<const std::byte> transmit,
        const SpiTransferOptions& options) = 0;
    virtual void close() noexcept = 0;

protected:
    SpiDevice() = default;
};

class SpiDriver {
public:
    virtual ~SpiDriver() = default;
    virtual std::expected<std::unique_ptr<SpiDevice>, BackendError> open(
        std::string_view device, std::uint8_t mode,
        std::uint32_t max_speed_hz, std::uint8_t bits_per_word,
        bool lsb_first) = 0;
};

std::shared_ptr<SpiDriver> create_spi_driver();
void register_spi_backend(
    Dispatcher& dispatcher,
    std::shared_ptr<SpiDriver> driver = create_spi_driver());

}  // namespace nrpc
