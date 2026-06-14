import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest

from nrpc import DataType, NrpcClient, RemoteError, TypedValue


def available_port() -> int:
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


class ClientServerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp_directory = tempfile.TemporaryDirectory()
        cls.log_path = os.path.join(
            cls.temp_directory.name, "nrpc-server.log"
        )
        cls.port = available_port()
        cls.server = subprocess.Popen(
            [
                os.environ["NRPC_SERVER"],
                str(cls.port),
                "--log-file",
                cls.log_path,
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if cls.server.poll() is not None:
                raise RuntimeError("nRPC server exited during startup")
            try:
                with socket.create_connection(("127.0.0.1", cls.port), 0.1):
                    return
            except OSError:
                time.sleep(0.02)
        raise TimeoutError("nRPC server did not start")

    @classmethod
    def tearDownClass(cls):
        cls.server.terminate()
        try:
            cls.server.wait(2)
        except subprocess.TimeoutExpired:
            cls.server.kill()
            cls.server.wait()
        cls.temp_directory.cleanup()

    def test_echo_round_trip(self):
        arguments = [
            TypedValue(DataType.INT16, [-7, 12]),
            True,
            "device",
            b"\x00\xff",
            None,
        ]
        with NrpcClient("127.0.0.1", self.port) as client:
            self.assertEqual(
                client.call(0, arguments),
                [[-7, 12], True, "device", b"\x00\xff", None],
            )

    def test_server_writes_events_and_packets_to_log_file(self):
        with NrpcClient("127.0.0.1", self.port) as client:
            self.assertEqual(client.call(0, ["logged"]), ["logged"])
        deadline = time.monotonic() + 1
        contents = ""
        while time.monotonic() < deadline:
            with open(self.log_path, encoding="utf-8") as log_file:
                contents = log_file.read()
            if "TX Response" in contents:
                break
            time.sleep(0.01)
        self.assertIn("nRPC listening on", contents)
        self.assertIn("client", contents)
        self.assertIn("RX Request function=0", contents)
        self.assertIn("TX Response function=0", contents)
        self.assertIn("bytes=", contents)

    def test_basic_rpc_example(self):
        completed = subprocess.run(
            [
                sys.executable,
                os.environ["BASIC_RPC_EXAMPLE"],
                "--port",
                str(self.port),
            ],
            check=True,
            capture_output=True,
            text=True,
            timeout=5,
        )
        self.assertIn("hello from Python", completed.stdout)

    def test_blocking_backend_does_not_block_other_client(self):
        started = threading.Event()

        def blocking_call():
            with NrpcClient("127.0.0.1", self.port) as client:
                started.set()
                client.call(1, [TypedValue(DataType.UINT32, 350)])

        thread = threading.Thread(target=blocking_call)
        thread.start()
        self.assertTrue(started.wait(1))
        time.sleep(0.05)
        begin = time.monotonic()
        with NrpcClient("127.0.0.1", self.port) as client:
            self.assertEqual(client.call(0, ["ready"]), ["ready"])
        elapsed = time.monotonic() - begin
        thread.join(2)
        self.assertFalse(thread.is_alive())
        self.assertLess(elapsed, 0.2)

    def test_server_timeout_returns_timeout_error(self):
        with NrpcClient("127.0.0.1", self.port) as client:
            with self.assertRaises(RemoteError) as raised:
                client.call(
                    1, [TypedValue(DataType.UINT32, 300)], timeout=50
                )
        self.assertEqual(raised.exception.code, 1002)

    def test_real_libserialport_reports_missing_port(self):
        with NrpcClient("127.0.0.1", self.port) as client:
            with self.assertRaises(RemoteError) as raised:
                client.open_serial_backend(
                    "__nrpc_missing_serial_port__",
                    timeout=1000,
                )
        self.assertEqual(raised.exception.code, 3002)

    def test_concurrent_clients_stress(self):
        failures = []

        def run_client(client_id):
            try:
                with NrpcClient("127.0.0.1", self.port) as client:
                    for sequence in range(50):
                        expected = [client_id, sequence, f"client-{client_id}"]
                        actual = client.call(0, expected)
                        if actual != expected:
                            failures.append((expected, actual))
                            return
            except Exception as error:
                failures.append(error)

        threads = [
            threading.Thread(target=run_client, args=(client_id,))
            for client_id in range(8)
        ]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join(5)
        self.assertTrue(all(not thread.is_alive() for thread in threads))
        self.assertEqual(failures, [])


if __name__ == "__main__":
    unittest.main()
