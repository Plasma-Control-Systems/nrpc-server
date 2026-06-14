#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "nrpc/arena.hpp"
#include "nrpc/protocol.hpp"
#include "nrpc/resource_manager.hpp"

namespace nrpc {

struct BackendError {
    std::uint32_t code;
    std::string message;
};

struct RequestContext {
    ClientId client_id;
    std::uint32_t timeout_ms;
    ResourceManager& resources;
};

using BackendResult = std::expected<std::vector<protocol::VariableView>, BackendError>;
using BackendHandler = std::function<BackendResult(
    const RequestContext&, std::span<const protocol::Variable>, Arena&)>;

class Dispatcher {
public:
    bool register_handler(std::uint8_t function_index, BackendHandler handler);
    [[nodiscard]] BackendResult dispatch(
        std::uint8_t function_index, const RequestContext& context,
        std::span<const protocol::Variable> arguments, Arena& arena) const;

private:
    std::unordered_map<std::uint8_t, BackendHandler> handlers_;
};

}  // namespace nrpc

