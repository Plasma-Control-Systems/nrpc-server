#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include "nrpc/discovery.hpp"
#include "nrpc/version.hpp"

namespace {

std::string decode_string(const nrpc::protocol::VariableView& variable) {
    EXPECT_EQ(variable.type, nrpc::protocol::DataType::string);
    EXPECT_EQ(variable.element_count, variable.data.size());
    EXPECT_FALSE(variable.data.empty());
    EXPECT_EQ(variable.data.back(), std::byte{0});
    std::string result;
    result.reserve(variable.data.size() - 1);
    for (const auto value : variable.data.first(variable.data.size() - 1)) {
        result.push_back(static_cast<char>(std::to_integer<unsigned char>(value)));
    }
    return result;
}

}  // namespace

TEST(Discovery, ReturnsVersionPlatformArchitectureAndSortedFunctions) {
    nrpc::Dispatcher dispatcher;
    ASSERT_TRUE(dispatcher.register_handler(
        17, [](const nrpc::RequestContext&, auto, nrpc::Arena&) {
            return nrpc::BackendResult(
                std::vector<nrpc::protocol::VariableView>{});
        }));
    ASSERT_TRUE(nrpc::register_discovery_handler(
        dispatcher, "linux", "x86_64", nrpc::version));
    ASSERT_TRUE(dispatcher.register_handler(
        0, [](const nrpc::RequestContext&, auto, nrpc::Arena&) {
            return nrpc::BackendResult(
                std::vector<nrpc::protocol::VariableView>{});
        }));

    nrpc::ResourceManager resources;
    nrpc::Arena arena(1024);
    const nrpc::RequestContext context{1, 100, resources};
    const auto result = dispatcher.dispatch(
        nrpc::discovery_function_index, context, {}, arena);

    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 5U);
    EXPECT_EQ(decode_string((*result)[0]), nrpc::version);
    EXPECT_EQ((*result)[1].type, nrpc::protocol::DataType::uint8);
    ASSERT_EQ((*result)[1].data.size(), 1U);
    EXPECT_EQ((*result)[1].data[0], std::byte{nrpc::protocol::version});
    EXPECT_EQ(decode_string((*result)[2]), "linux");
    EXPECT_EQ(decode_string((*result)[3]), "x86_64");
    EXPECT_EQ((*result)[4].type, nrpc::protocol::DataType::uint8);
    EXPECT_EQ((*result)[4].element_count, 3U);
    const std::vector expected_indices{
        std::byte{0}, std::byte{2}, std::byte{17}};
    EXPECT_TRUE(std::ranges::equal((*result)[4].data, expected_indices));
}

TEST(Discovery, RejectsArguments) {
    nrpc::Dispatcher dispatcher;
    ASSERT_TRUE(nrpc::register_discovery_handler(
        dispatcher, "windows", "AMD64", "1.0.0"));
    nrpc::ResourceManager resources;
    nrpc::Arena arena(128);
    const nrpc::RequestContext context{1, 100, resources};
    const nrpc::protocol::Variable argument{
        nrpc::protocol::DataType::null_value, 0, {}};

    const auto result = dispatcher.dispatch(
        nrpc::discovery_function_index, context,
        std::span<const nrpc::protocol::Variable>(&argument, 1), arena);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, 4U);
}
