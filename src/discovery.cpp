#include "nrpc/discovery.hpp"

#include "nrpc/error_codes.hpp"

#include <cstddef>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace nrpc {
namespace {

std::expected<std::span<std::byte>, BackendError> copy_to_arena(
    Arena& arena, std::span<const std::byte> value) {
    auto memory = arena.allocate(value.size(), 1);
    if (!memory) {
        return std::unexpected(BackendError{
            error_code::arena_exhausted, "response arena exhausted"});
    }
    std::ranges::copy(value, memory->begin());
    return *memory;
}

std::expected<std::span<std::byte>, BackendError> copy_string_to_arena(
    Arena& arena, std::string_view value) {
    std::vector<std::byte> encoded;
    encoded.reserve(value.size() + 1);
    for (const char character : value) {
        encoded.push_back(static_cast<std::byte>(character));
    }
    encoded.push_back(std::byte{0});
    return copy_to_arena(arena, encoded);
}

}  // namespace

bool register_discovery_handler(
    Dispatcher& dispatcher, std::string_view platform,
    std::string_view architecture, std::string_view server_version) {
    return dispatcher.register_handler(
        discovery_function_index,
        [&dispatcher, platform = std::string(platform),
         architecture = std::string(architecture),
         server_version = std::string(server_version)](
            const RequestContext&, std::span<const protocol::Variable> arguments,
            Arena& arena) -> BackendResult {
            if (!arguments.empty()) {
                return std::unexpected(BackendError{
                    4, "server info expects no arguments"});
            }

            auto version = copy_string_to_arena(arena, server_version);
            auto platform_value = copy_string_to_arena(arena, platform);
            auto architecture_value = copy_string_to_arena(arena, architecture);
            const auto indices = dispatcher.function_indices();
            auto index_bytes = copy_to_arena(
                arena, std::as_bytes(std::span<const std::uint8_t>(indices)));
            auto protocol_version = arena.allocate(1, 1);
            if (!version || !platform_value || !architecture_value ||
                !index_bytes || !protocol_version) {
                return std::unexpected(BackendError{
                    error_code::arena_exhausted, "response arena exhausted"});
            }
            (*protocol_version)[0] = static_cast<std::byte>(protocol::version);

            return std::vector<protocol::VariableView>{
                {protocol::DataType::string,
                 static_cast<std::uint32_t>(version->size()),
                 std::as_const(*version)},
                {protocol::DataType::uint8, 1, std::as_const(*protocol_version)},
                {protocol::DataType::string,
                 static_cast<std::uint32_t>(platform_value->size()),
                 std::as_const(*platform_value)},
                {protocol::DataType::string,
                 static_cast<std::uint32_t>(architecture_value->size()),
                 std::as_const(*architecture_value)},
                {protocol::DataType::uint8,
                 static_cast<std::uint32_t>(indices.size()),
                 std::as_const(*index_bytes)},
            };
        });
}

}  // namespace nrpc
