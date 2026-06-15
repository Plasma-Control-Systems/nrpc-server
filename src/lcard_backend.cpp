#include "nrpc/lcard_backend.hpp"

#include <atomic>
#include <bit>
#include <filesystem>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#define NOMINMAX
#include <windows.h>

#include "nrpc/error_codes.hpp"

namespace nrpc {
namespace {

using NativeHandle = std::uintptr_t;

inline constexpr std::uint32_t native_success = 0;
inline constexpr std::uint32_t native_not_supported = 1;
inline constexpr std::uint32_t native_timeout = 5;

#pragma pack(push, 1)
struct NativeSlotParameters {
    std::uint32_t base;
    std::uint32_t base_length;
    std::uint32_t base1;
    std::uint32_t base1_length;
    std::uint32_t memory;
    std::uint32_t memory_length;
    std::uint32_t memory1;
    std::uint32_t memory1_length;
    std::uint32_t irq;
    std::uint32_t board_type;
    std::uint32_t dsp_type;
    std::uint32_t dma;
    std::uint32_t dma_dac;
    std::uint32_t data_register;
    std::uint32_t idma_register;
    std::uint32_t command_register;
    std::uint32_t irq_reset;
    std::uint32_t data_array;
    std::uint32_t ready_register;
    std::uint32_t config_register;
};

struct NativeAsyncParameters {
    std::uint32_t type;
    std::uint32_t fifo;
    std::uint32_t irq_step;
    std::uint32_t pages;
    double rate_hz;
    std::uint32_t rate;
    std::uint32_t channel_count;
    std::uint32_t channels[128];
    std::uint32_t data[128];
    std::uint32_t mode;
};
#pragma pack(pop)

static_assert(sizeof(NativeSlotParameters) == 80);
static_assert(sizeof(NativeAsyncParameters) == 1060);

constexpr std::size_t max_async_values = 128;

std::uint64_t read_u64(std::span<const std::byte> data) {
    std::uint64_t value = 0;
    for (const auto byte : data) {
        value = (value << 8U) | std::to_integer<std::uint64_t>(byte);
    }
    return value;
}

std::uint32_t read_u32(std::span<const std::byte> data) {
    std::uint32_t value = 0;
    for (const auto byte : data) {
        value = (value << 8U) | std::to_integer<std::uint32_t>(byte);
    }
    return value;
}

double read_f64(std::span<const std::byte> data) {
    return std::bit_cast<double>(read_u64(data));
}

void write_u64(std::span<std::byte> output, std::uint64_t value) {
    for (std::size_t index = 0; index < output.size(); ++index) {
        const auto shift =
            static_cast<unsigned>((output.size() - index - 1U) * 8U);
        output[index] = static_cast<std::byte>((value >> shift) & 0xFFU);
    }
}

void write_u32(std::span<std::byte> output, std::uint32_t value) {
    for (std::size_t index = 0; index < output.size(); ++index) {
        const auto shift =
            static_cast<unsigned>((output.size() - index - 1U) * 8U);
        output[index] = static_cast<std::byte>((value >> shift) & 0xFFU);
    }
}

void write_f64(std::span<std::byte> output, double value) {
    write_u64(output, std::bit_cast<std::uint64_t>(value));
}

bool is_scalar(const protocol::Variable& variable, protocol::DataType type,
               std::size_t size) {
    return variable.type == type && variable.element_count == 1 &&
           variable.data.size() == size;
}

std::expected<std::string, BackendError> parse_string(
    const protocol::Variable& variable) {
    if (variable.type != protocol::DataType::string ||
        variable.data.empty() || variable.data.back() != std::byte{0}) {
        return std::unexpected(BackendError{
            lcard_error::invalid_arguments, "expected a string"});
    }
    return std::string(
        reinterpret_cast<const char*>(variable.data.data()),
        variable.data.size() - 1U);
}

std::expected<std::vector<std::uint32_t>, BackendError> parse_u32_array(
    const protocol::Variable& variable, std::string_view name) {
    if (variable.type != protocol::DataType::uint32 ||
        variable.element_count > max_async_values ||
        variable.data.size() !=
            static_cast<std::size_t>(variable.element_count) * 4U) {
        return std::unexpected(BackendError{
            lcard_error::invalid_arguments,
            std::string(name) + " must be a uint32 array of at most 128 values"});
    }
    std::vector<std::uint32_t> values;
    values.reserve(variable.element_count);
    for (std::size_t offset = 0; offset < variable.data.size(); offset += 4U) {
        values.push_back(read_u32(
            std::span(variable.data).subspan(offset, 4U)));
    }
    return values;
}

std::expected<LCardAsyncOperation, BackendError> parse_operation(
    std::uint32_t value) {
    if (value < static_cast<std::uint32_t>(LCardAsyncOperation::adc_config) ||
        value >
            static_cast<std::uint32_t>(LCardAsyncOperation::frequency_input)) {
        return std::unexpected(BackendError{
            lcard_error::invalid_arguments,
            "invalid LCard asynchronous operation"});
    }
    return static_cast<LCardAsyncOperation>(value);
}

std::expected<LCardBoardType, BackendError> parse_board(
    std::uint32_t value) {
    switch (static_cast<LCardBoardType>(value)) {
        case LCardBoardType::pci_a:
        case LCardBoardType::pci_b:
        case LCardBoardType::pci_c:
        case LCardBoardType::l791:
        case LCardBoardType::e440:
        case LCardBoardType::e140:
        case LCardBoardType::e2010:
        case LCardBoardType::e154:
        case LCardBoardType::e2010b:
        case LCardBoardType::e310:
            return static_cast<LCardBoardType>(value);
    }
    return std::unexpected(BackendError{
        lcard_error::invalid_arguments, "unsupported LCard board type"});
}

std::expected<void, BackendError> validate_async(
    LCardBoardType board, LCardAsyncOperation operation,
    std::uint32_t channel_count, std::span<const std::uint32_t> channels,
    std::span<const std::uint32_t> data, std::uint32_t mode) {
    const auto invalid = [](std::string message) {
        return std::expected<void, BackendError>(
            std::unexpected(BackendError{
                lcard_error::invalid_arguments, std::move(message)}));
    };
    const bool e310 = board == LCardBoardType::e310;
    if (operation == LCardAsyncOperation::adc_config) {
        if (!e310 || !channels.empty() || !data.empty() ||
            (channel_count & ~0xF0U) != 0 ||
            (mode != 0 && mode != 0x10000U)) {
            return invalid("invalid E310 ADC configuration");
        }
        return {};
    }
    if (operation == LCardAsyncOperation::adc_input) {
        if (board == LCardBoardType::e2010 ||
            board == LCardBoardType::e2010b) {
            return invalid("E2010 does not support asynchronous ADC input");
        }
        if (e310) {
            if (channel_count != 4 || !channels.empty() || !data.empty()) {
                return invalid("E310 ADC input returns four configured channels");
            }
        } else if (
            channel_count != 1 || channels.size() != 1 || !data.empty()) {
            return invalid("ADC input expects one channel");
        }
        return {};
    }
    if (operation == LCardAsyncOperation::ttl_config) {
        if (!data.empty() || !channels.empty()) {
            return invalid("TTL configuration does not accept arrays");
        }
        if ((board == LCardBoardType::pci_a ||
             board == LCardBoardType::pci_b)) {
            return invalid("TTL configuration is supported only by PCI_C");
        }
        if (e310) {
            if ((channel_count & ~0x7FFU) != 0 || mode > 1) {
                return invalid("invalid E310 TTL configuration");
            }
        } else if (channel_count != 0) {
            return invalid("TTL configuration uses mode only on this board");
        }
        return {};
    }
    if (operation == LCardAsyncOperation::ttl_input) {
        if (channel_count != 1 || !channels.empty() || !data.empty()) {
            return invalid("TTL input returns one value");
        }
        return {};
    }
    if (operation == LCardAsyncOperation::ttl_output) {
        if (channel_count != 1 || !channels.empty() || data.size() != 1) {
            return invalid("TTL output expects one value");
        }
        return {};
    }
    if (operation == LCardAsyncOperation::dac_output) {
        if (e310) {
            return invalid("selected board does not support asynchronous DAC output");
        }
        if (board == LCardBoardType::l791) {
            if (channel_count != 2 || channels.size() != 2 ||
                data.size() != 2 || mode != 0) {
                return invalid("L791 DAC output expects two channels and values");
            }
        } else if (board == LCardBoardType::e140 && mode == 2) {
            if (channel_count != 1 || !channels.empty() || data.size() != 2) {
                return invalid("E140 dual DAC output expects two values");
            }
        } else if (
            channel_count != 1 || !channels.empty() || data.size() != 1 ||
            mode > 1) {
            return invalid("invalid asynchronous DAC output");
        } else if (
            board == LCardBoardType::e154 &&
            (mode != 0 || data.front() > 0xFFU)) {
            return invalid("E154 DAC output expects one 8-bit value");
        }
        return {};
    }
    return invalid("operation is not exposed for the selected board");
}

BackendResult scalar_u64(Arena& arena, std::uint64_t value) {
    auto memory = arena.allocate(8, 8);
    if (!memory) {
        return std::unexpected(BackendError{
            error_code::arena_exhausted, "response arena exhausted"});
    }
    write_u64(*memory, value);
    return std::vector<protocol::VariableView>{
        {protocol::DataType::uint64, 1, std::as_const(*memory)}};
}

BackendResult uint32_values(
    Arena& arena, std::span<const std::uint32_t> values) {
    auto memory = arena.allocate(values.size() * 4U, 4);
    if (!memory) {
        return std::unexpected(BackendError{
            error_code::arena_exhausted, "response arena exhausted"});
    }
    for (std::size_t index = 0; index < values.size(); ++index) {
        write_u32(memory->subspan(index * 4U, 4U), values[index]);
    }
    return std::vector<protocol::VariableView>{{
        protocol::DataType::uint32,
        static_cast<std::uint32_t>(values.size()),
        std::as_const(*memory),
    }};
}

BackendError native_failure(
    std::string_view operation, std::uint32_t native_error) {
    std::uint32_t code = lcard_error::operation_failed;
    if (native_error == native_not_supported) {
        code = lcard_error::unsupported;
    } else if (native_error == native_timeout) {
        code = lcard_error::operation_timeout;
    }
    return BackendError{
        code,
        std::string(operation) + " failed with LCard error " +
            std::to_string(native_error)};
}

std::filesystem::path lcard_library_directory() {
    const auto length =
        GetEnvironmentVariableW(L"NRPC_LCARD_LIBRARY_DIR", nullptr, 0);
    if (length != 0) {
        std::wstring override_path(length, L'\0');
        GetEnvironmentVariableW(
            L"NRPC_LCARD_LIBRARY_DIR", override_path.data(), length);
        override_path.resize(length - 1U);
        return override_path;
    }
    return NRPC_LCARD_LIBRARY_DIR;
}

template <typename Function>
std::expected<Function, BackendError> load_function(
    HMODULE module, const char* name) {
    const auto address = GetProcAddress(module, name);
    if (address == nullptr) {
        return std::unexpected(BackendError{
            lcard_error::library_load_failed,
            std::string("wlcomp is missing ") + name});
    }
    return reinterpret_cast<Function>(address);
}

struct NativeFunctions {
    using LoadApiDll = NativeHandle (*)(char*);
    using FreeApiDll = std::uint32_t (*)(NativeHandle*);
    using CreateInstance = void* (*)(
        NativeHandle*, std::uint32_t, std::uint32_t*);
    using OpenDevice = NativeHandle (*)(NativeHandle*);
    using CloseDevice = std::uint32_t (*)(NativeHandle*);
    using GetSlot = std::uint32_t (*)(
        NativeHandle*, NativeSlotParameters*);
    using LoadDeviceBios = std::uint32_t (*)(NativeHandle*, char*);
    using AsyncIo = std::uint32_t (*)(
        NativeHandle*, NativeAsyncParameters*);

    LoadApiDll load_api_dll;
    FreeApiDll free_api_dll;
    CreateInstance create_instance;
    OpenDevice open_device;
    CloseDevice close_device;
    GetSlot get_slot;
    LoadDeviceBios load_bios;
    AsyncIo io_async;
};

std::expected<NativeFunctions, BackendError> load_functions(HMODULE module) {
    auto load_api_dll =
        load_function<NativeFunctions::LoadApiDll>(module, "LoadAPIDLL");
    auto free_api_dll =
        load_function<NativeFunctions::FreeApiDll>(module, "FreeAPIDLL");
    auto create_instance =
        load_function<NativeFunctions::CreateInstance>(
            module, "CallCreateInstance");
    auto open_device =
        load_function<NativeFunctions::OpenDevice>(module, "OpenLDevice");
    auto close_device =
        load_function<NativeFunctions::CloseDevice>(module, "CloseLDevice");
    auto get_slot =
        load_function<NativeFunctions::GetSlot>(module, "GetSlotParam");
    auto load_bios =
        load_function<NativeFunctions::LoadDeviceBios>(module, "LoadBios");
    auto io_async =
        load_function<NativeFunctions::AsyncIo>(module, "IoAsync");
    if (!load_api_dll) return std::unexpected(load_api_dll.error());
    if (!free_api_dll) return std::unexpected(free_api_dll.error());
    if (!create_instance) return std::unexpected(create_instance.error());
    if (!open_device) return std::unexpected(open_device.error());
    if (!close_device) return std::unexpected(close_device.error());
    if (!get_slot) return std::unexpected(get_slot.error());
    if (!load_bios) return std::unexpected(load_bios.error());
    if (!io_async) return std::unexpected(io_async.error());
    return NativeFunctions{
        *load_api_dll, *free_api_dll, *create_instance, *open_device,
        *close_device, *get_slot, *load_bios, *io_async};
}

class NativeLCardSession final : public LCardSession {
public:
    NativeLCardSession(
        HMODULE wrapper, NativeFunctions functions, NativeHandle api,
        NativeHandle native_interface)
        : wrapper_(wrapper),
          functions_(functions),
          api_(api),
          interface_(native_interface) {}

    ~NativeLCardSession() override { close(); }

    void close() noexcept override {
        if (interface_ != 0) {
            functions_.close_device(&interface_);
            interface_ = 0;
        }
        if (api_ != 0) {
            functions_.free_api_dll(&api_);
            api_ = 0;
        }
        if (wrapper_ != nullptr) {
            FreeLibrary(wrapper_);
            wrapper_ = nullptr;
        }
    }

    std::expected<LCardSlotInfo, BackendError> slot_info() override {
        NativeSlotParameters slot{};
        const auto result = functions_.get_slot(&interface_, &slot);
        if (result != native_success) {
            return std::unexpected(native_failure("GetSlotParam", result));
        }
        return LCardSlotInfo{
            slot.board_type,
            slot.dsp_type,
            slot.irq,
        };
    }

    std::expected<void, BackendError> load_bios(
        std::string_view path) override {
        std::string native_path(path);
        const auto result =
            functions_.load_bios(&interface_, native_path.data());
        if (result != native_success) {
            return std::unexpected(native_failure("LoadBios", result));
        }
        return {};
    }

    std::expected<LCardAsyncResult, BackendError> io_async(
        const LCardAsyncParameters& parameters) override {
        NativeAsyncParameters native{};
        native.type = static_cast<std::uint32_t>(parameters.operation);
        native.rate_hz = parameters.rate;
        native.mode = parameters.mode;
        native.channel_count = parameters.channel_count;
        for (std::size_t index = 0; index < parameters.channels.size(); ++index) {
            native.channels[index] = parameters.channels[index];
        }
        for (std::size_t index = 0; index < parameters.data.size(); ++index) {
            native.data[index] = parameters.data[index];
        }
        const auto result = functions_.io_async(&interface_, &native);
        if (result != native_success) {
            return std::unexpected(native_failure("IoAsync", result));
        }
        LCardAsyncResult output{
            native.rate_hz,
            native.channel_count,
            {},
        };
        for (std::size_t index = 0; index < output.data.size(); ++index) {
            output.data[index] = native.data[index];
        }
        return output;
    }

private:
    HMODULE wrapper_;
    NativeFunctions functions_;
    NativeHandle api_;
    NativeHandle interface_;
};

class NativeLCardDriver final : public LCardDriver {
public:
    std::expected<std::unique_ptr<LCardSession>, BackendError> open(
        std::uint32_t slot, LCardBoardType expected_board,
        std::string_view bios_path) override {
        const auto directory = lcard_library_directory();
#if defined(_WIN64)
        const auto wrapper_path = directory / "BIN" / "wlcomp64.dll";
        const auto api_path = directory / "BIN" / "lcomp64.dll";
#else
        const auto wrapper_path = directory / "BIN" / "wlcomp.dll";
        const auto api_path = directory / "BIN" / "lcomp.dll";
#endif
        HMODULE wrapper = LoadLibraryW(wrapper_path.c_str());
        if (wrapper == nullptr) {
            return std::unexpected(BackendError{
                lcard_error::library_load_failed,
                "failed to load " + wrapper_path.string()});
        }
        auto functions = load_functions(wrapper);
        if (!functions) {
            FreeLibrary(wrapper);
            return std::unexpected(functions.error());
        }
        std::string api_name = api_path.string();
        NativeHandle api = functions->load_api_dll(api_name.data());
        if (api == 0) {
            FreeLibrary(wrapper);
            return std::unexpected(BackendError{
                lcard_error::library_load_failed,
                "failed to load " + api_path.string()});
        }
        std::uint32_t error = native_success;
        const auto interface_pointer =
            functions->create_instance(&api, slot, &error);
        if (interface_pointer == nullptr || error != native_success) {
            functions->free_api_dll(&api);
            FreeLibrary(wrapper);
            return std::unexpected(native_failure("CallCreateInstance", error));
        }
        NativeHandle native_interface =
            reinterpret_cast<NativeHandle>(interface_pointer);
        const auto device_handle =
            functions->open_device(&native_interface);
        if (device_handle == 0 ||
            device_handle == std::numeric_limits<NativeHandle>::max()) {
            functions->close_device(&native_interface);
            functions->free_api_dll(&api);
            FreeLibrary(wrapper);
            return std::unexpected(BackendError{
                lcard_error::open_failed, "OpenLDevice failed"});
        }
        auto session = std::make_unique<NativeLCardSession>(
            wrapper, *functions, api, native_interface);
        auto actual_slot = session->slot_info();
        if (!actual_slot) {
            return std::unexpected(actual_slot.error());
        }
        if (actual_slot->board_type !=
            static_cast<std::uint32_t>(expected_board)) {
            return std::unexpected(BackendError{
                lcard_error::open_failed,
                "LCard slot contains board type " +
                    std::to_string(actual_slot->board_type) +
                    ", expected " +
                    std::to_string(static_cast<std::uint32_t>(expected_board))});
        }
        if (!bios_path.empty()) {
            if (auto loaded = session->load_bios(bios_path); !loaded) {
                return std::unexpected(loaded.error());
            }
        }
        return std::unique_ptr<LCardSession>(std::move(session));
    }
};

std::atomic<std::uint64_t> next_handle_id{1};

}  // namespace

std::shared_ptr<LCardDriver> create_lcard_driver() {
    return std::make_shared<NativeLCardDriver>();
}

void register_lcard_backend(
    Dispatcher& dispatcher, std::shared_ptr<LCardDriver> driver) {
    dispatcher.register_handler(
        lcard_function::open,
        [driver = std::move(driver)](
            const RequestContext& context,
            std::span<const protocol::Variable> arguments,
            Arena& arena) -> BackendResult {
            if (arguments.size() != 3 ||
                !is_scalar(arguments[0], protocol::DataType::uint32, 4) ||
                !is_scalar(arguments[1], protocol::DataType::uint32, 4)) {
                return std::unexpected(BackendError{
                    lcard_error::invalid_arguments,
                    "LCard open expects slot, board type, and BIOS path"});
            }
            auto board = parse_board(read_u32(arguments[1].data));
            auto bios_path = parse_string(arguments[2]);
            if (!board) {
                return std::unexpected(board.error());
            }
            if (!bios_path) {
                return std::unexpected(bios_path.error());
            }
            auto session = driver->open(
                read_u32(arguments[0].data), *board, *bios_path);
            if (!session) {
                return std::unexpected(session.error());
            }
            const auto handle_id = next_handle_id.fetch_add(1);
            if (!context.resources.register_handle(
                    context.client_id, handle_id, std::move(*session))) {
                return std::unexpected(BackendError{
                    lcard_error::open_failed,
                    "LCard handle registration failed"});
            }
            return scalar_u64(arena, handle_id);
        });

    dispatcher.register_handler(
        lcard_function::io_async,
        [](const RequestContext& context,
           std::span<const protocol::Variable> arguments,
           Arena& arena) -> BackendResult {
            if (arguments.size() != 7 ||
                !is_scalar(arguments[0], protocol::DataType::uint64, 8) ||
                !is_scalar(arguments[1], protocol::DataType::uint32, 4) ||
                !is_scalar(arguments[2], protocol::DataType::float64, 8) ||
                !is_scalar(arguments[3], protocol::DataType::uint32, 4) ||
                !is_scalar(arguments[4], protocol::DataType::uint32, 4)) {
                return std::unexpected(BackendError{
                    lcard_error::invalid_arguments,
                    "LCard IoAsync expects handle, operation, rate, mode, count, channels, data"});
            }
            auto operation = parse_operation(read_u32(arguments[1].data));
            auto channels = parse_u32_array(arguments[5], "channels");
            auto data = parse_u32_array(arguments[6], "data");
            if (!operation) return std::unexpected(operation.error());
            if (!channels) return std::unexpected(channels.error());
            if (!data) return std::unexpected(data.error());
            const auto channel_count = read_u32(arguments[4].data);
            auto result = context.resources.with_handle<LCardSession>(
                context.client_id, read_u64(arguments[0].data),
                [&](LCardSession& session) {
                    auto info = session.slot_info();
                    if (!info) {
                        return std::expected<LCardAsyncResult, BackendError>(
                            std::unexpected(info.error()));
                    }
                    auto board = parse_board(info->board_type);
                    if (!board) {
                        return std::expected<LCardAsyncResult, BackendError>(
                            std::unexpected(board.error()));
                    }
                    if (auto valid = validate_async(
                            *board, *operation, channel_count, *channels,
                            *data, read_u32(arguments[3].data));
                        !valid) {
                        return std::expected<LCardAsyncResult, BackendError>(
                            std::unexpected(valid.error()));
                    }
                    return session.io_async(LCardAsyncParameters{
                        *operation,
                        read_f64(arguments[2].data),
                        read_u32(arguments[3].data),
                        channel_count,
                        *channels,
                        *data,
                    });
                });
            if (!result) {
                return std::unexpected(BackendError{
                    lcard_error::invalid_handle, "unknown LCard handle"});
            }
            if (!*result) return std::unexpected(result->error());
            const auto count = std::min<std::size_t>(
                result->value().channel_count,
                max_async_values);
            auto rate_memory = arena.allocate(8, 8);
            auto data_memory = arena.allocate(count * 4U, 4);
            if (!rate_memory || !data_memory) {
                return std::unexpected(BackendError{
                    error_code::arena_exhausted,
                    "response arena exhausted"});
            }
            write_f64(*rate_memory, result->value().rate);
            for (std::size_t index = 0; index < count; ++index) {
                write_u32(
                    data_memory->subspan(index * 4U, 4U),
                    result->value().data[index]);
            }
            return std::vector<protocol::VariableView>{
                {protocol::DataType::float64, 1, std::as_const(*rate_memory)},
                {protocol::DataType::uint32,
                 static_cast<std::uint32_t>(count),
                 std::as_const(*data_memory)},
            };
        });

    dispatcher.register_handler(
        lcard_function::slot_info,
        [](const RequestContext& context,
           std::span<const protocol::Variable> arguments,
           Arena& arena) -> BackendResult {
            if (arguments.size() != 1 ||
                !is_scalar(arguments[0], protocol::DataType::uint64, 8)) {
                return std::unexpected(BackendError{
                    lcard_error::invalid_arguments,
                    "LCard slot info expects a handle"});
            }
            auto result = context.resources.with_handle<LCardSession>(
                context.client_id, read_u64(arguments[0].data),
                [](LCardSession& session) { return session.slot_info(); });
            if (!result) {
                return std::unexpected(BackendError{
                    lcard_error::invalid_handle, "unknown LCard handle"});
            }
            if (!*result) return std::unexpected(result->error());
            const std::array values{
                result->value().board_type,
                result->value().dsp_type,
                result->value().irq,
            };
            return uint32_values(arena, values);
        });

    dispatcher.register_handler(
        lcard_function::load_bios,
        [](const RequestContext& context,
           std::span<const protocol::Variable> arguments,
           Arena&) -> BackendResult {
            if (arguments.size() != 2 ||
                !is_scalar(arguments[0], protocol::DataType::uint64, 8)) {
                return std::unexpected(BackendError{
                    lcard_error::invalid_arguments,
                    "LCard BIOS load expects a handle and path"});
            }
            auto path = parse_string(arguments[1]);
            if (!path) return std::unexpected(path.error());
            auto result = context.resources.with_handle<LCardSession>(
                context.client_id, read_u64(arguments[0].data),
                [&](LCardSession& session) {
                    return session.load_bios(*path);
                });
            if (!result) {
                return std::unexpected(BackendError{
                    lcard_error::invalid_handle, "unknown LCard handle"});
            }
            if (!*result) return std::unexpected(result->error());
            return std::vector<protocol::VariableView>{};
        });

    dispatcher.register_handler(
        lcard_function::close,
        [](const RequestContext& context,
           std::span<const protocol::Variable> arguments,
           Arena&) -> BackendResult {
            if (arguments.size() != 1 ||
                !is_scalar(arguments[0], protocol::DataType::uint64, 8)) {
                return std::unexpected(BackendError{
                    lcard_error::invalid_arguments,
                    "LCard close expects a handle"});
            }
            if (!context.resources.remove(
                    context.client_id, read_u64(arguments[0].data))) {
                return std::unexpected(BackendError{
                    lcard_error::invalid_handle, "unknown LCard handle"});
            }
            return std::vector<protocol::VariableView>{};
        });
}

}  // namespace nrpc
