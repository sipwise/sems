#!/usr/bin/env python3

import sems_tester
import socket
import time


class TestBasic(sems_tester.TestCase):
    _config_base = "register-cache"
    _sip_port = 5060

    def testRegister(self):
        # query
        self.sendRecvSIP(
            b"""REGISTER sip:127.0.0.1 SIP/2.0
Via: SIP/2.0/UDP 127.0.0.1:57715;branch=z9hG4bKhedcec8e2a445fd80;rport
Max-Forwards: 6
To: <sip:monitoring@127.0.0.1>
From: monit <sip:monit@127.0.0.1>;tag=8667fc7c8b14e846
Call-ID: 1eb48ca58e6dbdbb
CSeq: 63102 REGISTER
Content-Length: 0
User-Agent: tester/5.33.0

""",
            b"""^SIP/2.0 200 OK
Via: SIP/2.0/UDP 127.0.0.1:57715;branch=z9hG4bKhedcec8e2a445fd80;rport=1;received=.*?
To: <sip:monitoring@127.0.0.1>;tag=.*?
From: monit <sip:monit@127.0.0.1>;tag=8667fc7c8b14e846
Call-ID: 1eb48ca58e6dbdbb
CSeq: 63102 REGISTER
Content-Length: 0

$""",
        )

        # register
        self.sendRecvSIP(
            b"""REGISTER sip:127.0.0.1 SIP/2.0
Via: SIP/2.0/UDP 127.0.0.1:57715;branch=z9hG4bKhedcec8e2a445fd80;rport
Max-Forwards: 6
To: <sip:monitoring@127.0.0.1>
From: monit <sip:monit@127.0.0.1>;tag=8667fc7c8b14e846
Call-ID: 1eb48ca58e6dbdbb
CSeq: 63104 REGISTER
Contact: <sip:127.0.0.1:57715>
Content-Length: 0
Expires: 5
User-Agent: tester/5.33.0

""",
            b"""^SIP/2.0 200 OK
Via: SIP/2.0/UDP 127.0.0.1:57715;branch=z9hG4bKhedcec8e2a445fd80;rport=2;received=.*?
To: <sip:monitoring@127.0.0.1>;tag=.*?
From: monit <sip:monit@127.0.0.1>;tag=8667fc7c8b14e846
Call-ID: 1eb48ca58e6dbdbb
CSeq: 63104 REGISTER
Contact: <sip:127.0.0.1:57715;transport=udp>;expires=[45]
Content-Length: 0

$""",
        )

        # wait for expire
        time.sleep(7)

        # query
        self.sendRecvSIP(
            b"""REGISTER sip:127.0.0.1 SIP/2.0
Via: SIP/2.0/UDP 127.0.0.1:57715;branch=z9hG4bKhedcec8e2a445fd80;rport
Max-Forwards: 6
To: <sip:monitoring@127.0.0.1>
From: monit <sip:monit@127.0.0.1>;tag=8667fc7c8b14e846
Call-ID: 1eb48ca58e6dbdbb
CSeq: 63105 REGISTER
Content-Length: 0
User-Agent: tester/5.33.0

""",
            b"""^SIP/2.0 200 OK
Via: SIP/2.0/UDP 127.0.0.1:57715;branch=z9hG4bKhedcec8e2a445fd80;rport=3;received=.*?
To: <sip:monitoring@127.0.0.1>;tag=.*?
From: monit <sip:monit@127.0.0.1>;tag=8667fc7c8b14e846
Call-ID: 1eb48ca58e6dbdbb
CSeq: 63105 REGISTER
Content-Length: 0

$""",
        )

        # wait for garbage collect
        time.sleep(10)

        # query again
        self.sendRecvSIP(
            b"""REGISTER sip:127.0.0.1 SIP/2.0
Via: SIP/2.0/UDP 127.0.0.1:57715;branch=z9hG4bKhedcec8e2a445fd80;rport
Max-Forwards: 6
To: <sip:monitoring@127.0.0.1>
From: monit <sip:monit@127.0.0.1>;tag=8667fc7c8b14e846
Call-ID: 1eb48ca58e6dbdbb
CSeq: 63106 REGISTER
Content-Length: 0
User-Agent: tester/5.33.0

""",
            b"""^SIP/2.0 200 OK
Via: SIP/2.0/UDP 127.0.0.1:57715;branch=z9hG4bKhedcec8e2a445fd80;rport=4;received=.*?
To: <sip:monitoring@127.0.0.1>;tag=.*?
From: monit <sip:monit@127.0.0.1>;tag=8667fc7c8b14e846
Call-ID: 1eb48ca58e6dbdbb
CSeq: 63106 REGISTER
Content-Length: 0

$""",
        )

        # update
        self.sendRecvSIP(
            b"""REGISTER sip:127.0.0.1 SIP/2.0
Via: SIP/2.0/UDP 127.0.0.1:57715;branch=z9hG4bKhedcec8e2a445fd80;rport
Max-Forwards: 6
To: <sip:monitoring@127.0.0.1>
From: monit <sip:monit@127.0.0.1>;tag=8667fc7c8b14e846
Call-ID: 1eb48ca58egsdfbb
CSeq: 63107 REGISTER
Contact: <sip:127.0.0.1:57715>
Content-Length: 0
Expires: 5
User-Agent: tester/5.33.0

""",
            b"""^SIP/2.0 200 OK
Via: SIP/2.0/UDP 127.0.0.1:57715;branch=z9hG4bKhedcec8e2a445fd80;rport=5;received=.*?
To: <sip:monitoring@127.0.0.1>;tag=.*?
From: monit <sip:monit@127.0.0.1>;tag=8667fc7c8b14e846
Call-ID: 1eb48ca58egsdfbb
CSeq: 63107 REGISTER
Contact: <sip:127.0.0.1:57715;transport=udp>;expires=[45]
Content-Length: 0

$""",
        )

        # query
        self.sendRecvSIP(
            b"""REGISTER sip:127.0.0.1 SIP/2.0
Via: SIP/2.0/UDP 127.0.0.1:57715;branch=z9hG4bKhedcec8e2a445fd80;rport
Max-Forwards: 6
To: <sip:monitoring@127.0.0.1>
From: monit <sip:monit@127.0.0.1>;tag=8667fc7c8b14e846
Call-ID: 1eb48ca58e6dbdbb
CSeq: 63108 REGISTER
Content-Length: 0
User-Agent: tester/5.33.0

""",
            b"""^SIP/2.0 200 OK
Via: SIP/2.0/UDP 127.0.0.1:57715;branch=z9hG4bKhedcec8e2a445fd80;rport=6;received=.*?
To: <sip:monitoring@127.0.0.1>;tag=.*?
From: monit <sip:monit@127.0.0.1>;tag=8667fc7c8b14e846
Call-ID: 1eb48ca58e6dbdbb
CSeq: 63108 REGISTER
Contact: sip:127.0.0.1:57715;transport=udp;expires=[45]
Content-Length: 0

$""",
        )

        # unregister
        self.sendRecvSIP(
            b"""REGISTER sip:127.0.0.1 SIP/2.0
Via: SIP/2.0/UDP 127.0.0.1:57715;branch=z9hG4bKhedcec8e2a445fd80;rport
Max-Forwards: 6
To: <sip:monitoring@127.0.0.1>
From: monit <sip:monit@127.0.0.1>;tag=8667fc7c8b14e846
Call-ID: 1eb48cdfbb
CSeq: 63109 REGISTER
Contact: *
Content-Length: 0
Expires: 0
User-Agent: tester/5.33.0

""",
            b"""^SIP/2.0 200 OK
Via: SIP/2.0/UDP 127.0.0.1:57715;branch=z9hG4bKhedcec8e2a445fd80;rport=7;received=.*?
To: <sip:monitoring@127.0.0.1>;tag=.*?
From: monit <sip:monit@127.0.0.1>;tag=8667fc7c8b14e846
Call-ID: 1eb48cdfbb
CSeq: 63109 REGISTER
Content-Length: 0

$""",
        )

        # query
        self.sendRecvSIP(
            b"""REGISTER sip:127.0.0.1 SIP/2.0
Via: SIP/2.0/UDP 127.0.0.1:57715;branch=z9hG4bKhedcec8e2a445fd80;rport
Max-Forwards: 6
To: <sip:monitoring@127.0.0.1>
From: monit <sip:monit@127.0.0.1>;tag=8667fc7c8b14e846
Call-ID: 1eb48ca58e6dbdbb
CSeq: 63110 REGISTER
Content-Length: 0
User-Agent: tester/5.33.0

""",
            b"""^SIP/2.0 200 OK
Via: SIP/2.0/UDP 127.0.0.1:57715;branch=z9hG4bKhedcec8e2a445fd80;rport=8;received=.*?
To: <sip:monitoring@127.0.0.1>;tag=.*?
From: monit <sip:monit@127.0.0.1>;tag=8667fc7c8b14e846
Call-ID: 1eb48ca58e6dbdbb
CSeq: 63110 REGISTER
Content-Length: 0

$""",
        )


if __name__ == "__main__":
    sems_tester.main()
