#!/usr/bin/env python3

"""SDP rewriting done by AmB2BMedia::replaceConnectionAddress() when SEMS
relays media: the media port becomes a local one, so the a=rtcp received from
the other party must be replaced with our own RTCP port rather than echoed."""

import re as _re
import sems_tester

# media ports advertised by the test endpoints, outside the SEMS relay range
_RTP_PORT_A = 9102
_RTP_PORT_B = 9104

# RTCP ports deliberately not RTP+1, so an echoed value cannot be mistaken
# for a correctly derived one
_RTCP_PORT_A = 9190
_RTCP_PORT_B = 9192

_SDP_A = (
    "v=0\n"
    "o=- 1000000001 1000000001 IN IP4 127.0.0.1\n"
    "s=-\n"
    "c=IN IP4 127.0.0.1\n"
    "t=0 0\n"
    f"m=audio {_RTP_PORT_A} RTP/AVP 0 8\n"
    "a=rtpmap:0 PCMU/8000\n"
    "a=rtpmap:8 PCMA/8000\n"
    f"a=rtcp:{_RTCP_PORT_A}\n"
    "a=sendrecv\n"
)

_SDP_B = (
    "v=0\n"
    "o=- 2000000001 2000000001 IN IP4 127.0.0.1\n"
    "s=-\n"
    "c=IN IP4 127.0.0.1\n"
    "t=0 0\n"
    f"m=audio {_RTP_PORT_B} RTP/AVP 0 8\n"
    "a=rtpmap:0 PCMU/8000\n"
    "a=rtpmap:8 PCMA/8000\n"
    f"a=rtcp:{_RTCP_PORT_B}\n"
    "a=sendrecv\n"
)

# SDP without a=rtcp - our own port must be stated all the same
_SDP_A_NO_RTCP = (
    "v=0\n"
    "o=- 1000000003 1000000003 IN IP4 127.0.0.1\n"
    "s=-\n"
    "c=IN IP4 127.0.0.1\n"
    "t=0 0\n"
    f"m=audio {_RTP_PORT_A} RTP/AVP 0 8\n"
    "a=rtpmap:0 PCMU/8000\n"
    "a=sendrecv\n"
)


def _hdr(msg, name):
    m = _re.search(name + r": ([^\r\n]+)", msg)
    return m.group(1) if m else ""


def _to_tag(msg):
    m = _re.search(r"To:[^\r\n]*tag=([A-Za-z0-9\-_\.]+)", msg)
    return m.group(1) if m else ""


def _media_port(msg):
    m = _re.search(r"m=audio (\d+)", msg)
    return int(m.group(1)) if m else None


def _rtcp_port(msg):
    m = _re.search(r"a=rtcp:(\d+)", msg)
    return int(m.group(1)) if m else None


def _make_invite(branch, call_id, sdp, cseq=1, src_port=57801):
    sdp_cr = sdp.replace("\n", "\r\n")
    return (
        f"INVITE sip:bob@voip.sipwise.local SIP/2.0\n"
        f"Via: SIP/2.0/UDP 127.0.0.1:{src_port};branch={branch};rport\n"
        f"Max-Forwards: 70\n"
        f"To: <sip:bob@voip.sipwise.local>\n"
        f"From: Alice <sip:alice@voip.sipwise.local>;tag=rtcp-tag-1\n"
        f"Call-ID: {call_id}\n"
        f"CSeq: {cseq} INVITE\n"
        f"Contact: <sip:alice@127.0.0.1:{src_port}>\n"
        f"Content-Type: application/sdp\n"
        f"Content-Length: {len(sdp_cr)}\n"
        "\n" + sdp
    )


def _make_ack(branch, call_id, to_tag, cseq=1, src_port=57801):
    return (
        f"ACK sip:bob@voip.sipwise.local SIP/2.0\n"
        f"Via: SIP/2.0/UDP 127.0.0.1:{src_port};branch={branch};rport\n"
        f"Max-Forwards: 70\n"
        f"To: <sip:bob@voip.sipwise.local>;tag={to_tag}\n"
        f"From: Alice <sip:alice@voip.sipwise.local>;tag=rtcp-tag-1\n"
        f"Call-ID: {call_id}\n"
        f"CSeq: {cseq} ACK\n"
        f"Content-Length: 0\n"
        "\n"
    )


def _make_bye(branch, call_id, to_tag, cseq=2, src_port=57801):
    return (
        f"BYE sip:bob@voip.sipwise.local SIP/2.0\n"
        f"Via: SIP/2.0/UDP 127.0.0.1:{src_port};branch={branch};rport\n"
        f"Max-Forwards: 70\n"
        f"To: <sip:bob@voip.sipwise.local>;tag={to_tag}\n"
        f"From: Alice <sip:alice@voip.sipwise.local>;tag=rtcp-tag-1\n"
        f"Call-ID: {call_id}\n"
        f"CSeq: {cseq} BYE\n"
        f"Content-Length: 0\n"
        "\n"
    )


def _make_200_ok_invite(invite_msg, sdp):
    to = _hdr(invite_msg, "To")
    sdp_cr = sdp.replace("\n", "\r\n")
    return (
        f"SIP/2.0 200 OK\n"
        f"Via: {_hdr(invite_msg, 'Via')}\n"
        f"To: {to if ';tag=' in to else to + ';tag=uas-rtcp-001'}\n"
        f"From: {_hdr(invite_msg, 'From')}\n"
        f"Call-ID: {_hdr(invite_msg, 'Call-ID')}\n"
        f"CSeq: {_hdr(invite_msg, 'CSeq')}\n"
        f"Contact: <sip:bob@127.0.0.1:5078>\n"
        f"Content-Type: application/sdp\n"
        f"Content-Length: {len(sdp_cr)}\n"
        "\n" + sdp
    )


def _make_200_ok_for(request_msg):
    return (
        f"SIP/2.0 200 OK\n"
        f"Via: {_hdr(request_msg, 'Via')}\n"
        f"To: {_hdr(request_msg, 'To')}\n"
        f"From: {_hdr(request_msg, 'From')}\n"
        f"Call-ID: {_hdr(request_msg, 'Call-ID')}\n"
        f"CSeq: {_hdr(request_msg, 'CSeq')}\n"
        f"Content-Length: 0\n"
        "\n"
    )


class TestRtcp(sems_tester.TestCase):
    _config_base = "rtcp"
    _sip_port = 5068
    _uas_port = 5078

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls._uas_sock = cls.makeUASSocket(cls._uas_port)

    @classmethod
    def tearDownClass(cls):
        cls._uas_sock.close()
        super().tearDownClass()

    def setUp(self):
        """Drop datagrams left over by the previous test."""
        self._uas_sock.settimeout(0)
        try:
            while True:
                self._uas_sock.recv(4096)
        except BlockingIOError:
            pass
        finally:
            self._uas_sock.settimeout(3)

    def assertOwnRtcpPort(self, msg, foreign_rtcp_port):
        """The SDP states an RTCP port matching its own media port, and no
        trace of the port advertised by the other side."""
        media = _media_port(msg)
        self.assertIsNotNone(media, "no m=audio in relayed SDP")
        self.assertNotEqual(media, foreign_rtcp_port - 1)
        self.assertEqual(_rtcp_port(msg), media + 1)
        self.assertNotIn(str(foreign_rtcp_port), msg)

    def _call(self, call_id, src_port, offer, answer):
        """Run a call through SEMS and return (offer relayed to leg B,
        answer relayed to leg A)."""
        leg_a = self.makeUACSocket(src_port)
        try:
            self.sendSIP(_make_invite("z9hG4bK-rtcp-1", call_id, offer,
                                      src_port=src_port), leg_a)
            self.recvSIP(leg_a)  # 100 Trying

            b2b_invite, sems_addr = self.recvB2BINVITE(self._uas_sock)
            b2b_cid = _hdr(b2b_invite, "Call-ID")

            self.sendToSIP(_make_200_ok_invite(b2b_invite, answer),
                           sems_addr, self._uas_sock)
            ok_a = self.recvSIP(leg_a)
            to_tag = _to_tag(ok_a)

            self.sendSIP(_make_ack("z9hG4bK-rtcp-1-ack", call_id, to_tag,
                                   src_port=src_port), leg_a)
            self.recvSIPForCall(self._uas_sock, b2b_cid)  # ACK

            self.sendSIP(_make_bye("z9hG4bK-rtcp-1-bye", call_id, to_tag,
                                   src_port=src_port), leg_a)
            bye_b = self.recvSIPForCall(self._uas_sock, b2b_cid)
            self.sendToSIP(_make_200_ok_for(bye_b), sems_addr, self._uas_sock)
            self.recvSIP(leg_a)  # 200 OK for BYE

            return b2b_invite, ok_a
        finally:
            leg_a.close()

    def testRtcpPortReplacedBothWays(self):
        """Neither party's a=rtcp may survive into the SDP SEMS sends out."""
        offer_b, answer_a = self._call(
            "test-rtcp-call-001@127.0.0.1", 57801, _SDP_A, _SDP_B)

        # offer towards leg B carries our RTCP port, not leg A's
        self.assertOwnRtcpPort(offer_b, _RTCP_PORT_A)
        # answer towards leg A carries our RTCP port, not leg B's
        self.assertOwnRtcpPort(answer_a, _RTCP_PORT_B)

    def testRtcpPortStatedWithoutOffered(self):
        """A party advertising no a=rtcp still gets ours stated back."""
        offer_b, _ = self._call(
            "test-rtcp-call-002@127.0.0.1", 57802, _SDP_A_NO_RTCP, _SDP_B)

        media = _media_port(offer_b)
        self.assertIsNotNone(media, "no m=audio in relayed SDP")
        self.assertEqual(_rtcp_port(offer_b), media + 1)


if __name__ == "__main__":
    sems_tester.main()
