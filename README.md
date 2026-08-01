# nRPC 0.1.0

C++23 nRPC server built on libuv. The wire format and behavior follow
[SPEC.md](SPEC.md).

nRPC exposes blocking equipment APIs through a small typed RPC protocol. A
client connects once, invokes numeric functions, and receives typed values or
a structured error. The server keeps socket, serial, NI-DAQmx, and LCard work
off the event-loop thread and automatically releases client-owned resources
when a connection closes.

## Features

- Linux and Windows builds from one CMake project
- persistent plaintext TCP or TLS connections
- strict, bounded wire decoding with a configurable 16 MiB default payload cap
- libuv worker-pool isolation for blocking instrument and driver operations
- connection-scoped handles with automatic disconnect cleanup
- TCP and native serial backends on supported desktop platforms
- runtime-loaded NI-DAQmx and LCard drivers without vendor SDK build dependencies
- INI configuration, command-line overrides, dual stderr/file logging, and
  optional packet hex dumps
- C++ unit tests plus end-to-end tests against the sibling Python client

## Documentation map

- This README covers installation, operation, backend contracts, errors, and
  extending the server.
- [SPEC.md](SPEC.md) is the authoritative nRPC 0.1.0 wire specification.
- [lcomp_pipeline.md](lcomp_pipeline.md) documents the board-specific LCard
  ADC/DAC pipeline.
- [AGENTS.md](AGENTS.md) contains repository conventions for coding agents and
  contributors.
- The sibling `../nrpc-client` repository documents the supported Python API.

## Quick start

After installing the platform dependencies described below, build a local
server and start it on port 9000.

Linux:

```sh
cmake -S . -B build -DPLATFORM=linux -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build --parallel
./build/nrpc_server 9000
```

Windows, from a Visual Studio Developer PowerShell:

```powershell
cmake -S . -B build -A x64 -DPLATFORM=windows -DBUILD_TESTING=OFF
cmake --build build --config Release --parallel
.\build\Release\nrpc_server.exe 9000
```

In another shell, install the sibling client and call the built-in echo
function:

```sh
python -m pip install --editable ../nrpc-client
python ../nrpc-client/examples/basic_rpc.py --host 127.0.0.1 --port 9000
```

Function `0` echoes its arguments. It is useful for verifying framing, typed
values, TLS, and deployment before connecting real equipment.

## Build

The project requires CMake 3.20 or newer, a C++23 compiler with
`std::expected`, and libuv. mbedTLS is optional and enables TLS connections.
GoogleTest is needed only when `BUILD_TESTING=ON`.

The build has two configurations:

- `Debug`: debug symbols enabled, no optimization
- `Release`: `O2` optimization enabled, no debug information

Select the target platform with `-DPLATFORM=windows` or `-DPLATFORM=linux`.
When omitted, CMake chooses the current host platform.

### Windows

Install Visual Studio 2022 Build Tools (or Visual Studio 2022) with the
**Desktop development with C++** workload. Build all libraries for the same
architecture as the server; the commands below use 64-bit libraries.

Windows dependencies are discovered under `thirdparty` by default. At minimum,
copy the libuv headers and library into this layout:

```text
thirdparty/
  include/
    uv.h
    uv/...
  libs/
    libuv.lib           # uv_a.lib, libuv_a.lib, or uv.lib also work
```

To include TLS support, add the mbedTLS headers and its three libraries:

```text
thirdparty/
  include/
    mbedtls/...
    psa/...
  libs/
    mbedtls.lib
    mbedx509.lib
    mbedcrypto.lib
```

Configure, build, and test from a Developer PowerShell prompt:

```powershell
cmake -S . -B build-windows -A x64 -DPLATFORM=windows -DBUILD_TESTING=ON
cmake --build build-windows --config Debug --parallel
ctest --test-dir build-windows -C Debug --output-on-failure
```

The executable is `build-windows\Debug\nrpc_server.exe`. While it is running,
Windows displays the nRPC icon in the taskbar notification area; hovering over
it shows the listening port. The icon is removed when the server exits.
If mbedTLS is not in
`thirdparty`, CMake prints a status message and builds the plaintext transport.
Use `-DNRPC_THIRDPARTY_DIR=C:\path\to\dependencies` to select another dependency
directory.

Create an optimized build without tests as follows:

```powershell
cmake -S . -B build-windows-release -A x64 -DPLATFORM=windows -DBUILD_TESTING=OFF
cmake --build build-windows-release --config Release --parallel
.\build-windows-release\Release\nrpc_server.exe 9000
```

The NI-DAQmx backend API is always registered. On Windows it loads the vendor
DLL at runtime, so vendor headers and import libraries are not required to
compile the server. NI-DAQmx requires `nicaiu.dll` from an installed NI-DAQmx
runtime on the target machine. If the runtime is missing, the client receives
the backend's runtime load error.

The LCard backend is also always registered and dynamically loads
`wlcomp64.dll` (or `wlcomp.dll` for a 32-bit build). WLCOMP then loads the
matching LComp runtime. Vendor headers and import libraries are not required
to compile the server. Install LComp 7 and ensure both DLLs are on the server
process DLL search path; the standard LCard installation directory is also
checked automatically.

### Linux (Ubuntu/Debian)

Install the compiler and development packages:

```sh
sudo apt update
sudo apt install build-essential cmake pkg-config libuv1-dev libmbedtls-dev libgtest-dev
```

Configure, build, and test:

```sh
cmake -S . -B build-linux \
  -DPLATFORM=linux \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build-linux --parallel
ctest --test-dir build-linux --output-on-failure
```

The executable is `build-linux/nrpc_server`. CMake discovers libuv and mbedTLS
through their CMake packages or `pkg-config`. Linux serial support uses the
native POSIX `termios` API and needs no additional serial library. The NI-DAQmx
function range is still registered; calls return runtime load errors because
the vendor runtime is Windows-only.

Create an optimized build without tests as follows:

```sh
cmake -S . -B build-linux-release \
  -DPLATFORM=linux \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF
cmake --build build-linux-release --parallel
./build-linux-release/nrpc_server 9000
```

### Build settings

| Setting | Default | Purpose |
| --- | --- | --- |
| `PLATFORM` | host platform | Must be `windows` or `linux` |
| `CMAKE_BUILD_TYPE` | `Debug` | Single-config generators only; must be `Debug` or `Release` |
| `BUILD_TESTING` | `ON` | Build the C++ test suite |
| `NRPC_THIRDPARTY_DIR` | `thirdparty` | Windows dependency directory containing `include/` and `libs/` |
| `NRPC_PYTHON_CLIENT_SOURCE_DIR` | legacy sibling auto-detection | Optional Python client tree for integration tests; set it explicitly for a sibling named `nrpc-client` |

Visual Studio and other multi-config generators expose only `Debug` and
`Release`. The built-in TCP, serial, and NI-DAQmx backend APIs are always
compiled and registered. TLS is compiled when mbedTLS is found; otherwise the
server builds without TLS support and reports an error if TLS certificates are
configured at runtime.

Packet hex dumps are disabled by default. Enable them at runtime with
`packet_dump = true` in the server configuration.
Per-call RPC execution timings are also disabled by default. Enable them with
`rpc_timing = true`; timings are written as `DEBUG` records in microseconds.

To install only the server executable into a staging directory:

```sh
cmake --install build-linux-release \
  --prefix artifacts/nrpc-runtime \
  --component runtime
```

With a Visual Studio build, add `--config Release` to the install command.

## Run

```sh
./build/nrpc_server 9000
```

The server appends all logs to `nrpc.log` and mirrors them to standard error.
Choose a different file with:

```sh
./build/nrpc_server 9000 --log-file /var/log/nrpc/server.log
```

### Server configuration

At startup, the server automatically loads `nrpc-server.ini` when that file is
located in the same directory as the executable. This works independently of
the current working directory. You can also load an explicit `.ini` file:

```sh
./build/nrpc_server --config nrpc-server.ini
```

Example `nrpc-server.ini`:

```ini
[server]
host = 0.0.0.0
port = 9000
workers = 4
backlog = 128
max_payload_size = 16777216
arena_size = 1048576
max_pending_requests = 256
max_pending_request_bytes = 268435456
default_timeout_ms = 0

[logging]
file = nrpc.log
packet_dump = false
rpc_timing = false

[tls]
enabled = false
certificate = server.crt
private_key = server.key
```

The `[server]` section controls the listener and request execution:

| Key | Default | Purpose |
| --- | --- | --- |
| `host` | `0.0.0.0` | IPv4 address to bind |
| `port` | `9000` | TCP port to bind |
| `workers` | libuv default | Sets `UV_THREADPOOL_SIZE` for blocking backend work |
| `backlog` | `128` | TCP listen backlog |
| `max_payload_size` | `16777216` | Maximum nRPC request payload in bytes |
| `arena_size` | `1048576` | Per-request response arena size in bytes |
| `max_pending_requests` | `256` | Maximum number of queued and running requests |
| `max_pending_request_bytes` | `268435456` | Maximum combined payload size of queued and running requests |
| `default_timeout_ms` | `0` | Timeout used when a request has no timeout; `0` disables the default |

The `[logging]` section controls log output:

| Key | Default | Purpose |
| --- | --- | --- |
| `file` | `nrpc.log` | Log file path |
| `packet_dump` | `false` | Write complete `RX Request` and `TX Response` packet hex dumps |
| `rpc_timing` | `false` | Write each RPC handler execution time as a `DEBUG` record in microseconds |

The `[tls]` section supports:

| Key | Default | Purpose |
| --- | --- | --- |
| `enabled` | `false` | Enable TLS for the listener |
| `certificate` | empty | Server certificate path, PEM or DER |
| `private_key` | empty | Matching private key path, PEM or DER |

TLS requires both `certificate` and `private_key` when `enabled = true`.
Boolean values accept `true/false`, `yes/no`, `on/off`, `enabled/disabled`,
and `1/0`.

Command-line options are still available and are useful for service overrides:

```sh
./build/nrpc_server --config nrpc-server.ini --host 127.0.0.1 --workers 8
```

Supported options are `--config PATH`, `--host HOST`, positional `port`,
`--log-file PATH`, `--workers N`, `--tls-cert PATH`, `--tls-key PATH`, and
`--no-tls`. The automatically discovered file is loaded first, so explicit
configuration files and command-line arguments override its values in command-line
order.

To encrypt nRPC client connections with mbedTLS, build with mbedTLS installed
(or place its headers and libraries under `thirdparty` on Windows), then pass a
server certificate and matching private key:

```bash
./build/nrpc_server 9000 --tls-cert server.crt --tls-key server.key
```

TLS is opt-in; without these two options the server continues to accept plain
TCP connections. A TLS-enabled listener accepts TLS connections only.

Each line is flushed immediately. When `packet_dump = true`, complete
`RX Request` and `TX Response` packet hex dumps are written to the same file.
When `rpc_timing = true`, each completed handler writes a line such as
`RPC Timing client=1 function=16 status=success duration_us=1234`.

The sample executable registers two mock functions:

- `0`: arena-backed echo
- `1`: blocking sleep accepting one network-order `uint32` duration in ms
- `16` through `19`: cross-platform TCP backend
- `32` through `35`: serial backend
- `64` through `74`: Windows NI-DAQmx backend
- `80` through `86`: Windows LCard/LComp backend

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
and POSIX `termios` file descriptors on Linux and macOS. Blocking operations
execute in the libuv worker pool, and handles are
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
The POSIX backend accepts baud rates exposed by the host's `termios` API.
Platform-specific modes that `termios` cannot represent return error `3008`;
this includes DTR/DSR flow control and, on macOS, mark/space parity.

## NI-DAQmx Backend

The Windows-only NI-DAQmx backend loads `nicaiu.dll` at runtime, so building the
server does not require NI headers or import libraries. Install the NI-DAQmx
runtime on the machine that hosts the hardware. Tasks belong to the client that
created them and are stopped and cleared automatically on close or disconnect.

`register_nidaqmx_backend(dispatcher)` installs this function table:

| Index | Operation | Arguments | Result |
| ---: | --- | --- | --- |
| 64 | Create AI voltage | `string channel`, `float64 min/max`, `int32 terminal` | `uint64 handle` |
| 65 | Create AI thermocouple | channel, min/max, type, CJC source/value/channel | `uint64 handle` |
| 66 | Create AO voltage | `string channel`, `float64 min/max` | `uint64 handle` |
| 67 | Create digital input | `string lines`, `bool all-lines grouping` | `uint64 handle` |
| 68 | Create digital output | `string lines`, `bool all-lines grouping` | `uint64 handle` |
| 69 | Start task | `uint64 handle` | no variables |
| 70 | Read analog | `uint64 handle`, `uint32 sample count` | `float64[] values` |
| 71 | Write analog | `uint64 handle`, `float64[] values` | `uint32 samples written` |
| 72 | Read digital | `uint64 handle`, `uint32 sample count` | `uint32[] values` |
| 73 | Write digital | `uint64 handle`, `uint32[] values` | `uint32 samples written` |
| 74 | Close task | `uint64 handle` | no variables |

Terminal configuration values are default `-1`, RSE `10083`, NRSE `10078`,
differential `10106`, and pseudodifferential `12529`. Thermocouple types use
the NI-DAQmx values B `10047`, E `10055`, J `10072`, K `10073`, N `10077`,
R `10082`, S `10085`, and T `10086`. CJC sources are built-in `10200`, constant
`10116`, and channel `10113`. The request timeout is passed to all reads and
writes. This subset creates on-demand, single-channel tasks; hardware-timed
sampling and explicit clock configuration are outside its scope.

## LCard Backend

The LCard backend owns one WLCOMP interface handle per nRPC handle and uses the
vendor's C ABI for lifecycle, asynchronous I/O, streams, and buffer access.
Devices and mapped ADC/DAC streams are stopped and
released on explicit close, client disconnect, or server shutdown. Internal
ADC correction is enabled at open for boards whose LComp implementation
supports it (L-761/L-780/L-783, E14-440, and E20-10M). Values remain raw
hardware codes; conversion to volts is intentionally left to application code
and the selected input range.

`register_lcard_backend(dispatcher)` installs this function table:

| Index | Operation | Arguments | Result |
| ---: | --- | --- | --- |
| 80 | Open | `uint32 slot`, `string firmware_path` | handle, board metadata, capability flags |
| 81 | Read one ADC code | handle and one channel descriptor | `int16 code` |
| 82 | Read an ADC buffer | handle, channel descriptor arrays, `float64 rate_khz`, optional `uint32 sample_count` | interleaved `int16[] codes`, actual rate |
| 83 | Write one DAC code | handle, `uint8 channel`, `uint16 code` | no variables |
| 84 | Write finite DAC frames | handle, two `uint16[]` arrays, frame rate | frame count, actual rate |
| 85 | Close | `uint64 handle` | no variables |
| 86 | Read an exact ADC sample count | handle, channel descriptor arrays, `uint32 samples_per_channel`, `float64 rate_khz` | interleaved `int16[] codes`, actual rate |

Normalized ADC descriptors contain a physical channel, positive full-scale
range, and input mode. Advanced callers can pass a complete vendor logical
channel code. Supported ranges are validated after the board is detected.
E20-10 uses `AdcIMask` for its 3.0, 1.0, and 0.3 V ranges.
E14-140 and E14-440 accept channels 0..15 in differential mode and 0..31 in
common-ground mode. The backend encodes common-ground logical channels with
bit 5 and rejects physical channel numbers outside the selected mode.

The `int16[]` response's protocol element count is the number of returned ADC
samples. Samples are interleaved as complete scans: channel 0, channel 1, ...,
then the next time row. New clients request up to 131,072 samples; when that
capacity is not divisible by the channel count, they request the largest
smaller number containing only complete scans. The legacy request without an
explicit sample count still returns one scan.

Function 86 accepts the requested number of samples for each selected channel.
It computes the total stream size, lets the LComp adapter normalize FIFO,
interrupt-step, page-count, and other board-specific parameters, and performs
one continuous acquisition even when the result spans multiple channel scans.
The requested and returned rates are aggregate ADC point rates in kHz. The
configured response arena limits the largest result that can be returned.

Timed DAC frames contain simultaneous DAC0/DAC1 codes. L-7xx and E14 streams
are interleaved, while L-791 frames are packed into one 32-bit point. Timed
output is unavailable for E-154 and E20-10 and for E14-140 revisions without
streaming DAC support. Since LComp has no universal exact one-shot DAC mode,
the backend stops when the mapped sync counter reaches the submitted sequence;
some hardware can emit a short buffered tail.

L-7xx, E14-440, and E20-10 devices require an explicit server-local firmware
path at open. Devices with resident firmware use an empty path.

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

The synchronous Python library is maintained in the separate `nrpc-client`
repository. Configure its location to enable cross-repository integration
tests. The current CMake fallback also recognizes a legacy sibling directory
named `nrpc-python-client`, but an explicitly configured path is preferred:

```sh
cmake -S . -B build \
  -DNRPC_PYTHON_CLIENT_SOURCE_DIR=/path/to/nrpc-client
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
| 5001 | Invalid NI-DAQmx backend arguments |
| 5002 | NI-DAQmx runtime load failed |
| 5003 | NI-DAQmx task/channel creation failed |
| 5004 | Invalid or foreign NI-DAQmx task handle |
| 5005 | NI-DAQmx operation failed |
| 5006 | Operation does not match task channel type |
| 6001 | Invalid LCard backend arguments |
| 6002 | WLCOMP or LComp runtime load failed |
| 6003 | LCard device open failed |
| 6004 | Invalid or foreign LCard handle |
| 6005 | LCard device or operation unsupported |
| 6006 | LCard operation failed |
| 6007 | LCard operation timed out |

Backend error codes otherwise pass through unchanged. The Python client raises
`RemoteError` for server/backend failures and `ProtocolError` for malformed
responses.

## Architecture

The executable in `src/main.cpp` parses configuration, configures libuv's
worker pool, opens the log, creates the shared `ResourceManager` and
`Dispatcher`, registers the built-in handlers, and owns the `Server` lifetime.

```text
client TCP/TLS connection
    |
    v
libuv loop: accept -> 16-byte header -> exact payload -> validate/decode
    |
    v
uv_queue_work: Dispatcher -> backend handler -> response Arena
    |
    v
libuv loop: encode response -> write -> read next request
```

The libuv loop owns accept, read, framing, timers, and writes. Complete requests
are dispatched through `uv_queue_work`, keeping blocking equipment drivers off
the I/O thread. Completion callbacks serialize arena-backed results and return
them on the loop thread. Connections use reference counting so disconnects and
timeouts cannot invalidate outstanding worker jobs.

Each connection allows one in-flight request. Reads occur in two framing stages:
the fixed 16-byte header followed by exactly `Payload Size` bytes. Parallel
clients can still execute concurrently; applications that need concurrent RPCs
should open multiple client connections.

`Dispatcher` maps an 8-bit function index to a handler. A handler receives a
`RequestContext` containing the connection's client ID, effective timeout, and
resource manager. `ResourceManager` keys each native handle by client and handle
ID so handles cannot be shared across connections. It invokes every handle's
nonthrowing `close()` during explicit close, disconnect cleanup, or shutdown.

Each job owns a fixed-capacity `Arena`. Handler response views must refer to
memory that remains valid until serialization, normally arena allocations.
This makes response memory bounded and avoids allocator churn in the hot path.
If the arena cannot satisfy an allocation, the handler returns error `1003`.

### Repository layout

| Path | Purpose |
| --- | --- |
| `include/nrpc/` | public C++ types, constants, and backend contracts |
| `src/server.cpp` | connection state, libuv reads/writes, timers, worker jobs |
| `src/protocol.cpp` | strict wire decoding and response encoding |
| `src/dispatcher.cpp` | function registration and backend error boundary |
| `src/resource_manager.cpp` | connection-scoped native-resource ownership |
| `src/*_backend.cpp` | RPC argument validation and backend operations |
| `src/serial_port_*.cpp` | Win32/POSIX serial implementations |
| `tests/` | unit, integration, simulated-instrument, and platform tests |
| `resources/` | Windows executable resources and notification icon |

## End-to-End Tests

`tests/mock_scpi_power_supply.py` is a standalone TCP instrument simulator with
stateful `VOLT`, `CURR`, `OUTP`, `MEAS:*`, `SYST:ERR?`, `*IDN?`, and `*RST`
commands. The end-to-end test starts this simulator and the actual compiled
`nrpc_server`, then routes SCPI traffic through TCP backend functions 16-19.

Cross-repository tests require Python 3.10 or newer and the standalone client
repository. Use verbose CTest output and `packet_dump = true` in the server
configuration to see listener logs and full nRPC packet dumps:

```sh
cmake -S . -B build -DPLATFORM=linux -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build -V -R integration.tcp_backend_scpi
```

## Troubleshooting

### The server fails to bind

Check whether another process owns the configured port and whether the bind
address exists on the host. Use `host = 127.0.0.1` for a local-only server or
`0.0.0.0` to listen on every IPv4 interface. Binding succeeds before the
Windows notification icon is created, so the log is the primary error source.

### A TLS client receives a handshake or certificate error

For INI configuration, TLS is enabled only when `enabled = true` and both the
certificate and private-key paths are configured.
A TLS listener accepts TLS connections only; a plaintext client cannot share
that port. Confirm that CMake found mbedTLS, that the certificate and key match,
and that the client verifies the DNS name present in the certificate.

### Requests time out unexpectedly

The request timeout is supplied by the client in milliseconds. A zero value
uses `default_timeout_ms`; if both are zero, the server has no default request
deadline. TCP, serial, NI-DAQmx, and LCard operations also pass the effective
timeout to their native I/O. Increase `workers` if independent blocking calls
are waiting for an undersized libuv worker pool.

### A vendor backend reports a load error

NI-DAQmx needs `nicaiu.dll` from an installed NI-DAQmx runtime. LCard needs the
matching WLCOMP/LComp runtime and architecture. Both are loaded by the running
server, so changing the Python environment does not fix a missing server-side
DLL. Review `nrpc.log` for the native loader detail returned with the backend
error.

### Integration tests are missing

CTest only registers cross-repository tests when it finds Python and the client
source tree. Set `NRPC_PYTHON_CLIENT_SOURCE_DIR` explicitly during CMake
configuration, then reconfigure the build directory. The automatic fallback
currently recognizes only the legacy sibling name `nrpc-python-client`.

### Packet-level debugging

Set `packet_dump = true` under `[logging]` and run the relevant CTest with `-V`.
Packet dumps can include instrument commands, measurements, or other sensitive
payloads; disable them after diagnosing the issue.
