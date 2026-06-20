#include "nrpc/nidaqmx_backend.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#define NOMINMAX
#include <windows.h>

#include "nrpc/error_codes.hpp"

namespace nrpc {
namespace {

using TaskHandle = void*;
using Bool32 = std::uint32_t;
using Int32 = std::int32_t;
using UInt32 = std::uint32_t;
using Float64 = double;

constexpr Int32 daqmx_val_volts = 10348;
constexpr Int32 daqmx_val_degrees_c = 10143;
constexpr Int32 daqmx_val_group_by_channel = 0;
constexpr std::uint32_t max_samples_per_call = 1024U * 1024U;

struct Functions {
    Int32(__cdecl* create_task)(const char*, TaskHandle*);
    Int32(__cdecl* create_ai_voltage)(TaskHandle, const char*, const char*, Int32,
                                     Float64, Float64, Int32, const char*);
    Int32(__cdecl* create_ai_thermocouple)(TaskHandle, const char*, const char*,
                                          Float64, Float64, Int32, Int32, Int32,
                                          Float64, const char*);
    Int32(__cdecl* create_ao_voltage)(TaskHandle, const char*, const char*,
                                     Float64, Float64, Int32, const char*);
    Int32(__cdecl* create_di)(TaskHandle, const char*, const char*, Int32);
    Int32(__cdecl* create_do)(TaskHandle, const char*, const char*, Int32);
    Int32(__cdecl* start_task)(TaskHandle);
    Int32(__cdecl* stop_task)(TaskHandle);
    Int32(__cdecl* clear_task)(TaskHandle);
    Int32(__cdecl* read_analog)(TaskHandle, Int32, Float64, Bool32, Float64*,
                               UInt32, Int32*, void*);
    Int32(__cdecl* write_analog)(TaskHandle, Int32, Bool32, Float64, Bool32,
                                const Float64*, Int32*, void*);
    Int32(__cdecl* read_digital)(TaskHandle, Int32, Float64, Bool32, UInt32*,
                                UInt32, Int32*, Int32*, void*);
    Int32(__cdecl* write_digital)(TaskHandle, Int32, Bool32, Float64, Bool32,
                                 const UInt32*, Int32*, void*);
    Int32(__cdecl* error_info)(char*, UInt32);
};

struct Library {
    HMODULE module{};
    Functions functions{};
    ~Library() { if (module != nullptr) FreeLibrary(module); }
};

template <typename T>
bool load_symbol(HMODULE module, const char* name, T& output) {
    output = reinterpret_cast<T>(GetProcAddress(module, name));
    return output != nullptr;
}

std::expected<std::shared_ptr<Library>, BackendError> load_library() {
    HMODULE module = LoadLibraryW(L"nicaiu.dll");
    if (module == nullptr) {
        return std::unexpected(BackendError{
            nidaqmx_error::library_load_failed,
            "failed to load NI-DAQmx runtime nicaiu.dll"});
    }
    auto library = std::make_shared<Library>();
    library->module = module;
    auto& f = library->functions;
    const bool loaded =
        load_symbol(module, "DAQmxCreateTask", f.create_task) &&
        load_symbol(module, "DAQmxCreateAIVoltageChan", f.create_ai_voltage) &&
        load_symbol(module, "DAQmxCreateAIThrmcplChan", f.create_ai_thermocouple) &&
        load_symbol(module, "DAQmxCreateAOVoltageChan", f.create_ao_voltage) &&
        load_symbol(module, "DAQmxCreateDIChan", f.create_di) &&
        load_symbol(module, "DAQmxCreateDOChan", f.create_do) &&
        load_symbol(module, "DAQmxStartTask", f.start_task) &&
        load_symbol(module, "DAQmxStopTask", f.stop_task) &&
        load_symbol(module, "DAQmxClearTask", f.clear_task) &&
        load_symbol(module, "DAQmxReadAnalogF64", f.read_analog) &&
        load_symbol(module, "DAQmxWriteAnalogF64", f.write_analog) &&
        load_symbol(module, "DAQmxReadDigitalU32", f.read_digital) &&
        load_symbol(module, "DAQmxWriteDigitalU32", f.write_digital) &&
        load_symbol(module, "DAQmxGetExtendedErrorInfo", f.error_info);
    if (!loaded) {
        return std::unexpected(BackendError{
            nidaqmx_error::library_load_failed,
            "NI-DAQmx runtime is missing a required symbol"});
    }
    return library;
}

BackendError native_error(const Functions& functions, std::string_view operation,
                          Int32 status,
                          std::uint32_t code = nidaqmx_error::operation_failed) {
    std::string detail(2048, '\0');
    functions.error_info(detail.data(), static_cast<UInt32>(detail.size()));
    const auto terminator = detail.find('\0');
    if (terminator != std::string::npos) detail.resize(terminator);
    std::string message(operation);
    message += " failed with NI-DAQmx error " + std::to_string(status);
    if (!detail.empty()) message += ": " + detail;
    return BackendError{code, std::move(message)};
}

class NativeTask final : public NidaqmxTask {
public:
    NativeTask(std::shared_ptr<Library> library, TaskHandle task,
               NidaqmxChannelType type)
        : library_(std::move(library)), task_(task), type_(type) {}
    ~NativeTask() override { close(); }

    NidaqmxChannelType channel_type() const noexcept override { return type_; }
    std::expected<void, BackendError> start() override {
        const auto status = library_->functions.start_task(task_);
        if (status < 0) return std::unexpected(native_error(library_->functions, "DAQmxStartTask", status));
        started_ = true;
        return {};
    }
    std::expected<std::size_t, BackendError> read_analog(
        std::span<double> values, std::uint32_t samples, double timeout) override {
        if (type_ != NidaqmxChannelType::analog_input) return wrong_type("analog input");
        Int32 read{};
        const auto status = library_->functions.read_analog(
            task_, checked_count(samples), timeout, daqmx_val_group_by_channel,
            values.data(), checked_size(values.size()), &read, nullptr);
        if (status < 0) return std::unexpected(native_error(library_->functions, "DAQmxReadAnalogF64", status));
        return static_cast<std::size_t>(read);
    }
    std::expected<std::size_t, BackendError> write_analog(
        std::span<const double> values, double timeout) override {
        if (type_ != NidaqmxChannelType::analog_output) return wrong_type("analog output");
        Int32 written{};
        const auto status = library_->functions.write_analog(
            task_, checked_count(values.size()), 0, timeout,
            daqmx_val_group_by_channel, values.data(), &written, nullptr);
        if (status < 0) return std::unexpected(native_error(library_->functions, "DAQmxWriteAnalogF64", status));
        return static_cast<std::size_t>(written);
    }
    std::expected<std::size_t, BackendError> read_digital(
        std::span<std::uint32_t> values, std::uint32_t samples,
        double timeout) override {
        if (type_ != NidaqmxChannelType::digital_input) return wrong_type("digital input");
        Int32 read{};
        Int32 bytes_per_sample{};
        const auto status = library_->functions.read_digital(
            task_, checked_count(samples), timeout, daqmx_val_group_by_channel,
            values.data(), checked_size(values.size()), &read, &bytes_per_sample,
            nullptr);
        if (status < 0) return std::unexpected(native_error(library_->functions, "DAQmxReadDigitalU32", status));
        return static_cast<std::size_t>(read);
    }
    std::expected<std::size_t, BackendError> write_digital(
        std::span<const std::uint32_t> values, double timeout) override {
        if (type_ != NidaqmxChannelType::digital_output) return wrong_type("digital output");
        Int32 written{};
        const auto status = library_->functions.write_digital(
            task_, checked_count(values.size()), 0, timeout,
            daqmx_val_group_by_channel, values.data(), &written, nullptr);
        if (status < 0) return std::unexpected(native_error(library_->functions, "DAQmxWriteDigitalU32", status));
        return static_cast<std::size_t>(written);
    }
    void close() noexcept override {
        if (task_ == nullptr) return;
        if (started_) library_->functions.stop_task(task_);
        library_->functions.clear_task(task_);
        task_ = nullptr;
        started_ = false;
    }

private:
    static Int32 checked_count(std::size_t value) {
        return static_cast<Int32>(std::min<std::size_t>(value,
            static_cast<std::size_t>(std::numeric_limits<Int32>::max())));
    }
    static UInt32 checked_size(std::size_t value) {
        return static_cast<UInt32>(std::min<std::size_t>(value,
            static_cast<std::size_t>(std::numeric_limits<UInt32>::max())));
    }
    static std::unexpected<BackendError> wrong_type(std::string_view expected) {
        return std::unexpected(BackendError{nidaqmx_error::wrong_channel_type,
            "task is not an " + std::string(expected) + " task"});
    }
    std::shared_ptr<Library> library_;
    TaskHandle task_{};
    NidaqmxChannelType type_;
    bool started_{};
};

class NativeDriver final : public NidaqmxDriver {
public:
    std::expected<std::unique_ptr<NidaqmxTask>, BackendError> create_ai_voltage(
        std::string_view physical, double minimum, double maximum,
        NidaqmxTerminalConfig terminal) override {
        return create(physical, NidaqmxChannelType::analog_input,
            [&](const Functions& f, TaskHandle task, const char* name) {
                return f.create_ai_voltage(task, name, "", static_cast<Int32>(terminal),
                                           minimum, maximum, daqmx_val_volts, nullptr);
            });
    }
    std::expected<std::unique_ptr<NidaqmxTask>, BackendError> create_ai_thermocouple(
        std::string_view physical, double minimum, double maximum,
        NidaqmxThermocoupleType type, NidaqmxCjcSource cjc_source,
        double cjc_value, std::string_view cjc_channel) override {
        const std::string channel(cjc_channel);
        return create(physical, NidaqmxChannelType::analog_input,
            [&](const Functions& f, TaskHandle task, const char* name) {
                return f.create_ai_thermocouple(
                    task, name, "", minimum, maximum, daqmx_val_degrees_c,
                    static_cast<Int32>(type), static_cast<Int32>(cjc_source),
                    cjc_value, channel.empty() ? nullptr : channel.c_str());
            });
    }
    std::expected<std::unique_ptr<NidaqmxTask>, BackendError> create_ao_voltage(
        std::string_view physical, double minimum, double maximum) override {
        return create(physical, NidaqmxChannelType::analog_output,
            [&](const Functions& f, TaskHandle task, const char* name) {
                return f.create_ao_voltage(task, name, "", minimum, maximum,
                                           daqmx_val_volts, nullptr);
            });
    }
    std::expected<std::unique_ptr<NidaqmxTask>, BackendError> create_digital(
        std::string_view lines, bool output, bool all_lines) override {
        return create(lines, output ? NidaqmxChannelType::digital_output
                                    : NidaqmxChannelType::digital_input,
            [&](const Functions& f, TaskHandle task, const char* name) {
                const auto grouping = all_lines ? 1 : 0;
                return output ? f.create_do(task, name, "", grouping)
                              : f.create_di(task, name, "", grouping);
            });
    }

private:
    template <typename Configure>
    std::expected<std::unique_ptr<NidaqmxTask>, BackendError> create(
        std::string_view physical, NidaqmxChannelType type, Configure configure) {
        auto library = load_library();
        if (!library) return std::unexpected(library.error());
        TaskHandle task{};
        auto status = (*library)->functions.create_task("", &task);
        if (status < 0) return std::unexpected(native_error((*library)->functions,
            "DAQmxCreateTask", status, nidaqmx_error::task_creation_failed));
        const std::string name(physical);
        status = configure((*library)->functions, task, name.c_str());
        if (status < 0) {
            const auto error = native_error((*library)->functions,
                "DAQmxCreateChannel", status, nidaqmx_error::task_creation_failed);
            (*library)->functions.clear_task(task);
            return std::unexpected(error);
        }
        return std::unique_ptr<NidaqmxTask>(
            std::make_unique<NativeTask>(*library, task, type));
    }
};

std::uint64_t read_u64(std::span<const std::byte> data) {
    std::uint64_t value{};
    for (auto byte : data) value = (value << 8U) | std::to_integer<std::uint64_t>(byte);
    return value;
}
std::uint32_t read_u32(std::span<const std::byte> data) {
    return static_cast<std::uint32_t>(read_u64(data));
}
std::int32_t read_i32(std::span<const std::byte> data) {
    return std::bit_cast<std::int32_t>(read_u32(data));
}
double read_f64(std::span<const std::byte> data) {
    return std::bit_cast<double>(read_u64(data));
}
void write_u64(std::span<std::byte> output, std::uint64_t value) {
    for (std::size_t i = 0; i < output.size(); ++i)
        output[i] = static_cast<std::byte>((value >> ((output.size() - i - 1U) * 8U)) & 0xffU);
}
bool scalar(const protocol::Variable& value, protocol::DataType type, std::size_t size) {
    return value.type == type && value.element_count == 1 && value.data.size() == size;
}
std::expected<std::string_view, BackendError> string_arg(const protocol::Variable& value) {
    if (value.type != protocol::DataType::string || value.data.empty() ||
        value.data.back() != std::byte{0})
        return std::unexpected(BackendError{nidaqmx_error::invalid_arguments, "invalid string argument"});
    return std::string_view(reinterpret_cast<const char*>(value.data.data()), value.data.size() - 1U);
}
template <typename T>
std::expected<std::vector<T>, BackendError> array_arg(
    const protocol::Variable& value, protocol::DataType type, std::size_t width) {
    if (value.type != type || value.data.size() != value.element_count * width)
        return std::unexpected(BackendError{nidaqmx_error::invalid_arguments, "invalid array argument"});
    std::vector<T> result(value.element_count);
    for (std::size_t i = 0; i < result.size(); ++i) {
        const auto bytes = std::span(value.data).subspan(i * width, width);
        if constexpr (std::same_as<T, double>) result[i] = read_f64(bytes);
        else result[i] = read_u32(bytes);
    }
    return result;
}
std::expected<std::uint64_t, BackendError> store_task(
    const RequestContext& context, std::unique_ptr<NidaqmxTask> task) {
    // ResourceManager's key space is shared by all backends. Keep NI task IDs
    // in their own range so a client's first socket and first DAQ task cannot
    // collide.
    static std::atomic<std::uint64_t> next{0x4e49000000000001ULL};
    const auto id = next.fetch_add(1);
    if (!context.resources.register_handle(context.client_id, id, std::move(task)))
        return std::unexpected(BackendError{nidaqmx_error::task_creation_failed, "task handle registration failed"});
    return id;
}
BackendResult handle_result(Arena& arena, std::uint64_t handle) {
    auto memory = arena.allocate(8, 8);
    if (!memory) return std::unexpected(BackendError{error_code::arena_exhausted, "response arena exhausted"});
    write_u64(*memory, handle);
    return std::vector<protocol::VariableView>{{protocol::DataType::uint64, 1, std::as_const(*memory)}};
}
double timeout_seconds(const RequestContext& context) {
    return static_cast<double>(context.timeout_ms) / 1000.0;
}
bool valid_range(double minimum, double maximum) {
    return std::isfinite(minimum) && std::isfinite(maximum) && minimum < maximum;
}
bool valid_terminal(std::int32_t value) {
    return value == -1 || value == 10083 || value == 10078 ||
           value == 10106 || value == 12529;
}
bool valid_thermocouple(std::int32_t value) {
    return value == 10047 || value == 10055 || value == 10072 ||
           value == 10073 || value == 10077 || value == 10082 ||
           value == 10085 || value == 10086;
}
bool valid_cjc(std::int32_t value) {
    return value == 10200 || value == 10116 || value == 10113;
}
template <typename Function>
BackendResult with_task(const RequestContext& context, std::uint64_t id, Function function) {
    auto result = context.resources.with_handle<NidaqmxTask>(context.client_id, id, function);
    if (!result) return std::unexpected(BackendError{nidaqmx_error::invalid_handle, "unknown NI-DAQmx task handle"});
    if (!*result) return std::unexpected(result->error());
    return std::vector<protocol::VariableView>{};
}

}  // namespace

std::shared_ptr<NidaqmxDriver> create_nidaqmx_driver() {
    return std::make_shared<NativeDriver>();
}

void register_nidaqmx_backend(Dispatcher& dispatcher,
                              std::shared_ptr<NidaqmxDriver> driver) {
    auto register_created = [&](std::uint8_t index, auto create) {
        dispatcher.register_handler(index,
            [driver, create](const RequestContext& context,
                             std::span<const protocol::Variable> args,
                             Arena& arena) -> BackendResult {
                auto task = create(*driver, args);
                if (!task) return std::unexpected(task.error());
                auto handle = store_task(context, std::move(*task));
                if (!handle) return std::unexpected(handle.error());
                return handle_result(arena, *handle);
            });
    };
    register_created(nidaqmx_function::create_ai_voltage,
        [](NidaqmxDriver& d, std::span<const protocol::Variable> a)
            -> std::expected<std::unique_ptr<NidaqmxTask>, BackendError> {
            if (a.size() != 4 || !scalar(a[1], protocol::DataType::float64, 8) ||
                !scalar(a[2], protocol::DataType::float64, 8) ||
                !scalar(a[3], protocol::DataType::int32, 4))
                return std::unexpected(BackendError{nidaqmx_error::invalid_arguments, "AI voltage expects channel, minimum, maximum, terminal configuration"});
            auto channel = string_arg(a[0]); if (!channel) return std::unexpected(channel.error());
            const auto minimum = read_f64(a[1].data); const auto maximum = read_f64(a[2].data);
            const auto terminal = read_i32(a[3].data);
            if (channel->empty() || !valid_range(minimum, maximum) || !valid_terminal(terminal))
                return std::unexpected(BackendError{nidaqmx_error::invalid_arguments, "invalid AI voltage channel configuration"});
            return d.create_ai_voltage(*channel, minimum, maximum,
                                       static_cast<NidaqmxTerminalConfig>(terminal));
        });
    register_created(nidaqmx_function::create_ai_thermocouple,
        [](NidaqmxDriver& d, std::span<const protocol::Variable> a)
            -> std::expected<std::unique_ptr<NidaqmxTask>, BackendError> {
            if (a.size() != 7 || !scalar(a[1], protocol::DataType::float64, 8) ||
                !scalar(a[2], protocol::DataType::float64, 8) ||
                !scalar(a[3], protocol::DataType::int32, 4) ||
                !scalar(a[4], protocol::DataType::int32, 4) ||
                !scalar(a[5], protocol::DataType::float64, 8))
                return std::unexpected(BackendError{nidaqmx_error::invalid_arguments, "thermocouple expects channel, range, type, CJC source/value/channel"});
            auto channel = string_arg(a[0]); auto cjc = string_arg(a[6]);
            if (!channel) return std::unexpected(channel.error()); if (!cjc) return std::unexpected(cjc.error());
            const auto minimum = read_f64(a[1].data); const auto maximum = read_f64(a[2].data);
            const auto type = read_i32(a[3].data); const auto source = read_i32(a[4].data);
            const auto cjc_value = read_f64(a[5].data);
            if (channel->empty() || !valid_range(minimum, maximum) ||
                !valid_thermocouple(type) || !valid_cjc(source) || !std::isfinite(cjc_value) ||
                (source == static_cast<std::int32_t>(NidaqmxCjcSource::channel) && cjc->empty()))
                return std::unexpected(BackendError{nidaqmx_error::invalid_arguments, "invalid thermocouple channel configuration"});
            return d.create_ai_thermocouple(*channel, minimum, maximum,
                static_cast<NidaqmxThermocoupleType>(type),
                static_cast<NidaqmxCjcSource>(source), cjc_value, *cjc);
        });
    register_created(nidaqmx_function::create_ao_voltage,
        [](NidaqmxDriver& d, std::span<const protocol::Variable> a)
            -> std::expected<std::unique_ptr<NidaqmxTask>, BackendError> {
            if (a.size() != 3 || !scalar(a[1], protocol::DataType::float64, 8) || !scalar(a[2], protocol::DataType::float64, 8))
                return std::unexpected(BackendError{nidaqmx_error::invalid_arguments, "AO voltage expects channel, minimum, maximum"});
            auto channel = string_arg(a[0]); if (!channel) return std::unexpected(channel.error());
            const auto minimum = read_f64(a[1].data); const auto maximum = read_f64(a[2].data);
            if (channel->empty() || !valid_range(minimum, maximum))
                return std::unexpected(BackendError{nidaqmx_error::invalid_arguments, "invalid AO voltage channel configuration"});
            return d.create_ao_voltage(*channel, minimum, maximum);
        });
    auto digital_create = [](bool output) {
        return [output](NidaqmxDriver& d, std::span<const protocol::Variable> a)
            -> std::expected<std::unique_ptr<NidaqmxTask>, BackendError> {
            if (a.size() != 2 || !scalar(a[1], protocol::DataType::boolean, 1))
                return std::unexpected(BackendError{nidaqmx_error::invalid_arguments, "digital task expects lines and all-lines grouping flag"});
            auto lines = string_arg(a[0]); if (!lines) return std::unexpected(lines.error());
            if (lines->empty()) return std::unexpected(BackendError{nidaqmx_error::invalid_arguments, "digital lines must not be empty"});
            return d.create_digital(*lines, output, a[1].data[0] != std::byte{0});
        };
    };
    register_created(nidaqmx_function::create_di, digital_create(false));
    register_created(nidaqmx_function::create_do, digital_create(true));

    dispatcher.register_handler(nidaqmx_function::start,
        [](const RequestContext& c, std::span<const protocol::Variable> a, Arena&) -> BackendResult {
            if (a.size() != 1 || !scalar(a[0], protocol::DataType::uint64, 8))
                return std::unexpected(BackendError{nidaqmx_error::invalid_arguments, "start expects a task handle"});
            return with_task(c, read_u64(a[0].data), [](NidaqmxTask& task) { return task.start(); });
        });
    dispatcher.register_handler(nidaqmx_function::read_analog,
        [](const RequestContext& c, std::span<const protocol::Variable> a, Arena& arena) -> BackendResult {
            if (a.size() != 2 || !scalar(a[0], protocol::DataType::uint64, 8) || !scalar(a[1], protocol::DataType::uint32, 4))
                return std::unexpected(BackendError{nidaqmx_error::invalid_arguments, "analog read expects handle and sample count"});
            const auto count = read_u32(a[1].data); if (count == 0 || count > max_samples_per_call) return std::unexpected(BackendError{nidaqmx_error::invalid_arguments, "sample count is out of range"});
            std::vector<double> values(count); auto result = c.resources.with_handle<NidaqmxTask>(c.client_id, read_u64(a[0].data), [&](NidaqmxTask& t) { return t.read_analog(values, count, timeout_seconds(c)); });
            if (!result) return std::unexpected(BackendError{nidaqmx_error::invalid_handle, "unknown NI-DAQmx task handle"}); if (!*result) return std::unexpected(result->error());
            values.resize(std::min(values.size(), result->value())); auto memory = arena.allocate(values.size() * 8U, 8); if (!memory) return std::unexpected(BackendError{error_code::arena_exhausted, "response arena exhausted"});
            for (std::size_t i = 0; i < values.size(); ++i) write_u64(memory->subspan(i * 8U, 8), std::bit_cast<std::uint64_t>(values[i]));
            return std::vector<protocol::VariableView>{{protocol::DataType::float64, static_cast<std::uint32_t>(values.size()), std::as_const(*memory)}};
        });
    dispatcher.register_handler(nidaqmx_function::read_digital,
        [](const RequestContext& c, std::span<const protocol::Variable> a, Arena& arena) -> BackendResult {
            if (a.size() != 2 || !scalar(a[0], protocol::DataType::uint64, 8) || !scalar(a[1], protocol::DataType::uint32, 4)) return std::unexpected(BackendError{nidaqmx_error::invalid_arguments, "digital read expects handle and sample count"});
            const auto count = read_u32(a[1].data); if (count == 0 || count > max_samples_per_call) return std::unexpected(BackendError{nidaqmx_error::invalid_arguments, "sample count is out of range"}); std::vector<std::uint32_t> values(count);
            auto result = c.resources.with_handle<NidaqmxTask>(c.client_id, read_u64(a[0].data), [&](NidaqmxTask& t) { return t.read_digital(values, count, timeout_seconds(c)); });
            if (!result) return std::unexpected(BackendError{nidaqmx_error::invalid_handle, "unknown NI-DAQmx task handle"}); if (!*result) return std::unexpected(result->error()); values.resize(std::min(values.size(), result->value()));
            auto memory = arena.allocate(values.size() * 4U, 4); if (!memory) return std::unexpected(BackendError{error_code::arena_exhausted, "response arena exhausted"}); for (std::size_t i = 0; i < values.size(); ++i) write_u64(memory->subspan(i * 4U, 4), values[i]);
            return std::vector<protocol::VariableView>{{protocol::DataType::uint32, static_cast<std::uint32_t>(values.size()), std::as_const(*memory)}};
        });
    auto register_write = [&](std::uint8_t index, bool analog) {
        dispatcher.register_handler(index, [analog](const RequestContext& c, std::span<const protocol::Variable> a, Arena& arena) -> BackendResult {
            if (a.size() != 2 || !scalar(a[0], protocol::DataType::uint64, 8)) return std::unexpected(BackendError{nidaqmx_error::invalid_arguments, "write expects handle and values"});
            std::expected<std::size_t, BackendError> written = std::unexpected(BackendError{});
            if (analog) { auto values = array_arg<double>(a[1], protocol::DataType::float64, 8); if (!values || values->empty()) return std::unexpected(values ? BackendError{nidaqmx_error::invalid_arguments, "values must not be empty"} : values.error()); auto result = c.resources.with_handle<NidaqmxTask>(c.client_id, read_u64(a[0].data), [&](NidaqmxTask& t) { return t.write_analog(*values, timeout_seconds(c)); }); if (!result) return std::unexpected(BackendError{nidaqmx_error::invalid_handle, "unknown NI-DAQmx task handle"}); written = std::move(*result); }
            else { auto values = array_arg<std::uint32_t>(a[1], protocol::DataType::uint32, 4); if (!values || values->empty()) return std::unexpected(values ? BackendError{nidaqmx_error::invalid_arguments, "values must not be empty"} : values.error()); auto result = c.resources.with_handle<NidaqmxTask>(c.client_id, read_u64(a[0].data), [&](NidaqmxTask& t) { return t.write_digital(*values, timeout_seconds(c)); }); if (!result) return std::unexpected(BackendError{nidaqmx_error::invalid_handle, "unknown NI-DAQmx task handle"}); written = std::move(*result); }
            if (!written) return std::unexpected(written.error()); auto memory = arena.allocate(4, 4); if (!memory) return std::unexpected(BackendError{error_code::arena_exhausted, "response arena exhausted"}); write_u64(*memory, written.value()); return std::vector<protocol::VariableView>{{protocol::DataType::uint32, 1, std::as_const(*memory)}};
        });
    };
    register_write(nidaqmx_function::write_analog, true);
    register_write(nidaqmx_function::write_digital, false);
    dispatcher.register_handler(nidaqmx_function::close,
        [](const RequestContext& c, std::span<const protocol::Variable> a, Arena&) -> BackendResult {
            if (a.size() != 1 || !scalar(a[0], protocol::DataType::uint64, 8)) return std::unexpected(BackendError{nidaqmx_error::invalid_arguments, "close expects a task handle"});
            if (!c.resources.remove(c.client_id, read_u64(a[0].data))) return std::unexpected(BackendError{nidaqmx_error::invalid_handle, "unknown NI-DAQmx task handle"}); return std::vector<protocol::VariableView>{};
        });
}

}  // namespace nrpc
