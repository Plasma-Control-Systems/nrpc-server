#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <functional>
#include <iostream>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "nrpc/application_config.hpp"
#include "nrpc/lcard_backend.hpp"
#include "nrpc/nidaqmx_backend.hpp"
#include "nrpc/serial_backend.hpp"
#include "nrpc/server.hpp"
#include "nrpc/tcp_backend.hpp"

int main(int argc, char** argv) {
    std::vector<std::string_view> arguments;
    arguments.reserve(static_cast<std::size_t>(argc - 1));
    for (int index = 1; index < argc; ++index) arguments.emplace_back(argv[index]);

    nrpc::ApplicationConfig config;
    try {
        auto parsed = nrpc::parse_application_config(arguments);
        if (parsed.help_requested) {
            std::cout << nrpc::application_usage();
            return 0;
        }
        config = std::move(parsed.config);
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 2;
    }

    try {
        if (config.worker_count) {
            nrpc::configure_worker_pool(*config.worker_count);
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
    logger.log(nrpc::LogLevel::info,
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
