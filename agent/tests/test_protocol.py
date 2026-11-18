#!/usr/bin/env python3
"""协议级集成测试：启动真实 agent 进程，走本机 TCP。

覆盖：auth、ping、exec、超时、shell 模式、push/pull、路径拒绝。
不依赖 Android/ADB。
"""

import base64
import hashlib
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import unittest

TOKEN = "test-token-0123456789"


def recv_exact(sock, length):
    chunks = []
    remaining = length
    while remaining:
        chunk = sock.recv(remaining)
        if not chunk:
            raise ConnectionError("connection closed")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def send_frame(sock, payload):
    sock.sendall(struct.pack(">I", len(payload)) + payload)


def recv_frame(sock):
    (length,) = struct.unpack(">I", recv_exact(sock, 4))
    if length == 0 or length > 1024 * 1024:
        raise ValueError(f"invalid frame length: {length}")
    return recv_exact(sock, length)


def send_json(sock, obj):
    send_frame(sock, b"\x01" + json.dumps(obj).encode("utf-8"))


def recv_json(sock):
    payload = recv_frame(sock)
    assert payload[0] == 0x01, f"expected JSON frame, got {payload[0]}"
    return json.loads(payload[1:])


def send_binary(sock, offset, data):
    send_frame(sock, b"\x02" + struct.pack(">Q", offset) + data)


class AgentProtocolTest(unittest.TestCase):
    agent_path = None

    def setUp(self):
        self.state_dir = tempfile.mkdtemp(prefix="agent-state-")
        # 明文测试模式下 --allow-plaintext 走 token；危险能力仍从 peers.txt 读取，
        # 这里预置一条带 dangerous 的 peer，让 shell 测试能执行。
        self.peers_path = os.path.join(self.state_dir, "peers.txt")
        with open(self.peers_path, "w", encoding="utf-8") as handle:
            handle.write("11" * 32 + " test-peer exec,push,pull,dangerous\n")
        self.proc = subprocess.Popen(
            [self.agent_path, "--port", "0", "--token", TOKEN,
             "--allow-plaintext", "--state-dir", self.state_dir],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        line = self.proc.stdout.readline().strip()
        if not line.startswith("PORT="):
            self.fail(f"agent did not report port: {line!r}")
        self.port = int(line.split("=", 1)[1])
        self.sock = socket.create_connection(("127.0.0.1", self.port), timeout=5)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        send_json(self.sock, {"op": "auth", "token": TOKEN})
        auth = recv_json(self.sock)
        self.assertEqual(auth["op"], "auth_ok")

    def tearDown(self):
        try:
            self.sock.close()
        finally:
            self.proc.terminate()
            self.proc.wait(timeout=5)
            shutil.rmtree(self.state_dir, ignore_errors=True)

    def test_second_connection_not_blocked(self):
        """第一个连接空闲时，第二个连接也必须能被受理。

        agent 旧实现是单线程 accept；一个僵死的旧会话会占住服务器，
        表现为桌面端“连接极慢，重启手机端服务才连上”。
        """
        second = socket.create_connection(("127.0.0.1", self.port), timeout=5)
        try:
            second.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            send_json(second, {"op": "auth", "token": TOKEN})
            auth = recv_json(second)
            self.assertEqual(auth["op"], "auth_ok")

            send_json(second, {"op": "ping", "id": 99})
            pong = recv_json(second)
            self.assertEqual(pong["op"], "pong")
        finally:
            second.close()

    def test_ping(self):
        send_json(self.sock, {"op": "ping", "id": 1})
        pong = recv_json(self.sock)
        self.assertEqual(pong["op"], "pong")
        self.assertEqual(pong["protocol"], 1)
        self.assertIn("uid", pong)

    def test_exec_argv(self):
        send_json(
            self.sock,
            {"op": "exec", "id": 2, "argv": ["/bin/echo", "hello"], "timeout_ms": 2000},
        )
        stdout = b""
        while True:
            message = recv_json(self.sock)
            if message["op"] == "stdout":
                stdout += base64.b64decode(message["data"])
            elif message["op"] == "exit":
                self.assertEqual(message["code"], 0)
                self.assertFalse(message["timed_out"])
                break
        self.assertEqual(stdout, b"hello\n")

    def test_shell_requires_dangerous(self):
        os.rename(self.peers_path, self.peers_path + ".bak")
        try:
            send_json(
                self.sock,
                {
                    "op": "exec",
                    "id": 9,
                    "shell": True,
                    "command": "exit 7",
                    "timeout_ms": 2000,
                },
            )
            message = recv_json(self.sock)
            self.assertEqual(message["op"], "error")
            self.assertEqual(message["code"], "permission_denied")
        finally:
            os.rename(self.peers_path + ".bak", self.peers_path)

    def test_exec_shell_exit_code(self):
        send_json(
            self.sock,
            {
                "op": "exec",
                "id": 3,
                "shell": True,
                "command": "exit 7",
                "timeout_ms": 2000,
            },
        )
        while True:
            message = recv_json(self.sock)
            if message["op"] == "exit":
                self.assertEqual(message["code"], 7)
                break

    def test_exec_timeout(self):
        send_json(
            self.sock,
            {
                "op": "exec",
                "id": 4,
                "argv": ["/bin/sh", "-c", "sleep 2"],
                "timeout_ms": 100,
            },
        )
        while True:
            message = recv_json(self.sock)
            if message["op"] == "exit":
                self.assertTrue(message["timed_out"])
                break

    def test_push_and_pull(self):
        with tempfile.TemporaryDirectory() as directory:
            target = os.path.join(directory, "upload.bin")
            data = b"phone-agent-payload" * 100
            digest = hashlib.sha256(data).hexdigest()

            send_json(
                self.sock,
                {
                    "op": "push",
                    "id": 5,
                    "path": target,
                    "size": len(data),
                    "mode": "0644",
                    "sha256": digest,
                    "overwrite": False,
                },
            )
            ready = recv_json(self.sock)
            self.assertEqual(ready["op"], "push_ready")

            offset = 0
            while offset < len(data):
                chunk = data[offset : offset + 4096]
                send_binary(self.sock, offset, chunk)
                offset += len(chunk)
            done = recv_json(self.sock)
            self.assertEqual(done["op"], "push_done")

            with open(target, "rb") as handle:
                self.assertEqual(handle.read(), data)

            send_json(self.sock, {"op": "pull", "id": 6, "path": target})
            meta = recv_json(self.sock)
            self.assertEqual(meta["op"], "pull_meta")
            self.assertEqual(meta["size"], len(data))
            self.assertEqual(meta["sha256"], digest)

            received = bytearray()
            while True:
                payload = recv_frame(self.sock)
                if payload[0] == 0x02:
                    (offset,) = struct.unpack(">Q", payload[1:9])
                    self.assertEqual(offset, len(received))
                    received.extend(payload[9:])
                else:
                    message = json.loads(payload[1:])
                    self.assertEqual(message["op"], "pull_done")
                    break
            self.assertEqual(bytes(received), data)

    def test_rejects_unsafe_path(self):
        send_json(
            self.sock,
            {
                "op": "push",
                "id": 7,
                "path": "/tmp/../etc/passwd",
                "size": 1,
                "mode": "0644",
                "sha256": "0" * 64,
                "overwrite": True,
            },
        )
        reply = recv_json(self.sock)
        self.assertEqual(reply["op"], "error")

    def test_rejects_bad_token(self):
        # agent 是单客户端：先断开主连接，server 才会回到 accept。
        self.sock.close()
        self.sock = socket.socket()
        other = socket.create_connection(("127.0.0.1", self.port), timeout=5)
        send_json(other, {"op": "auth", "token": "wrong"})
        with self.assertRaises((ConnectionError, ValueError)):
            recv_json(other)
        other.close()


if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit("usage: test_protocol.py <path-to-phone_agent>")
    AgentProtocolTest.agent_path = os.path.abspath(sys.argv.pop(1))
    unittest.main(verbosity=2)
