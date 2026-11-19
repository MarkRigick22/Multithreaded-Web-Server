"""Black-box tests of a freshly spawned server. Run from the repository root."""
import concurrent.futures as futures
import contextlib
import http.client
import shutil
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]


@contextlib.contextmanager
def server(workers, capacity, delay=0):
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    with tempfile.TemporaryDirectory() as directory:
        public = Path(directory, "public")
        public.mkdir()
        for name in ("home.html", "output.cgi", "favicon.ico"):
            shutil.copy2(ROOT / "public" / name, public / name)
        (public / "forbidden").mkdir()
        # A successful CGI can write more than a pipe buffer.
        large = public / "large.cgi"
        large.write_text("#!/bin/sh\nexec head -c 131072 /dev/zero\n")
        large.chmod(0o700)
        process = subprocess.Popen([str(ROOT / "server"), *map(str, (port, workers, capacity, delay))],
                                   cwd=directory)
        try:
            deadline = time.monotonic() + 15
            while process.poll() is None:
                try:
                    with socket.create_connection(("127.0.0.1", port), .3):
                        break
                except OSError:
                    if time.monotonic() > deadline:
                        raise TimeoutError("server startup")
                    time.sleep(.025)
            if process.poll() is not None:
                raise RuntimeError(f"server exited with {process.returncode}")
            yield port
            assert process.poll() is None, process.returncode
        finally:
            process.terminate()
            try:
                assert process.wait(15) == 0, process.returncode
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
                raise


def request(port, method="GET", path="/"):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=20)
    try:
        conn.request(method, path)
        response = conn.getresponse()
        body = response.read()
        assert len(body) == int(response.getheader("Content-Length"))
        stats = {k: v.removeprefix(": ") for k, v in response.getheaders() if k.startswith("Stat-")}
        assert len(stats) == 9
        record = "".join(f"{k}:: {v}\r\n" for k, v in stats.items()).encode() + b"\r\n"
        return response.status, stats, body, record
    finally:
        conn.close()


class Integration(unittest.TestCase):
    def test_counters_static_cgi_errors(self):
        with server(1, 2, -1) as port:
            records = b""
            for i, (method, path, code) in enumerate([
                ("GET", "/", 200), ("GET", "/output.cgi?0", 200), ("GET", "/missing", 404),
                ("GET", "/forbidden", 403), ("PUT", "/", 501), ("POST", "/", 200)], 1):
                status, stats, body, record = request(port, method, path)
                self.assertEqual((status, int(stats["Stat-Thread-Count"])), (code, i))
                self.assertEqual(int(stats["Stat-Thread-Id"]), 1)
                self.assertEqual(int(stats["Stat-Thread-Static"]), 1)
                self.assertEqual(int(stats["Stat-Thread-Dynamic"]), int(i >= 2))
                self.assertEqual(int(stats["Stat-Thread-Post"]), int(method == "POST"))
                if code == 200:
                    stamps = [float(stats[k]) for k in ("Stat-Req-Arrival", "Stat-Req-Dispatch",
                                                       "Stat-Log-Arrival", "Stat-Log-Dispatch")]
                    self.assertEqual(stamps, sorted(stamps))
                    if method == "GET":
                        records += record
                        self.assertIn(b"Web Page" if i == 1 else b"CGI program", body)
                    else:
                        self.assertEqual(body, records)

    def test_parallel_snapshots(self):
        for workers, capacity in [(1, 1), (4, 2), (4, 12)]:
            with self.subTest(workers=workers, capacity=capacity), server(workers, capacity) as port:
                methods = ["GET", "GET", "POST"] * 48
                with futures.ThreadPoolExecutor(12) as pool:
                    results = list(pool.map(lambda m: request(port, m), methods))
                final = request(port, "POST")[2]
                records = []
                counts = {}
                for method, (status, stats, body, record) in zip(methods, results):
                    self.assertEqual(status, 200)
                    worker = int(stats["Stat-Thread-Id"])
                    self.assertTrue(1 <= worker <= workers)
                    counts.setdefault(worker, []).append(int(stats["Stat-Thread-Count"]))
                    if method == "GET":
                        records.append(record)
                    else:
                        self.assertTrue(final.startswith(body))
                        self.assertTrue(not body or body.endswith(b"\r\n\r\n"))
                for seq in counts.values():
                    self.assertEqual(sorted(seq), list(range(1, len(seq) + 1)))
                self.assertCountEqual(final.split(b"\r\n\r\n")[:-1], [r[:-4] for r in records])

    def test_fifo_and_capacity(self):
        # Incomplete first request occupies the only worker. Later connections
        # enter the TCP backlog in order. At capacity one, accept occurs only
        # after the previous request completes its log operation.
        for capacity in (1, 4):
            with server(1, capacity) as port, contextlib.ExitStack() as stack:
                sockets = []
                for _ in range(4):
                    sock = stack.enter_context(socket.create_connection(("127.0.0.1", port), 10))
                    sock.sendall(b"GET / HTTP/1.0\r\n")
                    sockets.append(sock)
                prev = 0
                for sock in reversed(sockets):
                    sock.sendall(b"\r\n")
                for index, sock in enumerate(sockets, 1):
                    r = http.client.HTTPResponse(sock)
                    r.begin()
                    r.read()
                    self.assertEqual(int(r.getheader("Stat-Thread-Count")[2:]), index)
                    if capacity == 1:
                        self.assertGreaterEqual(float(r.getheader("Stat-Req-Arrival")[2:]), prev)
                    prev = float(r.getheader("Stat-Log-Dispatch")[2:])
                    r.close()

    def test_debug_sleep(self):
        with server(3, 3, 1) as port:
            for method in ("GET", "POST"):
                stats = request(port, method)[1]
                delta = float(stats["Stat-Log-Dispatch"]) - float(stats["Stat-Log-Arrival"])
                self.assertGreaterEqual(delta, .99)

    def test_disconnect_and_bad_headers(self):
        with server(1, 1) as port:
            with socket.create_connection(("127.0.0.1", port), 3) as sock:
                sock.sendall(b"GET / HTTP/1.0\r\nHost:")
            for raw in (b"nonsense\r\n\r\n", b"GET / HTTP/1.0\r\ninvalid\r\n\r\n"):
                with socket.create_connection(("127.0.0.1", port), 5) as sock:
                    sock.sendall(raw)
                    self.assertIn(b"400 Bad Request", sock.recv(1024))
            self.assertEqual(request(port)[0], 200)

    def test_large_cgi_and_failed_cgi(self):
        with server(2, 2) as port:
            status, _, body, _ = request(port, "GET", "/large.cgi")
            self.assertEqual(status, 200)
            self.assertEqual(body, bytes(131072))
            self.assertEqual(request(port, "GET", "/output.cgi?invalid")[0], 500)
            self.assertEqual(request(port)[0], 200)

    def test_invalid_cli(self):
        for args in [[], ["0", "1", "1", "0"], ["65536", "1", "1", "0"],
                     ["1234", "1a", "1", "0"], ["1234", "1", "-1", "0"],
                     ["1234", "1", "1", "xyz"]]:
            p = subprocess.run([str(ROOT / "server"), *args], capture_output=True, timeout=4)
            self.assertEqual(p.returncode, 1)
            self.assertRegex(p.stderr.decode(), "Usage:|Invalid integer")


if __name__ == "__main__":
    signal.alarm(180)
    unittest.main(verbosity=2)
