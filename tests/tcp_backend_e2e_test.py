import os
import socket
import subprocess
import sys
import time
import unittest

from nrpc import NrpcClient


def available_port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def wait_for_port(process, port):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("process exited during startup")
        try:
            with socket.create_connection(("127.0.0.1", port), 0.1):
                return
        except OSError:
            time.sleep(0.02)
    raise TimeoutError(f"port {port} did not become ready")


class TcpBackendEndToEndTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.instrument_port = available_port()
        print(
            f"starting mock SCPI power supply on 127.0.0.1:{cls.instrument_port}",
            flush=True,
        )
        cls.instrument = subprocess.Popen(
            [
                sys.executable,
                os.environ["MOCK_SCPI"],
                "--port",
                str(cls.instrument_port),
            ],
        )
        wait_for_port(cls.instrument, cls.instrument_port)

        cls.nrpc_port = available_port()
        print(
            f"starting nRPC server {os.environ['NRPC_SERVER']} "
            f"on 127.0.0.1:{cls.nrpc_port}",
            flush=True,
        )
        cls.server = subprocess.Popen(
            [os.environ["NRPC_SERVER"], str(cls.nrpc_port)],
        )
        wait_for_port(cls.server, cls.nrpc_port)

    @classmethod
    def tearDownClass(cls):
        for process in (cls.server, cls.instrument):
            process.terminate()
            try:
                process.wait(2)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()

    def test_scpi_power_supply_over_remote_tcp_backend(self):
        with NrpcClient("127.0.0.1", self.nrpc_port) as client:
            with client.open_tcp_backend(
                "127.0.0.1", self.instrument_port, timeout=1000
            ) as tcp:
                self.assertEqual(
                    self.query(tcp, "*IDN?"),
                    "nRPC,MockPSU,PSU0001,1.0",
                )
                self.command(tcp, "*RST")
                self.command(tcp, "VOLT 12.5")
                self.command(tcp, "CURR 2.0")
                self.command(tcp, "OUTP ON")

                self.assertEqual(self.query(tcp, "VOLT?"), "12.500")
                self.assertEqual(self.query(tcp, "CURR?"), "2.000")
                self.assertEqual(self.query(tcp, "OUTP?"), "1")
                self.assertEqual(self.query(tcp, "MEAS:VOLT?"), "12.500")
                self.assertEqual(self.query(tcp, "MEAS:CURR?"), "1.250")

                self.command(tcp, "VOLT 100")
                self.assertEqual(
                    self.query(tcp, "SYST:ERR?"),
                    '-222,"Data out of range"',
                )

    def test_tcp_scpi_example(self):
        completed = subprocess.run(
            [
                sys.executable,
                os.environ["TCP_SCPI_EXAMPLE"],
                "--nrpc-port",
                str(self.nrpc_port),
                "--device-host",
                "127.0.0.1",
                "--device-port",
                str(self.instrument_port),
            ],
            check=True,
            capture_output=True,
            text=True,
            timeout=5,
        )
        self.assertEqual(
            completed.stdout.strip(), "nRPC,MockPSU,PSU0001,1.0"
        )

    @staticmethod
    def command(tcp, command):
        data = command.encode("ascii") + b"\n"
        written = tcp.write(data, timeout=1000)
        if written != len(data):
            raise AssertionError("short TCP write")

    @classmethod
    def query(cls, tcp, command):
        cls.command(tcp, command)
        return tcp.read_until(timeout=1000).rstrip(b"\n").decode("ascii")


if __name__ == "__main__":
    unittest.main()
