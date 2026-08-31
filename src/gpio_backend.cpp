#include "nrpc/gpio_backend.hpp"

#include <atomic>
#include <string>
#include <utility>

#include "nrpc/error_codes.hpp"

namespace nrpc {

#if NRPC_HAS_LIBGPIOD
std::shared_ptr<GpioDriver> create_libgpiod_driver();
#endif

namespace {

std::uint64_t read_unsigned(std::span<const std::byte> data) {
    std::uint64_t value{};
    for (const auto byte : data) {
        value = (value << 8U) | std::to_integer<std::uint64_t>(byte);
    }
    return value;
}

void write_unsigned(std::span<std::byte> output, std::uint64_t value) {
    for (std::size_t index = 0; index < output.size(); ++index) {
        output[index] = static_cast<std::byte>(
            (value >> ((output.size() - index - 1U) * 8U)) & 0xffU);
    }
}

bool scalar(const protocol::Variable& value, protocol::DataType type,
            std::size_t size) {
    return value.type == type && value.element_count == 1U &&
           value.data.size() == size;
}

std::expected<std::string, BackendError> string_argument(
    const protocol::Variable& value) {
    if (value.type != protocol::DataType::string || value.data.empty() ||
        value.data.back() != std::byte{0}) {
        return std::unexpected(BackendError{
            gpio_error::invalid_arguments, "expected a string"});
    }
    return std::string(reinterpret_cast<const char*>(value.data.data()),
                       value.data.size() - 1U);
}

class UnavailableDriver final : public GpioDriver {
public:
    std::expected<std::unique_ptr<GpioLine>, BackendError> open(
        std::string_view, std::uint32_t, GpioDirection, bool, bool,
        std::string_view) override {
        return std::unexpected(BackendError{
            gpio_error::library_unavailable,
            "GPIO backend requires Linux and libgpiod development files"});
    }
};

std::atomic<std::uint64_t> next_handle{0x4750494f00000001ULL};

BackendResult handle_error() {
    return std::unexpected(BackendError{
        gpio_error::invalid_handle, "unknown or foreign GPIO line handle"});
}

}  // namespace

std::shared_ptr<GpioDriver> create_gpio_driver() {
#if NRPC_HAS_LIBGPIOD
    return create_libgpiod_driver();
#else
    return std::make_shared<UnavailableDriver>();
#endif
}

void register_gpio_backend(Dispatcher& dispatcher,
                           std::shared_ptr<GpioDriver> driver) {
    dispatcher.register_handler(
        gpio_function::open,
        [driver](const RequestContext& context,
                 std::span<const protocol::Variable> arguments,
                 Arena& arena) -> BackendResult {
            if (arguments.size() != 6U ||
                !scalar(arguments[1], protocol::DataType::uint32, 4U) ||
                !scalar(arguments[2], protocol::DataType::uint8, 1U) ||
                !scalar(arguments[3], protocol::DataType::boolean, 1U) ||
                !scalar(arguments[4], protocol::DataType::boolean, 1U)) {
                return std::unexpected(BackendError{
                    gpio_error::invalid_arguments,
                    "GPIO open expects chip, offset, direction, initial value, "
                    "active-low flag, and consumer"});
            }
            auto chip = string_argument(arguments[0]);
            auto consumer = string_argument(arguments[5]);
            const auto direction_value = read_unsigned(arguments[2].data);
            if (!chip || !consumer || chip->empty() || consumer->empty() ||
                direction_value >
                    static_cast<std::uint8_t>(GpioDirection::output)) {
                return std::unexpected(BackendError{
                    gpio_error::invalid_arguments,
                    "invalid GPIO chip, direction, or consumer"});
            }
            auto memory = arena.allocate(8U, 8U);
            if (!memory) {
                return std::unexpected(BackendError{
                    error_code::arena_exhausted,
                    "response arena exhausted"});
            }
            auto line = driver->open(
                *chip, static_cast<std::uint32_t>(read_unsigned(arguments[1].data)),
                static_cast<GpioDirection>(direction_value),
                arguments[3].data[0] == std::byte{1},
                arguments[4].data[0] == std::byte{1}, *consumer);
            if (!line) return std::unexpected(line.error());

            const auto handle = next_handle.fetch_add(1U);
            if (!context.resources.register_handle(
                    context.client_id, handle, std::move(*line))) {
                return std::unexpected(BackendError{
                    gpio_error::line_request_failed,
                    "GPIO line handle registration failed"});
            }
            write_unsigned(*memory, handle);
            return std::vector<protocol::VariableView>{{
                protocol::DataType::uint64, 1U, std::as_const(*memory)}};
        });

    dispatcher.register_handler(
        gpio_function::read,
        [](const RequestContext& context,
           std::span<const protocol::Variable> arguments,
           Arena& arena) -> BackendResult {
            if (arguments.size() != 1U ||
                !scalar(arguments[0], protocol::DataType::uint64, 8U)) {
                return std::unexpected(BackendError{
                    gpio_error::invalid_arguments,
                    "GPIO read expects one uint64 handle"});
            }
            auto memory = arena.allocate(1U, 1U);
            if (!memory) {
                return std::unexpected(BackendError{
                    error_code::arena_exhausted,
                    "response arena exhausted"});
            }
            auto result = context.resources.with_handle<GpioLine>(
                context.client_id, read_unsigned(arguments[0].data),
                [](GpioLine& line) { return line.read(); });
            if (!result) return handle_error();
            if (!*result) return std::unexpected(result->error());
            (*memory)[0] = **result ? std::byte{1} : std::byte{0};
            return std::vector<protocol::VariableView>{{
                protocol::DataType::boolean, 1U, std::as_const(*memory)}};
        });

    dispatcher.register_handler(
        gpio_function::write,
        [](const RequestContext& context,
           std::span<const protocol::Variable> arguments,
           Arena&) -> BackendResult {
            if (arguments.size() != 2U ||
                !scalar(arguments[0], protocol::DataType::uint64, 8U) ||
                !scalar(arguments[1], protocol::DataType::boolean, 1U)) {
                return std::unexpected(BackendError{
                    gpio_error::invalid_arguments,
                    "GPIO write expects a uint64 handle and bool value"});
            }
            auto result = context.resources.with_handle<GpioLine>(
                context.client_id, read_unsigned(arguments[0].data),
                [&](GpioLine& line) {
                    return line.write(arguments[1].data[0] == std::byte{1});
                });
            if (!result) return handle_error();
            if (!*result) return std::unexpected(result->error());
            return std::vector<protocol::VariableView>{};
        });

    dispatcher.register_handler(
        gpio_function::close,
        [](const RequestContext& context,
           std::span<const protocol::Variable> arguments,
           Arena&) -> BackendResult {
            if (arguments.size() != 1U ||
                !scalar(arguments[0], protocol::DataType::uint64, 8U)) {
                return std::unexpected(BackendError{
                    gpio_error::invalid_arguments,
                    "GPIO close expects one uint64 handle"});
            }
            const auto handle = read_unsigned(arguments[0].data);
            if (!context.resources.find<GpioLine>(context.client_id, handle) ||
                !context.resources.remove(context.client_id, handle)) {
                return handle_error();
            }
            return std::vector<protocol::VariableView>{};
        });
}

}  // namespace nrpc
