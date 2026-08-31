#include "nrpc/dispatcher.hpp"

#include <algorithm>
#include <exception>
#include <utility>

namespace nrpc {

bool Dispatcher::register_handler(std::uint8_t function_index, BackendHandler handler) {
    return handlers_.emplace(function_index, std::move(handler)).second;
}

std::vector<std::uint8_t> Dispatcher::function_indices() const {
    std::vector<std::uint8_t> result;
    result.reserve(handlers_.size());
    for (const auto& [index, handler] : handlers_) {
        static_cast<void>(handler);
        result.push_back(index);
    }
    std::ranges::sort(result);
    return result;
}

BackendResult Dispatcher::dispatch(
    std::uint8_t function_index, const RequestContext& context,
    std::span<const protocol::Variable> arguments, Arena& arena) const {
    const auto iterator = handlers_.find(function_index);
    if (iterator == handlers_.end()) {
        return std::unexpected(BackendError{1, "unknown function index"});
    }
    try {
        return iterator->second(context, arguments, arena);
    } catch (const std::exception& exception) {
        return std::unexpected(BackendError{2, exception.what()});
    } catch (...) {
        return std::unexpected(BackendError{2, "unknown backend exception"});
    }
}

}  // namespace nrpc
