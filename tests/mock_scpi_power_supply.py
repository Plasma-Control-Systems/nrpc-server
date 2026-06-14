#!/usr/bin/env python3

import argparse
import socketserver
import threading


class PowerSupplyState:
    def __init__(self):
        self.voltage = 0.0
        self.current_limit = 1.0
        self.output = False
        self.errors = []
        self.lock = threading.Lock()

    def execute(self, command):
        normalized = command.strip()
        upper = normalized.upper()
        with self.lock:
            if upper == "*IDN?":
                return "nRPC,MockPSU,PSU0001,1.0"
            if upper == "*RST":
                self.voltage = 0.0
                self.current_limit = 1.0
                self.output = False
                self.errors.clear()
                return None
            if upper in {"VOLT?", "VOLTAGE?"}:
                return f"{self.voltage:.3f}"
            if upper.startswith("VOLT ") or upper.startswith("VOLTAGE "):
                return self._set_float(normalized, "voltage", 0.0, 60.0)
            if upper in {"CURR?", "CURRENT?"}:
                return f"{self.current_limit:.3f}"
            if upper.startswith("CURR ") or upper.startswith("CURRENT "):
                return self._set_float(
                    normalized, "current_limit", 0.0, 10.0
                )
            if upper == "OUTP?":
                return "1" if self.output else "0"
            if upper.startswith("OUTP "):
                value = normalized.split(maxsplit=1)[1].upper()
                if value in {"1", "ON"}:
                    self.output = True
                elif value in {"0", "OFF"}:
                    self.output = False
                else:
                    self.errors.append('-222,"Data out of range"')
                return None
            if upper == "MEAS:VOLT?":
                return f"{self.voltage if self.output else 0.0:.3f}"
            if upper == "MEAS:CURR?":
                measured = min(self.current_limit, self.voltage / 10.0)
                return f"{measured if self.output else 0.0:.3f}"
            if upper == "SYST:ERR?":
                return (
                    self.errors.pop(0)
                    if self.errors
                    else '0,"No error"'
                )
            self.errors.append('-113,"Undefined header"')
            return None

    def _set_float(self, command, attribute, minimum, maximum):
        try:
            value = float(command.split(maxsplit=1)[1])
        except (IndexError, ValueError):
            self.errors.append('-128,"Numeric data error"')
            return None
        if not minimum <= value <= maximum:
            self.errors.append('-222,"Data out of range"')
            return None
        setattr(self, attribute, value)
        return None


class ScpiHandler(socketserver.StreamRequestHandler):
    def handle(self):
        while line := self.rfile.readline():
            try:
                command = line.decode("ascii")
            except UnicodeDecodeError:
                self.server.state.errors.append('-102,"Syntax error"')
                continue
            response = self.server.state.execute(command)
            if response is not None:
                self.wfile.write(response.encode("ascii") + b"\n")
                self.wfile.flush()


class ScpiServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, address):
        self.state = PowerSupplyState()
        super().__init__(address, ScpiHandler)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, required=True)
    arguments = parser.parse_args()
    with ScpiServer((arguments.host, arguments.port)) as server:
        server.serve_forever()


if __name__ == "__main__":
    main()

