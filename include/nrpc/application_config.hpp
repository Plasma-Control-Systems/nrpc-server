#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>

#include "nrpc/server.hpp"

namespace nrpc {

struct ApplicationConfig {
    ServerConfig server;
    std::filesystem::path log_path{"nrpc.log"};
    std::optional<std::size_t> worker_count;
    bool tls_enabled{false};
    bool packet_dump_enabled{false};
    bool rpc_timing_enabled{false};
};

struct ApplicationConfigResult {
    ApplicationConfig config;
    bool help_requested{false};
};

// Arguments do not include the executable name. If automatic_config_path exists,
// it is loaded before processing arguments. Later arguments override values
// supplied by earlier arguments, including values loaded from INI files.
ApplicationConfigResult parse_application_config(
    std::span<const std::string_view> arguments);
ApplicationConfigResult parse_application_config(
    std::span<const std::string_view> arguments,
    const std::filesystem::path& automatic_config_path);

// Returns nrpc-server.ini in the directory containing the running executable.
std::filesystem::path automatic_application_config_path();

void load_application_config(const std::filesystem::path& path,
                             ApplicationConfig& config);
void configure_worker_pool(std::size_t worker_count);
std::string_view application_usage() noexcept;

}  // namespace nrpc
