import os
import time
import re
import socket
import subprocess
import tempfile
import typing
import unittest


class TestCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tmpdir = tempfile.TemporaryDirectory()
        tmpsock = tmpdir.name + "/notify.sock"

        sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        sock.bind(tmpsock)

        env = {"NOTIFY_SOCKET": tmpsock}
        if os.environ.get("LD_PRELOAD"):
            env["LD_PRELOAD"] = os.environ.get("LD_PRELOAD")
        if os.environ.get("TEST_SOCKET_PATH"):
            env["TEST_SOCKET_PATH"] = os.environ.get("TEST_SOCKET_PATH")

        binary = os.environ.get("BINARY", "core/sems")

        os.makedirs("t/run/", exist_ok=True)
        cmdline = [
            binary,
            "-f",
            "t/" + cls._config_base + ".conf",
            "-P",
            "t/run/" + str(os.getpid()) + ".pid",
        ]

        vg_opt = os.environ.get("WITH_VALGRIND")
        if vg_opt:
            cmdline.insert(0, "valgrind")
            if isinstance(vg_opt, str) and vg_opt == "full":
                cmdline.insert(1, "--leak-check=full")
                cmdline.insert(1, "--show-leak-kinds=all")

        cls._proc = subprocess.Popen(
            cmdline,
            env=env,
        )

        cls._sd_msg = sock.recv(1000)

        sock.close()
        os.unlink(tmpsock)
        tmpdir.cleanup()

    @classmethod
    def tearDownClass(cls):
        cls._proc.terminate()
        cls._proc.wait()
        try:
            os.unlink("t/run/" + str(os.getpid()) + ".pid")
        except FileNotFoundError:
            pass

    def testSdNotify(self):
        self.assertEqual(self._sd_msg, b"READY=1")

    def testPreload(self):
        self.assertEqual(os.environ.get("RTPE_PRELOAD_TEST_ACTIVE"), "1")

    @classmethod
    def makeSIPSocket(cls):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(3)
        sock.connect(("127.0.0.1", cls._sip_port))
        return sock

    @classmethod
    def makeXMLRPCSocket(cls):
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(3)
        sock.connect(("127.0.0.1", cls._xmlrpc_port))
        return sock

    def sendSIP(self, msg: str, sock: socket = None):
        s = sock
        if not s:
            s = self.makeSIPSocket()
        s.send(msg.replace("\n", "\r\n").encode("utf-8"))
        if not sock:
            s.close()

    def sendRecvSIP(self, msg: str, exp: str, sock: socket = None):
        s = sock
        if not s:
            s = self.makeSIPSocket()
        r = re.compile(exp.replace("\n", "[\\r\\n]{1,2}"), re.DOTALL)
        self.sendSIP(msg, s)
        m = s.recv(1000).decode("utf-8")
        self.assertRegex(m, r)
        if not sock:
            s.close()

    @classmethod
    def makeUASSocket(cls, port):
        """Bind socket on given port — simulates UAS/callee (leg B)."""
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(3)
        sock.bind(("127.0.0.1", port))
        return sock

    @classmethod
    def makeUACSocket(cls, port):
        """Bind to port and connect to SIP server — simulates UAC/caller (leg A).
        Using a bound socket ensures the Contact header IP:port is reachable
        when the server needs to send requests (BYE, re-INVITE) back to the UAC."""
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(3)
        sock.bind(("127.0.0.1", port))
        sock.connect(("127.0.0.1", cls._sip_port))
        return sock

    def recvSIP(self, sock: socket.socket) -> str:
        """Receive a SIP message without asserting."""
        return sock.recv(4096).decode("utf-8")

    def recvFromSIP(self, sock: socket.socket) -> typing.Tuple[str, tuple]:
        """Receive a SIP message and return (msg, sender_addr)."""
        data, addr = sock.recvfrom(4096)
        return data.decode("utf-8"), addr

    def sendToSIP(self, msg: str, addr: tuple, sock: socket.socket):
        """Send SIP message to a specific address from a bound socket."""
        sock.sendto(msg.replace("\n", "\r\n").encode("utf-8"), addr)

    def recvB2BINVITE(self, sock: socket.socket) -> typing.Tuple[str, tuple]:
        """Receive a B2B INVITE from the shared UAS socket, skipping stale non-INVITE messages
        (e.g. BYE/ACK retransmissions from the previous test that slipped past tearDown)."""
        for _ in range(10):
            msg, addr = self.recvFromSIP(sock)
            if msg.startswith("INVITE"):
                return msg, addr
        raise AssertionError("No B2B INVITE received after 10 attempts")

    def recvSIPForCall(self, sock: socket.socket, b2b_call_id: str) -> str:
        """Receive a SIP message that belongs to the given B2B Call-ID, discarding any
        stale messages from previous tests that have a different Call-ID."""
        for _ in range(10):
            msg = self.recvSIP(sock)
            if b2b_call_id in msg:
                return msg
        raise AssertionError(f"No SIP message for Call-ID {b2b_call_id} received")

    def assertSIP(self, msg: str, exp: str):
        """Assert a SIP message matches a regex pattern."""
        r = re.compile(exp.replace("\n", "[\\r\\n]{1,2}"), re.DOTALL)
        self.assertRegex(msg, r)

    def sendRecvXMLRPC(self, req: bytes, exp: bytes, sock: socket = None):
        s = sock
        if not s:
            s = self.makeXMLRPCSocket()
        req_len = len(req)
        req = (
            b"POST / HTTP/1.1\r\nConnection: close\r\nHost: 127.0.0.1:"
            + bytes(self._xmlrpc_port)
            + b"\r\nUser-Agent: Tester\r\nContent-Type: text/xml\r\nContent-Length: "
            + bytes(str(req_len), "latin1")
            + b"\r\n\r\n"
            + req
        )
        head_r = re.compile(
            b"^HTTP/1.1 200 OK[\r\n]{1,2}Server: XMLRPC\\+\\+ 0.8[\r\n]{1,2}Content-Type: text/xml[\r\n]{1,2}Content-length: \\d+[\r\n]{2,4}",
            re.DOTALL,
        )
        r = re.compile(exp.replace(b"\n", b"[\r\n]{1,2}"), re.DOTALL)
        s.send(req)
        m = s.recv(1000)
        matches = head_r.match(m)
        self.assertTrue(matches)
        m = m.removeprefix(matches[0])
        self.assertRegex(m, r)
        if not sock:
            s.close()


def main():
    unittest.main()
