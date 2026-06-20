#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string_view>

#include "nrpc/dispatcher.hpp"

namespace nrpc {

namespace nidaqmx_function {
inline constexpr std::uint8_t create_ai_voltage = 64;
inline constexpr std::uint8_t create_ai_thermocouple = 65;
inline constexpr std::uint8_t create_ao_voltage = 66;
inline constexpr std::uint8_t create_di = 67;
inline constexpr std::uint8_t create_do = 68;
inline constexpr std::uint8_t start = 69;
inline constexpr std::uint8_t read_analog = 70;
inline constexpr std::uint8_t write_analog = 71;
inline constexpr std::uint8_t read_digital = 72;
inline constexpr std::uint8_t write_digital = 73;
inline constexpr std::uint8_t close = 74;
}  // namespace nidaqmx_function

namespace nidaqmx_error {
inline constexpr std::uint32_t invalid_arguments = 5001;
inline constexpr std::uint32_t library_load_failed = 5002;
inline constexpr std::uint32_t task_creation_failed = 5003;
inline constexpr std::uint32_t invalid_handle = 5004;
inline constexpr std::uint32_t operation_failed = 5005;
inline constexpr std::uint32_t wrong_channel_type = 5006;
}  // namespace nidaqmx_error

enum class NidaqmxChannelType : std::uint8_t {
    analog_input,
    analog_output,
    digital_input,
    digital_output,
};

enum class NidaqmxTerminalConfig : std::int32_t {
    default_value = -1,
    rse = 10083,
    nrse = 10078,
    differential = 10106,
    pseudodifferential = 12529,
};

enum class NidaqmxThermocoupleType : std::int32_t {
    b = 10047, e = 10055, j = 10072, k = 10073,
    n = 10077, r = 10082, s = 10085, t = 10086,
};

enum class NidaqmxCjcSource : std::int32_t {
    built_in = 10200,
    constant_value = 10116,
    channel = 10113,
};

class NidaqmxTask {
public:
    virtual ~NidaqmxTask() = default;
    NidaqmxTask() = default;
    NidaqmxTask(const NidaqmxTask&) = delete;
    NidaqmxTask& operator=(const NidaqmxTask&) = delete;

    [[nodiscard]] virtual NidaqmxChannelType channel_type() const noexcept = 0;
    virtual std::expected<void, BackendError> start() = 0;
    virtual std::expected<std::size_t, BackendError> read_analog(
        std::span<double> values, std::uint32_t samples_per_channel,
        double timeout_seconds) = 0;
    virtual std::expected<std::size_t, BackendError> write_analog(
        std::span<const double> values, double timeout_seconds) = 0;
    virtual std::expected<std::size_t, BackendError> read_digital(
        std::span<std::uint32_t> values, std::uint32_t samples_per_channel,
        double timeout_seconds) = 0;
    virtual std::expected<std::size_t, BackendError> write_digital(
        std::span<const std::uint32_t> values, double timeout_seconds) = 0;
    virtual void close() noexcept = 0;
};

class NidaqmxDriver {
public:
    virtual ~NidaqmxDriver() = default;
    virtual std::expected<std::unique_ptr<NidaqmxTask>, BackendError>
    create_ai_voltage(std::string_view physical_channel, double minimum,
                      double maximum, NidaqmxTerminalConfig terminal) = 0;
    virtual std::expected<std::unique_ptr<NidaqmxTask>, BackendError>
    create_ai_thermocouple(
        std::string_view physical_channel, double minimum, double maximum,
        NidaqmxThermocoupleType type, NidaqmxCjcSource cjc_source,
        double cjc_value, std::string_view cjc_channel) = 0;
    virtual std::expected<std::unique_ptr<NidaqmxTask>, BackendError>
    create_ao_voltage(std::string_view physical_channel, double minimum,
                      double maximum) = 0;
    virtual std::expected<std::unique_ptr<NidaqmxTask>, BackendError>
    create_digital(std::string_view lines, bool output,
                   bool one_channel_for_all_lines) = 0;
};

std::shared_ptr<NidaqmxDriver> create_nidaqmx_driver();
void register_nidaqmx_backend(
    Dispatcher& dispatcher,
    std::shared_ptr<NidaqmxDriver> driver = create_nidaqmx_driver());

}  // namespace nrpc
