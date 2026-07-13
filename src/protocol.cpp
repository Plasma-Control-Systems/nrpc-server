#include "nrpc/protocol.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>

namespace nrpc::protocol {
namespace {

std::uint32_t read_u32(const std::byte* input) {
    return (std::to_integer<std::uint32_t>(input[0]) << 24U) |
           (std::to_integer<std::uint32_t>(input[1]) << 16U) |
           (std::to_integer<std::uint32_t>(input[2]) << 8U) |
           std::to_integer<std::uint32_t>(input[3]);
}

void append_u32(std::vector<std::byte>& output, std::uint32_t value) {
    output.push_back(static_cast<std::byte>((value >> 24U) & 0xFFU));
    output.push_back(static_cast<std::byte>((value >> 16U) & 0xFFU));
    output.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
    output.push_back(static_cast<std::byte>(value & 0xFFU));
}

std::optional<std::size_t> element_size(DataType type) {
    switch (type) {
        case DataType::null_value: return 0;
        case DataType::int8:
        case DataType::uint8:
        case DataType::boolean:
        case DataType::string:
        case DataType::bytes: return 1;
        case DataType::int16:
        case DataType::uint16: return 2;
        case DataType::int32:
        case DataType::uint32:
        case DataType::float32: return 4;
        case DataType::int64:
        case DataType::uint64:
        case DataType::float64: return 8;
    }
    return std::nullopt;
}

template <typename VariableType>
std::expected<void, ParseError> validate_variable(const VariableType& variable) {
    const auto size = element_size(variable.type);
    if (!size.has_value()) {
        return std::unexpected(ParseError::invalid_data_type);
    }
    if (variable.type == DataType::null_value) {
        if (variable.element_count != 0 || !variable.data.empty()) {
            return std::unexpected(ParseError::invalid_null);
        }
        return {};
    }
    const auto expected = static_cast<std::uint64_t>(variable.element_count) * *size;
    if (expected != variable.data.size()) {
        return std::unexpected(ParseError::invalid_element_size);
    }
    if (variable.type == DataType::boolean &&
        std::ranges::any_of(variable.data, [](std::byte value) {
            return value != std::byte{0} && value != std::byte{1};
        })) {
        return std::unexpected(ParseError::invalid_boolean);
    }
    if (variable.type == DataType::string &&
        (variable.data.empty() || variable.data.back() != std::byte{0})) {
        return std::unexpected(ParseError::invalid_string);
    }
    return {};
}

template <typename VariableRange>
std::uint32_t payload_size(const VariableRange& variables) {
    std::uint64_t size = variables.size() * variable_entry_size;
    for (const auto& variable : variables) {
        size += variable.data.size();
    }
    if (size > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("nRPC payload exceeds uint32");
    }
    return static_cast<std::uint32_t>(size);
}

template <typename VariableRange>
void append_variables(std::vector<std::byte>& output, const VariableRange& variables) {
    for (const auto& variable : variables) {
        output.push_back(static_cast<std::byte>(variable.type));
        output.insert(output.end(), 3, std::byte{0});
        append_u32(output, static_cast<std::uint32_t>(variable.data.size()));
        append_u32(output, variable.element_count);
        output.insert(output.end(), 4, std::byte{0});
    }
    for (const auto& variable : variables) {
        output.insert(output.end(), variable.data.begin(), variable.data.end());
    }
}

}  // namespace

std::uint32_t read_payload_size(std::span<const std::byte, header_size> header) {
    return read_u32(header.data() + 4);
}

std::expected<Request, ParseError> decode_request(
    std::span<const std::byte> packet, std::uint32_t max_payload_size) {
    if (packet.size() < header_size) {
        return std::unexpected(ParseError::truncated_header);
    }
    if (packet[0] != static_cast<std::byte>(request_magic)) {
        return std::unexpected(ParseError::invalid_magic);
    }
    if (packet[1] != static_cast<std::byte>(version)) {
        return std::unexpected(ParseError::unsupported_version);
    }
    if (std::ranges::any_of(packet.subspan(12, 4),
                            [](std::byte value) { return value != std::byte{0}; })) {
        return std::unexpected(ParseError::nonzero_reserved);
    }
    const auto count = std::to_integer<std::uint8_t>(packet[3]);
    const auto declared_payload = read_u32(packet.data() + 4);
    if (declared_payload > max_payload_size) {
        return std::unexpected(ParseError::payload_too_large);
    }
    if (packet.size() != header_size + declared_payload) {
        return std::unexpected(ParseError::payload_size_mismatch);
    }
    const auto table_size = static_cast<std::size_t>(count) * variable_entry_size;
    if (table_size > declared_payload) {
        return std::unexpected(ParseError::truncated_variable_table);
    }

    Request request{
        .function_index = std::to_integer<std::uint8_t>(packet[2]),
        .timeout_ms = read_u32(packet.data() + 8),
        .variables = {},
    };
    request.variables.reserve(count);
    std::size_t data_offset = header_size + table_size;
    for (std::size_t index = 0; index < count; ++index) {
        const auto* entry = packet.data() + header_size + index * variable_entry_size;
        if (entry[1] != std::byte{0} || entry[2] != std::byte{0} ||
            entry[3] != std::byte{0} ||
            std::ranges::any_of(std::span(entry + 12, 4),
                                [](std::byte value) { return value != std::byte{0}; })) {
            return std::unexpected(ParseError::nonzero_reserved);
        }
        const auto data_size = read_u32(entry + 4);
        if (data_size > packet.size() - data_offset) {
            return std::unexpected(ParseError::payload_size_mismatch);
        }
        Variable variable{
            .type = static_cast<DataType>(std::to_integer<std::uint8_t>(entry[0])),
            .element_count = read_u32(entry + 8),
            .data = std::vector<std::byte>(
                packet.begin() + static_cast<std::ptrdiff_t>(data_offset),
                packet.begin() + static_cast<std::ptrdiff_t>(data_offset + data_size)),
        };
        if (const auto valid = validate_variable(variable); !valid) {
            return std::unexpected(valid.error());
        }
        data_offset += data_size;
        request.variables.push_back(std::move(variable));
    }
    if (data_offset != packet.size()) {
        return std::unexpected(ParseError::payload_size_mismatch);
    }
    return request;
}

std::vector<std::byte> encode_request(const Request& request) {
    if (request.variables.size() > std::numeric_limits<std::uint8_t>::max()) {
        throw std::length_error("nRPC request has more than 255 variables");
    }
    for (const auto& variable : request.variables) {
        if (!validate_variable(variable)) {
            throw std::invalid_argument("invalid nRPC request variable");
        }
    }
    const auto size = payload_size(request.variables);
    std::vector<std::byte> output;
    output.reserve(header_size + size);
    output.push_back(static_cast<std::byte>(request_magic));
    output.push_back(static_cast<std::byte>(version));
    output.push_back(static_cast<std::byte>(request.function_index));
    output.push_back(static_cast<std::byte>(request.variables.size()));
    append_u32(output, size);
    append_u32(output, request.timeout_ms);
    output.insert(output.end(), 4, std::byte{0});
    append_variables(output, request.variables);
    return output;
}

std::vector<std::byte> encode_response(const Response& response) {
    std::vector<std::byte> error_message;
    std::array<VariableView, 1> error_variables{};
    std::span<const VariableView> variables = response.variables;
    if (response.status == StatusCode::error && !response.error_message.empty()) {
        error_message.reserve(response.error_message.size() + 1U);
        for (const auto character : response.error_message) {
            error_message.push_back(static_cast<std::byte>(character));
        }
        error_message.push_back(std::byte{0});
        error_variables[0] = VariableView{
            DataType::string,
            static_cast<std::uint32_t>(error_message.size()),
            error_message,
        };
        variables = error_variables;
    }
    const auto count = variables.size();
    if (count > std::numeric_limits<std::uint8_t>::max()) {
        throw std::length_error("nRPC response has more than 255 variables");
    }
    if (response.status == StatusCode::success ||
        response.status == StatusCode::error) {
        for (const auto& variable : variables) {
            if (!validate_variable(variable)) {
                throw std::invalid_argument("invalid nRPC response variable");
            }
        }
    }
    const auto size = payload_size(variables);
    std::vector<std::byte> output;
    output.reserve(header_size + size);
    output.push_back(static_cast<std::byte>(response.status));
    output.push_back(static_cast<std::byte>(version));
    output.push_back(static_cast<std::byte>(response.function_index));
    output.push_back(static_cast<std::byte>(count));
    append_u32(output, size);
    append_u32(output, response.status == StatusCode::error ? response.error_code : 0U);
    output.insert(output.end(), 4, std::byte{0});
    append_variables(output, variables);
    return output;
}

std::string to_string(ParseError error) {
    switch (error) {
        case ParseError::truncated_header: return "truncated header";
        case ParseError::invalid_magic: return "invalid request magic";
        case ParseError::unsupported_version: return "unsupported protocol version";
        case ParseError::nonzero_reserved: return "reserved bytes must be zero";
        case ParseError::payload_too_large: return "payload exceeds configured limit";
        case ParseError::payload_size_mismatch: return "payload size mismatch";
        case ParseError::truncated_variable_table: return "truncated variable table";
        case ParseError::invalid_data_type: return "invalid data type";
        case ParseError::invalid_element_size: return "element count and data size mismatch";
        case ParseError::invalid_null: return "invalid null variable";
        case ParseError::invalid_boolean: return "invalid boolean value";
        case ParseError::invalid_string: return "string is not null terminated";
    }
    return "unknown parse error";
}

}  // namespace nrpc::protocol
