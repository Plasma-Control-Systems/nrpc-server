#include "nrpc/serial_backend.hpp"

#include <atomic>
#include <limits>
#include <string>
#include <utility>

namespace nrpc {
namespace {

BackendError serial_failure(std::uint32_t code, std::string_view operation,
                            sp_return result) {
    if (result == SP_ERR_FAIL) {
        char* message = sp_last_error_message();
        std::string detail =
            message == nullptr ? "operating system error" : message;
        if (message != nullptr) {
            sp_free_error_message(message);
        }
        return BackendError{
            code, std::string(operation) + " failed: " + detail};
    }
    if (result == SP_ERR_SUPP) {
        return BackendError{
            serial_error::unsupported,
            std::string(operation) + " is not supported"};
    }
    if (result == SP_ERR_MEM) {
        return BackendError{
            code, std::string(operation) + " failed: out of memory"};
    }
    return BackendError{
        code, std::string(operation) + " failed: invalid arguments"};
}

std::uint64_t read_u64(std::span<const std::byte> data) {
    std::uint64_t value = 0;
    for (const auto byte : data) {
        value = (value << 8U) | std::to_integer<std::uint64_t>(byte);
    }
    return value;
}

std::uint32_t read_u32(std::span<const std::byte> data) {
    std::uint32_t value = 0;
    for (const auto byte : data) {
        value = (value << 8U) | std::to_integer<std::uint32_t>(byte);
    }
    return value;
}

void write_u64(std::span<std::byte> output, std::uint64_t value) {
    for (std::size_t index = 0; index < output.size(); ++index) {
        const auto shift =
            static_cast<unsigned>((output.size() - index - 1U) * 8U);
        output[index] =
            static_cast<std::byte>((value >> shift) & 0xFFU);
    }
}

void write_u32(std::span<std::byte> output, std::uint32_t value) {
    for (std::size_t index = 0; index < output.size(); ++index) {
        const auto shift =
            static_cast<unsigned>((output.size() - index - 1U) * 8U);
        output[index] =
            static_cast<std::byte>((value >> shift) & 0xFFU);
    }
}

bool is_scalar(const protocol::Variable& variable, protocol::DataType type,
               std::size_t size) {
    return variable.type == type && variable.element_count == 1 &&
           variable.data.size() == size;
}

std::expected<std::string, BackendError> parse_string(
    const protocol::Variable& variable) {
    if (variable.type != protocol::DataType::string ||
        variable.data.empty() || variable.data.back() != std::byte{0}) {
        return std::unexpected(BackendError{
            serial_error::invalid_arguments, "expected a string"});
    }
    return std::string(
        reinterpret_cast<const char*>(variable.data.data()),
        variable.data.size() - 1U);
}

std::expected<SerialParity, BackendError> parse_parity(std::uint8_t value) {
    if (value > static_cast<std::uint8_t>(SerialParity::space)) {
        return std::unexpected(BackendError{
            serial_error::invalid_arguments, "invalid serial parity"});
    }
    return static_cast<SerialParity>(value);
}

std::expected<SerialFlowControl, BackendError> parse_flow(std::uint8_t value) {
    if (value > static_cast<std::uint8_t>(SerialFlowControl::dtr_dsr)) {
        return std::unexpected(BackendError{
            serial_error::invalid_arguments, "invalid serial flow control"});
    }
    return static_cast<SerialFlowControl>(value);
}

sp_parity to_native(SerialParity parity) {
    return static_cast<sp_parity>(parity);
}

sp_flowcontrol to_native(SerialFlowControl flow) {
    switch (flow) {
        case SerialFlowControl::none: return SP_FLOWCONTROL_NONE;
        case SerialFlowControl::software: return SP_FLOWCONTROL_XONXOFF;
        case SerialFlowControl::rts_cts: return SP_FLOWCONTROL_RTSCTS;
        case SerialFlowControl::dtr_dsr: return SP_FLOWCONTROL_DTRDSR;
    }
    return SP_FLOWCONTROL_NONE;
}

BackendResult scalar_u64(Arena& arena, std::uint64_t value) {
    auto memory = arena.allocate(8, 8);
    if (!memory) {
        return std::unexpected(BackendError{
            error_code::arena_exhausted, "response arena exhausted"});
    }
    write_u64(*memory, value);
    return std::vector<protocol::VariableView>{
        {protocol::DataType::uint64, 1, std::as_const(*memory)}};
}

BackendResult scalar_u32(Arena& arena, std::uint32_t value) {
    auto memory = arena.allocate(4, 4);
    if (!memory) {
        return std::unexpected(BackendError{
            error_code::arena_exhausted, "response arena exhausted"});
    }
    write_u32(*memory, value);
    return std::vector<protocol::VariableView>{
        {protocol::DataType::uint32, 1, std::as_const(*memory)}};
}

std::atomic<std::uint64_t> next_handle_id{1};

}  // namespace

SerialPort::~SerialPort() { close(); }

std::expected<std::unique_ptr<SerialPort>, BackendError> SerialPort::open(
    std::string_view name, const SerialConfig& config) {
    sp_port* port = nullptr;
    const std::string port_name(name);
    auto result = sp_get_port_by_name(port_name.c_str(), &port);
    if (result < SP_OK) {
        return std::unexpected(
            serial_failure(serial_error::open_failed,
                           "sp_get_port_by_name", result));
    }
    auto serial = std::unique_ptr<SerialPort>(new SerialPort(port));
    result = sp_open(port, SP_MODE_READ_WRITE);
    if (result < SP_OK) {
        return std::unexpected(
            serial_failure(serial_error::open_failed, "sp_open", result));
    }
    if (config.baud_rate >
        static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        return std::unexpected(BackendError{
            serial_error::invalid_arguments, "baud rate is too large"});
    }
    serial->opened_ = true;
    const auto configure = [&](sp_return setting,
                               std::string_view operation)
        -> std::expected<void, BackendError> {
        if (setting < SP_OK) {
            return std::unexpected(serial_failure(
                serial_error::configuration_failed, operation, setting));
        }
        return {};
    };
    if (auto setting = configure(
            sp_set_baudrate(port, static_cast<int>(config.baud_rate)),
            "sp_set_baudrate");
        !setting) {
        return std::unexpected(setting.error());
    }
    if (auto setting = configure(
            sp_set_bits(port, static_cast<int>(config.data_bits)),
            "sp_set_bits");
        !setting) {
        return std::unexpected(setting.error());
    }
    if (auto setting = configure(
            sp_set_parity(port, to_native(config.parity)),
            "sp_set_parity");
        !setting) {
        return std::unexpected(setting.error());
    }
    if (auto setting = configure(
            sp_set_stopbits(port, static_cast<int>(config.stop_bits)),
            "sp_set_stopbits");
        !setting) {
        return std::unexpected(setting.error());
    }
    if (auto setting = configure(
            sp_set_flowcontrol(port, to_native(config.flow_control)),
            "sp_set_flowcontrol");
        !setting) {
        return std::unexpected(setting.error());
    }
    return serial;
}

std::expected<std::size_t, BackendError> SerialPort::write(
    std::span<const std::byte> data, std::uint32_t timeout_ms) {
    const auto result = sp_blocking_write(
        port_, data.data(), data.size(), timeout_ms);
    if (result < SP_OK) {
        return std::unexpected(serial_failure(
            serial_error::write_failed, "sp_blocking_write", result));
    }
    if (result == 0 && !data.empty() && timeout_ms != 0) {
        return std::unexpected(BackendError{
            serial_error::operation_timeout, "serial write timed out"});
    }
    return static_cast<std::size_t>(result);
}

std::expected<std::size_t, BackendError> SerialPort::read(
    std::span<std::byte> output, std::uint32_t timeout_ms) {
    const auto result = sp_blocking_read_next(
        port_, output.data(), output.size(), timeout_ms);
    if (result < SP_OK) {
        return std::unexpected(serial_failure(
            serial_error::read_failed, "sp_blocking_read_next", result));
    }
    if (result == 0 && timeout_ms != 0) {
        return std::unexpected(BackendError{
            serial_error::operation_timeout, "serial read timed out"});
    }
    return static_cast<std::size_t>(result);
}

void SerialPort::close() noexcept {
    if (port_ != nullptr) {
        if (opened_) {
            sp_close(port_);
        }
        sp_free_port(port_);
        port_ = nullptr;
        opened_ = false;
    }
}

void register_serial_backend(Dispatcher& dispatcher) {
    dispatcher.register_handler(
        serial_function::open,
        [](const RequestContext& context,
           std::span<const protocol::Variable> arguments,
           Arena& arena) -> BackendResult {
            if (arguments.size() != 6 ||
                !is_scalar(arguments[1], protocol::DataType::uint32, 4) ||
                !is_scalar(arguments[2], protocol::DataType::uint8, 1) ||
                !is_scalar(arguments[3], protocol::DataType::uint8, 1) ||
                !is_scalar(arguments[4], protocol::DataType::uint8, 1) ||
                !is_scalar(arguments[5], protocol::DataType::uint8, 1)) {
                return std::unexpected(BackendError{
                    serial_error::invalid_arguments,
                    "serial open expects name, baud, bits, parity, stop bits, flow"});
            }
            auto name = parse_string(arguments[0]);
            auto parity = parse_parity(
                std::to_integer<std::uint8_t>(arguments[3].data[0]));
            auto flow = parse_flow(
                std::to_integer<std::uint8_t>(arguments[5].data[0]));
            if (!name) {
                return std::unexpected(name.error());
            }
            if (!parity) {
                return std::unexpected(parity.error());
            }
            if (!flow) {
                return std::unexpected(flow.error());
            }
            const SerialConfig config{
                .baud_rate = read_u32(arguments[1].data),
                .data_bits =
                    std::to_integer<std::uint8_t>(arguments[2].data[0]),
                .parity = *parity,
                .stop_bits =
                    std::to_integer<std::uint8_t>(arguments[4].data[0]),
                .flow_control = *flow,
            };
            if (config.baud_rate == 0 ||
                config.data_bits < 5 || config.data_bits > 8 ||
                (config.stop_bits != 1 && config.stop_bits != 2)) {
                return std::unexpected(BackendError{
                    serial_error::invalid_arguments,
                    "invalid serial configuration"});
            }
            auto port = SerialPort::open(*name, config);
            if (!port) {
                return std::unexpected(port.error());
            }
            const auto handle_id = next_handle_id.fetch_add(1);
            if (!context.resources.register_handle(
                    context.client_id, handle_id, std::move(*port))) {
                return std::unexpected(BackendError{
                    serial_error::open_failed,
                    "serial handle registration failed"});
            }
            return scalar_u64(arena, handle_id);
        });

    dispatcher.register_handler(
        serial_function::write,
        [](const RequestContext& context,
           std::span<const protocol::Variable> arguments,
           Arena& arena) -> BackendResult {
            if (arguments.size() != 2 ||
                !is_scalar(arguments[0], protocol::DataType::uint64, 8) ||
                arguments[1].type != protocol::DataType::bytes) {
                return std::unexpected(BackendError{
                    serial_error::invalid_arguments,
                    "serial write expects handle and bytes"});
            }
            const auto handle_id = read_u64(arguments[0].data);
            auto result = context.resources.with_handle<SerialPort>(
                context.client_id, handle_id,
                [&](SerialPort& port) {
                    return port.write(
                        arguments[1].data, context.timeout_ms);
                });
            if (!result) {
                return std::unexpected(BackendError{
                    serial_error::invalid_handle,
                    "unknown serial handle"});
            }
            if (!*result) {
                return std::unexpected(result->error());
            }
            return scalar_u32(
                arena, static_cast<std::uint32_t>(**result));
        });

    dispatcher.register_handler(
        serial_function::read,
        [](const RequestContext& context,
           std::span<const protocol::Variable> arguments,
           Arena& arena) -> BackendResult {
            if (arguments.size() != 2 ||
                !is_scalar(arguments[0], protocol::DataType::uint64, 8) ||
                !is_scalar(arguments[1], protocol::DataType::uint32, 4)) {
                return std::unexpected(BackendError{
                    serial_error::invalid_arguments,
                    "serial read expects handle and max size"});
            }
            const auto handle_id = read_u64(arguments[0].data);
            const auto max_size = read_u32(arguments[1].data);
            if (max_size == 0) {
                return std::unexpected(BackendError{
                    serial_error::invalid_arguments,
                    "serial read size must be nonzero"});
            }
            auto memory = arena.allocate(max_size, 1);
            if (!memory) {
                return std::unexpected(BackendError{
                    error_code::arena_exhausted,
                    "response arena exhausted"});
            }
            auto result = context.resources.with_handle<SerialPort>(
                context.client_id, handle_id,
                [&](SerialPort& port) {
                    return port.read(*memory, context.timeout_ms);
                });
            if (!result) {
                return std::unexpected(BackendError{
                    serial_error::invalid_handle,
                    "unknown serial handle"});
            }
            if (!*result) {
                return std::unexpected(result->error());
            }
            return std::vector<protocol::VariableView>{{
                protocol::DataType::bytes,
                static_cast<std::uint32_t>(**result),
                std::as_const(*memory).first(**result),
            }};
        });

    dispatcher.register_handler(
        serial_function::close,
        [](const RequestContext& context,
           std::span<const protocol::Variable> arguments,
           Arena&) -> BackendResult {
            if (arguments.size() != 1 ||
                !is_scalar(arguments[0], protocol::DataType::uint64, 8)) {
                return std::unexpected(BackendError{
                    serial_error::invalid_arguments,
                    "serial close expects a handle"});
            }
            if (!context.resources.remove(
                    context.client_id, read_u64(arguments[0].data))) {
                return std::unexpected(BackendError{
                    serial_error::invalid_handle,
                    "unknown serial handle"});
            }
            return std::vector<protocol::VariableView>{};
        });
}

}  // namespace nrpc
