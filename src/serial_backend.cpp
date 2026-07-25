#include "nrpc/serial_backend.hpp"

#include <atomic>
#include <string>
#include <utility>

namespace nrpc {
namespace {

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
            auto response = arena.allocate(8, 8);
            if (!response) {
                return std::unexpected(BackendError{
                    error_code::arena_exhausted, "response arena exhausted"});
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
            write_u64(*response, handle_id);
            return std::vector<protocol::VariableView>{
                {protocol::DataType::uint64, 1, std::as_const(*response)}};
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
