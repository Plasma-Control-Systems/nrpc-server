# nRPC 0.1.0

C++23 nRPC server built on libuv. The wire format and behavior follow
[SPEC.md](SPEC.md).

## Build

Dependencies:

- CMake 3.20 or newer
- C++23 compiler with `std::expected` support
- libuv
- libserialport (optional on Unix; required for the Unix serial backend)
- LCard lcomp/wlcomp (optional; Windows only)
- GoogleTest (test builds only)

Supported and continuously tested platforms:

- Linux with GCC
- macOS with AppleClang
- Windows with MSVC and Winsock 2

On Windows, dependencies are read from a local `thirdparty` directory instead
of a package manager. This folder is for application/runtime dependencies, not
test-only libraries:

```text
thirdparty/
  include/
    uv.h
  libs/
    uv_a.lib            # or libuv_a.lib / libuv.lib / uv.lib
    wlcomp64.dll        # optional, for runtime LCard access
    lcomp64.dll         # optional, for runtime LCard access
```

Use `-DNRPC_THIRDPARTY_DIR=...` if the folder is not under the repository root.
Windows serial support uses native Win32 APIs without an external serial
dependency. The LCard backend is built on Windows and loads `wlcomp`/`lcomp`
at runtime from `C:\Program Files (x86)\LCard\LIBRARY\BIN` by default;
override it with `-DNRPC_LCARD_LIBRARY_DIR=...`, disable it with
`-DNRPC_ENABLE_LCARD_BACKEND=OFF`, or set `NRPC_LCARD_LIBRARY_DIR` at
runtime.

On Linux and macOS, the build accepts libuv through a CMake config package or
through `pkg-config`. On Unix, if libserialport is not available, the server
builds without functions 32 through 35; pass
`-DNRPC_ENABLE_SERIAL_BACKEND=OFF` to select that configuration explicitly.
Compiler warning flags are selected separately for MSVC and GCC/Clang.
When tests are enabled, CMake first looks for an installed `GTest::gtest_main`
package and otherwise downloads GoogleTest into the build tree with
FetchContent. Disable that fallback with `-DNRPC_FETCH_GTEST=OFF`.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

A Debug build defines `DEBUG`, enabling full request and response hex dumps.
libuv's worker count can be selected before startup with `UV_THREADPOOL_SIZE`.

### Release Build

For Ninja, Unix Makefiles, and other single-configuration generators:

```sh
cmake -S . -B build-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF
cmake --build build-release --parallel
```

Run the optimized server:

```sh
./build-release/nrpc_server 9000
```

For Visual Studio on Windows, select the configuration when building:

```powershell
cmake -S . -B build-release -A x64 -DBUILD_TESTING=OFF
cmake --build build-release --config Release --parallel
.\build-release\Release\nrpc_server.exe 9000
```

Create a runtime artifact that contains only the server executable:

```powershell
cmake --install build-release --config Release `
  --prefix artifacts\nrpc-runtime `
  --component runtime
```

Build tests as a separate artifact so GoogleTest remains a build/test
dependency and is not part of the shipped application:

```powershell
cmake -S . -B build-tests -A x64 -DBUILD_TESTING=ON
cmake --build build-tests --config Debug --parallel
ctest --test-dir build-tests --build-config Debug --output-on-failure
cmake --install build-tests --config Debug `
  --prefix artifacts\nrpc-tests `
  --component tests
```

To test the Release configuration, omit `-DBUILD_TESTING=OFF` and run:

```sh
ctest --test-dir build-release --build-config Release --output-on-failure
```

Release builds retain `ERROR`, `WARN`, and `INFO` logging but omit Debug packet
hex dumps.

## Run

```sh
./build/nrpc_server 9000
```

The server appends all logs to `nrpc.log` and mirrors them to standard error.
Choose a different file with:

```sh
./build/nrpc_server 9000 --log-file /var/log/nrpc/server.log
```

Each line is flushed immediately. Debug builds write complete `RX Request` and
`TX Response` packet hex dumps to the same file.

The sample executable registers two mock functions:

- `0`: arena-backed echo
- `1`: blocking sleep accepting one network-order `uint32` duration in ms
- `16` through `19`: cross-platform TCP backend
- `32` through `35`: serial backend
- `48` through `52`: Windows LCard lcomp/wlcomp backend

The sleep backend exists to exercise worker isolation and timeout handling.

## TCP Backend

The TCP backend uses operating-system sockets directly:

- POSIX sockets on Linux and other Unix-like systems
- Winsock 2 on Windows, linked through `ws2_32`
- `getaddrinfo` for IPv4/IPv6 and hostname resolution
- blocking send/receive calls isolated in libuv worker threads
- request timeout propagation to connect, send, and receive operations
- automatic socket closure when the owning nRPC client disconnects

`register_tcp_backend(dispatcher)` installs this function table:

| Index | Operation | Arguments | Result |
| ---: | --- | --- | --- |
| 16 | Open | `string host`, `uint16 port` | `uint64 handle` |
| 17 | Write | `uint64 handle`, `bytes data` | `uint32 bytes_written` |
| 18 | Read | `uint64 handle`, `uint32 max_size` | `bytes data` |
| 19 | Close | `uint64 handle` | no variables |

Handles are scoped to the nRPC connection that created them and cannot be used
by another client.

## Serial Backend

The serial backend uses native Win32 serial handles for COM ports on Windows
and [libserialport](https://sigrok.org/wiki/Libserialport) for Unix serial
devices. Blocking operations execute in the libuv worker pool, and handles are
automatically closed when their owning nRPC client disconnects.

`register_serial_backend(dispatcher)` installs this function table:

| Index | Operation | Arguments | Result |
| ---: | --- | --- | --- |
| 32 | Open | `string name`, `uint32 baud`, four `uint8` settings | `uint64 handle` |
| 33 | Write | `uint64 handle`, `bytes data` | `uint32 bytes_written` |
| 34 | Read | `uint64 handle`, `uint32 max_size` | `bytes data` |
| 35 | Close | `uint64 handle` | no variables |

The four open settings are data bits, parity, stop bits, and flow control.
Parity codes are none `0`, odd `1`, even `2`, mark `3`, and space `4`. Flow
control codes are none `0`, software `1`, RTS/CTS `2`, and DTR/DSR `3`.

## LCard Backend

The Windows-only LCard backend dynamically loads the installed wlcomp wrapper,
which in turn loads the matching 32-bit or 64-bit lcomp library. Device handles
are scoped to their owning nRPC connection and close automatically on
disconnect.

`register_lcard_backend(dispatcher)` installs this function table:

| Index | Operation | Arguments | Result |
| ---: | --- | --- | --- |
| 48 | Open | `uint32 slot`, `uint32 board type`, `string BIOS path` | `uint64 handle` |
| 49 | IoAsync | handle, operation, rate, mode, count/mask, channels, data | actual rate, data |
| 50 | Slot info | `uint64 handle` | board type, DSP type, IRQ |
| 51 | Load BIOS | `uint64 handle`, `string path` | no variables |
| 52 | Close | `uint64 handle` | no variables |

The server rejects open when the physical slot's `BoardType` differs from the
requested type. Supported values are PCI A `5`, PCI B `6`, PCI C `14`, L791
`19`, E440 `30`, E140 `31`, E2010 `32`, E154 `38`, E2010B `39`, and E310 `77`.

The BIOS path may be empty during open. IoAsync operation codes follow wlcomp:
ADC configure `3`, TTL configure `4`, DAC configure `5`, ADC input `6`, TTL
input `7`, TTL output `8`, DAC output `9`, and frequency input `10`. Channels
and data are `uint32` arrays with at most 128 elements. The server validates
device-specific layouts before calling wlcomp, including E310 ADC masks and
four-value reads, L791 paired DAC output, E140 dual DAC output, E154 8-bit DAC
values, E2010's lack of asynchronous ADC input, and PCI C-only TTL direction
configuration.

## Server API

Register a backend with `nrpc::Dispatcher::register_handler`. A handler receives
the client context, decoded variables, and a fixed-capacity response arena. It
returns `nrpc::BackendResult`, an `std::expected` containing response variable
views or `BackendError`.

Response data must live in the supplied arena until the handler returns:

```cpp
dispatcher.register_handler(
    10, [](const nrpc::RequestContext& context,
           std::span<const nrpc::protocol::Variable> arguments,
           nrpc::Arena& arena) -> nrpc::BackendResult {
        auto output = arena.allocate(4, 4);
        if (!output) {
            return std::unexpected(nrpc::BackendError{
                nrpc::error_code::arena_exhausted, "arena exhausted"});
        }
        // Fill output using network byte order.
        return std::vector<nrpc::protocol::VariableView>{{
            nrpc::protocol::DataType::uint32, 1, std::as_const(*output)}};
    });
```

`ResourceManager` owns noncopyable `NrpcHandle` objects and closes all resources
associated with a client when its TCP connection ends.

## Python Client

The synchronous Python library is maintained in the separate
`nrpc-python-client` repository. Keep both repositories beside each other to
enable cross-repository integration tests automatically, or configure its
location explicitly:

```sh
cmake -S . -B build \
  -DNRPC_ENABLE_PYTHON_INTEGRATION_TESTS=ON \
  -DNRPC_PYTHON_CLIENT_SOURCE_DIR=/path/to/nrpc-python-client
```

Without that path, the server and all C++ unit tests build independently.

## Errors

Server-reserved error codes:

| Code | Meaning |
| ---: | --- |
| 1001 | Invalid protocol frame |
| 1002 | Server operation timeout |
| 1003 | Response arena exhausted |
| 1004 | Internal server error |
| 2001 | Invalid TCP backend arguments |
| 2002 | TCP hostname resolution failed |
| 2003 | TCP connection failed |
| 2004 | Invalid or foreign TCP handle |
| 2005 | TCP write failed |
| 2006 | TCP read failed or peer disconnected |
| 2007 | TCP socket operation timed out |
| 3001 | Invalid serial backend arguments |
| 3002 | Serial port open failed |
| 3003 | Serial configuration failed |
| 3004 | Invalid or foreign serial handle |
| 3005 | Serial write failed |
| 3006 | Serial read failed |
| 3007 | Serial operation timed out |
| 3008 | Serial operation unsupported |
| 4001 | Invalid LCard backend arguments |
| 4002 | LCard library load failed |
| 4003 | LCard device open failed |
| 4004 | Invalid or foreign LCard handle |
| 4005 | LCard operation failed |
| 4006 | LCard operation unsupported |
| 4007 | LCard operation timed out |

Backend error codes otherwise pass through unchanged. The Python client raises
`RemoteError` for server/backend failures and `ProtocolError` for malformed
responses.

## Architecture

The libuv loop owns accept, read, framing, timers, and writes. Complete requests
are dispatched through `uv_queue_work`, keeping blocking equipment drivers off
the I/O thread. Completion callbacks serialize arena-backed results and return
them on the loop thread. Connections use reference counting so disconnects and
timeouts cannot invalidate outstanding worker jobs.

Each connection allows one in-flight request. Reads occur in two framing stages:
the fixed 16-byte header followed by exactly `Payload Size` bytes.

## End-to-End Tests

`tests/mock_scpi_power_supply.py` is a standalone TCP instrument simulator with
stateful `VOLT`, `CURR`, `OUTP`, `MEAS:*`, `SYST:ERR?`, `*IDN?`, and `*RST`
commands. The end-to-end test starts this simulator and the actual compiled
`nrpc_server`, then routes SCPI traffic through TCP backend functions 16-19.

Cross-repository tests require Python 3.10 or newer and the standalone client
repository. Use a Debug build and verbose CTest output to see listener logs and
full nRPC packet dumps:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build -V -R integration.tcp_backend_scpi
```
