#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string_view>

#include "nrpc/dispatcher.hpp"

namespace nrpc {

namespace lcard_function {
inline constexpr std::uint8_t open = 48;
inline constexpr std::uint8_t io_async = 49;
inline constexpr std::uint8_t slot_info = 50;
inline constexpr std::uint8_t load_bios = 51;
inline constexpr std::uint8_t close = 52;
}  // namespace lcard_function

namespace lcard_error {
inline constexpr std::uint32_t invalid_arguments = 4001;
inline constexpr std::uint32_t library_load_failed = 4002;
inline constexpr std::uint32_t open_failed = 4003;
inline constexpr std::uint32_t invalid_handle = 4004;
inline constexpr std::uint32_t operation_failed = 4005;
inline constexpr std::uint32_t unsupported = 4006;
inline constexpr std::uint32_t operation_timeout = 4007;
}  // namespace lcard_error

enum class LCardAsyncOperation : std::uint32_t {
    adc_config = 3,
    ttl_config = 4,
    dac_config = 5,
    adc_input = 6,
    ttl_input = 7,
    ttl_output = 8,
    dac_output = 9,
    frequency_input = 10,
};

enum class LCardBoardType : std::uint32_t {
    pci_a = 5,
    pci_b = 6,
    pci_c = 14,
    l791 = 19,
    e440 = 30,
    e140 = 31,
    e2010 = 32,
    e154 = 38,
    e2010b = 39,
    e310 = 77,
};

struct LCardSlotInfo {
    std::uint32_t board_type;
    std::uint32_t dsp_type;
    std::uint32_t irq;
};

struct LCardAsyncParameters {
    LCardAsyncOperation operation;
    double rate;
    std::uint32_t mode;
    std::uint32_t channel_count;
    std::span<const std::uint32_t> channels;
    std::span<const std::uint32_t> data;
};

struct LCardAsyncResult {
    double rate;
    std::uint32_t channel_count;
    std::array<std::uint32_t, 128> data;
};

class LCardSession {
public:
    virtual ~LCardSession() = default;
    LCardSession() = default;
    LCardSession(const LCardSession&) = delete;
    LCardSession& operator=(const LCardSession&) = delete;

    virtual std::expected<LCardSlotInfo, BackendError> slot_info() = 0;
    virtual std::expected<void, BackendError> load_bios(
        std::string_view path) = 0;
    virtual std::expected<LCardAsyncResult, BackendError> io_async(
        const LCardAsyncParameters& parameters) = 0;
    virtual void close() noexcept = 0;
};

class LCardDriver {
public:
    virtual ~LCardDriver() = default;

    virtual std::expected<std::unique_ptr<LCardSession>, BackendError> open(
        std::uint32_t slot, LCardBoardType expected_board,
        std::string_view bios_path) = 0;
};

std::shared_ptr<LCardDriver> create_lcard_driver();
void register_lcard_backend(
    Dispatcher& dispatcher,
    std::shared_ptr<LCardDriver> driver = create_lcard_driver());

}  // namespace nrpc
