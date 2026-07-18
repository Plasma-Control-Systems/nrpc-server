#ifdef _WIN32

#define NOMINMAX
#include <windows.h>

#include "nrpc/lcard_backend.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>

namespace nrpc {
namespace {

using U32 = std::uint32_t;
using U16 = std::uint16_t;
using U8 = std::uint8_t;

constexpr U32 l_success = 0;
constexpr U32 l_not_supported = 1;
constexpr U32 stream_adc = 1;
constexpr U32 stream_dac = 2;
constexpr U32 async_adc_input = 6;
constexpr U32 async_dac_output = 9;
constexpr U32 adc_parameter = 1;
constexpr U32 dac_parameter = 2;
constexpr U32 point_size_parameter = 10001;
constexpr U32 case_dac0 = 0;
constexpr U32 case_dac1 = 1;
constexpr U32 case_adc0 = 2;
constexpr U32 case_adc1 = 3;
constexpr U32 board_pcia = 5;
constexpr U32 board_pcib = 6;
constexpr U32 board_pcic = 14;
constexpr U32 board_l791 = 19;
constexpr U32 board_e440 = 30;
constexpr U32 board_e140 = 31;
constexpr U32 board_e2010 = 32;
constexpr U32 board_e154 = 38;
constexpr U32 board_e2010b = 39;

#pragma pack(push, 1)
struct SlotParameters {
    U32 base, base_l, base1, base_l1, memory, memory_l, memory1, memory_l1;
    U32 irq, board_type, dsp_type, dma, dma_dac;
    U32 dta_reg, idma_reg, cmd_reg, irq_rst, dta_array, rdy_reg, cfg_reg;
};
struct DaqParameters { U32 type, fifo, irq_step, pages; };
struct AsyncParameters : DaqParameters {
    double rate_khz;
    U32 rate, channel_count, channels[128], data[128], mode;
};
struct DacParameters0 : DaqParameters {
    U32 auto_init;
    double rate_khz;
    U32 rate, irq_enabled, dac_enabled, dac_number;
};
struct DacParameters1 : DaqParameters {
    U32 auto_init;
    double rate_khz;
    U32 rate, irq_enabled, dac_enabled, reserved;
};
struct AdcParameters0 : DaqParameters {
    U32 auto_init;
    double rate_khz, frame_period_ms, scale_value;
    U32 rate, frame_period, scale, fp_delay, sync_type, sync_sensitivity;
    U32 sync_mode, sync_channel, sync_threshold, channel_count, channels[128];
    U32 irq_enabled, adc_enabled;
};
struct AdcParameters1 : DaqParameters {
    U32 auto_init;
    double rate_khz, frame_period_ms;
    U16 reserved1, digital_rate;
    U32 dm_enabled, rate, frame_period, start_count, stop_count, sync_type;
    U32 sync_mode, sync_threshold, sync_source, adc_input_mask;
    U32 channel_count, channels[128], irq_enabled, adc_enabled;
};
struct DescriptorLegacy {
    char serial[9], name[5], revision, dsp[5];
    U32 quartz;
    U16 dac_present, reserved[7], adc_coefficients[8], dac_coefficients[4];
    U16 custom[32];
};
struct DescriptorL791 {
    U16 crc;
    char serial[16], name[16], revision, dsp[5];
    U32 quartz;
    U16 dac_present;
    float adc_coefficients[16], dac_coefficients[4];
    U16 custom;
};
struct DescriptorE440 {
    char serial[9], name[7], revision, dsp[5], dac_present;
    U32 quartz;
    char reserved[13];
    U16 adc_coefficients[8], dac_coefficients[4], custom[32];
};
struct DescriptorE140 {
    char serial[9], name[11], revision, dsp[11], dac_present;
    U32 quartz;
    char reserved[3];
    float adc_coefficients[8], dac_coefficients[4];
    U16 custom[20];
};
struct DescriptorE2010 {
    char name[16], serial[16], dsp[16];
    U32 quartz;
    char revision, dac_present;
    float adc_coefficients[24], dac_coefficients[4];
    U16 custom[44], crc;
};
#pragma pack(pop)

using VendorHandle = std::uintptr_t;

struct Functions {
    VendorHandle(__cdecl* load_api)(char*);
    U32(__cdecl* free_api)(VendorHandle*);
    void*(__cdecl* create_instance)(VendorHandle*, U32, U32*);
    U32(__cdecl* get_slot)(VendorHandle*, SlotParameters*);
    VendorHandle(__cdecl* open_device)(VendorHandle*);
    U32(__cdecl* close_device)(VendorHandle*);
    U32(__cdecl* load_bios)(VendorHandle*, char*);
    U32(__cdecl* plata_test)(VendorHandle*);
    U32(__cdecl* read_descriptor)(VendorHandle*, void*);
    U32(__cdecl* io_async)(VendorHandle*, AsyncParameters*);
    U32(__cdecl* request_stream)(VendorHandle*, U32*, U32);
    U32(__cdecl* fill_parameters)(VendorHandle*, void*, U32);
    U32(__cdecl* set_stream)(VendorHandle*, void*, U32, U32*, void**, void**, U32);
    U32(__cdecl* get_parameter)(VendorHandle*, U32, U32*);
    void*(__cdecl* get_device2)(VendorHandle*, U32*);
    U32(__cdecl* init_start_ex)(VendorHandle*, U32);
    U32(__cdecl* start_ex)(VendorHandle*, U32);
    U32(__cdecl* stop_ex)(VendorHandle*, U32);
    U32(__cdecl* release_device2)(VendorHandle*);
};

struct Library {
    HMODULE module{};
    VendorHandle lcomp{};
    Functions functions{};
    ~Library() {
        if (lcomp != 0U && functions.free_api != nullptr)
            functions.free_api(&lcomp);
        if (module != nullptr) FreeLibrary(module);
    }
};

template <typename T>
bool load_symbol(HMODULE module, const char* name, T& output) {
    output = reinterpret_cast<T>(GetProcAddress(module, name));
    return output != nullptr;
}

BackendError vendor_error(std::string operation, U32 status,
                          U32 code = lcard_error::operation_failed) {
    operation += " failed with WLCOMP/LComp status " + std::to_string(status);
    return {status == l_not_supported ? lcard_error::unsupported : code,
            std::move(operation)};
}

std::expected<std::shared_ptr<Library>, BackendError> load_library() {
    const wchar_t* wrapper_name = sizeof(void*) == 8U ? L"wlcomp64.dll" : L"wlcomp.dll";
    HMODULE module = LoadLibraryW(wrapper_name);
    if (module == nullptr) {
        std::array<wchar_t, 32768> program_files{};
        const auto length = GetEnvironmentVariableW(
            L"ProgramFiles(x86)", program_files.data(),
            static_cast<DWORD>(program_files.size()));
        if (length > 0U && length < program_files.size()) {
            std::wstring installed(program_files.data(), length);
            installed += L"\\LCard\\LIBRARY\\BIN\\";
            installed += wrapper_name;
            module = LoadLibraryW(installed.c_str());
        }
    }
    if (module == nullptr)
        return std::unexpected(BackendError{lcard_error::library_load_failed,
            "failed to load " + std::string(sizeof(void*) == 8U ? "wlcomp64.dll" : "wlcomp.dll")});
    auto library = std::make_shared<Library>();
    library->module = module;
    auto& f = library->functions;
    const bool loaded =
        load_symbol(module, "LoadAPIDLL", f.load_api) &&
        load_symbol(module, "FreeAPIDLL", f.free_api) &&
        load_symbol(module, "CallCreateInstance", f.create_instance) &&
        load_symbol(module, "GetSlotParam", f.get_slot) &&
        load_symbol(module, "OpenLDevice", f.open_device) &&
        load_symbol(module, "CloseLDevice", f.close_device) &&
        load_symbol(module, "LoadBios", f.load_bios) &&
        load_symbol(module, "PlataTest", f.plata_test) &&
        load_symbol(module, "ReadPlataDescr", f.read_descriptor) &&
        load_symbol(module, "IoAsync", f.io_async) &&
        load_symbol(module, "RequestBufferStream", f.request_stream) &&
        load_symbol(module, "FillDAQparameters", f.fill_parameters) &&
        load_symbol(module, "SetParametersStream", f.set_stream) &&
        load_symbol(module, "GetParameter", f.get_parameter) &&
        load_symbol(module, "Get_LDEV2_Interface", f.get_device2) &&
        load_symbol(module, "InitStartLDeviceEx", f.init_start_ex) &&
        load_symbol(module, "StartLDeviceEx", f.start_ex) &&
        load_symbol(module, "StopLDeviceEx", f.stop_ex) &&
        load_symbol(module, "Release_LDEV2_Interface", f.release_device2);
    if (!loaded)
        return std::unexpected(BackendError{lcard_error::library_load_failed,
                                             "WLCOMP is missing a required export"});

    std::string lcomp_name = sizeof(void*) == 8U ? "lcomp64.dll" : "lcomp.dll";
    library->lcomp = f.load_api(lcomp_name.data());
    if (library->lcomp == 0U) {
        std::array<char, 32768> program_files{};
        const auto length = GetEnvironmentVariableA(
            "ProgramFiles(x86)", program_files.data(),
            static_cast<DWORD>(program_files.size()));
        if (length > 0U && length < program_files.size()) {
            std::string installed(program_files.data(), length);
            installed += "\\LCard\\LIBRARY\\BIN\\" + lcomp_name;
            library->lcomp = f.load_api(installed.data());
        }
    }
    if (library->lcomp == 0U)
        return std::unexpected(BackendError{lcard_error::library_load_failed,
                                             "WLCOMP failed to load the LComp runtime"});
    return library;
}

std::string fixed_string(const char* value, std::size_t size) {
    const auto* end = std::find(value, value + size, '\0');
    return std::string(value, end);
}

bool needs_firmware(U32 type) {
    return type == board_pcia || type == board_pcib || type == board_pcic ||
           type == board_e440 || type == board_e2010 || type == board_e2010b;
}
bool is_adc1(U32 type) {
    return type == board_l791 || type == board_e2010 || type == board_e2010b;
}

class NativeDevice final : public LcardDevice {
public:
    NativeDevice(std::shared_ptr<Library> library, VendorHandle device,
                 VendorHandle device2, SlotParameters slot,
                 LcardDeviceInfo info)
        : library_(std::move(library)), device_(device), device2_(device2),
          slot_(slot), info_(std::move(info)) {}
    ~NativeDevice() override { close(); }

    const LcardDeviceInfo& info() const noexcept override { return info_; }

    std::expected<std::int16_t, BackendError> read_adc(
        const LcardAdcChannel& channel, double timeout) override {
        std::scoped_lock lock(mutex_);
        if (is_adc1(slot_.board_type) && slot_.board_type != board_l791) {
            auto frame = read_frame_locked(std::span(&channel, 1), 1U, 100.0,
                                           timeout);
            if (!frame) return std::unexpected(frame.error());
            return frame->codes.front();
        }
        auto logical = logical_channel(channel);
        if (!logical) return std::unexpected(logical.error());
        AsyncParameters parameters{};
        parameters.type = async_adc_input;
        parameters.channel_count = 1;
        parameters.channels[0] = *logical;
        const auto status = library_->functions.io_async(&device_, &parameters);
        if (status != l_success)
            return std::unexpected(vendor_error("IoAsync ADC", status));
        return std::bit_cast<std::int16_t>(static_cast<U16>(parameters.data[0]));
    }

    std::expected<LcardAdcFrame, BackendError> read_adc_frame(
        std::span<const LcardAdcChannel> channels, U32 sample_count,
        double rate, double timeout) override {
        std::scoped_lock lock(mutex_);
        return read_frame_locked(channels, sample_count, rate, timeout);
    }

    std::expected<void, BackendError> write_dac(
        U8 channel, U16 code, double) override {
        std::scoped_lock lock(mutex_);
        if (!info_.dac_present)
            return std::unexpected(BackendError{lcard_error::unsupported,
                                                 "device has no DAC"});
        if (channel > (slot_.board_type == board_e154 ? 0U : 1U))
            return std::unexpected(BackendError{lcard_error::invalid_arguments,
                                                 "DAC channel is out of range"});
        const U16 maximum = slot_.board_type == board_e154 ? 0xffU :
                            slot_.board_type == board_e140 ? 0xffffU : 0xfffU;
        if (code > maximum)
            return std::unexpected(BackendError{lcard_error::invalid_arguments,
                                                 "DAC code exceeds device width"});
        AsyncParameters parameters{};
        parameters.type = async_dac_output;
        if (slot_.board_type == board_l791) {
            parameters.channels[channel] = 1;
            parameters.data[channel] = code;
        } else {
            parameters.mode = channel;
            parameters.data[0] = code;
        }
        const auto status = library_->functions.io_async(&device_, &parameters);
        if (status != l_success)
            return std::unexpected(vendor_error("IoAsync DAC", status));
        return {};
    }

    std::expected<LcardDacWriteResult, BackendError> write_dac_frames(
        std::span<const U16> dac0, std::span<const U16> dac1,
        double frame_rate, double timeout) override {
        std::scoped_lock lock(mutex_);
        if ((info_.capabilities & lcard_capability::write_dac_frames) == 0U)
            return std::unexpected(BackendError{lcard_error::unsupported,
                                                 "timed DAC is not supported"});
        const U16 stream_maximum = slot_.board_type == board_e140 ? 0xffffU : 0xfffU;
        if (std::ranges::any_of(dac0, [&](U16 v) { return v > stream_maximum; }) ||
            std::ranges::any_of(dac1, [&](U16 v) { return v > stream_maximum; }))
            return std::unexpected(BackendError{lcard_error::invalid_arguments,
                                                 "DAC stream codes must be 12-bit"});
        const bool packed = slot_.board_type == board_l791;
        const bool e140 = slot_.board_type == board_e140;
        const bool pc32 = slot_.board_type == board_pcic &&
                          info_.board_name.find("L780") != std::string::npos;
        const U32 point_count = static_cast<U32>(dac0.size() * (packed ? 1U : 2U));
        U32 requested_words = point_count * ((packed || pc32) ? 2U : 1U);
        auto status = library_->functions.request_stream(
            &device_, &requested_words, stream_dac);
        if (status != l_success)
            return std::unexpected(vendor_error("RequestBufferStream DAC", status));

        double actual_rate{};
        U32 used = requested_words;
        void* data{};
        void* sync{};
        U32 irq_step{};
        if (packed) {
            DacParameters1 parameters{};
            parameters.type = dac_parameter;
            parameters.rate_khz = frame_rate;
            parameters.fifo = 0;
            parameters.irq_step = std::min<U32>(2048U, std::max<U32>(1U, point_count));
            parameters.pages = std::max<U32>(2U, (point_count + parameters.irq_step - 1U) / parameters.irq_step);
            parameters.irq_enabled = 1;
            parameters.dac_enabled = 1;
            status = library_->functions.fill_parameters(
                &device_, &parameters, case_dac1);
            actual_rate = parameters.rate_khz;
            irq_step = parameters.irq_step;
            if (status == l_success)
                status = library_->functions.set_stream(
                    &device_, &parameters, case_dac1, &used, &data, &sync,
                    stream_dac);
        } else if (pc32) {
            auto* words = static_cast<U32*>(data);
            for (std::size_t i = 0; i < dac0.size(); ++i) {
                words[i * 2U] = dac0[i];
                words[i * 2U + 1U] = static_cast<U32>(dac1[i] | 0x1000U);
            }
        } else {
            DacParameters0 parameters{};
            parameters.type = dac_parameter;
            parameters.rate_khz = frame_rate * 2.0;
            parameters.fifo = e140 ? 0U : std::clamp<U32>(point_count, 32U, 2016U);
            parameters.irq_step = e140 ? 2048U : parameters.fifo;
            parameters.pages = std::max<U32>(2U, (point_count + parameters.irq_step - 1U) / parameters.irq_step);
            parameters.irq_enabled = 1;
            parameters.dac_enabled = 1;
            status = library_->functions.fill_parameters(
                &device_, &parameters, case_dac0);
            actual_rate = parameters.rate_khz / 2.0;
            irq_step = parameters.irq_step;
            if (status == l_success)
                status = library_->functions.set_stream(
                    &device_, &parameters, case_dac0, &used, &data, &sync,
                    stream_dac);
        }
        if (status != l_success)
            return std::unexpected(vendor_error("configure DAC stream", status));
        if (data == nullptr || sync == nullptr || used < point_count)
            return std::unexpected(BackendError{lcard_error::operation_failed,
                                                 "DAC mapped buffer is too small"});
        if (packed) {
            auto* words = static_cast<U32*>(data);
            for (std::size_t i = 0; i < dac0.size(); ++i) {
                U32 word = dac0[i] | (1U << 12U) |
                           (static_cast<U32>(dac1[i]) << 16U) | (1U << 28U);
                if ((i + 1U) % irq_step == 0U || i + 1U == dac0.size())
                    word |= 1U << 31U;
                words[i] = word;
            }
        } else {
            auto* words = static_cast<U16*>(data);
            for (std::size_t i = 0; i < dac0.size(); ++i) {
                words[i * 2U] = dac0[i];
                words[i * 2U + 1U] = e140 ? dac1[i]
                                          : static_cast<U16>(dac1[i] | 0x1000U);
            }
        }
        return run_dac(point_count, static_cast<U32>(dac0.size()), actual_rate,
                       sync, timeout);
    }

    void close() noexcept override {
        std::scoped_lock lock(mutex_);
        if (device2_ != 0U) {
            library_->functions.stop_ex(&device2_, stream_adc);
            library_->functions.stop_ex(&device2_, stream_dac);
            library_->functions.release_device2(&device2_);
            device2_ = 0U;
        }
        if (device_ != 0U) {
            library_->functions.close_device(&device_);
            device_ = 0U;
        }
    }

private:
    std::expected<U32, BackendError> logical_channel(
        const LcardAdcChannel& value) const {
        if (value.raw) return value.channel;
        if (slot_.board_type == board_e2010 || slot_.board_type == board_e2010b) {
            if (value.channel > 3U)
                return std::unexpected(BackendError{lcard_error::invalid_arguments,
                                                     "E20-10 ADC channel is out of range"});
            if (value.mode == LcardAdcInputMode::common_ground)
                return std::unexpected(BackendError{lcard_error::unsupported,
                    "E20-10 common-ground input mode is unsupported"});
            return value.channel;
        }
        const bool e14 = slot_.board_type == board_e140 ||
                         slot_.board_type == board_e440;
        if (!e14 && value.mode != LcardAdcInputMode::differential)
            return std::unexpected(BackendError{lcard_error::unsupported,
                "use a raw logical channel for this input mode"});
        std::span<const double> ranges;
        static constexpr std::array l7_ranges{10.0, 2.5, 0.625, 0.15625};
        static constexpr std::array l783_ranges{10.0, 5.0, 2.5, 1.25};
        static constexpr std::array l791_ranges{10.0, 5.0, 2.5, 1.25, 0.625, 0.3125, 0.15625, 0.078125};
        static constexpr std::array e154_ranges{5.0, 1.6, 0.5, 0.16};
        if (slot_.board_type == board_l791) ranges = l791_ranges;
        else if (slot_.board_type == board_e154) ranges = e154_ranges;
        else if (info_.board_name.find("L783") != std::string::npos) ranges = l783_ranges;
        else ranges = l7_ranges;
        const auto iterator = std::ranges::find_if(ranges, [&](double candidate) {
            return std::abs(candidate - value.range) <= candidate * 1e-6;
        });
        if (iterator == ranges.end())
            return std::unexpected(BackendError{lcard_error::invalid_arguments,
                                                 "ADC range is not supported"});
        const auto index = static_cast<U32>(iterator - ranges.begin());
        const U32 max_channel = e14
            ? (value.mode == LcardAdcInputMode::common_ground ? 31U : 15U)
            : slot_.board_type == board_l791 ? 15U
            : slot_.board_type == board_e154 ? 7U : 31U;
        if (value.channel > max_channel)
            return std::unexpected(BackendError{lcard_error::invalid_arguments,
                                                 "ADC channel is out of range"});
        U32 input_mode{};
        if (e14) {
            if (value.mode == LcardAdcInputMode::zero)
                input_mode = 0x10U;
            else if (value.mode == LcardAdcInputMode::common_ground)
                input_mode = 0x20U;
        }
        return value.channel | input_mode | (index << 6U);
    }

    std::expected<LcardAdcFrame, BackendError> read_frame_locked(
        std::span<const LcardAdcChannel> channels, U32 sample_count,
        double rate, double timeout) {
        std::array<U32, 128> logical{};
        for (std::size_t i = 0; i < channels.size(); ++i) {
            auto encoded = logical_channel(channels[i]);
            if (!encoded) return std::unexpected(encoded.error());
            logical[i] = *encoded;
        }
        U32 point_size = 2;
        if (library_->functions.get_parameter(
                &device_, point_size_parameter, &point_size) != l_success ||
            point_size < 2U)
            point_size = 2;
        const U32 words_per_point = (point_size + 1U) / 2U;
        U32 requested_words = sample_count * words_per_point;
        auto status = library_->functions.request_stream(
            &device_, &requested_words, stream_adc);
        if (status != l_success)
            return std::unexpected(vendor_error("RequestBufferStream ADC", status));
        U32 used = requested_words;
        void* data{};
        void* sync{};
        double actual_rate{};
        if (is_adc1(slot_.board_type)) {
            AdcParameters1 parameters{};
            parameters.type = adc_parameter;
            parameters.rate_khz = rate;
            parameters.frame_period_ms = 0;
            parameters.fifo = slot_.board_type == board_l791 ? 128U : 0U;
            parameters.irq_step = slot_.board_type == board_l791 ? 2048U : 4096U;
            parameters.pages = std::max<U32>(
                2U, (sample_count + parameters.irq_step - 1U) /
                         parameters.irq_step);
            parameters.sync_type = 0;
            if (slot_.board_type == board_e2010 || slot_.board_type == board_e2010b) {
                auto mask = e2010_mask(channels);
                if (!mask) return std::unexpected(mask.error());
                parameters.adc_input_mask = *mask;
                parameters.sync_type = 1;
            }
            parameters.channel_count = static_cast<U32>(channels.size());
            std::copy_n(logical.begin(), channels.size(), parameters.channels);
            parameters.irq_enabled = 1;
            parameters.adc_enabled = 1;
            status = library_->functions.fill_parameters(
                &device_, &parameters, case_adc1);
            actual_rate = parameters.rate_khz;
            if (status == l_success)
                status = library_->functions.set_stream(
                    &device_, &parameters, case_adc1, &used, &data, &sync,
                    stream_adc);
        } else {
            AdcParameters0 parameters{};
            parameters.type = adc_parameter;
            parameters.rate_khz = rate;
            parameters.frame_period_ms = 0;
            parameters.fifo = (slot_.board_type == board_e140 || slot_.board_type == board_e154) ? 0U : 1024U;
            parameters.irq_step = 4096;
            parameters.pages = std::max<U32>(
                2U, (sample_count + parameters.irq_step - 1U) /
                         parameters.irq_step);
            parameters.sync_type = (slot_.board_type == board_pcia || slot_.board_type == board_pcib || slot_.board_type == board_pcic) ? 3U : 0U;
            parameters.channel_count = static_cast<U32>(channels.size());
            std::copy_n(logical.begin(), channels.size(), parameters.channels);
            parameters.irq_enabled = 1;
            parameters.adc_enabled = 1;
            status = library_->functions.fill_parameters(
                &device_, &parameters, case_adc0);
            actual_rate = parameters.rate_khz;
            if (status == l_success)
                status = library_->functions.set_stream(
                    &device_, &parameters, case_adc0, &used, &data, &sync,
                    stream_adc);
        }
        if (status != l_success)
            return std::unexpected(vendor_error("configure ADC stream", status));
        if (data == nullptr || sync == nullptr || used < sample_count)
            return std::unexpected(BackendError{lcard_error::operation_failed,
                                                 "ADC mapped buffer is too small"});
        if (device2_ == 0U)
            return std::unexpected(BackendError{lcard_error::unsupported,
                                                 "WLCOMP LDEV2 interface is unavailable"});
        status = library_->functions.init_start_ex(&device2_, stream_adc);
        if (status == l_success)
            status = library_->functions.start_ex(&device2_, stream_adc);
        if (status != l_success) {
            library_->functions.stop_ex(&device2_, stream_adc);
            return std::unexpected(vendor_error("start ADC stream", status));
        }
        const auto waited = wait_for(sync, sample_count, timeout);
        library_->functions.stop_ex(&device2_, stream_adc);
        if (!waited) return std::unexpected(waited.error());
        LcardAdcFrame result;
        result.actual_rate_khz = actual_rate;
        result.codes.resize(sample_count);
        const auto* bytes = static_cast<const std::byte*>(data);
        for (std::size_t i = 0; i < sample_count; ++i) {
            U16 code{};
            std::memcpy(&code, bytes + i * point_size, sizeof(code));
            result.codes[i] = std::bit_cast<std::int16_t>(code);
        }
        return result;
    }

    std::expected<U32, BackendError> e2010_mask(
        std::span<const LcardAdcChannel> channels) const {
        static constexpr std::array<U32, 4> signal{0x0400U, 0x0200U, 0x1000U, 0x0800U};
        static constexpr std::array<U32, 4> range_10{0x0008U, 0x0002U, 0x8000U, 0x2000U};
        static constexpr std::array<U32, 4> range_03{0x0010U, 0x0004U, 0x0100U, 0x4000U};
        U32 mask{};
        for (const auto& channel : channels) {
            if (channel.channel > 3U)
                return std::unexpected(BackendError{lcard_error::invalid_arguments,
                                                     "E20-10 ADC channel is out of range"});
            const auto index = static_cast<std::size_t>(channel.channel);
            if (channel.raw) {
                mask |= signal[index];
                continue;
            }
            if (channel.mode != LcardAdcInputMode::zero)
                mask |= signal[index];
            if (std::abs(channel.range - 3.0) <= 3e-6) {
                // Zero bits select the +/-3 V range.
            } else if (std::abs(channel.range - 1.0) <= 1e-6) {
                mask |= range_10[index];
            } else if (std::abs(channel.range - 0.3) <= 3e-7) {
                mask |= range_03[index];
            } else {
                return std::unexpected(BackendError{lcard_error::invalid_arguments,
                    "E20-10 supports full-scale ranges 3.0, 1.0, and 0.3 V"});
            }
        }
        return mask;
    }

    std::expected<void, BackendError> wait_for(void* sync, U32 target,
                                               double timeout) const {
        if (sync == nullptr)
            return std::unexpected(BackendError{lcard_error::operation_failed,
                                                 "stream sync pointer is null"});
        const auto start = GetTickCount64();
        const auto timeout_ms = timeout <= 0.0 ? 0ULL :
            static_cast<ULONGLONG>(std::ceil(timeout * 1000.0));
        auto* counter = static_cast<volatile U32*>(sync);
        while (*counter < target) {
            if (timeout_ms == 0ULL || GetTickCount64() - start >= timeout_ms)
                return std::unexpected(BackendError{lcard_error::timeout,
                                                     "LCard stream timed out"});
            Sleep(1);
        }
        return {};
    }

    std::expected<LcardDacWriteResult, BackendError> run_dac(
        U32 point_count, U32 frames, double actual_rate, void* sync,
        double timeout) {
        if (device2_ == 0U)
            return std::unexpected(BackendError{lcard_error::unsupported,
                                                 "WLCOMP LDEV2 interface is unavailable"});
        auto status = library_->functions.init_start_ex(&device2_, stream_dac);
        if (status == l_success)
            status = library_->functions.start_ex(&device2_, stream_dac);
        if (status != l_success) {
            library_->functions.stop_ex(&device2_, stream_dac);
            return std::unexpected(vendor_error("start DAC stream", status));
        }
        auto waited = wait_for(sync, point_count, timeout);
        library_->functions.stop_ex(&device2_, stream_dac);
        if (!waited) return std::unexpected(waited.error());
        return LcardDacWriteResult{frames, actual_rate};
    }

    std::shared_ptr<Library> library_;
    VendorHandle device_{};
    VendorHandle device2_{};
    SlotParameters slot_{};
    LcardDeviceInfo info_;
    std::mutex mutex_;
};

class NativeDriver final : public LcardDriver {
public:
    std::expected<std::unique_ptr<LcardDevice>, BackendError> open(
        U32 slot_number, std::string_view firmware) override {
        auto library = load_library();
        if (!library) return std::unexpected(library.error());
        auto& f = (*library)->functions;
        U32 status{};
        void* interface_pointer = f.create_instance(
            &(*library)->lcomp, slot_number, &status);
        if (interface_pointer == nullptr || status != l_success)
            return std::unexpected(BackendError{lcard_error::open_failed,
                                                 "WLCOMP CallCreateInstance failed"});
        VendorHandle device = reinterpret_cast<VendorHandle>(interface_pointer);
        auto fail = [&](BackendError error) -> std::expected<std::unique_ptr<LcardDevice>, BackendError> {
            f.close_device(&device);
            return std::unexpected(std::move(error));
        };
        if (f.open_device(&device) ==
            reinterpret_cast<VendorHandle>(INVALID_HANDLE_VALUE))
            return fail({lcard_error::open_failed, "OpenLDevice failed"});
        SlotParameters slot{};
        status = f.get_slot(&device, &slot);
        if (status != l_success)
            return fail(vendor_error("GetSlotParam", status, lcard_error::open_failed));
        if (needs_firmware(slot.board_type) && firmware.empty())
            return fail({lcard_error::open_failed,
                         "this device requires an explicit firmware path"});
        if (!firmware.empty()) {
            std::string path(firmware);
            status = f.load_bios(&device, path.data());
            if (status != l_success)
                return fail(vendor_error("LoadBios", status, lcard_error::open_failed));
        }
        status = f.plata_test(&device);
        if (status != l_success)
            return fail(vendor_error("PlataTest", status, lcard_error::open_failed));
        std::array<std::byte, 256> descriptor{};
        status = f.read_descriptor(&device, descriptor.data());
        if (status != l_success)
            return fail(vendor_error("ReadPlataDescr", status, lcard_error::open_failed));

        LcardDeviceInfo info{};
        info.board_type = slot.board_type;
        switch (slot.board_type) {
        case board_l791: {
            const auto& value = *reinterpret_cast<const DescriptorL791*>(descriptor.data());
            info.board_name = fixed_string(value.name, sizeof(value.name));
            info.revision = static_cast<U8>(value.revision);
            info.dac_present = value.dac_present != 0;
            break;
        }
        case board_e440: {
            const auto& value = *reinterpret_cast<const DescriptorE440*>(descriptor.data());
            info.board_name = fixed_string(value.name, sizeof(value.name));
            info.revision = static_cast<U8>(value.revision);
            info.dac_present = value.dac_present != 0;
            break;
        }
        case board_e140:
        case board_e154: {
            const auto& value = *reinterpret_cast<const DescriptorE140*>(descriptor.data());
            info.board_name = fixed_string(value.name, sizeof(value.name));
            info.revision = static_cast<U8>(value.revision);
            info.dac_present = value.dac_present != 0;
            break;
        }
        case board_e2010:
        case board_e2010b: {
            const auto& value = *reinterpret_cast<const DescriptorE2010*>(descriptor.data());
            info.board_name = fixed_string(value.name, sizeof(value.name));
            info.revision = static_cast<U8>(value.revision);
            info.dac_present = value.dac_present != 0;
            break;
        }
        default: {
            const auto& value = *reinterpret_cast<const DescriptorLegacy*>(descriptor.data());
            info.board_name = fixed_string(value.name, sizeof(value.name));
            info.revision = static_cast<U8>(value.revision);
            info.dac_present = value.dac_present != 0;
            break;
        }
        }
        info.capabilities = lcard_capability::read_adc |
                            lcard_capability::read_adc_frame;
        if (info.dac_present) info.capabilities |= lcard_capability::write_dac;
        const bool e140_stream = slot.board_type == board_e140 &&
            info.revision != static_cast<U8>('A') &&
            info.board_name.find('M') != std::string::npos;
        if (info.dac_present &&
            (slot.board_type == board_pcia || slot.board_type == board_pcib ||
             slot.board_type == board_pcic || slot.board_type == board_e440 ||
             slot.board_type == board_l791 || e140_stream))
            info.capabilities |= lcard_capability::write_dac_frames;

        U32 device2_status{};
        void* device2_pointer = f.get_device2(&device, &device2_status);
        VendorHandle device2 = device2_status == l_success
            ? reinterpret_cast<VendorHandle>(device2_pointer) : 0U;
        return std::make_unique<NativeDevice>(*library, device, device2, slot,
                                              std::move(info));
    }
};

}  // namespace

std::shared_ptr<LcardDriver> create_lcard_native_driver() {
    return std::make_shared<NativeDriver>();
}

}  // namespace nrpc

#endif
