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


def wait_for_server(process, port: int) -> None:
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


class ClientServerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp_directory = tempfile.TemporaryDirectory()
        cls.log_path = os.path.join(
            cls.temp_directory.name, "nrpc-server.log"
        )
        cls.port = available_port()
        cls.config_path = os.path.join(
            cls.temp_directory.name, "nrpc-server.ini"
        )
        with open(cls.config_path, "w", encoding="utf-8") as config_file:
            config_file.write(
                "[server]\n"
                "host = 127.0.0.1\n"
                f"port = {cls.port}\n"
                "\n"
                "[logging]\n"
                f"file = {cls.log_path}\n"
                "packet_dump = true\n"
            )
        cls.server = subprocess.Popen(
            [
                os.environ["NRPC_SERVER"],
                "--config",
                cls.config_path,
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        wait_for_server(cls.server, cls.port)

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

    def test_ini_config_controls_startup_and_default_timeout(self):
        port = available_port()
        log_path = os.path.join(self.temp_directory.name, "ini-server.log")
        config_path = os.path.join(self.temp_directory.name, "nrpc-server.ini")
        with open(config_path, "w", encoding="utf-8") as config_file:
            config_file.write(
                "[server]\n"
                "host = 127.0.0.1\n"
                f"port = {port}\n"
                "workers = 2\n"
                "default_timeout_ms = 50\n"
                "\n"
                "[logging]\n"
                f"file = {log_path}\n"
                "packet_dump = false\n"
                "\n"
                "[tls]\n"
                "enabled = false\n"
            )

        server = subprocess.Popen(
            [os.environ["NRPC_SERVER"], "--config", config_path],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        try:
            wait_for_server(server, port)
            with NrpcClient("127.0.0.1", port) as client:
                self.assertEqual(client.call(0, ["configured"]), ["configured"])
                with self.assertRaises(RemoteError) as raised:
                    client.call(1, [TypedValue(DataType.UINT32, 300)])
            self.assertEqual(raised.exception.code, 1002)
            with open(log_path, encoding="utf-8") as log_file:
                self.assertIn("UV_THREADPOOL_SIZE=2", log_file.read())
        finally:
            server.terminate()
            try:
                server.wait(2)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()

    def test_serial_backend_reports_missing_port(self):
        if os.environ["NRPC_HAS_SERIAL_BACKEND"] != "ON":
            self.skipTest("server was built without serial support")
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
