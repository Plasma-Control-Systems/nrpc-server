#include "nrpc/lcard_backend.hpp"

#include <atomic>
#include <bit>
#include <cmath>
#include <concepts>
#include <cstring>
#include <limits>
#include <utility>

#include "nrpc/error_codes.hpp"

namespace nrpc {

#ifdef _WIN32
std::shared_ptr<LcardDriver> create_lcard_native_driver();
#endif

namespace {

constexpr std::uint32_t max_channels = 128;
constexpr std::uint32_t max_adc_samples = 131072;
constexpr std::uint32_t max_dac_frames = 262144;

std::uint64_t read_unsigned(std::span<const std::byte> data) {
    std::uint64_t value{};
    for (const auto byte : data)
        value = (value << 8U) | std::to_integer<std::uint64_t>(byte);
    return value;
}
double read_double(std::span<const std::byte> data) {
    return std::bit_cast<double>(read_unsigned(data));
}
void write_unsigned(std::span<std::byte> output, std::uint64_t value) {
    for (std::size_t i = 0; i < output.size(); ++i)
        output[i] = static_cast<std::byte>(
            (value >> ((output.size() - i - 1U) * 8U)) & 0xffU);
}
bool scalar(const protocol::Variable& value, protocol::DataType type,
            std::size_t width) {
    return value.type == type && value.element_count == 1U &&
           value.data.size() == width;
}
std::expected<std::string_view, BackendError> string_argument(
    const protocol::Variable& value) {
    if (value.type != protocol::DataType::string || value.data.empty() ||
        value.data.back() != std::byte{0})
        return std::unexpected(BackendError{lcard_error::invalid_arguments,
                                             "invalid string argument"});
    return std::string_view(reinterpret_cast<const char*>(value.data.data()),
                            value.data.size() - 1U);
}
template <typename T>
std::expected<std::vector<T>, BackendError> numeric_array(
    const protocol::Variable& value, protocol::DataType type,
    std::size_t width) {
    if (value.type != type ||
        value.data.size() != static_cast<std::size_t>(value.element_count) * width)
        return std::unexpected(BackendError{lcard_error::invalid_arguments,
                                             "invalid array argument"});
    std::vector<T> output(value.element_count);
    for (std::size_t i = 0; i < output.size(); ++i) {
        const auto bytes = std::span(value.data).subspan(i * width, width);
        if constexpr (std::same_as<T, double>)
            output[i] = read_double(bytes);
        else
            output[i] = static_cast<T>(read_unsigned(bytes));
    }
    return output;
}

std::expected<std::vector<LcardAdcChannel>, BackendError> channel_arguments(
    std::span<const protocol::Variable> args, std::size_t first) {
    if (args.size() < first + 4U)
        return std::unexpected(BackendError{lcard_error::invalid_arguments,
                                             "missing ADC channel arrays"});
    auto raw = numeric_array<std::uint8_t>(args[first], protocol::DataType::boolean, 1);
    auto channel = numeric_array<std::uint32_t>(args[first + 1U], protocol::DataType::uint32, 4);
    auto range = numeric_array<double>(args[first + 2U], protocol::DataType::float64, 8);
    auto mode = numeric_array<std::uint8_t>(args[first + 3U], protocol::DataType::uint8, 1);
    if (!raw || !channel || !range || !mode)
        return std::unexpected(BackendError{lcard_error::invalid_arguments,
                                             "invalid ADC channel arrays"});
    const auto count = raw->size();
    if (count == 0U || count > max_channels || channel->size() != count ||
        range->size() != count || mode->size() != count)
        return std::unexpected(BackendError{lcard_error::invalid_arguments,
                                             "ADC channel arrays have invalid lengths"});
    std::vector<LcardAdcChannel> output;
    output.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        if ((*raw)[i] > 1U || (*mode)[i] > 2U ||
            (!(*raw)[i] && (!std::isfinite((*range)[i]) || (*range)[i] <= 0.0)))
            return std::unexpected(BackendError{lcard_error::invalid_arguments,
                                                 "invalid ADC channel descriptor"});
        output.push_back(LcardAdcChannel{
            (*raw)[i] != 0U, (*channel)[i], (*range)[i],
            static_cast<LcardAdcInputMode>((*mode)[i])});
    }
    return output;
}

std::expected<std::uint64_t, BackendError> store_device(
    const RequestContext& context, std::unique_ptr<LcardDevice> device) {
    static std::atomic<std::uint64_t> next{0x4c43000000000001ULL};
    const auto handle = next.fetch_add(1U);
    if (!context.resources.register_handle(context.client_id, handle,
                                            std::move(device)))
        return std::unexpected(BackendError{lcard_error::open_failed,
                                             "device handle registration failed"});
    return handle;
}

std::expected<std::span<std::byte>, BackendError> allocate(
    Arena& arena, std::size_t size, std::size_t alignment) {
    auto memory = arena.allocate(size, alignment);
    if (!memory)
        return std::unexpected(BackendError{error_code::arena_exhausted,
                                             "response arena exhausted"});
    return *memory;
}

protocol::VariableView integer_view(protocol::DataType type,
                                    std::span<std::byte> memory,
                                    std::uint64_t value) {
    write_unsigned(memory, value);
    return {type, 1, std::as_const(memory)};
}

double timeout_seconds(const RequestContext& context) {
    return static_cast<double>(context.timeout_ms) / 1000.0;
}

template <typename Function>
auto with_device(const RequestContext& context, std::uint64_t handle,
                 Function&& function) {
    return context.resources.with_handle<LcardDevice>(
        context.client_id, handle, std::forward<Function>(function));
}

class UnavailableDriver final : public LcardDriver {
public:
    std::expected<std::unique_ptr<LcardDevice>, BackendError> open(
        std::uint32_t, std::string_view) override {
        return std::unexpected(BackendError{
            lcard_error::library_load_failed,
            "LCard LComp is available only on Windows"});
    }
};

}  // namespace

std::shared_ptr<LcardDriver> create_lcard_driver() {
#ifdef _WIN32
    return create_lcard_native_driver();
#else
    return std::make_shared<UnavailableDriver>();
#endif
}

void register_lcard_backend(Dispatcher& dispatcher,
                            std::shared_ptr<LcardDriver> driver) {
    dispatcher.register_handler(lcard_function::open,
        [driver](const RequestContext& context,
                 std::span<const protocol::Variable> args,
                 Arena& arena) -> BackendResult {
            if (args.size() != 2U ||
                !scalar(args[0], protocol::DataType::uint32, 4))
                return std::unexpected(BackendError{lcard_error::invalid_arguments,
                    "LCard open expects slot and firmware path"});
            auto firmware = string_argument(args[1]);
            if (!firmware) return std::unexpected(firmware.error());
            auto device = driver->open(
                static_cast<std::uint32_t>(read_unsigned(args[0].data)), *firmware);
            if (!device) return std::unexpected(device.error());
            const auto info = (*device)->info();
            auto handle = store_device(context, std::move(*device));
            if (!handle) return std::unexpected(handle.error());

            auto handle_mem = allocate(arena, 8, 8);
            auto type_mem = allocate(arena, 4, 4);
            auto name_mem = allocate(arena, info.board_name.size() + 1U, 1);
            auto revision_mem = allocate(arena, 1, 1);
            auto dac_mem = allocate(arena, 1, 1);
            auto capabilities_mem = allocate(arena, 4, 4);
            if (!handle_mem || !type_mem || !name_mem || !revision_mem ||
                !dac_mem || !capabilities_mem)
                return std::unexpected(BackendError{error_code::arena_exhausted,
                                                     "response arena exhausted"});
            std::memcpy(name_mem->data(), info.board_name.data(), info.board_name.size());
            name_mem->back() = std::byte{0};
            (*revision_mem)[0] = static_cast<std::byte>(info.revision);
            (*dac_mem)[0] = info.dac_present ? std::byte{1} : std::byte{0};
            return std::vector<protocol::VariableView>{
                integer_view(protocol::DataType::uint64, *handle_mem, *handle),
                integer_view(protocol::DataType::uint32, *type_mem, info.board_type),
                {protocol::DataType::string,
                 static_cast<std::uint32_t>(info.board_name.size() + 1U),
                 std::as_const(*name_mem)},
                {protocol::DataType::uint8, 1, std::as_const(*revision_mem)},
                {protocol::DataType::boolean, 1, std::as_const(*dac_mem)},
                integer_view(protocol::DataType::uint32, *capabilities_mem,
                             info.capabilities)};
        });

    dispatcher.register_handler(lcard_function::read_adc,
        [](const RequestContext& context,
           std::span<const protocol::Variable> args,
           Arena& arena) -> BackendResult {
            if (args.size() != 5U ||
                !scalar(args[0], protocol::DataType::uint64, 8))
                return std::unexpected(BackendError{lcard_error::invalid_arguments,
                    "LCard ADC read expects handle and one channel descriptor"});
            auto channels = channel_arguments(args, 1);
            if (!channels || channels->size() != 1U)
                return std::unexpected(channels ? BackendError{
                    lcard_error::invalid_arguments, "exactly one ADC channel is required"}
                    : channels.error());
            auto result = with_device(context, read_unsigned(args[0].data),
                [&](LcardDevice& device) {
                    return device.read_adc(channels->front(), timeout_seconds(context));
                });
            if (!result) return std::unexpected(BackendError{lcard_error::invalid_handle,
                                                              "unknown LCard device handle"});
            if (!*result) return std::unexpected(result->error());
            auto memory = allocate(arena, 2, 2);
            if (!memory) return std::unexpected(memory.error());
            return std::vector<protocol::VariableView>{integer_view(
                protocol::DataType::int16, *memory,
                std::bit_cast<std::uint16_t>(result->value()))};
        });

    dispatcher.register_handler(lcard_function::read_adc_frame,
        [](const RequestContext& context,
           std::span<const protocol::Variable> args,
           Arena& arena) -> BackendResult {
            if ((args.size() != 6U && args.size() != 7U) ||
                !scalar(args[0], protocol::DataType::uint64, 8) ||
                !scalar(args[5], protocol::DataType::float64, 8) ||
                (args.size() == 7U &&
                 !scalar(args[6], protocol::DataType::uint32, 4)))
                return std::unexpected(BackendError{lcard_error::invalid_arguments,
                    "LCard ADC frame expects handle, channel arrays, rate, and optional sample count"});
            auto channels = channel_arguments(args, 1);
            const auto rate = read_double(args[5].data);
            if (!channels) return std::unexpected(channels.error());
            const auto sample_count = args.size() == 7U
                ? static_cast<std::uint32_t>(read_unsigned(args[6].data))
                : static_cast<std::uint32_t>(channels->size());
            if (!std::isfinite(rate) || rate <= 0.0)
                return std::unexpected(BackendError{lcard_error::invalid_arguments,
                                                     "ADC rate must be positive"});
            if (sample_count == 0U || sample_count > max_adc_samples ||
                sample_count % channels->size() != 0U)
                return std::unexpected(BackendError{lcard_error::invalid_arguments,
                    "ADC sample count must contain complete channel scans and not exceed 131072"});
            auto result = with_device(context, read_unsigned(args[0].data),
                [&](LcardDevice& device) {
                    return device.read_adc_frame(*channels, sample_count, rate,
                                                 timeout_seconds(context));
                });
            if (!result) return std::unexpected(BackendError{lcard_error::invalid_handle,
                                                              "unknown LCard device handle"});
            if (!*result) return std::unexpected(result->error());
            auto codes = allocate(arena, result->value().codes.size() * 2U, 2);
            auto actual = allocate(arena, 8, 8);
            if (!codes || !actual)
                return std::unexpected(BackendError{error_code::arena_exhausted,
                                                     "response arena exhausted"});
            for (std::size_t i = 0; i < result->value().codes.size(); ++i)
                write_unsigned(codes->subspan(i * 2U, 2),
                    std::bit_cast<std::uint16_t>(result->value().codes[i]));
            write_unsigned(*actual,
                           std::bit_cast<std::uint64_t>(result->value().actual_rate_khz));
            return std::vector<protocol::VariableView>{
                {protocol::DataType::int16,
                 static_cast<std::uint32_t>(result->value().codes.size()),
                 std::as_const(*codes)},
                {protocol::DataType::float64, 1, std::as_const(*actual)}};
        });

    dispatcher.register_handler(lcard_function::write_dac,
        [](const RequestContext& context,
           std::span<const protocol::Variable> args, Arena&) -> BackendResult {
            if (args.size() != 3U ||
                !scalar(args[0], protocol::DataType::uint64, 8) ||
                !scalar(args[1], protocol::DataType::uint8, 1) ||
                !scalar(args[2], protocol::DataType::uint16, 2))
                return std::unexpected(BackendError{lcard_error::invalid_arguments,
                    "LCard DAC write expects handle, channel, and code"});
            auto result = with_device(context, read_unsigned(args[0].data),
                [&](LcardDevice& device) {
                    return device.write_dac(
                        static_cast<std::uint8_t>(read_unsigned(args[1].data)),
                        static_cast<std::uint16_t>(read_unsigned(args[2].data)),
                        timeout_seconds(context));
                });
            if (!result) return std::unexpected(BackendError{lcard_error::invalid_handle,
                                                              "unknown LCard device handle"});
            if (!*result) return std::unexpected(result->error());
            return std::vector<protocol::VariableView>{};
        });

    dispatcher.register_handler(lcard_function::write_dac_frames,
        [](const RequestContext& context,
           std::span<const protocol::Variable> args,
           Arena& arena) -> BackendResult {
            if (args.size() != 4U ||
                !scalar(args[0], protocol::DataType::uint64, 8) ||
                !scalar(args[3], protocol::DataType::float64, 8))
                return std::unexpected(BackendError{lcard_error::invalid_arguments,
                    "LCard DAC frames expect handle, two code arrays, and rate"});
            auto dac0 = numeric_array<std::uint16_t>(args[1], protocol::DataType::uint16, 2);
            auto dac1 = numeric_array<std::uint16_t>(args[2], protocol::DataType::uint16, 2);
            const auto rate = read_double(args[3].data);
            if (!dac0 || !dac1 || dac0->empty() || dac0->size() != dac1->size() ||
                dac0->size() > max_dac_frames || !std::isfinite(rate) || rate <= 0.0)
                return std::unexpected(BackendError{lcard_error::invalid_arguments,
                    "DAC arrays must have equal lengths and a positive rate"});
            auto result = with_device(context, read_unsigned(args[0].data),
                [&](LcardDevice& device) {
                    return device.write_dac_frames(*dac0, *dac1, rate,
                                                   timeout_seconds(context));
                });
            if (!result) return std::unexpected(BackendError{lcard_error::invalid_handle,
                                                              "unknown LCard device handle"});
            if (!*result) return std::unexpected(result->error());
            auto count = allocate(arena, 4, 4);
            auto actual = allocate(arena, 8, 8);
            if (!count || !actual)
                return std::unexpected(BackendError{error_code::arena_exhausted,
                                                     "response arena exhausted"});
            write_unsigned(*count, result->value().frames_written);
            write_unsigned(*actual,
                std::bit_cast<std::uint64_t>(result->value().actual_rate_khz));
            return std::vector<protocol::VariableView>{
                {protocol::DataType::uint32, 1, std::as_const(*count)},
                {protocol::DataType::float64, 1, std::as_const(*actual)}};
        });

    dispatcher.register_handler(lcard_function::close,
        [](const RequestContext& context,
           std::span<const protocol::Variable> args, Arena&) -> BackendResult {
            if (args.size() != 1U ||
                !scalar(args[0], protocol::DataType::uint64, 8))
                return std::unexpected(BackendError{lcard_error::invalid_arguments,
                                                     "LCard close expects a handle"});
            if (!context.resources.remove(context.client_id,
                                           read_unsigned(args[0].data)))
                return std::unexpected(BackendError{lcard_error::invalid_handle,
                                                     "unknown LCard device handle"});
            return std::vector<protocol::VariableView>{};
        });
}

}  // namespace nrpc
