# nRPC Server Agent Guide

## Scope and source of truth

This file applies to the entire `nrpc-server` repository. The project is a C++23, libuv-based nRPC 0.1.0 server for remote laboratory and industrial equipment access.

Use these sources in this order:

1. `SPEC.md` is authoritative for the wire format and protocol behavior.
2. Public contracts live in `include/nrpc/*.hpp`.
3. Implementations live in `src/*.cpp`.
4. `README.md` documents supported builds, runtime configuration, backends, and errors.
5. Tests describe required edge cases. Do not weaken protocol validation to make a test pass.

The sibling `../nrpc-client` repository implements the Python peer. When changing function indices, wire types, argument order, error codes, timeouts, or response shapes, update both repositories, their READMEs/examples, and integration tests together. Do not edit generated build output or vendored dependencies under `build-*`, `temp`, or `thirdparty`.

## Architecture

The executable is assembled in `src/main.cpp`: it parses configuration, creates the logger, `ResourceManager`, and `Dispatcher`, registers every backend, then starts `Server`.

The request path is:

```text
TCP/TLS connection
  -> libuv loop reads 16-byte header and exact payload
  -> protocol::decode_request validates and owns decoded variables
  -> uv_queue_work runs Dispatcher/backend off the I/O loop
  -> handler writes response data into the per-request Arena
  -> loop-thread completion encodes and writes the response
```

Important ownership and concurrency rules:

- The libuv loop owns accepting, framing, timers, and writes. Backends may block because handlers execute in the libuv worker pool.
- Each connection permits one in-flight request. A client must open separate connections for concurrent calls.
- Connections are reference-counted so disconnects and timeouts cannot invalidate pending work.
- `RequestContext` supplies `client_id`, effective `timeout_ms`, and the shared `ResourceManager`.
- Hardware/socket handles are scoped by `(client_id, handle_id)`. `ResourceManager` automatically calls the nonthrowing `close()` of every `NrpcHandle` on explicit removal, disconnect, or shutdown.
- Use `ResourceManager::with_handle` when operating on a handle so its holder remains alive after the manager lock is released. Reject missing, foreign, or wrong-type handles.
- `Arena` is fixed-capacity response storage. Every `VariableView::data` returned by a handler must point into storage that remains valid through response serialization; normally allocate it from the supplied arena. Return error `1003` on exhaustion.
- A timed-out job may still finish in a worker. Do not let late completion write a second response or access destroyed connection state.

### Main components

| Area | Files | Responsibility |
| --- | --- | --- |
| Application | `src/main.cpp`, `application_config.*` | CLI/INI merging, logging, worker-pool setup, backend registration |
| Server | `server.*`, `tls_transport.*` | libuv connection state, staged reads, work dispatch, timeouts, response writes, optional mbedTLS |
| Protocol | `protocol.*`, `SPEC.md` | strict request decoding and response encoding in network byte order |
| Dispatch | `dispatcher.*` | maps 8-bit function indices to handlers; converts handler exceptions to errors |
| Lifetime | `resource_manager.*` | connection-scoped, type-erased resource ownership and cleanup |
| Memory | `arena.*` | aligned, bounded per-request response allocations |
| Backends | `tcp_backend.*`, `serial_backend.*`, `nidaqmx_backend.*`, `lcard_backend.*` | typed RPC adapters around blocking native APIs |
| Native/platform | `serial_port_*`, `lcard_native_win32.cpp`, notification icon | OS/vendor-specific implementation details |

## Protocol and server API

nRPC uses a persistent TCP connection, optionally wrapped in TLS. Request and response headers and variable-table entries are 16 bytes. All multibyte numbers are big-endian. The request magic is `0xF0`, version is `0x01`, function and variable counts are 8-bit, and payloads default to a 16 MiB maximum. Supported values are null, signed/unsigned integers of 8/16/32/64 bits, float32/64, bool, NUL-terminated UTF-8 string, and raw bytes. Numeric and boolean values may be arrays. Reserved bytes must be zero. Preserve the strict checks in `protocol.cpp`; consult `SPEC.md` before changing framing.

Register application functions with `Dispatcher::register_handler(uint8_t, BackendHandler)`. Registration returns false for a duplicate index. A handler has this shape:

```cpp
nrpc::BackendResult handler(
    const nrpc::RequestContext& context,
    std::span<const nrpc::protocol::Variable> arguments,
    nrpc::Arena& arena);
```

`BackendResult` is `std::expected<std::vector<protocol::VariableView>, BackendError>`. Validate argument count, type, element count, and byte size before decoding. Decode/encode numeric data explicitly in network byte order; never cast an unaligned byte span to a native numeric pointer. Return a specific `BackendError{code, message}` for expected failures. The dispatcher translates unexpected C++ exceptions to an internal backend error.

Minimal registration example:

```cpp
dispatcher.register_handler(
    10, [](const nrpc::RequestContext&,
           std::span<const nrpc::protocol::Variable> args,
           nrpc::Arena& arena) -> nrpc::BackendResult {
        if (args.size() != 1 ||
            args[0].type != nrpc::protocol::DataType::uint32 ||
            args[0].element_count != 1 || args[0].data.size() != 4) {
            return std::unexpected(nrpc::BackendError{7001, "expected one uint32"});
        }
        auto output = arena.allocate(4, 4);
        if (!output) {
            return std::unexpected(nrpc::BackendError{
                nrpc::error_code::arena_exhausted, "response arena exhausted"});
        }
        std::ranges::copy(args[0].data, output->begin());
        return std::vector<nrpc::protocol::VariableView>{{
            nrpc::protocol::DataType::uint32, 1, std::as_const(*output)}};
    });
```

Built-in function table:

| Indices | Backend | Operations |
| --- | --- | --- |
| `0`, `1` | sample | arena-backed echo; blocking sleep with one `uint32` milliseconds argument |
| `16..19` | TCP | open, write, read, close |
| `32..35` | serial | open, write, read, close |
| `64..74` | NI-DAQmx | create AI/TC/AO/DI/DO, start, analog/digital read/write, close |
| `80..85` | LCard | open, single/frame ADC, single/finite-frame DAC, close |

The exact argument and result schemas are documented in `README.md` and implemented in the matching backend source. Server-reserved errors are `1001..1004`; TCP uses `2001..2007`, serial `3001..3008`, NI-DAQmx `5001..5006`, and LCard `6001..6007`. Keep symbolic constants in the public headers and readable descriptions in the Python client synchronized.

## Backend constraints

- TCP uses OS sockets and `getaddrinfo`; propagate the request timeout to connect/read/write and support IPv4, IPv6, and hostnames.
- Serial uses native Win32 handles or POSIX `termios`. Keep platform-specific behavior behind `serial_port_*`. Mark/space parity and DTR/DSR are not universally representable; return `3008` when unsupported.
- NI-DAQmx is always registered. Windows loads `nicaiu.dll` dynamically; other platforms/runtime absence must produce a runtime-load error, not a build dependency.
- LCard is always registered and dynamically loads WLCOMP/LComp on Windows. Preserve injectable driver abstractions used by unit tests. Raw ADC/DAC codes are intentional; voltage conversion belongs to applications. See `lcomp_pipeline.md` before changing stream sequencing or board-specific behavior.
- Native resource `close()` implementations must be idempotent and `noexcept`.

## Configuration and running

CLI options are `--config PATH`, `--host HOST`, positional port, `--log-file PATH`, `--workers N`, `--tls-cert PATH`, `--tls-key PATH`, and `--no-tls`. Later CLI arguments override earlier config-file values. INI sections are `[server]` (`host`, `port`, `workers`, `backlog`, `max_payload_size`, `arena_size`, `default_timeout_ms`), `[logging]` (`file`, `packet_dump`, `rpc_timing`), and `[tls]` (`enabled`, `certificate`, `private_key`). TLS requires both certificate and key and is compiled only when mbedTLS is found. Packet dumps and per-call RPC timings are runtime opt-in.

Build and test from the repository root. On Windows use a Visual Studio Developer PowerShell with x64 libuv dependencies under `thirdparty`:

```powershell
cmake -S . -B build-windows -A x64 -DPLATFORM=windows -DBUILD_TESTING=ON
cmake --build build-windows --config Debug --parallel
ctest --test-dir build-windows -C Debug --output-on-failure
```

On Linux:

```sh
cmake -S . -B build-linux -DPLATFORM=linux -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build-linux --parallel
ctest --test-dir build-linux --output-on-failure
```

The test target is `nrpc_tests`; GoogleTest cases are discovered by CTest. If the sibling Python client is not auto-detected, configure `-DNRPC_PYTHON_CLIENT_SOURCE_DIR=/path/to/nrpc-client` to enable `integration.python_client_server`, `integration.tcp_backend_scpi`, and the Linux serial integration test. For focused debugging, use `ctest --test-dir <build> -R <regex> -V`. Do not claim hardware validation when only mock-driver tests ran.

## Change checklist

- Add or update focused GoogleTests for parsing, dispatch, lifetime, error, timeout, and platform behavior.
- Preserve network byte order, strict reserved-byte validation, payload limits, and one-response semantics.
- Keep the libuv loop free of blocking equipment work.
- Ensure all success response spans have valid arena-backed lifetime.
- Ensure every created native handle is connection-scoped and cleaned up on all failure/disconnect paths.
- Update `README.md`, `SPEC.md` when protocol behavior changes, and sibling client API/error mappings/examples when the wire contract changes.
- Run the narrow tests first, then the complete applicable CTest suite and cross-repository integration tests.
