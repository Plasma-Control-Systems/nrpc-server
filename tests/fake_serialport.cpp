#ifdef _MSC_VER
#define LIBSERIALPORT_MSBUILD
#endif
#include <libserialport.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

struct sp_port {
    std::mutex mutex;
    std::vector<unsigned char> response;
    double voltage{0.0};
    bool output{false};
    bool opened{false};
};

namespace {

sp_return count_result(std::size_t count) {
    return static_cast<sp_return>(static_cast<int>(count));
}

void execute(sp_port& port, std::string command) {
    if (!command.empty() && command.back() == '\n') {
        command.pop_back();
    }
    std::string response;
    if (command == "*IDN?") {
        response = "nRPC,FakeLibserialportPSU,SERIAL0001,1.0\n";
    } else if (command.starts_with("VOLT ")) {
        port.voltage = std::stod(command.substr(5));
    } else if (command == "VOLT?") {
        response = std::to_string(port.voltage) + "\n";
    } else if (command == "OUTP ON") {
        port.output = true;
    } else if (command == "OUTP OFF") {
        port.output = false;
    } else if (command == "MEAS:VOLT?") {
        response =
            std::to_string(port.output ? port.voltage : 0.0) + "\n";
    } else {
        response = "ERROR\n";
    }
    port.response.insert(
        port.response.end(), response.begin(), response.end());
}

}  // namespace

extern "C" {

sp_return sp_get_port_by_name(const char* portname, sp_port** port_ptr) {
    if (portname == nullptr || port_ptr == nullptr ||
        std::strcmp(portname, "MOCK0") != 0) {
        return SP_ERR_ARG;
    }
    *port_ptr = new sp_port;
    return SP_OK;
}

void sp_free_port(sp_port* port) { delete port; }

sp_return sp_open(sp_port* port, sp_mode flags) {
    if (port == nullptr || flags != SP_MODE_READ_WRITE) {
        return SP_ERR_ARG;
    }
    port->opened = true;
    return SP_OK;
}

sp_return sp_close(sp_port* port) {
    if (port == nullptr) {
        return SP_ERR_ARG;
    }
    port->opened = false;
    return SP_OK;
}

sp_return sp_set_baudrate(sp_port* port, int baudrate) {
    return port != nullptr && baudrate > 0 ? SP_OK : SP_ERR_ARG;
}

sp_return sp_set_bits(sp_port* port, int bits) {
    return port != nullptr && bits >= 5 && bits <= 8 ? SP_OK : SP_ERR_ARG;
}

sp_return sp_set_parity(sp_port* port, sp_parity parity) {
    return port != nullptr && parity >= SP_PARITY_NONE &&
                   parity <= SP_PARITY_SPACE
               ? SP_OK
               : SP_ERR_ARG;
}

sp_return sp_set_stopbits(sp_port* port, int stopbits) {
    return port != nullptr && (stopbits == 1 || stopbits == 2)
               ? SP_OK
               : SP_ERR_ARG;
}

sp_return sp_set_flowcontrol(sp_port* port, sp_flowcontrol flowcontrol) {
    return port != nullptr && flowcontrol >= SP_FLOWCONTROL_NONE &&
                   flowcontrol <= SP_FLOWCONTROL_DTRDSR
               ? SP_OK
               : SP_ERR_ARG;
}

sp_return sp_blocking_write(sp_port* port, const void* buffer,
                            std::size_t count, unsigned int) {
    if (port == nullptr || buffer == nullptr || !port->opened) {
        return SP_ERR_ARG;
    }
    std::scoped_lock lock(port->mutex);
    execute(
        *port,
        std::string(static_cast<const char*>(buffer), count));
    return count_result(count);
}

sp_return sp_blocking_read_next(sp_port* port, void* buffer,
                                std::size_t count, unsigned int) {
    if (port == nullptr || buffer == nullptr || count == 0 ||
        !port->opened) {
        return SP_ERR_ARG;
    }
    std::scoped_lock lock(port->mutex);
    const auto available = std::min(count, port->response.size());
    if (available == 0) {
        return count_result(0);
    }
    std::memcpy(buffer, port->response.data(), available);
    port->response.erase(
        port->response.begin(),
        port->response.begin() +
            static_cast<std::ptrdiff_t>(available));
    return count_result(available);
}

char* sp_last_error_message(void) {
    constexpr char message[] = "fake libserialport error";
    auto* result = static_cast<char*>(std::malloc(sizeof(message)));
    if (result != nullptr) {
        std::memcpy(result, message, sizeof(message));
    }
    return result;
}

void sp_free_error_message(char* message) { std::free(message); }

}  // extern "C"

