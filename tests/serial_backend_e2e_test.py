import os
import socket
import subprocess
import sys
import time
import unittest

from nrpc import NrpcClient, SerialParity


def available_port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def wait_for_port(process, port):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("nRPC server exited during startup")
        try:
            with socket.create_connection(("127.0.0.1", port), 0.1):
                return
        except OSError:
            time.sleep(0.02)
    raise TimeoutError("nRPC server did not start")


class SerialBackendEndToEndTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.nrpc_port = available_port()
        cls.server = subprocess.Popen(
            [os.environ["NRPC_SERIAL_TEST_SERVER"], str(cls.nrpc_port)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        wait_for_port(cls.server, cls.nrpc_port)

    @classmethod
    def tearDownClass(cls):
        cls.server.terminate()
        try:
            cls.server.wait(2)
        except subprocess.TimeoutExpired:
            cls.server.kill()
            cls.server.wait()
    def test_serial_round_trip_through_backend(self):
        with NrpcClient("127.0.0.1", self.nrpc_port) as client:
            with client.open_serial_backend(
                "MOCK0",
                baud_rate=115200,
                parity=SerialParity.NONE,
                timeout=1000,
            ) as serial:
                self.assertEqual(
                    self.query(serial, "*IDN?"),
                    "nRPC,FakeLibserialportPSU,SERIAL0001,1.0",
                )
                self.command(serial, "VOLT 7.5")
                self.command(serial, "OUTP ON")
                self.assertEqual(
                    float(self.query(serial, "VOLT?")), 7.5
                )
                self.assertEqual(
                    float(self.query(serial, "MEAS:VOLT?")), 7.5
                )

    def test_serial_scpi_example(self):
        completed = subprocess.run(
            [
                sys.executable,
                os.environ["SERIAL_SCPI_EXAMPLE"],
                "--nrpc-port",
                str(self.nrpc_port),
                "--port",
                "MOCK0",
                "--baud-rate",
                "115200",
            ],
            check=True,
            capture_output=True,
            text=True,
            timeout=5,
        )
        self.assertEqual(
            completed.stdout.strip(),
            "nRPC,FakeLibserialportPSU,SERIAL0001,1.0",
        )

    @staticmethod
    def command(serial, command):
        payload = command.encode("ascii") + b"\n"
        written = serial.write(payload, timeout=1000)
        if written != len(payload):
            raise AssertionError("short serial write")

    @classmethod
    def query(cls, serial, command):
        cls.command(serial, command)
        return serial.read_until(timeout=1000).rstrip(b"\n").decode("ascii")


if __name__ == "__main__":
    unittest.main()
