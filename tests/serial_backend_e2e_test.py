import os
import pty
import select
import socket
import subprocess
import sys
import threading
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


class FakeSerialInstrument:
    def __init__(self, master_fd):
        self.master_fd = master_fd
        self.voltage = 0.0
        self.output = False
        self.stopping = threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)

    def start(self):
        self.thread.start()

    def stop(self):
        self.stopping.set()
        self.thread.join(2)
        os.close(self.master_fd)

    def run(self):
        pending = bytearray()
        while not self.stopping.is_set():
            readable, _, _ = select.select([self.master_fd], [], [], 0.1)
            if not readable:
                continue
            try:
                chunk = os.read(self.master_fd, 4096)
            except OSError:
                continue
            pending.extend(chunk)
            while b"\n" in pending:
                command, _, pending = pending.partition(b"\n")
                response = self.execute(command.decode("ascii"))
                if response is not None:
                    os.write(self.master_fd, response.encode("ascii") + b"\n")

    def execute(self, command):
        if command == "*IDN?":
            return "nRPC,FakeNativeSerialPSU,SERIAL0001,1.0"
        if command.startswith("VOLT "):
            self.voltage = float(command[5:])
        elif command == "VOLT?":
            return str(self.voltage)
        elif command == "OUTP ON":
            self.output = True
        elif command == "OUTP OFF":
            self.output = False
        elif command == "MEAS:VOLT?":
            return str(self.voltage if self.output else 0.0)
        else:
            return "ERROR"
        return None


class SerialBackendEndToEndTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.master_fd, slave_fd = pty.openpty()
        cls.serial_path = os.ttyname(slave_fd)
        os.close(slave_fd)
        cls.instrument = FakeSerialInstrument(cls.master_fd)
        cls.instrument.start()
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
        cls.instrument.stop()

    def test_serial_round_trip_through_backend(self):
        with NrpcClient("127.0.0.1", self.nrpc_port) as client:
            with client.open_serial_backend(
                self.serial_path,
                baud_rate=115200,
                parity=SerialParity.NONE,
                timeout=1000,
            ) as serial:
                self.assertEqual(
                    self.query(serial, "*IDN?"),
                    "nRPC,FakeNativeSerialPSU,SERIAL0001,1.0",
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
                self.serial_path,
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
            "nRPC,FakeNativeSerialPSU,SERIAL0001,1.0",
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
