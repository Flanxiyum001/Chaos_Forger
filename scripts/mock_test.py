#!/usr/bin/env python3
"""Mock Docker Engine over a UNIX socket for testing Forger without Docker.

Serves GET /_ping and GET /containers/json; records every POST it receives in
a log file so tests can assert that strikes landed. Run it, point a Forger
config at the socket path, run Forger --once, then stop it.
"""
import json
import os
import socket
import socketserver
import sys
import threading
import time

SOCKET = "/tmp/forger-test.sock"
POST_LOG = "/tmp/forger-post.log"

# Failure injection for testing Forger's error handling: when set, POSTs whose
# path contains this id get a 500 instead of 204 (FORGER_MOCK_DOOMED_ID env).
DOOMED_ID = os.environ.get("FORGER_MOCK_DOOMED_ID", "")

# Latency injection (FORGER_MOCK_DELAY_MS env): every POST sleeps this long
# before answering, so tests can hold a strike in flight across a SIGINT.
DELAY_MS = int(os.environ.get("FORGER_MOCK_DELAY_MS", "0"))
DOOMED_ID = os.environ.get("FORGER_MOCK_DOOMED_ID", "")

CONTAINERS = [
    {
        "Id": "a" * 64,
        "Names": ["/forger-web-1"],
        "Image": "nginx:alpine",
        "State": "running",
        "Status": "Up 2 minutes",
    },
    {
        "Id": "b" * 64,
        "Names": ["/forger-cache-1"],
        "Image": "redis:alpine",
        "State": "running",
        "Status": "Up 2 minutes",
    },
    {
        "Id": "c" * 64,
        "Names": ["/unrelated-app"],
        "Image": "forger-web-image:latest",  # image contains a rule substring on purpose
        "State": "running",
        "Status": "Up 3 days",
    },
]


def http_response(body: bytes, status: int = 200, reason: str = "OK") -> bytes:
    head = (
        f"HTTP/1.1 {status} {reason}\r\n"
        f"Content-Type: application/json\r\n"
        f"Content-Length: {len(body)}\r\n"
        f"Connection: close\r\n\r\n"
    ).encode()
    return head + body


class MockDockerHandler(socketserver.BaseRequestHandler):
    def handle(self):
        data = b""
        while b"\r\n\r\n" not in data:
            chunk = self.request.recv(4096)
            if not chunk:
                return
            data += chunk
        head = data.split(b"\r\n\r\n", 1)[0].decode(errors="replace")
        lines = head.split("\r\n")
        method, path, _version = lines[0].split(" ", 2)

        if path == "/_ping" and method == "GET":
            self.request.sendall(http_response(b"OK"))
        elif method == "GET" and path.split("?", 1)[0].endswith("/containers/json"):
            # Accept /containers/json and /v1.41/containers/json?all=false
            self.request.sendall(http_response(json.dumps(CONTAINERS).encode()))
        elif method == "POST":
            with threading.Lock():
                with open(POST_LOG, "a") as f:
                    f.write(f"POST {path}\n")
            if DELAY_MS > 0:
                time.sleep(DELAY_MS / 1000.0)
            if DOOMED_ID and DOOMED_ID in path:
                body = b'{"message":"simulated engine failure"}'
                self.request.sendall(http_response(body, 500, "Internal Server Error"))
            else:
                self.request.sendall(http_response(b"{}", 204, "No Content"))
        else:
            self.request.sendall(http_response(b'{"message":"not found"}', 404, "Not Found"))


class UnixServer(socketserver.ThreadingUnixStreamServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    try:
        os.unlink(SOCKET)
    except FileNotFoundError:
        pass
    try:
        os.unlink(POST_LOG)
    except FileNotFoundError:
        pass

    server = UnixServer(SOCKET, MockDockerHandler)
    print(f"mock docker engine listening on {SOCKET}; POSTs logged to {POST_LOG}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        try:
            os.unlink(SOCKET)
        except FileNotFoundError:
            pass


if __name__ == "__main__":
    sys.exit(main())
