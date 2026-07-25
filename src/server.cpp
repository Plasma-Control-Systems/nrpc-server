#include "nrpc/server.hpp"

#include <uv.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if NRPC_HAS_TLS
#include "tls_transport.hpp"
#endif

namespace nrpc {
namespace {

std::uint16_t network_to_host(std::uint16_t value) {
    if constexpr (std::endian::native == std::endian::little) {
        return std::byteswap(value);
    }
    return value;
}

template <typename... Values>
std::string concatenate(Values&&... values) {
    std::ostringstream output;
    (output << ... << std::forward<Values>(values));
    return output.str();
}

}  // namespace

class Server::Impl {
public:
    Impl(ServerConfig config, Dispatcher& dispatcher, ResourceManager& resources,
         Logger& logger)
        : config_(std::move(config)),
          dispatcher_(dispatcher),
          resources_(resources),
          logger_(logger) {
        if (config_.max_pending_requests == 0 ||
            config_.max_pending_request_bytes == 0) {
            throw std::invalid_argument(
                "pending request limits must be greater than zero");
        }
#if NRPC_HAS_TLS
        const bool has_certificate = !config_.tls_certificate_path.empty();
        const bool has_key = !config_.tls_private_key_path.empty();
        if (has_certificate != has_key) {
            throw std::invalid_argument(
                "TLS requires both a certificate and a private key");
        }
        if (has_certificate) {
            tls_context_ = std::make_unique<TlsContext>(
                config_.tls_certificate_path, config_.tls_private_key_path);
        }
#else
        if (!config_.tls_certificate_path.empty() ||
            !config_.tls_private_key_path.empty()) {
            throw std::runtime_error("this build has no mbedTLS support");
        }
#endif
        check_uv(uv_loop_init(&loop_), "uv_loop_init");
        server_.data = this;
        const int tcp_status = uv_tcp_init(&loop_, &server_);
        if (tcp_status < 0) {
            uv_loop_close(&loop_);
            check_uv(tcp_status, "uv_tcp_init");
        }
    }

    ~Impl() {
        if (!stopped_) {
            stop();
        }
        uv_run(&loop_, UV_RUN_DEFAULT);
        uv_loop_close(&loop_);
    }

    void start() {
        sockaddr_in address{};
        check_uv(uv_ip4_addr(config_.host.c_str(), config_.port, &address), "uv_ip4_addr");
        check_uv(uv_tcp_bind(&server_, reinterpret_cast<const sockaddr*>(&address), 0),
                 "uv_tcp_bind");
        check_uv(uv_listen(reinterpret_cast<uv_stream_t*>(&server_), config_.backlog,
                           &Impl::on_connection),
                 "uv_listen");
        sockaddr_storage bound{};
        int length = sizeof(bound);
        check_uv(uv_tcp_getsockname(&server_, reinterpret_cast<sockaddr*>(&bound), &length),
                 "uv_tcp_getsockname");
        bound_port_ =
            network_to_host(reinterpret_cast<const sockaddr_in*>(&bound)->sin_port);
        started_ = true;
        logger_.log(
            LogLevel::info,
            concatenate("nRPC listening on ", config_.host, ':', bound_port_,
#if NRPC_HAS_TLS
                        tls_context_ ? " (TLS)" : ""
#else
                        ""
#endif
                        ));
    }

    void run() { uv_run(&loop_, UV_RUN_DEFAULT); }

    void stop() {
        if (stopped_) {
            return;
        }
        stopped_ = true;
        uv_walk(
            &loop_,
            [](uv_handle_t* handle, void* argument) {
                auto* self = static_cast<Impl*>(argument);
                if (!uv_is_closing(handle)) {
                    if (handle == reinterpret_cast<uv_handle_t*>(&self->server_)) {
                        uv_close(handle, nullptr);
                    } else if (handle->type == UV_TCP) {
                        close_connection(*static_cast<Connection*>(handle->data));
                    } else if (handle->type == UV_TIMER) {
                        uv_close(handle, &Impl::on_timer_closed);
                    } else {
                        uv_close(handle, nullptr);
                    }
                }
            },
            this);
    }

    std::uint16_t port() const noexcept { return bound_port_; }

private:
    struct Connection;

    struct Work {
        uv_work_t request{};
        Connection* connection;
        protocol::Request decoded;
        std::uint32_t timeout_ms;
        std::size_t payload_bytes;
        Arena arena;
        std::vector<std::byte> response;
        std::atomic_bool canceled{false};
        bool execution_failed{false};

        Work(Connection* connection_value, protocol::Request request_value,
             std::uint32_t timeout_ms_value, std::size_t payload_bytes_value,
             std::size_t arena_size)
            : connection(connection_value),
              decoded(std::move(request_value)),
              timeout_ms(timeout_ms_value),
              payload_bytes(payload_bytes_value),
              arena(arena_size) {
            request.data = this;
        }
    };

    struct Write {
        uv_write_t request{};
        Connection* connection;
        std::vector<std::byte> bytes;
        bool close_after;
        bool restart_request;
    };

    struct CleanupWork {
        uv_work_t request{};
        ResourceManager* resources;
        ClientId client_id;
    };

    struct Connection {
        uv_tcp_t tcp{};
        uv_timer_t timer{};
        Impl* server;
        ClientId id;
        std::atomic_uint references{1};
        std::vector<std::byte> input;
        std::size_t target_size{protocol::header_size};
        bool timer_initialized{false};
        bool closing{false};
        bool response_sent{false};
        Work* active_work{nullptr};
#if NRPC_HAS_TLS
        std::unique_ptr<TlsSession> tls;
#endif

        Connection(Impl* owner, ClientId client_id) : server(owner), id(client_id) {}

        void retain() { references.fetch_add(1, std::memory_order_relaxed); }
        void release() {
            if (references.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                delete this;
            }
        }
    };

    static void check_uv(int status, const char* operation) {
        if (status < 0) {
            throw std::runtime_error(concatenate(
                operation, " failed: ", uv_strerror(status)));
        }
    }

    static void on_connection(uv_stream_t* stream, int status) {
        auto& self = *static_cast<Impl*>(stream->data);
        if (status < 0) {
            self.logger_.log(LogLevel::error,
                             concatenate("accept notification failed: ",
                                         uv_strerror(status)));
            return;
        }
        auto* connection = new Connection(&self, self.next_client_id_++);
        connection->tcp.data = connection;
        if (uv_tcp_init(&self.loop_, &connection->tcp) < 0) {
            delete connection;
            return;
        }
        if (uv_accept(stream, reinterpret_cast<uv_stream_t*>(&connection->tcp)) < 0) {
            uv_close(reinterpret_cast<uv_handle_t*>(&connection->tcp),
                     &Impl::on_connection_closed);
            return;
        }
#if NRPC_HAS_TLS
        if (self.tls_context_) {
            try {
                connection->tls = self.tls_context_->create_session();
            } catch (const std::exception& exception) {
                self.logger_.log(LogLevel::warn,
                                 concatenate("TLS session setup failed: ", exception.what()));
                close_connection(*connection);
                return;
            }
        }
#endif
        self.logger_.log(LogLevel::info,
                         concatenate("client ", connection->id, " connected"));
        start_read(*connection);
    }

    static void start_read(Connection& connection) {
        connection.input.clear();
        connection.target_size = protocol::header_size;
        connection.response_sent = false;
        const int status = uv_read_start(reinterpret_cast<uv_stream_t*>(&connection.tcp),
                                         &Impl::allocate_read, &Impl::on_read);
        if (status < 0) {
            close_connection(connection);
        }
    }

    static void allocate_read(uv_handle_t*, std::size_t suggested, uv_buf_t* buffer) {
        const auto size = static_cast<unsigned int>(
            std::max<std::size_t>(1U,
                std::min<std::size_t>(suggested, 64U * 1024U)));
        *buffer = uv_buf_init(new (std::nothrow) char[size], size);
        if (buffer->base == nullptr) {
            buffer->len = 0;
        }
    }

    static void on_read(uv_stream_t* stream, ssize_t count, const uv_buf_t* buffer) {
        std::unique_ptr<char[]> storage(buffer->base);
        auto& connection = *static_cast<Connection*>(stream->data);
        if (count < 0) {
            if (count != UV_EOF) {
                connection.server->logger_.log(
                    LogLevel::warn,
                    concatenate("client ", connection.id, " read failed: ",
                                uv_strerror(static_cast<int>(count))));
            }
            close_connection(connection);
            return;
        }
        if (count == 0) {
            return;
        }
        const auto* begin = reinterpret_cast<const std::byte*>(buffer->base);
#if NRPC_HAS_TLS
        if (connection.tls) {
            try {
                connection.tls->receive(
                    std::span(begin, static_cast<std::size_t>(count)));
                connection.tls->handshake();
                flush_tls_output(connection, false, false);
                if (!connection.tls->established()) return;
                auto plaintext = connection.tls->read_plaintext();
                connection.input.insert(connection.input.end(), plaintext.begin(),
                                        plaintext.end());
            } catch (const std::exception& exception) {
                connection.server->logger_.log(
                    LogLevel::warn,
                    concatenate("client ", connection.id, " TLS failed: ",
                                exception.what()));
                close_connection(connection);
                return;
            }
        } else
#endif
        {
            connection.input.insert(connection.input.end(), begin,
                                    begin + static_cast<std::size_t>(count));
        }
        process_input(connection);
    }

    static void process_input(Connection& connection) {
        if (connection.input.size() >= protocol::header_size &&
            connection.target_size == protocol::header_size) {
            std::array<std::byte, protocol::header_size> header{};
            std::ranges::copy_n(connection.input.begin(), protocol::header_size, header.begin());
            const auto payload = protocol::read_payload_size(header);
            if (payload > connection.server->config_.max_payload_size) {
                send_error(connection, 0, error_code::protocol_error, true,
                           "request payload exceeds configured limit");
                return;
            }
            connection.target_size = protocol::header_size + payload;
        }
        if (connection.input.size() < connection.target_size) {
            return;
        }
        uv_read_stop(reinterpret_cast<uv_stream_t*>(&connection.tcp));
        if (connection.input.size() != connection.target_size) {
            send_error(connection, function_index(connection),
                       error_code::protocol_error, true,
                       "request payload size mismatch");
            return;
        }

        auto decoded = protocol::decode_request(
            connection.input, connection.server->config_.max_payload_size);
        if (!decoded) {
            connection.server->logger_.log(
                LogLevel::warn,
                concatenate("client ", connection.id, " invalid request: ",
                            protocol::to_string(decoded.error())));
            send_error(connection, function_index(connection),
                       error_code::protocol_error, true,
                       protocol::to_string(decoded.error()));
            return;
        }
        if (connection.server->logger_.packet_dump_enabled()) {
            connection.server->logger_.packet(
                LogLevel::debug, "RX Request", connection.input,
                concatenate("function=",
                            static_cast<unsigned int>(decoded->function_index),
                            " payload=",
                            connection.input.size() - protocol::header_size));
        }
        queue_work(connection, std::move(*decoded));
        if (connection.input.capacity() > 64U * 1024U) {
            std::vector<std::byte>().swap(connection.input);
        } else {
            connection.input.clear();
        }
    }

    static std::uint8_t function_index(const Connection& connection) {
        if (connection.active_work != nullptr) {
            return connection.active_work->decoded.function_index;
        }
        return connection.input.size() > 2
                   ? std::to_integer<std::uint8_t>(connection.input[2])
                   : 0;
    }

    static void queue_work(Connection& connection, protocol::Request request) {
        const auto request_function = request.function_index;
        const std::uint32_t timeout_ms =
            request.timeout_ms != 0 ? request.timeout_ms
                                    : connection.server->config_.default_timeout_ms;
        std::size_t payload_bytes = request.variables.size() *
                                    protocol::variable_entry_size;
        for (const auto& variable : request.variables) {
            payload_bytes += variable.data.size();
        }
        auto& server = *connection.server;
        if (server.pending_work_count_ >= server.config_.max_pending_requests ||
            payload_bytes > server.config_.max_pending_request_bytes ||
            server.pending_work_bytes_ >
                server.config_.max_pending_request_bytes - payload_bytes) {
            send_error(connection, request_function, error_code::internal_error, true,
                       "server request queue capacity exceeded");
            return;
        }
        auto* work = new Work(&connection, std::move(request), timeout_ms,
                              payload_bytes,
                              connection.server->config_.arena_size);
        connection.active_work = work;
        connection.retain();
        if (work->timeout_ms != 0) {
            if (!connection.timer_initialized) {
                if (uv_timer_init(&connection.server->loop_, &connection.timer) < 0) {
                    delete work;
                    connection.active_work = nullptr;
                    connection.release();
                    send_error(connection, request_function,
                               error_code::internal_error, true,
                               "failed to initialize request timeout timer");
                    return;
                }
                connection.timer_initialized = true;
                connection.timer.data = &connection;
                connection.retain();
            }
            uv_timer_start(&connection.timer, &Impl::on_timeout,
                           work->timeout_ms, 0);
        }
        const int status =
            uv_queue_work(&connection.server->loop_, &work->request, &Impl::execute_work,
                          &Impl::after_work);
        if (status < 0) {
            if (connection.timer_initialized) {
                uv_timer_stop(&connection.timer);
            }
            delete work;
            connection.active_work = nullptr;
            connection.release();
            send_error(connection, request_function,
                       error_code::internal_error, true,
                       concatenate("failed to queue request work: ",
                                    uv_strerror(status)));
            return;
        }
        ++server.pending_work_count_;
        server.pending_work_bytes_ += payload_bytes;
    }

    static void execute_work(uv_work_t* request) {
        auto& work = *static_cast<Work*>(request->data);
        if (work.canceled.load(std::memory_order_acquire)) {
            return;
        }
        try {
            auto& connection = *work.connection;
            const RequestContext context{
                connection.id,
                work.timeout_ms,
                connection.server->resources_,
                &work.canceled,
            };
            const auto started = std::chrono::steady_clock::now();
            auto result = connection.server->dispatcher_.dispatch(
                work.decoded.function_index, context, work.decoded.variables,
                work.arena);
            const auto duration =
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - started);
            if (connection.server->logger_.rpc_timing_enabled()) {
                connection.server->logger_.rpc_timing(
                    result ? concatenate(
                                 "client=", connection.id, " function=",
                                 static_cast<unsigned int>(
                                     work.decoded.function_index),
                                 " status=success")
                           : concatenate(
                                 "client=", connection.id, " function=",
                                 static_cast<unsigned int>(
                                     work.decoded.function_index),
                                 " status=error error=", result.error().code),
                    duration);
            }
            if (result) {
                work.response = protocol::encode_response(protocol::Response{
                    .status = protocol::StatusCode::success,
                    .function_index = work.decoded.function_index,
                    .error_code = 0,
                    .variables = std::move(*result),
                });
            } else {
                work.response = protocol::encode_response(protocol::Response{
                    .status = protocol::StatusCode::error,
                    .function_index = work.decoded.function_index,
                    .error_code = result.error().code,
                    .error_message = result.error().message,
                    .variables = {},
                });
            }
        } catch (...) {
            work.execution_failed = true;
            work.response.clear();
        }
    }

    static void after_work(uv_work_t* request, int status) {
        std::unique_ptr<Work> work(static_cast<Work*>(request->data));
        auto& connection = *work->connection;
        connection.active_work = nullptr;
        --connection.server->pending_work_count_;
        connection.server->pending_work_bytes_ -= work->payload_bytes;
        if (connection.timer_initialized) {
            uv_timer_stop(&connection.timer);
        }
        if (!connection.closing && !connection.response_sent) {
            if (status == UV_ECANCELED) {
                send_error(connection, work->decoded.function_index,
                           error_code::internal_error, true,
                           "request work was canceled");
            } else if (work->execution_failed) {
                send_error(connection, work->decoded.function_index,
                           error_code::internal_error, true,
                           "request processing failed");
            } else {
                send_response(connection, std::move(work->response), false);
            }
        }
        if (connection.closing) {
            queue_client_cleanup(*connection.server, connection.id);
        }
        connection.release();
    }

    static void on_timeout(uv_timer_t* timer) {
        auto& connection = *static_cast<Connection*>(timer->data);
        if (!connection.response_sent && !connection.closing) {
            if (connection.active_work != nullptr) {
                connection.active_work->canceled.store(true, std::memory_order_release);
                uv_cancel(reinterpret_cast<uv_req_t*>(
                    &connection.active_work->request));
            }
            send_error(connection, function_index(connection),
                       error_code::timeout, true,
                       "request timed out on the server");
        }
    }

    static void send_error(Connection& connection, std::uint8_t index,
                           std::uint32_t code, bool close_after,
                           std::string message = {}) {
        send_response(connection,
                      protocol::encode_response(protocol::Response{
                          .status = protocol::StatusCode::error,
                          .function_index = index,
                          .error_code = code,
                          .error_message = std::move(message),
                          .variables = {},
                      }),
                      close_after);
    }

    static void send_response(Connection& connection, std::vector<std::byte> response,
                              bool close_after) {
        if (connection.response_sent || connection.closing) {
            return;
        }
        connection.response_sent = true;
        const auto response_status = std::to_integer<unsigned int>(response[0]);
        if (connection.server->logger_.packet_dump_enabled()) {
            connection.server->logger_.packet(
                LogLevel::debug, "TX Response", response,
                concatenate("function=",
                            std::to_integer<unsigned int>(response[2]),
                            " payload=", response.size() - protocol::header_size,
                            " status=", response_status));
        }
#if NRPC_HAS_TLS
        if (connection.tls) {
            try {
                connection.tls->write_plaintext(response);
                flush_tls_output(connection, close_after, true);
            } catch (const std::exception& exception) {
                connection.server->logger_.log(
                    LogLevel::warn,
                    concatenate("client ", connection.id, " TLS write failed: ",
                                exception.what()));
                close_connection(connection);
            }
            return;
        }
#endif
        queue_write(connection, std::move(response), close_after, true);
    }

    static void queue_write(Connection& connection, std::vector<std::byte> bytes,
                            bool close_after, bool restart_request) {
        if (bytes.empty()) return;
        auto* write = new Write{
            .request = {},
            .connection = &connection,
            .bytes = std::move(bytes),
            .close_after = close_after,
            .restart_request = restart_request,
        };
        write->request.data = write;
        connection.retain();
        auto buffer = uv_buf_init(
            reinterpret_cast<char*>(write->bytes.data()),
            static_cast<unsigned int>(write->bytes.size()));
        const int write_status = uv_write(&write->request,
                                          reinterpret_cast<uv_stream_t*>(&connection.tcp),
                                          &buffer, 1, &Impl::after_write);
        if (write_status < 0) {
            delete write;
            connection.release();
            close_connection(connection);
        }
    }

    static void after_write(uv_write_t* request, int status) {
        std::unique_ptr<Write> write(static_cast<Write*>(request->data));
        auto& connection = *write->connection;
        if (status < 0 || write->close_after) {
            close_connection(connection);
        } else if (write->restart_request && !connection.closing) {
            start_read(connection);
        }
        connection.release();
    }

#if NRPC_HAS_TLS
    static void flush_tls_output(Connection& connection, bool close_after,
                                 bool restart_request) {
        auto encrypted = connection.tls->take_encrypted_output();
        queue_write(connection, std::move(encrypted), close_after, restart_request);
    }
#endif

    static void close_connection(Connection& connection) {
        if (connection.closing) {
            return;
        }
        connection.closing = true;
        if (connection.active_work != nullptr) {
            connection.active_work->canceled.store(true, std::memory_order_release);
            uv_cancel(reinterpret_cast<uv_req_t*>(&connection.active_work->request));
        }
        uv_read_stop(reinterpret_cast<uv_stream_t*>(&connection.tcp));
        if (connection.timer_initialized && !uv_is_closing(
                reinterpret_cast<uv_handle_t*>(&connection.timer))) {
            uv_timer_stop(&connection.timer);
            uv_close(reinterpret_cast<uv_handle_t*>(&connection.timer),
                     &Impl::on_timer_closed);
        }
        queue_client_cleanup(*connection.server, connection.id);
        connection.server->logger_.log(
            LogLevel::info,
            concatenate("client ", connection.id, " disconnected"));
        if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(&connection.tcp))) {
            uv_close(reinterpret_cast<uv_handle_t*>(&connection.tcp),
                     &Impl::on_connection_closed);
        }
    }

    static void on_connection_closed(uv_handle_t* handle) {
        auto* connection = static_cast<Connection*>(handle->data);
        if (connection != nullptr) {
            connection->release();
        }
    }

    static void on_timer_closed(uv_handle_t* handle) {
        auto* connection = static_cast<Connection*>(handle->data);
        if (connection != nullptr) {
            connection->release();
        }
    }

    static void queue_client_cleanup(Impl& server, ClientId client_id) {
        auto* work = new (std::nothrow) CleanupWork{
            .request = {},
            .resources = &server.resources_,
            .client_id = client_id,
        };
        if (work == nullptr) {
            server.resources_.cleanup_client(client_id);
            return;
        }
        work->request.data = work;
        const int status = uv_queue_work(
            &server.loop_, &work->request,
            [](uv_work_t* request) {
                auto* cleanup = static_cast<CleanupWork*>(request->data);
                cleanup->resources->cleanup_client(cleanup->client_id);
            },
            [](uv_work_t* request, int) {
                delete static_cast<CleanupWork*>(request->data);
            });
        if (status < 0) {
            delete work;
            server.resources_.cleanup_client(client_id);
        }
    }

    ServerConfig config_;
    Dispatcher& dispatcher_;
    ResourceManager& resources_;
    Logger& logger_;
#if NRPC_HAS_TLS
    std::unique_ptr<TlsContext> tls_context_;
#endif
    uv_loop_t loop_{};
    uv_tcp_t server_{};
    ClientId next_client_id_{1};
    std::uint16_t bound_port_{0};
    bool started_{false};
    bool stopped_{false};
    std::size_t pending_work_count_{0};
    std::size_t pending_work_bytes_{0};
};

Server::Server(ServerConfig config, Dispatcher& dispatcher, ResourceManager& resources,
               Logger& logger)
    : impl_(std::make_unique<Impl>(
          std::move(config), dispatcher, resources, logger)) {}

Server::~Server() = default;
void Server::start() { impl_->start(); }
void Server::run() { impl_->run(); }
void Server::stop() { impl_->stop(); }
std::uint16_t Server::port() const noexcept { return impl_->port(); }

}  // namespace nrpc
