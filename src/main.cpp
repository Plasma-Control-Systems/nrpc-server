#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "nrpc/server.hpp"
#include "nrpc/nidaqmx_backend.hpp"
#include "nrpc/lcard_backend.hpp"
#include "nrpc/serial_backend.hpp"
#include "nrpc/tcp_backend.hpp"

#ifdef _WIN32
#include <stdlib.h>
#endif

namespace {

struct AppConfig {
    nrpc::ServerConfig server;
    std::filesystem::path log_path{"nrpc.log"};
    std::optional<std::size_t> worker_count;
    bool tls_enabled{false};
    bool packet_dump_enabled{false};
};

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
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        throw std::runtime_error("invalid integer for " + std::string(name) + ": " +
                                 std::string(text));
    }
    return value;
}

std::uint16_t parse_port(std::string_view text) {
    return parse_integer<std::uint16_t>(text, "port");
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

void apply_config_value(AppConfig& config, std::string section, std::string key,
                        std::string value) {
    section = lowercase(trim(section));
    key = lowercase(trim(key));
    value = unquote(trim(value));

    if (section.empty() || section == "server") {
        if (key == "host") {
            config.server.host = value;
        } else if (key == "port") {
            config.server.port = parse_port(value);
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
        } else if (key == "default_timeout_ms" || key == "request_timeout_ms" ||
                   key == "timeout_ms") {
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
            config.packet_dump_enabled = parse_bool(value, "logging.packet_dump");
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

void load_ini_config(const std::filesystem::path& path, AppConfig& config) {
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
        if (parsed.empty()) {
            continue;
        }
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

void set_worker_count(std::size_t workers) {
    const std::string value = std::to_string(workers);
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

void print_usage() {
    std::cout << "usage: nrpc_server [port] [--config PATH] [--host HOST] "
                 "[--log-file PATH] [--workers N] "
                 "[--tls-cert PATH --tls-key PATH] [--no-tls]\n";
}

}  // namespace

int main(int argc, char** argv) {
    AppConfig config;
    bool port_seen = false;
    bool tls_option_seen = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--config" || argument == "-c") {
            if (++index >= argc) {
                std::cerr << "missing path after " << argument << '\n';
                return 2;
            }
            try {
                load_ini_config(argv[index], config);
            } catch (const std::exception& exception) {
                std::cerr << exception.what() << '\n';
                return 2;
            }
            continue;
        }
        if (argument == "--host") {
            if (++index >= argc) {
                std::cerr << "missing host after --host\n";
                return 2;
            }
            config.server.host = argv[index];
            continue;
        }
        if (argument == "--log-file") {
            if (++index >= argc) {
                std::cerr << "missing path after --log-file\n";
                return 2;
            }
            config.log_path = argv[index];
            continue;
        }
        if (argument == "--workers") {
            if (++index >= argc) {
                std::cerr << "missing count after --workers\n";
                return 2;
            }
            try {
                config.worker_count =
                    parse_integer<std::size_t>(argv[index], "workers");
            } catch (const std::exception& exception) {
                std::cerr << exception.what() << '\n';
                return 2;
            }
            if (*config.worker_count == 0) {
                std::cerr << "worker count must be greater than zero\n";
                return 2;
            }
            continue;
        }
        if (argument == "--tls-cert" || argument == "--tls-key") {
            if (++index >= argc) {
                std::cerr << "missing path after " << argument << '\n';
                return 2;
            }
            if (argument == "--tls-cert") {
                config.server.tls_certificate_path = argv[index];
            } else {
                config.server.tls_private_key_path = argv[index];
            }
            config.tls_enabled = true;
            tls_option_seen = true;
            continue;
        }
        if (argument == "--no-tls") {
            config.tls_enabled = false;
            tls_option_seen = true;
            continue;
        }
        if (argument == "--help" || argument == "-h") {
            print_usage();
            return 0;
        }
        if (port_seen) {
            print_usage();
            return 2;
        }
        try {
            config.server.port = parse_port(argument);
        } catch (const std::exception&) {
            std::cerr << "invalid port: " << argument << '\n';
            return 2;
        }
        port_seen = true;
    }

    const bool tls_paths_present =
        !config.server.tls_certificate_path.empty() &&
        !config.server.tls_private_key_path.empty();
    if (!tls_option_seen && tls_paths_present) {
        config.tls_enabled = true;
    }
    if (config.tls_enabled &&
        (config.server.tls_certificate_path.empty() ||
         config.server.tls_private_key_path.empty())) {
        std::cerr << "TLS requires certificate and private_key paths\n";
        return 2;
    }
    if (!config.tls_enabled) {
        config.server.tls_certificate_path.clear();
        config.server.tls_private_key_path.clear();
    }

    try {
        if (config.worker_count) {
            set_worker_count(*config.worker_count);
        }
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }

    std::ofstream log_file(config.log_path, std::ios::app);
    if (!log_file) {
        std::cerr << "failed to open log file: " << config.log_path.string() << '\n';
        return 1;
    }

    nrpc::Logger logger(
        {std::ref(std::clog), std::ref(log_file)},
        config.packet_dump_enabled ? nrpc::LogLevel::debug : nrpc::LogLevel::info,
        config.packet_dump_enabled);
    logger.log(
        nrpc::LogLevel::info,
        "writing server logs to " + config.log_path.string());
    if (config.worker_count) {
        logger.log(nrpc::LogLevel::info,
                   "using UV_THREADPOOL_SIZE=" +
                       std::to_string(*config.worker_count));
    }
    nrpc::ResourceManager resources;
    nrpc::Dispatcher dispatcher;
    nrpc::register_nidaqmx_backend(dispatcher);
    nrpc::register_lcard_backend(dispatcher);
    nrpc::register_serial_backend(dispatcher);
    nrpc::register_tcp_backend(dispatcher);
    dispatcher.register_handler(
        0, [](const nrpc::RequestContext&,
              std::span<const nrpc::protocol::Variable> arguments,
              nrpc::Arena& arena) -> nrpc::BackendResult {
            std::vector<nrpc::protocol::VariableView> result;
            result.reserve(arguments.size());
            for (const auto& argument : arguments) {
                auto memory = arena.allocate(argument.data.size(), 1);
                if (!memory) {
                    return std::unexpected(nrpc::BackendError{
                        nrpc::error_code::arena_exhausted, "response arena exhausted"});
                }
                std::ranges::copy(argument.data, memory->begin());
                result.push_back(
                    {argument.type, argument.element_count, std::as_const(*memory)});
            }
            return result;
        });
    dispatcher.register_handler(
        1, [](const nrpc::RequestContext&,
              std::span<const nrpc::protocol::Variable> arguments,
              nrpc::Arena&) -> nrpc::BackendResult {
            if (arguments.size() != 1 ||
                arguments[0].type != nrpc::protocol::DataType::uint32 ||
                arguments[0].element_count != 1 || arguments[0].data.size() != 4) {
                return std::unexpected(
                    nrpc::BackendError{3, "sleep expects one uint32 duration"});
            }
            const auto* data = arguments[0].data.data();
            const auto duration =
                (std::to_integer<std::uint32_t>(data[0]) << 24U) |
                (std::to_integer<std::uint32_t>(data[1]) << 16U) |
                (std::to_integer<std::uint32_t>(data[2]) << 8U) |
                std::to_integer<std::uint32_t>(data[3]);
            std::this_thread::sleep_for(std::chrono::milliseconds(duration));
            return std::vector<nrpc::protocol::VariableView>{};
        });

    try {
        nrpc::Server server(config.server, dispatcher, resources, logger);
        server.start();
        server.run();
    } catch (const std::exception& exception) {
        logger.log(nrpc::LogLevel::error, exception.what());
        return 1;
    }
    return 0;
}
