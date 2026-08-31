#include "nrpc/spi_backend.hpp"

#include <algorithm>
#include <atomic>
#include <string>
#include <utility>

#include "nrpc/error_codes.hpp"

namespace nrpc {

#if NRPC_HAS_SPIDEV
std::shared_ptr<SpiDriver> create_spidev_driver();
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
            spi_error::invalid_arguments, "expected a string"});
    }
    return std::string(reinterpret_cast<const char*>(value.data.data()),
                       value.data.size() - 1U);
}

class UnavailableDriver final : public SpiDriver {
public:
    std::expected<std::unique_ptr<SpiDevice>, BackendError> open(
        std::string_view, std::uint8_t, std::uint32_t, std::uint8_t,
        bool) override {
        return std::unexpected(BackendError{
            spi_error::unavailable,
            "SPI backend requires Linux with the spidev userspace API"});
    }
};

std::atomic<std::uint64_t> next_handle{0x5350490000000001ULL};

BackendResult handle_error() {
    return std::unexpected(BackendError{
        spi_error::invalid_handle, "unknown or foreign SPI device handle"});
}

}  // namespace

std::shared_ptr<SpiDriver> create_spi_driver() {
#if NRPC_HAS_SPIDEV
    return create_spidev_driver();
#else
    return std::make_shared<UnavailableDriver>();
#endif
}

void register_spi_backend(Dispatcher& dispatcher,
                          std::shared_ptr<SpiDriver> driver) {
    dispatcher.register_handler(
        spi_function::open,
        [driver](const RequestContext& context,
                 std::span<const protocol::Variable> arguments,
                 Arena& arena) -> BackendResult {
            if (arguments.size() != 5U ||
                !scalar(arguments[1], protocol::DataType::uint8, 1U) ||
                !scalar(arguments[2], protocol::DataType::uint32, 4U) ||
                !scalar(arguments[3], protocol::DataType::uint8, 1U) ||
                !scalar(arguments[4], protocol::DataType::boolean, 1U)) {
                return std::unexpected(BackendError{
                    spi_error::invalid_arguments,
                    "SPI open expects device, mode, maximum speed, bits per "
                    "word, and LSB-first flag"});
            }
            auto device = string_argument(arguments[0]);
            const auto mode = read_unsigned(arguments[1].data);
            const auto speed = read_unsigned(arguments[2].data);
            const auto bits = read_unsigned(arguments[3].data);
            if (!device || device->empty() || mode > 3U || speed == 0U ||
                bits == 0U) {
                return std::unexpected(BackendError{
                    spi_error::invalid_arguments,
                    "invalid SPI device, mode, speed, or bits per word"});
            }
            auto memory = arena.allocate(8U, 8U);
            if (!memory) {
                return std::unexpected(BackendError{
                    error_code::arena_exhausted,
                    "response arena exhausted"});
            }
            auto opened = driver->open(
                *device, static_cast<std::uint8_t>(mode),
                static_cast<std::uint32_t>(speed),
                static_cast<std::uint8_t>(bits),
                arguments[4].data[0] == std::byte{1});
            if (!opened) return std::unexpected(opened.error());

            const auto handle = next_handle.fetch_add(1U);
            if (!context.resources.register_handle(
                    context.client_id, handle, std::move(*opened))) {
                return std::unexpected(BackendError{
                    spi_error::open_failed,
                    "SPI device handle registration failed"});
            }
            write_unsigned(*memory, handle);
            return std::vector<protocol::VariableView>{{
                protocol::DataType::uint64, 1U, std::as_const(*memory)}};
        });

    dispatcher.register_handler(
        spi_function::transfer,
        [](const RequestContext& context,
           std::span<const protocol::Variable> arguments,
           Arena& arena) -> BackendResult {
            if (arguments.size() != 5U ||
                !scalar(arguments[0], protocol::DataType::uint64, 8U) ||
                arguments[1].type != protocol::DataType::bytes ||
                arguments[1].data.empty() ||
                !scalar(arguments[2], protocol::DataType::uint32, 4U) ||
                !scalar(arguments[3], protocol::DataType::uint16, 2U) ||
                !scalar(arguments[4], protocol::DataType::boolean, 1U)) {
                return std::unexpected(BackendError{
                    spi_error::invalid_arguments,
                    "SPI transfer expects handle, non-empty bytes, speed, "
                    "delay, and CS-change flag"});
            }
            auto memory = arena.allocate(arguments[1].data.size(), 1U);
            if (!memory) {
                return std::unexpected(BackendError{
                    error_code::arena_exhausted,
                    "response arena exhausted"});
            }
            const SpiTransferOptions options{
                static_cast<std::uint32_t>(read_unsigned(arguments[2].data)),
                static_cast<std::uint16_t>(read_unsigned(arguments[3].data)),
                arguments[4].data[0] == std::byte{1}};
            auto result = context.resources.with_handle<SpiDevice>(
                context.client_id, read_unsigned(arguments[0].data),
                [&](SpiDevice& device) {
                    return device.transfer(arguments[1].data, options);
                });
            if (!result) return handle_error();
            if (!*result) return std::unexpected(result->error());
            if ((*result)->size() != memory->size()) {
                return std::unexpected(BackendError{
                    spi_error::transfer_failed,
                    "SPI driver returned an invalid receive length"});
            }
            std::ranges::copy(**result, memory->begin());
            return std::vector<protocol::VariableView>{{
                protocol::DataType::bytes,
                static_cast<std::uint32_t>(memory->size()),
                std::as_const(*memory)}};
        });

    dispatcher.register_handler(
        spi_function::close,
        [](const RequestContext& context,
           std::span<const protocol::Variable> arguments,
           Arena&) -> BackendResult {
            if (arguments.size() != 1U ||
                !scalar(arguments[0], protocol::DataType::uint64, 8U)) {
                return std::unexpected(BackendError{
                    spi_error::invalid_arguments,
                    "SPI close expects one uint64 handle"});
            }
            const auto handle = read_unsigned(arguments[0].data);
            if (!context.resources.find<SpiDevice>(context.client_id, handle) ||
                !context.resources.remove(context.client_id, handle)) {
                return handle_error();
            }
            return std::vector<protocol::VariableView>{};
        });
}

}  // namespace nrpc
