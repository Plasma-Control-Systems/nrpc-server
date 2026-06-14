#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace nrpc::protocol {

inline constexpr std::uint8_t request_magic = 0xF0;
inline constexpr std::uint8_t version = 0x01;
inline constexpr std::size_t header_size = 16;
inline constexpr std::size_t variable_entry_size = 16;
inline constexpr std::uint32_t default_max_payload_size = 16U * 1024U * 1024U;

enum class DataType : std::uint8_t {
    null_value = 0x00,
    int8 = 0x01,
    uint8 = 0x02,
    int16 = 0x03,
    uint16 = 0x04,
    int32 = 0x05,
    uint32 = 0x06,
    int64 = 0x07,
    uint64 = 0x08,
    float32 = 0x09,
    float64 = 0x10,
    boolean = 0xA0,
    string = 0xA1,
    bytes = 0xA2,
};

enum class StatusCode : std::uint8_t { success = 0x00, error = 0x01 };

enum class ParseError {
    truncated_header,
    invalid_magic,
    unsupported_version,
    nonzero_reserved,
    payload_too_large,
    payload_size_mismatch,
    truncated_variable_table,
    invalid_data_type,
    invalid_element_size,
    invalid_null,
    invalid_boolean,
    invalid_string,
};

struct Variable {
    DataType type{DataType::null_value};
    std::uint32_t element_count{0};
    std::vector<std::byte> data;
};

struct VariableView {
    DataType type{DataType::null_value};
    std::uint32_t element_count{0};
    std::span<const std::byte> data;
};

struct Request {
    std::uint8_t function_index{0};
    std::uint32_t timeout_ms{0};
    std::vector<Variable> variables;
};

struct Response {
    StatusCode status{StatusCode::success};
    std::uint8_t function_index{0};
    std::uint32_t error_code{0};
    std::vector<VariableView> variables;
};

[[nodiscard]] std::expected<Request, ParseError> decode_request(
    std::span<const std::byte> packet,
    std::uint32_t max_payload_size = default_max_payload_size);
[[nodiscard]] std::vector<std::byte> encode_request(const Request& request);
[[nodiscard]] std::vector<std::byte> encode_response(const Response& response);
[[nodiscard]] std::uint32_t read_payload_size(std::span<const std::byte, header_size> header);
[[nodiscard]] std::string to_string(ParseError error);

}  // namespace nrpc::protocol

