#!/usr/bin/env python3
import socket
import subprocess
import sys
import tempfile
import time

agent = sys.argv[1]
state = tempfile.mkdtemp(prefix="agent_timeout_")
proc = subprocess.Popen(
    [agent, "--port", "0", "--state-dir", state,
     "--pair-code", "123456", "--io-timeout-ms", "300"],
    stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
try:
    port = None
    for _ in range(12):
        line = proc.stdout.readline()
        if line.startswith("PORT="):
            port = int(line.strip().split("=", 1)[1])
            break
    assert port, "agent did not print PORT"

    slow = socket.create_connection(("127.0.0.1", port), timeout=2)
    time.sleep(0.9)
    slow.settimeout(1.0)
    try:
        data = slow.recv(1)
    except socket.timeout:
        raise AssertionError("slow connection was not closed")
    assert data == b"", data
    slow.close()

    # The accept loop must still be alive after the slow worker timed out.
    fast = socket.create_connection(("127.0.0.1", port), timeout=2)
    fast.close()

    # Trickle one byte per read-timeout window. Per-read SO_RCVTIMEO alone
    # would keep this connection alive forever; the auth deadline must close
    # it regardless of how many bytes arrived.
    trickle = socket.create_connection(("127.0.0.1", port), timeout=2)
    trickle.sendall(b"\x00\x01\x00\x00")  # plausible 65536-byte frame
    started = time.monotonic()
    closed = False
    for _ in range(40):
        try:
            trickle.send(b"x")
        except OSError:
            closed = True
            break
        trickle.settimeout(0.05)
        try:
            if trickle.recv(1) == b"":
                closed = True
                break
        except socket.timeout:
            pass
        time.sleep(0.05)
    elapsed = time.monotonic() - started
    trickle.close()
    assert closed, "trickled auth frame never hit the absolute deadline"
    assert elapsed < 2.0, "auth deadline took %.2fs" % elapsed

    # Worker slot must be released after the deadline close.
    fast = socket.create_connection(("127.0.0.1", port), timeout=2)
    fast.close()

    print("test_connection_timeout: OK")
finally:
    proc.terminate()
    try:
        proc.wait(timeout=2)
    except subprocess.TimeoutExpired:
        proc.kill()
