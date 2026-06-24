#include "nrpc/lcard_backend.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
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
inline constexpr std::uint32_t native_l_adc_param = 1;
inline constexpr std::uint32_t native_l_stream_adc = 1;
inline constexpr std::uint32_t native_l_point_size = 10001;
inline constexpr std::uint32_t native_wadc_par_0 = 2;
inline constexpr std::uint32_t native_wadc_par_1 = 3;
inline constexpr std::uint32_t default_adc_buffer_points = 131072;
inline constexpr std::uint32_t default_adc_timeout_ms = 5000;

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

struct NativeAdcParameters0 {
    std::uint32_t type;
    std::uint32_t fifo;
    std::uint32_t irq_step;
    std::uint32_t pages;
    std::uint32_t auto_init;
    double rate_hz;
    double frame_delay;
    double scale;
    std::uint32_t rate;
    std::uint32_t frame;
    std::uint32_t scale_code;
    std::uint32_t first_point_delay;
    std::uint32_t sync_type;
    std::uint32_t sync_sensitivity;
    std::uint32_t sync_mode;
    std::uint32_t sync_channel;
    std::uint32_t sync_threshold;
    std::uint32_t channel_count;
    std::uint32_t channels[128];
    std::uint32_t irq_enabled;
    std::uint32_t adc_enabled;
};

struct NativeAdcParameters1 {
    std::uint32_t type;
    std::uint32_t fifo;
    std::uint32_t irq_step;
    std::uint32_t pages;
    std::uint32_t auto_init;
    double rate_hz;
    double frame_delay;
    std::uint16_t reserved1;
    std::uint16_t digital_rate;
    std::uint32_t data_marker_enabled;
    std::uint32_t rate;
    std::uint32_t frame;
    std::uint32_t start_count;
    std::uint32_t stop_count;
    std::uint32_t sync_type;
    std::uint32_t sync_mode;
    std::uint32_t sync_threshold;
    std::uint32_t sync_source;
    std::uint32_t adc_input_mask;
    std::uint32_t channel_count;
    std::uint32_t channels[128];
    std::uint32_t irq_enabled;
    std::uint32_t adc_enabled;
};
#pragma pack(pop)

static_assert(sizeof(NativeSlotParameters) == 80);
static_assert(sizeof(NativeAsyncParameters) == 1060);
static_assert(sizeof(NativeAdcParameters0) == 604);
static_assert(sizeof(NativeAdcParameters1) == 604);

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
    if (operation == LCardAsyncOperation::adc_input) {
        if (board == LCardBoardType::e310) {
            return invalid("E310 ADC stream input is not supported");
        }
        if (channel_count == 0 || channel_count > max_async_values ||
            channels.size() != channel_count || !data.empty()) {
            return invalid("ADC stream input expects one or more channels");
        }
        return {};
    }
    if (operation == LCardAsyncOperation::dac_output) {
        if (board == LCardBoardType::e310) {
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
    return invalid("only ADC stream input and asynchronous DAC output are supported");
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

std::filesystem::path lcard_bios_directory() {
    const auto length =
        GetEnvironmentVariableW(L"NRPC_LCARD_BIOS_DIR", nullptr, 0);
    if (length != 0) {
        std::wstring override_path(length, L'\0');
        GetEnvironmentVariableW(
            L"NRPC_LCARD_BIOS_DIR", override_path.data(), length);
        override_path.resize(length - 1U);
        return override_path;
    }
    return NRPC_LCARD_BIOS_DIR;
}

std::filesystem::path first_existing_path(
    const std::filesystem::path& first,
    const std::filesystem::path& fallback) {
    if (std::filesystem::exists(first)) {
        return first;
    }
    return fallback;
}

std::string lower_ascii(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool is_lcard_bios_file(const std::filesystem::path& path) {
    const auto extension = lower_ascii(path.extension().string());
    return extension == ".bio" || extension == ".pld" || extension == ".bin";
}

std::span<const std::string_view> automatic_bios_candidates(
    LCardBoardType board) {
    using namespace std::literals;

    static constexpr std::array pci_a{"pci_a"sv, "pc_a"sv, "pci"sv};
    static constexpr std::array pci_b{"pci_b"sv, "pc_b"sv, "pci"sv};
    static constexpr std::array pci_c{"pci_c"sv, "pc_c"sv, "pci"sv};
    static constexpr std::array l791{"l791"sv};
    static constexpr std::array e440{"e440"sv};
    static constexpr std::array e140{"e140"sv, "l761"sv};
    static constexpr std::array e2010{"e2010"sv};
    static constexpr std::array e154{"e154"sv};
    static constexpr std::array e2010b{"e2010b"sv, "e2010"sv};
    static constexpr std::array e310{"e310"sv};

    switch (board) {
        case LCardBoardType::pci_a:
            return pci_a;
        case LCardBoardType::pci_b:
            return pci_b;
        case LCardBoardType::pci_c:
            return pci_c;
        case LCardBoardType::l791:
            return l791;
        case LCardBoardType::e440:
            return e440;
        case LCardBoardType::e140:
            return e140;
        case LCardBoardType::e2010:
            return e2010;
        case LCardBoardType::e154:
            return e154;
        case LCardBoardType::e2010b:
            return e2010b;
        case LCardBoardType::e310:
            return e310;
    }
    return {};
}

std::optional<std::filesystem::path> automatic_bios_path(
    LCardBoardType board) {
    using namespace std::literals;

    const auto directory = lcard_bios_directory();
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error)) {
        return std::nullopt;
    }

    const auto candidates = automatic_bios_candidates(board);
    for (const auto candidate : candidates) {
        for (const auto extension : {".bio"sv, ".pld"sv, ".bin"sv}) {
            const auto path =
                directory /
                (std::string(candidate) + std::string(extension));
            if (std::filesystem::is_regular_file(path, error)) {
                return path;
            }
        }
    }

    for (const auto& entry :
         std::filesystem::directory_iterator(directory, error)) {
        if (error || !entry.is_regular_file(error) ||
            !is_lcard_bios_file(entry.path())) {
            continue;
        }
        const auto stem = lower_ascii(entry.path().stem().string());
        const auto filename = lower_ascii(entry.path().filename().string());
        for (const auto candidate : candidates) {
            if (stem == candidate ||
                filename.find(candidate) != std::string::npos) {
                return entry.path();
            }
        }
    }
    return std::nullopt;
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
    using PlataTest = std::uint32_t (*)(NativeHandle*);
    using ReadPlataDescr = std::uint32_t (*)(NativeHandle*, void*);
    using RequestBufferStream = std::uint32_t (*)(
        NativeHandle*, std::uint32_t*, std::uint32_t);
    using FillDaqParameters = std::uint32_t (*)(
        NativeHandle*, void*, std::uint32_t);
    using SetParametersStream = std::uint32_t (*)(
        NativeHandle*, void*, std::uint32_t, std::uint32_t*,
        void**, void**, std::uint32_t);
    using InitStartDevice = std::uint32_t (*)(NativeHandle*);
    using StartDevice = std::uint32_t (*)(NativeHandle*);
    using StopDevice = std::uint32_t (*)(NativeHandle*);
    using GetParameter = std::uint32_t (*)(
        NativeHandle*, std::uint32_t, std::uint32_t*);
    using EnableCorrection = std::uint32_t (*)(NativeHandle*, std::uint16_t);
    using AsyncIo = std::uint32_t (*)(
        NativeHandle*, NativeAsyncParameters*);

    LoadApiDll load_api_dll;
    FreeApiDll free_api_dll;
    CreateInstance create_instance;
    OpenDevice open_device;
    CloseDevice close_device;
    GetSlot get_slot;
    LoadDeviceBios load_bios;
    PlataTest plata_test;
    ReadPlataDescr read_plata_descr;
    RequestBufferStream request_buffer_stream;
    FillDaqParameters fill_daq_parameters;
    SetParametersStream set_parameters_stream;
    InitStartDevice init_start_device;
    StartDevice start_device;
    StopDevice stop_device;
    GetParameter get_parameter;
    EnableCorrection enable_correction;
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
    auto plata_test =
        load_function<NativeFunctions::PlataTest>(module, "PlataTest");
    auto read_plata_descr =
        load_function<NativeFunctions::ReadPlataDescr>(
            module, "ReadPlataDescr");
    auto request_buffer_stream =
        load_function<NativeFunctions::RequestBufferStream>(
            module, "RequestBufferStream");
    auto fill_daq_parameters =
        load_function<NativeFunctions::FillDaqParameters>(
            module, "FillDAQparameters");
    auto set_parameters_stream =
        load_function<NativeFunctions::SetParametersStream>(
            module, "SetParametersStream");
    auto init_start_device =
        load_function<NativeFunctions::InitStartDevice>(
            module, "InitStartLDevice");
    auto start_device =
        load_function<NativeFunctions::StartDevice>(module, "StartLDevice");
    auto stop_device =
        load_function<NativeFunctions::StopDevice>(module, "StopLDevice");
    auto get_parameter =
        load_function<NativeFunctions::GetParameter>(module, "GetParameter");
    auto enable_correction =
        load_function<NativeFunctions::EnableCorrection>(
            module, "EnableCorrection");
    auto io_async =
        load_function<NativeFunctions::AsyncIo>(module, "IoAsync");
    if (!load_api_dll) return std::unexpected(load_api_dll.error());
    if (!free_api_dll) return std::unexpected(free_api_dll.error());
    if (!create_instance) return std::unexpected(create_instance.error());
    if (!open_device) return std::unexpected(open_device.error());
    if (!close_device) return std::unexpected(close_device.error());
    if (!get_slot) return std::unexpected(get_slot.error());
    if (!load_bios) return std::unexpected(load_bios.error());
    if (!plata_test) return std::unexpected(plata_test.error());
    if (!read_plata_descr) return std::unexpected(read_plata_descr.error());
    if (!request_buffer_stream) {
        return std::unexpected(request_buffer_stream.error());
    }
    if (!fill_daq_parameters) {
        return std::unexpected(fill_daq_parameters.error());
    }
    if (!set_parameters_stream) {
        return std::unexpected(set_parameters_stream.error());
    }
    if (!init_start_device) return std::unexpected(init_start_device.error());
    if (!start_device) return std::unexpected(start_device.error());
    if (!stop_device) return std::unexpected(stop_device.error());
    if (!get_parameter) return std::unexpected(get_parameter.error());
    if (!enable_correction) return std::unexpected(enable_correction.error());
    if (!io_async) return std::unexpected(io_async.error());
    return NativeFunctions{
        *load_api_dll, *free_api_dll, *create_instance, *open_device,
        *close_device, *get_slot, *load_bios, *plata_test, *read_plata_descr,
        *request_buffer_stream, *fill_daq_parameters, *set_parameters_stream,
        *init_start_device, *start_device, *stop_device, *get_parameter,
        *enable_correction, *io_async};
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
        if (result != native_success && result != native_not_supported) {
            return std::unexpected(native_failure("LoadBios", result));
        }
        return {};
    }

    std::expected<LCardAsyncResult, BackendError> io_async(
        const LCardAsyncParameters& parameters) override {
        if (parameters.operation == LCardAsyncOperation::adc_input) {
            return read_adc_stream(parameters);
        }
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

    std::expected<void, BackendError> plata_test() {
        const auto result = functions_.plata_test(&interface_);
        if (result != native_success && result != native_not_supported) {
            return std::unexpected(native_failure("PlataTest", result));
        }
        return {};
    }

    std::expected<void, BackendError> read_plata_descr() {
        std::array<std::byte, 256> descriptor{};
        const auto result =
            functions_.read_plata_descr(&interface_, descriptor.data());
        if (result != native_success && result != native_not_supported) {
            return std::unexpected(native_failure("ReadPlataDescr", result));
        }
        return {};
    }

private:
    struct StopGuard {
        NativeFunctions* functions{};
        NativeHandle* native_interface{};
        bool active{};

        ~StopGuard() {
            if (active) {
                functions->stop_device(native_interface);
            }
        }
    };

    static double requested_or_default(double value, double fallback) {
        return value > 0.0 ? value : fallback;
    }

    static void copy_channels(
        std::span<std::uint32_t, 128> destination,
        std::span<const std::uint32_t> source) {
        for (std::size_t index = 0; index < source.size(); ++index) {
            destination[index] = source[index];
        }
    }

    std::expected<LCardAsyncResult, BackendError> read_adc_stream(
        const LCardAsyncParameters& parameters) {
        auto info = slot_info();
        if (!info) {
            return std::unexpected(info.error());
        }
        auto board = parse_board(info->board_type);
        if (!board) {
            return std::unexpected(board.error());
        }

        const auto timeout_ms =
            parameters.timeout_ms == 0 ? default_adc_timeout_ms
                                       : parameters.timeout_ms;
        std::uint32_t buffer_points = default_adc_buffer_points;
        const auto buffer_result = functions_.request_buffer_stream(
            &interface_, &buffer_points, native_l_stream_adc);
        if (buffer_result != native_success) {
            return std::unexpected(
                native_failure("RequestBufferStream", buffer_result));
        }
        if (buffer_points < parameters.channel_count) {
            return std::unexpected(BackendError{
                lcard_error::operation_failed,
                "ADC stream buffer is smaller than the requested channel frame"});
        }

        void* data = nullptr;
        void* sync = nullptr;
        double effective_rate = parameters.rate;
        std::uint32_t set_size = buffer_points;
        std::uint32_t point_size = 2;

        if (*board == LCardBoardType::pci_a ||
            *board == LCardBoardType::pci_b ||
            *board == LCardBoardType::pci_c ||
            *board == LCardBoardType::e440 ||
            *board == LCardBoardType::e140 ||
            *board == LCardBoardType::e154) {
            NativeAdcParameters0 adc{};
            adc.type = native_l_adc_param;
            adc.fifo = (*board == LCardBoardType::pci_a ||
                        *board == LCardBoardType::pci_b ||
                        *board == LCardBoardType::pci_c)
                           ? 1024U
                           : 4096U;
            adc.irq_step = adc.fifo;
            adc.pages = (*board == LCardBoardType::pci_a ||
                         *board == LCardBoardType::pci_b ||
                         *board == LCardBoardType::pci_c)
                            ? 128U
                            : 32U;
            adc.auto_init = 0;
            adc.rate_hz = requested_or_default(parameters.rate, 100.0);
            adc.frame_delay = 0.0;
            adc.sync_type = (*board == LCardBoardType::pci_a ||
                             *board == LCardBoardType::pci_b ||
                             *board == LCardBoardType::pci_c)
                                ? 3U
                                : 0U;
            adc.channel_count = parameters.channel_count;
            copy_channels(std::span(adc.channels), parameters.channels);
            adc.irq_enabled = 1;
            adc.adc_enabled = 1;

            auto result = functions_.fill_daq_parameters(
                &interface_, &adc, native_wadc_par_0);
            if (result != native_success) {
                return std::unexpected(
                    native_failure("FillDAQparameters", result));
            }
            effective_rate = adc.rate_hz;

            result = functions_.set_parameters_stream(
                &interface_, &adc, native_wadc_par_0, &set_size, &data, &sync,
                native_l_stream_adc);
            if (result != native_success) {
                return std::unexpected(
                    native_failure("SetParametersStream", result));
            }
        } else {
            NativeAdcParameters1 adc{};
            adc.type = native_l_adc_param;
            if (*board == LCardBoardType::l791) {
                adc.fifo = 128;
                adc.irq_step = 1024;
                adc.pages = 128;
                adc.rate_hz = requested_or_default(parameters.rate, 200.0);
                adc.frame_delay = 0.005;
                adc.sync_type = 0;
                adc.sync_source = 0;
            } else {
                adc.fifo = 4096;
                adc.irq_step = 4096;
                adc.pages = 32;
                adc.rate_hz = requested_or_default(parameters.rate, 1000.0);
                adc.frame_delay = 0.001;
                adc.sync_type = 0x01;
                adc.sync_source = 0x00;
                adc.adc_input_mask =
                    parameters.mode == 0 ? 0x0400U : parameters.mode;
            }
            adc.auto_init = 0;
            adc.channel_count = parameters.channel_count;
            copy_channels(std::span(adc.channels), parameters.channels);
            adc.irq_enabled = 1;
            adc.adc_enabled = 1;

            auto result = functions_.fill_daq_parameters(
                &interface_, &adc, native_wadc_par_1);
            if (result != native_success) {
                return std::unexpected(
                    native_failure("FillDAQparameters", result));
            }
            effective_rate = adc.rate_hz;

            result = functions_.set_parameters_stream(
                &interface_, &adc, native_wadc_par_1, &set_size, &data, &sync,
                native_l_stream_adc);
            if (result != native_success) {
                return std::unexpected(
                    native_failure("SetParametersStream", result));
            }
        }

        if (data == nullptr || sync == nullptr) {
            return std::unexpected(BackendError{
                lcard_error::operation_failed,
                "ADC stream did not return data and sync pointers"});
        }

        if (auto result = functions_.get_parameter(
                &interface_, native_l_point_size, &point_size);
            result != native_success && result != native_not_supported) {
            return std::unexpected(native_failure("GetParameter", result));
        }
        if (point_size < sizeof(std::uint16_t)) {
            point_size = sizeof(std::uint16_t);
        }

        if (auto result = functions_.enable_correction(&interface_, 1);
            result != native_success && result != native_not_supported) {
            return std::unexpected(native_failure("EnableCorrection", result));
        }

        auto result = functions_.init_start_device(&interface_);
        if (result != native_success) {
            return std::unexpected(native_failure("InitStartLDevice", result));
        }
        result = functions_.start_device(&interface_);
        if (result != native_success) {
            return std::unexpected(native_failure("StartLDevice", result));
        }

        StopGuard stop_guard{&functions_, &interface_, true};
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeout_ms);
        auto* sync_words = static_cast<volatile std::uint32_t*>(sync);
        while (*sync_words < parameters.channel_count) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return std::unexpected(BackendError{
                    lcard_error::operation_timeout,
                    "ADC stream read timed out"});
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        LCardAsyncResult output{
            effective_rate,
            parameters.channel_count,
            {},
        };
        const auto* bytes = static_cast<const std::byte*>(data);
        for (std::uint32_t index = 0; index < parameters.channel_count; ++index) {
            std::uint16_t sample = 0;
            std::memcpy(
                &sample, bytes + static_cast<std::size_t>(index) * point_size,
                sizeof(sample));
            output.data[index] = sample;
        }

        stop_guard.active = false;
        result = functions_.stop_device(&interface_);
        if (result != native_success) {
            return std::unexpected(native_failure("StopLDevice", result));
        }
        return output;
    }

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
        const auto wrapper_path = first_existing_path(
            directory / "wlcomp64.dll", directory / "BIN" / "wlcomp64.dll");
        const auto api_path = first_existing_path(
            directory / "lcomp64.dll", directory / "BIN" / "lcomp64.dll");
#else
        const auto wrapper_path = first_existing_path(
            directory / "wlcomp.dll", directory / "BIN" / "wlcomp.dll");
        const auto api_path = first_existing_path(
            directory / "lcomp.dll", directory / "BIN" / "lcomp.dll");
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
        } else if (const auto discovered_bios =
                       automatic_bios_path(expected_board)) {
            std::vector<std::string> automatic_names{
                discovered_bios->string(),
                discovered_bios->filename().string(),
                lower_ascii(discovered_bios->filename().string()),
                discovered_bios->stem().string(),
                lower_ascii(discovered_bios->stem().string()),
            };
            for (const auto& automatic_name : automatic_names) {
                if (auto loaded = session->load_bios(automatic_name); loaded) {
                    break;
                }
            }
        }
        if (auto tested = session->plata_test(); !tested) {
            return std::unexpected(tested.error());
        }
        if (auto descriptor = session->read_plata_descr(); !descriptor) {
            return std::unexpected(descriptor.error());
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
                        context.timeout_ms,
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
