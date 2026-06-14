#include "nrpc/server.hpp"

#include <uv.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

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
        check_uv(uv_loop_init(&loop_), "uv_loop_init");
        server_.data = this;
        check_uv(uv_tcp_init(&loop_, &server_), "uv_tcp_init");
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
        logger_.log(LogLevel::info,
                    concatenate("nRPC listening on ", config_.host, ':', bound_port_));
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
        Arena arena;
        std::vector<std::byte> response;

        Work(Connection* connection_value, protocol::Request request_value,
             std::size_t arena_size)
            : connection(connection_value),
              decoded(std::move(request_value)),
              arena(arena_size) {
            request.data = this;
        }
    };

    struct Write {
        uv_write_t request{};
        Connection* connection;
        std::vector<std::byte> bytes;
        bool close_after;
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
        const auto size = static_cast<unsigned int>(std::min<std::size_t>(suggested, 64U * 1024U));
        *buffer = uv_buf_init(new char[size], size);
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
        connection.input.insert(connection.input.end(), begin,
                                begin + static_cast<std::size_t>(count));
        process_input(connection);
    }

    static void process_input(Connection& connection) {
        if (connection.input.size() >= protocol::header_size &&
            connection.target_size == protocol::header_size) {
            std::array<std::byte, protocol::header_size> header{};
            std::ranges::copy_n(connection.input.begin(), protocol::header_size, header.begin());
            const auto payload = protocol::read_payload_size(header);
            if (payload > connection.server->config_.max_payload_size) {
                send_error(connection, 0, error_code::protocol_error, true);
                return;
            }
            connection.target_size = protocol::header_size + payload;
        }
        if (connection.input.size() < connection.target_size) {
            return;
        }
        uv_read_stop(reinterpret_cast<uv_stream_t*>(&connection.tcp));
        if (connection.input.size() != connection.target_size) {
            send_error(connection, function_index(connection), error_code::protocol_error, true);
            return;
        }

        auto decoded = protocol::decode_request(
            connection.input, connection.server->config_.max_payload_size);
        if (!decoded) {
            connection.server->logger_.log(
                LogLevel::warn,
                concatenate("client ", connection.id, " invalid request: ",
                            protocol::to_string(decoded.error())));
            send_error(connection, function_index(connection), error_code::protocol_error, true);
            return;
        }
#ifdef DEBUG
        connection.server->logger_.packet(
            LogLevel::debug, "RX Request", connection.input,
            concatenate("function=", static_cast<unsigned int>(decoded->function_index),
                        " payload=",
                        connection.input.size() - protocol::header_size));
#endif
        queue_work(connection, std::move(*decoded));
    }

    static std::uint8_t function_index(const Connection& connection) {
        return connection.input.size() > 2
                   ? std::to_integer<std::uint8_t>(connection.input[2])
                   : 0;
    }

    static void queue_work(Connection& connection, protocol::Request request) {
        auto* work = new Work(&connection, std::move(request),
                              connection.server->config_.arena_size);
        connection.retain();
        if (work->decoded.timeout_ms != 0) {
            if (!connection.timer_initialized) {
                if (uv_timer_init(&connection.server->loop_, &connection.timer) < 0) {
                    delete work;
                    connection.release();
                    send_error(connection, function_index(connection),
                               error_code::internal_error, true);
                    return;
                }
                connection.timer_initialized = true;
                connection.timer.data = &connection;
                connection.retain();
            }
            uv_timer_start(&connection.timer, &Impl::on_timeout,
                           work->decoded.timeout_ms, 0);
        }
        const int status =
            uv_queue_work(&connection.server->loop_, &work->request, &Impl::execute_work,
                          &Impl::after_work);
        if (status < 0) {
            if (connection.timer_initialized) {
                uv_timer_stop(&connection.timer);
            }
            delete work;
            connection.release();
            send_error(connection, function_index(connection), error_code::internal_error, true);
        }
    }

    static void execute_work(uv_work_t* request) {
        auto& work = *static_cast<Work*>(request->data);
        auto& connection = *work.connection;
        const RequestContext context{
            connection.id,
            work.decoded.timeout_ms,
            connection.server->resources_,
        };
        auto result = connection.server->dispatcher_.dispatch(
            work.decoded.function_index, context, work.decoded.variables, work.arena);
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
                .variables = {},
            });
        }
    }

    static void after_work(uv_work_t* request, int status) {
        std::unique_ptr<Work> work(static_cast<Work*>(request->data));
        auto& connection = *work->connection;
        if (connection.timer_initialized) {
            uv_timer_stop(&connection.timer);
        }
        if (!connection.closing && !connection.response_sent) {
            if (status == UV_ECANCELED) {
                send_error(connection, work->decoded.function_index,
                           error_code::internal_error, true);
            } else {
                send_response(connection, std::move(work->response), false);
            }
        }
        connection.release();
    }

    static void on_timeout(uv_timer_t* timer) {
        auto& connection = *static_cast<Connection*>(timer->data);
        if (!connection.response_sent && !connection.closing) {
            send_error(connection, function_index(connection), error_code::timeout, true);
        }
    }

    static void send_error(Connection& connection, std::uint8_t index,
                           std::uint32_t code, bool close_after) {
        send_response(connection,
                      protocol::encode_response(protocol::Response{
                          .status = protocol::StatusCode::error,
                          .function_index = index,
                          .error_code = code,
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
#ifdef DEBUG
        const auto response_status = std::to_integer<unsigned int>(response[0]);
        connection.server->logger_.packet(
            LogLevel::debug, "TX Response", response,
            concatenate("function=", std::to_integer<unsigned int>(response[2]),
                        " payload=", response.size() - protocol::header_size,
                        " status=", response_status));
#endif
        auto* write = new Write{
            .request = {},
            .connection = &connection,
            .bytes = std::move(response),
            .close_after = close_after,
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
        } else if (!connection.closing) {
            start_read(connection);
        }
        connection.release();
    }

    static void close_connection(Connection& connection) {
        if (connection.closing) {
            return;
        }
        connection.closing = true;
        uv_read_stop(reinterpret_cast<uv_stream_t*>(&connection.tcp));
        if (connection.timer_initialized && !uv_is_closing(
                reinterpret_cast<uv_handle_t*>(&connection.timer))) {
            uv_timer_stop(&connection.timer);
            uv_close(reinterpret_cast<uv_handle_t*>(&connection.timer),
                     &Impl::on_timer_closed);
        }
        connection.server->resources_.cleanup_client(connection.id);
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

    ServerConfig config_;
    Dispatcher& dispatcher_;
    ResourceManager& resources_;
    Logger& logger_;
    uv_loop_t loop_{};
    uv_tcp_t server_{};
    ClientId next_client_id_{1};
    std::uint16_t bound_port_{0};
    bool started_{false};
    bool stopped_{false};
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
