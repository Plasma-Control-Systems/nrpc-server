#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "nrpc/protocol.hpp"

namespace {

template <typename T>
std::vector<std::byte> network_bytes(T value) {
    std::vector<std::byte> bytes(sizeof(T));
    auto native = std::bit_cast<std::array<std::byte, sizeof(T)>>(value);
    if constexpr (std::endian::native == std::endian::little) {
        std::reverse(native.begin(), native.end());
    }
    std::ranges::copy(native, bytes.begin());
    return bytes;
}

}  // namespace

TEST(Protocol, RoundTripsRequestInNetworkByteOrder) {
    nrpc::protocol::Request request{
        .function_index = 7,
        .timeout_ms = 0x01020304,
        .variables = {
            {nrpc::protocol::DataType::uint32, 1, network_bytes<std::uint32_t>(0x11223344)},
            {nrpc::protocol::DataType::string, 3,
             {std::byte{'o'}, std::byte{'k'}, std::byte{0}}},
        },
    };

    const auto packet = nrpc::protocol::encode_request(request);
    EXPECT_EQ(packet[0], std::byte{0xF0});
    EXPECT_EQ(packet[8], std::byte{0x01});
    EXPECT_EQ(packet[9], std::byte{0x02});
    EXPECT_EQ(packet[10], std::byte{0x03});
    EXPECT_EQ(packet[11], std::byte{0x04});

    const auto decoded = nrpc::protocol::decode_request(packet);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->function_index, 7);
    EXPECT_EQ(decoded->timeout_ms, 0x01020304U);
    ASSERT_EQ(decoded->variables.size(), 2U);
    EXPECT_EQ(decoded->variables[0].data, request.variables[0].data);
}

TEST(Protocol, RejectsMalformedPackets) {
    EXPECT_EQ(nrpc::protocol::decode_request(std::array<std::byte, 3>{}).error(),
              nrpc::protocol::ParseError::truncated_header);

    nrpc::protocol::Request request{
        .variables = {{nrpc::protocol::DataType::boolean, 1, {std::byte{1}}}},
    };
    auto packet = nrpc::protocol::encode_request(request);
    packet[0] = std::byte{0};
    EXPECT_EQ(nrpc::protocol::decode_request(packet).error(),
              nrpc::protocol::ParseError::invalid_magic);

    packet = nrpc::protocol::encode_request(request);
    packet.pop_back();
    EXPECT_EQ(nrpc::protocol::decode_request(packet).error(),
              nrpc::protocol::ParseError::payload_size_mismatch);
}

TEST(Protocol, EncodesErrorWithoutVariables) {
    const nrpc::protocol::Response response{
        .status = nrpc::protocol::StatusCode::error,
        .function_index = 9,
        .error_code = 42,
        .variables = {},
    };
    const auto packet = nrpc::protocol::encode_response(response);
    ASSERT_EQ(packet.size(), nrpc::protocol::header_size);
    EXPECT_EQ(packet[0], std::byte{1});
    EXPECT_EQ(packet[2], std::byte{9});
    EXPECT_EQ(packet[3], std::byte{0});
    EXPECT_EQ(packet[11], std::byte{42});
}

TEST(Protocol, RejectsInvalidResponseVariableMetadata) {
    const std::array data{std::byte{1}, std::byte{2}};
    const nrpc::protocol::Response response{
        .variables = {{
            nrpc::protocol::DataType::uint32,
            1,
            data,
        }},
    };
    EXPECT_THROW(
        {
            const auto packet = nrpc::protocol::encode_response(response);
            (void)packet;
        },
        std::invalid_argument);
}
