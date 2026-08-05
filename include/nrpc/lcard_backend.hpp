#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "nrpc/dispatcher.hpp"

namespace nrpc {

namespace lcard_function {
inline constexpr std::uint8_t open = 80;
inline constexpr std::uint8_t read_adc = 81;
inline constexpr std::uint8_t read_adc_frame = 82;
inline constexpr std::uint8_t write_dac = 83;
inline constexpr std::uint8_t write_dac_frames = 84;
inline constexpr std::uint8_t close = 85;
inline constexpr std::uint8_t read_adc_samples = 86;
}  // namespace lcard_function

namespace lcard_error {
inline constexpr std::uint32_t invalid_arguments = 6001;
inline constexpr std::uint32_t library_load_failed = 6002;
inline constexpr std::uint32_t open_failed = 6003;
inline constexpr std::uint32_t invalid_handle = 6004;
inline constexpr std::uint32_t unsupported = 6005;
inline constexpr std::uint32_t operation_failed = 6006;
inline constexpr std::uint32_t timeout = 6007;
}  // namespace lcard_error

namespace lcard_capability {
inline constexpr std::uint32_t read_adc = 1U << 0U;
inline constexpr std::uint32_t read_adc_frame = 1U << 1U;
inline constexpr std::uint32_t write_dac = 1U << 2U;
inline constexpr std::uint32_t write_dac_frames = 1U << 3U;
inline constexpr std::uint32_t read_adc_samples = 1U << 4U;
inline constexpr std::uint32_t hardware_trigger = 1U << 5U;
}  // namespace lcard_capability

enum class LcardAdcInputMode : std::uint8_t {
    differential = 0,
    common_ground = 1,
    zero = 2,
};

struct LcardAdcChannel {
    bool raw{};
    std::uint32_t channel{};  // Physical channel or complete vendor logical code.
    double range{};           // Positive full-scale voltage; ignored for raw.
    LcardAdcInputMode mode{LcardAdcInputMode::differential};
};

enum class LcardAdcTriggerMode : std::uint8_t {
    disabled = 0,
    external_rising = 1,
    external_falling = 2,
    analog_rising = 3,
    analog_falling = 4,
    analog_above = 5,
    analog_below = 6,
};

struct LcardAdcTrigger {
    LcardAdcTriggerMode mode{LcardAdcTriggerMode::disabled};
    LcardAdcChannel channel{};
    std::int16_t threshold{};
};

struct LcardDeviceInfo {
    std::uint32_t board_type{};
    std::string board_name;
    std::uint8_t revision{};
    bool dac_present{};
    std::uint32_t capabilities{};
};

struct LcardAdcFrame {
    std::vector<std::int16_t> codes;
    double actual_rate_khz{};
};

struct LcardDacWriteResult {
    std::uint32_t frames_written{};
    double actual_rate_khz{};
};

class LcardDevice {
public:
    virtual ~LcardDevice() = default;
    LcardDevice() = default;
    LcardDevice(const LcardDevice&) = delete;
    LcardDevice& operator=(const LcardDevice&) = delete;

    [[nodiscard]] virtual const LcardDeviceInfo& info() const noexcept = 0;
    virtual std::expected<std::int16_t, BackendError> read_adc(
        const LcardAdcChannel& channel, double timeout_seconds) = 0;
    virtual std::expected<LcardAdcFrame, BackendError> read_adc_frame(
        std::span<const LcardAdcChannel> channels, std::uint32_t sample_count,
        double rate_khz, const LcardAdcTrigger& trigger,
        double timeout_seconds) = 0;
    virtual std::expected<void, BackendError> write_dac(
        std::uint8_t channel, std::uint16_t code, double timeout_seconds) = 0;
    virtual std::expected<LcardDacWriteResult, BackendError> write_dac_frames(
        std::span<const std::uint16_t> dac0,
        std::span<const std::uint16_t> dac1, double frame_rate_khz,
        double timeout_seconds) = 0;
    virtual void close() noexcept = 0;
};

class LcardDriver {
public:
    virtual ~LcardDriver() = default;
    virtual std::expected<std::unique_ptr<LcardDevice>, BackendError> open(
        std::uint32_t slot, std::string_view firmware_path) = 0;
};

std::shared_ptr<LcardDriver> create_lcard_driver();
void register_lcard_backend(
    Dispatcher& dispatcher,
    std::shared_ptr<LcardDriver> driver = create_lcard_driver());

}  // namespace nrpc
