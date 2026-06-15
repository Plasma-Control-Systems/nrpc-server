#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <ranges>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include "nrpc/server.hpp"
#if NRPC_HAS_SERIAL_BACKEND
#include "nrpc/serial_backend.hpp"
#endif
#include "nrpc/tcp_backend.hpp"

int main(int argc, char** argv) {
    std::uint16_t port = 9000;
    std::filesystem::path log_path{"nrpc.log"};
    bool port_seen = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--log-file") {
            if (++index >= argc) {
                std::cerr << "missing path after --log-file\n";
                return 2;
            }
            log_path = argv[index];
            continue;
        }
        if (argument == "--help" || argument == "-h") {
            std::cout << "usage: nrpc_server [port] [--log-file PATH]\n";
            return 0;
        }
        if (port_seen) {
            std::cerr << "usage: nrpc_server [port] [--log-file PATH]\n";
            return 2;
        }
        const auto result = std::from_chars(
            argument.data(), argument.data() + argument.size(), port);
        if (result.ec != std::errc{} ||
            result.ptr != argument.data() + argument.size()) {
            std::cerr << "invalid port: " << argument << '\n';
            return 2;
        }
        port_seen = true;
    }

    std::ofstream log_file(log_path, std::ios::app);
    if (!log_file) {
        std::cerr << "failed to open log file: " << log_path.string() << '\n';
        return 1;
    }

    nrpc::Logger logger({std::ref(std::clog), std::ref(log_file)},
#ifdef DEBUG
                        nrpc::LogLevel::debug
#else
                        nrpc::LogLevel::info
#endif
    );
    logger.log(
        nrpc::LogLevel::info,
        "writing server logs to " + log_path.string());
    nrpc::ResourceManager resources;
    nrpc::Dispatcher dispatcher;
#if NRPC_HAS_SERIAL_BACKEND
    nrpc::register_serial_backend(dispatcher);
#endif
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
        nrpc::Server server({.port = port}, dispatcher, resources, logger);
        server.start();
        server.run();
    } catch (const std::exception& exception) {
        logger.log(nrpc::LogLevel::error, exception.what());
        return 1;
    }
    return 0;
}
