#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "nrpc/application_config.hpp"

namespace {

TEST(ApplicationConfigTest, ParsesCommandLineOverrides) {
    constexpr std::array arguments{
        std::string_view{"--host"},      std::string_view{"127.0.0.1"},
        std::string_view{"7000"},        std::string_view{"--workers"},
        std::string_view{"6"},           std::string_view{"--log-file"},
        std::string_view{"server.log"},  std::string_view{"--tls-cert"},
        std::string_view{"server.crt"},  std::string_view{"--tls-key"},
        std::string_view{"server.key"},
    };

    const auto result = nrpc::parse_application_config(arguments);

    EXPECT_FALSE(result.help_requested);
    EXPECT_EQ(result.config.server.host, "127.0.0.1");
    EXPECT_EQ(result.config.server.port, 7000);
    ASSERT_TRUE(result.config.worker_count);
    EXPECT_EQ(*result.config.worker_count, 6U);
    EXPECT_EQ(result.config.log_path, "server.log");
    EXPECT_TRUE(result.config.tls_enabled);
    EXPECT_EQ(result.config.server.tls_certificate_path, "server.crt");
    EXPECT_EQ(result.config.server.tls_private_key_path, "server.key");
}

TEST(ApplicationConfigTest, LoadsIniAndAllowsLaterArgumentsToOverrideIt) {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
                      ("nrpc-config-test-" + std::to_string(suffix) + ".ini");
    {
        std::ofstream output(path);
        ASSERT_TRUE(output);
        output << "[server]\n"
                  "host = 0.0.0.0\n"
                  "port = 9001\n"
                  "workers = 3\n"
                  "backlog = 64\n"
                  "max_pending_requests = 12\n"
                  "max_pending_request_bytes = 8388608\n"
                  "[logging]\n"
                  "file = from-ini.log\n"
                  "packet_dump = yes\n"
                  "rpc_timing = on\n";
    }

    const std::string config_path = path.string();
    const std::array arguments{
        std::string_view{"--config"}, std::string_view{config_path},
        std::string_view{"--host"}, std::string_view{"127.0.0.1"},
        std::string_view{"9100"},
    };
    const auto result = nrpc::parse_application_config(arguments);
    std::filesystem::remove(path);

    EXPECT_EQ(result.config.server.host, "127.0.0.1");
    EXPECT_EQ(result.config.server.port, 9100);
    EXPECT_EQ(result.config.server.backlog, 64);
    EXPECT_EQ(result.config.server.max_pending_requests, 12U);
    EXPECT_EQ(result.config.server.max_pending_request_bytes, 8388608U);
    ASSERT_TRUE(result.config.worker_count);
    EXPECT_EQ(*result.config.worker_count, 3U);
    EXPECT_EQ(result.config.log_path, "from-ini.log");
    EXPECT_TRUE(result.config.packet_dump_enabled);
    EXPECT_TRUE(result.config.rpc_timing_enabled);
}

TEST(ApplicationConfigTest, LoadsAutomaticIniBeforeCommandLineArguments) {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
                      ("nrpc-auto-config-test-" + std::to_string(suffix) + ".ini");
    {
        std::ofstream output(path);
        ASSERT_TRUE(output);
        output << "[server]\n"
                  "host = 0.0.0.0\n"
                  "port = 9001\n"
                  "backlog = 64\n";
    }

    constexpr std::array arguments{
        std::string_view{"--host"}, std::string_view{"127.0.0.1"},
        std::string_view{"9100"},
    };
    const auto result = nrpc::parse_application_config(arguments, path);
    std::filesystem::remove(path);

    EXPECT_EQ(result.config.server.host, "127.0.0.1");
    EXPECT_EQ(result.config.server.port, 9100);
    EXPECT_EQ(result.config.server.backlog, 64);
}

TEST(ApplicationConfigTest, IgnoresMissingAutomaticIni) {
    const auto path = std::filesystem::temp_directory_path() /
                      "nrpc-missing-automatic-config.ini";
    std::filesystem::remove(path);

    constexpr std::array arguments{std::string_view{"--host"},
                                   std::string_view{"127.0.0.1"}};
    const auto result = nrpc::parse_application_config(arguments, path);

    EXPECT_EQ(result.config.server.host, "127.0.0.1");
}

TEST(ApplicationConfigTest, DisabledIniTlsStaysDisabledWhenPathsArePresent) {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
                      ("nrpc-disabled-tls-test-" + std::to_string(suffix) + ".ini");
    {
        std::ofstream output(path);
        ASSERT_TRUE(output);
        output << "[tls]\n"
                  "enabled = false\n"
                  "certificate = server.crt\n"
                  "private_key = server.key\n";
    }

    const std::array<std::string_view, 0> arguments{};
    const auto result = nrpc::parse_application_config(arguments, path);
    std::filesystem::remove(path);

    EXPECT_FALSE(result.config.tls_enabled);
    EXPECT_TRUE(result.config.server.tls_certificate_path.empty());
    EXPECT_TRUE(result.config.server.tls_private_key_path.empty());
}

TEST(ApplicationConfigTest, AutomaticIniPathIsNextToExecutable) {
    const auto path = nrpc::automatic_application_config_path();

    EXPECT_EQ(path.filename(), "nrpc-server.ini");
    EXPECT_TRUE(path.is_absolute());
}

TEST(ApplicationConfigTest, RejectsInvalidWorkerCount) {
    constexpr std::array arguments{std::string_view{"--workers"},
                                   std::string_view{"0"}};
    EXPECT_THROW(nrpc::parse_application_config(arguments), std::runtime_error);
}

TEST(ApplicationConfigTest, RejectsZeroPendingRequestLimits) {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
                      ("nrpc-config-limits-" + std::to_string(suffix) + ".ini");
    {
        std::ofstream output(path);
        ASSERT_TRUE(output);
        output << "[server]\nmax_pending_requests = 0\n";
    }
    const std::string path_text = path.string();
    const std::array config_arguments{
        std::string_view{"--config"}, std::string_view{path_text}};
    EXPECT_THROW(nrpc::parse_application_config(config_arguments),
                 std::runtime_error);
    std::filesystem::remove(path);
}

TEST(ApplicationConfigTest, ReportsHelpWithoutRequiringConfiguration) {
    constexpr std::array arguments{std::string_view{"--help"}};
    const auto result = nrpc::parse_application_config(arguments);
    EXPECT_TRUE(result.help_requested);
}

}  // namespace
