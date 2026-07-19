#!/usr/bin/env python3

import csv
import socket
import sys
import time
from pathlib import Path


ADDRESS = ("127.0.0.1", 18080)


def exchange(payload, shutdown_write=False, timeout=1.5):
    response = b""
    outcome = "response"
    try:
        with socket.create_connection(ADDRESS, timeout=timeout) as connection:
            connection.settimeout(timeout)
            connection.sendall(payload)
            if shutdown_write:
                connection.shutdown(socket.SHUT_WR)
            while len(response) < 4096:
                try:
                    block = connection.recv(4096 - len(response))
                except socket.timeout:
                    outcome = "timeout"
                    break
                if not block:
                    outcome = "closed"
                    break
                response += block
    except (ConnectionError, OSError) as error:
        outcome = f"socket-error:{type(error).__name__}"
    first_line = response.split(b"\r\n", 1)[0].decode("ascii", errors="replace")
    return outcome, first_line, len(response)


def health_check():
    outcome, first_line, _ = exchange(
        b"GET /bench/1k HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"
    )
    return outcome in ("response", "closed") and " 200 " in first_line


def slow_headers():
    payload = (
        b"GET /bench/1k HTTP/1.1\r\n"
        b"Host: localhost\r\n"
        b"Connection: close\r\n\r\n"
    )
    response = b""
    outcome = "response"
    try:
        with socket.create_connection(ADDRESS, timeout=2) as connection:
            connection.settimeout(2)
            for byte in payload:
                connection.send(bytes((byte,)))
                time.sleep(0.003)
            response = connection.recv(4096)
    except socket.timeout:
        outcome = "timeout"
    except (ConnectionError, OSError) as error:
        outcome = f"socket-error:{type(error).__name__}"
    first_line = response.split(b"\r\n", 1)[0].decode("ascii", errors="replace")
    return outcome, first_line, len(response)


def blocked_readers(count=128):
    connections = []
    try:
        for _ in range(count):
            connection = socket.create_connection(ADDRESS, timeout=1)
            connection.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
            connection.sendall(
                b"GET /bench/64k HTTP/1.1\r\nHost: localhost\r\n"
                b"Connection: close\r\n\r\n"
            )
            connections.append(connection)
        time.sleep(2)
        return "held", "", len(connections)
    finally:
        for connection in connections:
            connection.close()


def main():
    if len(sys.argv) != 2:
        print("usage: robustness_http1.py <output.csv>", file=sys.stderr)
        return 2

    cases = [
        (
            "conflicting-content-length",
            b"POST /bench/discard HTTP/1.1\r\nHost: localhost\r\n"
            b"Content-Length: 4\r\nContent-Length: 5\r\n\r\nabcde",
            False,
        ),
        (
            "content-length-and-transfer-encoding",
            b"POST /bench/discard HTTP/1.1\r\nHost: localhost\r\n"
            b"Content-Length: 4\r\nTransfer-Encoding: chunked\r\n\r\n"
            b"4\r\ntest\r\n0\r\n\r\n",
            False,
        ),
        (
            "invalid-chunk-size",
            b"POST /bench/discard HTTP/1.1\r\nHost: localhost\r\n"
            b"Transfer-Encoding: chunked\r\n\r\nxyz\r\ntest\r\n",
            True,
        ),
        (
            "missing-chunk-terminator",
            b"POST /bench/discard HTTP/1.1\r\nHost: localhost\r\n"
            b"Transfer-Encoding: chunked\r\n\r\n4\r\ntest\r\n",
            True,
        ),
        (
            "declared-body-then-half-close",
            b"POST /bench/discard HTTP/1.1\r\nHost: localhost\r\n"
            b"Content-Length: 65536\r\n\r\npartial",
            True,
        ),
        (
            "oversized-uri-32k",
            b"GET /" + b"x" * 32768 + b" HTTP/1.1\r\nHost: localhost\r\n\r\n",
            False,
        ),
        (
            "oversized-header-64k",
            b"GET /bench/1k HTTP/1.1\r\nHost: localhost\r\nX-Large: "
            + b"x" * 65536
            + b"\r\n\r\n",
            False,
        ),
        (
            "many-headers",
            b"GET /bench/1k HTTP/1.1\r\nHost: localhost\r\n"
            + b"".join(f"X-{index}: value\r\n".encode() for index in range(2000))
            + b"\r\n",
            False,
        ),
    ]

    rows = []
    for name, payload, shutdown_write in cases:
        outcome, first_line, received = exchange(payload, shutdown_write)
        rows.append((name, outcome, first_line, received, health_check()))
    outcome, first_line, received = slow_headers()
    rows.append(("slow-headers", outcome, first_line, received, health_check()))
    outcome, first_line, received = blocked_readers()
    rows.append(("blocked-response-readers", outcome, first_line, received, health_check()))

    output = Path(sys.argv[1])
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(("case", "outcome", "first_line", "bytes", "health_after"))
        writer.writerows(rows)

    if not all(row[-1] for row in rows):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
