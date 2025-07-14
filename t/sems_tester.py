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

    def sendSIP(self, msg: bytes, sock: socket = None):
        s = sock
        if not s:
            s = self.makeSIPSocket()
        msg = msg.replace(b"\n", b"\r\n")
        s.send(msg)
        if not sock:
            s.close()

    def sendRecvSIP(self, msg: bytes, exp: bytes, sock: socket = None):
        s = sock
        if not s:
            s = self.makeSIPSocket()
        r = re.compile(exp.replace(b"\n", b"[\r\n]{1,2}"), re.DOTALL)
        self.sendSIP(msg, s)
        m = s.recv(1000)
        self.assertRegex(m, r)
        if not sock:
            s.close()

    def sendRecvXMLRPC(self, req: bytes, exp: bytes, sock: socket = None):
        s = sock
        if not s:
            s = self.makeXMLRPCSocket()
        l = len(req)
        req = (
            b"POST / HTTP/1.1\r\nConnection: close\r\nHost: 127.0.0.1:"
            + bytes(self._xmlrpc_port)
            + b"\r\nUser-Agent: Tester\r\nContent-Type: text/xml\r\nContent-Length: "
            + bytes(str(l), "latin1")
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
