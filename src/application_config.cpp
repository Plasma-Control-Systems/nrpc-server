#include "nrpc/application_config.hpp"

#include <charconv>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <stdlib.h>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace nrpc {
namespace {

std::string trim(std::string_view value) {
    auto first = value.begin();
    auto last = value.end();
    while (first != last &&
           std::isspace(static_cast<unsigned char>(*first)) != 0) {
        ++first;
    }
    while (first != last &&
           std::isspace(static_cast<unsigned char>(*(last - 1))) != 0) {
        --last;
    }
    return std::string(first, last);
}

std::string lowercase(std::string value) {
    for (char& character : value) {
        character = static_cast<char>(
            std::tolower(static_cast<unsigned char>(character)));
    }
    return value;
}

std::string strip_inline_comment(std::string_view line) {
    bool quoted = false;
    char quote = '\0';
    for (std::size_t index = 0; index < line.size(); ++index) {
        const char character = line[index];
        if ((character == '"' || character == '\'') &&
            (index == 0 || line[index - 1] != '\\')) {
            if (!quoted) {
                quoted = true;
                quote = character;
            } else if (quote == character) {
                quoted = false;
            }
        }
        if (!quoted && (character == ';' || character == '#')) {
            return trim(line.substr(0, index));
        }
    }
    return trim(line);
}

std::string unquote(std::string value) {
    if (value.size() >= 2 &&
        ((value.front() == '"' && value.back() == '"') ||
         (value.front() == '\'' && value.back() == '\''))) {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

template <typename Integer>
Integer parse_integer(std::string_view text, std::string_view name) {
    Integer value{};
    const auto result =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        throw std::runtime_error("invalid integer for " + std::string(name) +
                                 ": " + std::string(text));
    }
    return value;
}

bool parse_bool(std::string value, std::string_view name) {
    value = lowercase(trim(value));
    if (value == "1" || value == "true" || value == "yes" || value == "on" ||
        value == "enabled") {
        return true;
    }
    if (value == "0" || value == "false" || value == "no" || value == "off" ||
        value == "disabled") {
        return false;
    }
    throw std::runtime_error("invalid boolean for " + std::string(name) + ": " +
                             value);
}

void apply_config_value(ApplicationConfig& config, std::string section,
                        std::string key, std::string value) {
    section = lowercase(trim(section));
    key = lowercase(trim(key));
    value = unquote(trim(value));

    if (section.empty() || section == "server") {
        if (key == "host") {
            config.server.host = value;
        } else if (key == "port") {
            config.server.port = parse_integer<std::uint16_t>(value, "port");
        } else if (key == "workers" || key == "worker_count" ||
                   key == "worker_threads") {
            config.worker_count = parse_integer<std::size_t>(value, key);
            if (*config.worker_count == 0) {
                throw std::runtime_error("worker count must be greater than zero");
            }
        } else if (key == "backlog") {
            config.server.backlog = parse_integer<int>(value, key);
            if (config.server.backlog <= 0) {
                throw std::runtime_error("backlog must be greater than zero");
            }
        } else if (key == "max_payload_size") {
            config.server.max_payload_size =
                parse_integer<std::uint32_t>(value, key);
        } else if (key == "arena_size") {
            config.server.arena_size = parse_integer<std::size_t>(value, key);
        } else if (key == "max_pending_requests") {
            config.server.max_pending_requests =
                parse_integer<std::size_t>(value, key);
            if (config.server.max_pending_requests == 0) {
                throw std::runtime_error(
                    "max_pending_requests must be greater than zero");
            }
        } else if (key == "max_pending_request_bytes") {
            config.server.max_pending_request_bytes =
                parse_integer<std::size_t>(value, key);
            if (config.server.max_pending_request_bytes == 0) {
                throw std::runtime_error(
                    "max_pending_request_bytes must be greater than zero");
            }
        } else if (key == "default_timeout_ms" ||
                   key == "request_timeout_ms" || key == "timeout_ms") {
            config.server.default_timeout_ms =
                parse_integer<std::uint32_t>(value, key);
        } else {
            throw std::runtime_error("unknown server config key: " + key);
        }
    } else if (section == "logging" || section == "log") {
        if (key == "file" || key == "path") {
            config.log_path = value;
        } else if (key == "packet_dump" || key == "packet_dumps" ||
                   key == "debug_packets") {
            config.packet_dump_enabled =
                parse_bool(value, "logging.packet_dump");
        } else if (key == "rpc_timing" || key == "rpc_timings" ||
                   key == "debug_rpc_timing") {
            config.rpc_timing_enabled =
                parse_bool(value, "logging.rpc_timing");
        } else {
            throw std::runtime_error("unknown logging config key: " + key);
        }
    } else if (section == "tls") {
        if (key == "enabled" || key == "enable") {
            config.tls_enabled = parse_bool(value, "tls.enabled");
        } else if (key == "certificate" || key == "cert" ||
                   key == "cert_file" || key == "certificate_path") {
            config.server.tls_certificate_path = value;
        } else if (key == "private_key" || key == "key" ||
                   key == "key_file" || key == "private_key_path") {
            config.server.tls_private_key_path = value;
        } else {
            throw std::runtime_error("unknown TLS config key: " + key);
        }
    } else {
        throw std::runtime_error("unknown config section: " + section);
    }
}

std::string_view next_argument(std::span<const std::string_view> arguments,
                               std::size_t& index, std::string_view option,
                               std::string_view description) {
    ++index;
    if (index >= arguments.size()) {
        throw std::runtime_error("missing " + std::string(description) +
                                 " after " + std::string(option));
    }
    return arguments[index];
}

}  // namespace

void load_application_config(const std::filesystem::path& path,
                             ApplicationConfig& config) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("failed to open config file: " + path.string());
    }

    std::string section;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        std::string parsed = strip_inline_comment(line);
        if (parsed.empty()) continue;
        if (parsed.front() == '[') {
            if (parsed.back() != ']') {
                throw std::runtime_error("invalid section header in " +
                                         path.string() + ":" +
                                         std::to_string(line_number));
            }
            section = trim(std::string_view(parsed).substr(1, parsed.size() - 2));
            continue;
        }
        const auto separator = parsed.find('=');
        if (separator == std::string::npos) {
            throw std::runtime_error("expected key=value in " + path.string() +
                                     ":" + std::to_string(line_number));
        }
        try {
            apply_config_value(config, section, parsed.substr(0, separator),
                               parsed.substr(separator + 1));
        } catch (const std::exception& exception) {
            throw std::runtime_error(path.string() + ":" +
                                     std::to_string(line_number) + ": " +
                                     exception.what());
        }
    }
}

ApplicationConfigResult parse_application_config(
    std::span<const std::string_view> arguments) {
    return parse_application_config(arguments, std::filesystem::path{});
}

ApplicationConfigResult parse_application_config(
    std::span<const std::string_view> arguments,
    const std::filesystem::path& automatic_config_path) {
    ApplicationConfigResult result;
    bool port_seen = false;

    if (!automatic_config_path.empty()) {
        std::error_code error;
        const bool config_exists =
            std::filesystem::exists(automatic_config_path, error);
        if (error) {
            throw std::runtime_error("failed to inspect automatic config file: " +
                                     automatic_config_path.string() + ": " +
                                     error.message());
        }
        if (config_exists) {
            load_application_config(automatic_config_path, result.config);
        }
    }

    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const std::string_view argument = arguments[index];
        if (argument == "--config" || argument == "-c") {
            load_application_config(
                next_argument(arguments, index, argument, "path"), result.config);
        } else if (argument == "--host") {
            result.config.server.host =
                next_argument(arguments, index, argument, "host");
        } else if (argument == "--log-file") {
            result.config.log_path =
                next_argument(arguments, index, argument, "path");
        } else if (argument == "--workers") {
            const auto value = next_argument(arguments, index, argument, "count");
            result.config.worker_count =
                parse_integer<std::size_t>(value, "workers");
            if (*result.config.worker_count == 0) {
                throw std::runtime_error("worker count must be greater than zero");
            }
        } else if (argument == "--tls-cert" || argument == "--tls-key") {
            const auto value = next_argument(arguments, index, argument, "path");
            if (argument == "--tls-cert") {
                result.config.server.tls_certificate_path = value;
            } else {
                result.config.server.tls_private_key_path = value;
            }
            result.config.tls_enabled = true;
        } else if (argument == "--no-tls") {
            result.config.tls_enabled = false;
        } else if (argument == "--help" || argument == "-h") {
            result.help_requested = true;
            return result;
        } else {
            if (port_seen) {
                throw std::runtime_error("unexpected argument: " +
                                         std::string(argument));
            }
            try {
                result.config.server.port =
                    parse_integer<std::uint16_t>(argument, "port");
            } catch (const std::exception&) {
                throw std::runtime_error("invalid port: " + std::string(argument));
            }
            port_seen = true;
        }
    }

    if (result.config.tls_enabled &&
        (result.config.server.tls_certificate_path.empty() ||
         result.config.server.tls_private_key_path.empty())) {
        throw std::runtime_error("TLS requires certificate and private_key paths");
    }
    if (!result.config.tls_enabled) {
        result.config.server.tls_certificate_path.clear();
        result.config.server.tls_private_key_path.clear();
    }
    return result;
}

std::filesystem::path automatic_application_config_path() {
#ifdef _WIN32
    std::vector<wchar_t> buffer(256);
    for (;;) {
        const DWORD length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            throw std::runtime_error(
                "failed to determine executable path: " +
                std::system_category().message(
                    static_cast<int>(GetLastError())));
        }
        if (length < buffer.size()) {
            return std::filesystem::path(buffer.data(), buffer.data() + length)
                       .parent_path() /
                   "nrpc-server.ini";
        }
        buffer.resize(buffer.size() * 2);
    }
#else
    std::error_code error;
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
    if (error) {
        throw std::runtime_error("failed to determine executable path: " +
                                 error.message());
    }
    return executable.parent_path() / "nrpc-server.ini";
#endif
}

void configure_worker_pool(std::size_t worker_count) {
    const std::string value = std::to_string(worker_count);
#ifdef _WIN32
    if (_putenv_s("UV_THREADPOOL_SIZE", value.c_str()) != 0) {
        throw std::runtime_error("failed to set UV_THREADPOOL_SIZE");
    }
#else
    if (setenv("UV_THREADPOOL_SIZE", value.c_str(), 1) != 0) {
        throw std::runtime_error("failed to set UV_THREADPOOL_SIZE");
    }
#endif
}

std::string_view application_usage() noexcept {
    return "usage: nrpc_server [port] [--config PATH] [--host HOST] "
           "[--log-file PATH] [--workers N] "
           "[--tls-cert PATH --tls-key PATH] [--no-tls]\n";
}

}  // namespace nrpc
