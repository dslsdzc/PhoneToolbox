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

    print("test_connection_timeout: OK")
finally:
    proc.terminate()
    try:
        proc.wait(timeout=2)
    except subprocess.TimeoutExpired:
        proc.kill()
