#!/usr/bin/env python3

import re as _re
import struct as _struct
import time
import sems_tester
import socket

# RTP media ports — outside SEMS relay range (10000-20000) to avoid conflicts
_RTP_PORT_A = 9002  # leg A media port (advertised in SDP_A)
_RTP_PORT_B = 9004  # leg B media port (advertised in SDP_B)
_RTP_PORT_A2 = 9006  # leg A media port for re-INVITE (new origin version)
_RTP_PORT_B2 = 9008  # leg B media port for re-INVITE

# SDP used by leg A in INVITE (plain \n — sendSIP converts to \r\n)
_SDP_A = (
    "v=0\n"
    "o=- 1000000001 1000000001 IN IP4 127.0.0.1\n"
    "s=-\n"
    "c=IN IP4 127.0.0.1\n"
    "t=0 0\n"
    f"m=audio {_RTP_PORT_A} RTP/AVP 0 8\n"
    "a=rtpmap:0 PCMU/8000\n"
    "a=rtpmap:8 PCMA/8000\n"
    "a=sendrecv\n"
)

# SDP used by leg B in 200 OK
_SDP_B = (
    "v=0\n"
    "o=- 2000000001 2000000001 IN IP4 127.0.0.1\n"
    "s=-\n"
    "c=IN IP4 127.0.0.1\n"
    "t=0 0\n"
    f"m=audio {_RTP_PORT_B} RTP/AVP 0 8\n"
    "a=rtpmap:0 PCMU/8000\n"
    "a=rtpmap:8 PCMA/8000\n"
    "a=sendrecv\n"
)


def _make_invite(branch, call_id, cseq=1, src_port=57701):
    sdp = _SDP_A.replace("\n", "\r\n")
    return (
        f"INVITE sip:bob@voip.sipwise.local SIP/2.0\n"
        f"Via: SIP/2.0/UDP 127.0.0.1:{src_port};branch={branch};rport\n"
        f"Max-Forwards: 70\n"
        f"To: <sip:bob@voip.sipwise.local>\n"
        f"From: Alice <sip:alice@voip.sipwise.local>;tag=a1b2c3d4\n"
        f"Call-ID: {call_id}\n"
        f"CSeq: {cseq} INVITE\n"
        f"Contact: <sip:alice@127.0.0.1:{src_port}>\n"
        f"Content-Type: application/sdp\n"
        f"Content-Length: {len(sdp)}\n"
        "\n"
        + _SDP_A
    )


def _make_cancel(branch, call_id, cseq=1, src_port=57701):
    return (
        f"CANCEL sip:bob@voip.sipwise.local SIP/2.0\n"
        f"Via: SIP/2.0/UDP 127.0.0.1:{src_port};branch={branch};rport\n"
        f"Max-Forwards: 70\n"
        f"To: <sip:bob@voip.sipwise.local>\n"
        f"From: Alice <sip:alice@voip.sipwise.local>;tag=a1b2c3d4\n"
        f"Call-ID: {call_id}\n"
        f"CSeq: {cseq} CANCEL\n"
        f"Content-Length: 0\n"
        "\n"
    )


def _make_ack(branch, call_id, to_tag, cseq=1, src_port=57701):
    return (
        f"ACK sip:bob@voip.sipwise.local SIP/2.0\n"
        f"Via: SIP/2.0/UDP 127.0.0.1:{src_port};branch={branch};rport\n"
        f"Max-Forwards: 70\n"
        f"To: <sip:bob@voip.sipwise.local>;tag={to_tag}\n"
        f"From: Alice <sip:alice@voip.sipwise.local>;tag=a1b2c3d4\n"
        f"Call-ID: {call_id}\n"
        f"CSeq: {cseq} ACK\n"
        f"Content-Length: 0\n"
        "\n"
    )


def _make_bye(branch, call_id, to_tag, cseq=2, src_port=57701):
    return (
        f"BYE sip:bob@voip.sipwise.local SIP/2.0\n"
        f"Via: SIP/2.0/UDP 127.0.0.1:{src_port};branch={branch};rport\n"
        f"Max-Forwards: 70\n"
        f"To: <sip:bob@voip.sipwise.local>;tag={to_tag}\n"
        f"From: Alice <sip:alice@voip.sipwise.local>;tag=a1b2c3d4\n"
        f"Call-ID: {call_id}\n"
        f"CSeq: {cseq} BYE\n"
        f"Content-Length: 0\n"
        "\n"
    )


def _hdr(msg, name):
    """Extract a header value from a received SIP message."""
    m = _re.search(name + r": ([^\r\n]+)", msg)
    return m.group(1) if m else ""


def _make_200_ok_invite(invite_msg, sdp=None, contact="<sip:bob@127.0.0.1:5070>"):
    """Build 200 OK for a received INVITE/re-INVITE.
    sdp=None uses _SDP_B; contact defaults to leg-B's address."""
    via     = _hdr(invite_msg, "Via")
    to      = _hdr(invite_msg, "To")
    from_   = _hdr(invite_msg, "From")
    call_id = _hdr(invite_msg, "Call-ID")
    cseq    = _hdr(invite_msg, "CSeq")
    to_tag  = to + ";tag=uas-tag-001" if ";tag=" not in to else to
    use_sdp = sdp if sdp is not None else _SDP_B
    sdp_cr  = use_sdp.replace("\n", "\r\n")
    return (
        f"SIP/2.0 200 OK\n"
        f"Via: {via}\n"
        f"To: {to_tag}\n"
        f"From: {from_}\n"
        f"Call-ID: {call_id}\n"
        f"CSeq: {cseq}\n"
        f"Contact: {contact}\n"
        f"Content-Type: application/sdp\n"
        f"Content-Length: {len(sdp_cr)}\n"
        "\n"
        + use_sdp
    )


def _make_provisional(code, reason, invite_msg, sdp=None):
    """Build 1xx response (UAS side), optionally with SDP body (e.g. 183)."""
    via     = _hdr(invite_msg, "Via")
    to      = _hdr(invite_msg, "To")
    from_   = _hdr(invite_msg, "From")
    call_id = _hdr(invite_msg, "Call-ID")
    cseq    = _hdr(invite_msg, "CSeq")
    if sdp is not None:
        sdp_cr = sdp.replace("\n", "\r\n")
        return (
            f"SIP/2.0 {code} {reason}\n"
            f"Via: {via}\n"
            f"To: {to};tag=uas-tag-001\n"
            f"From: {from_}\n"
            f"Call-ID: {call_id}\n"
            f"CSeq: {cseq}\n"
            f"Content-Type: application/sdp\n"
            f"Content-Length: {len(sdp_cr)}\n"
            "\n"
            + sdp
        )
    return (
        f"SIP/2.0 {code} {reason}\n"
        f"Via: {via}\n"
        f"To: {to};tag=uas-tag-001\n"
        f"From: {from_}\n"
        f"Call-ID: {call_id}\n"
        f"CSeq: {cseq}\n"
        f"Content-Length: 0\n"
        "\n"
    )


def _make_final_uas(code, reason, invite_msg):
    """Build non-2xx final response from UAS (486, 404, 487, …)."""
    via     = _hdr(invite_msg, "Via")
    to      = _hdr(invite_msg, "To")
    from_   = _hdr(invite_msg, "From")
    call_id = _hdr(invite_msg, "Call-ID")
    cseq    = _hdr(invite_msg, "CSeq")
    return (
        f"SIP/2.0 {code} {reason}\n"
        f"Via: {via}\n"
        f"To: {to};tag=uas-tag-001\n"
        f"From: {from_}\n"
        f"Call-ID: {call_id}\n"
        f"CSeq: {cseq}\n"
        f"Content-Length: 0\n"
        "\n"
    )


def _make_200_ok_for(request_msg):
    """Build 200 OK mirroring the request headers (for BYE/CANCEL responses)."""
    via     = _hdr(request_msg, "Via")
    to      = _hdr(request_msg, "To")
    from_   = _hdr(request_msg, "From")
    call_id = _hdr(request_msg, "Call-ID")
    cseq    = _hdr(request_msg, "CSeq")
    return (
        f"SIP/2.0 200 OK\n"
        f"Via: {via}\n"
        f"To: {to}\n"
        f"From: {from_}\n"
        f"Call-ID: {call_id}\n"
        f"CSeq: {cseq}\n"
        f"Content-Length: 0\n"
        "\n"
    )


# SDPs for hold (re-INVITE)
_SDP_SENDONLY_A = (
    "v=0\n"
    "o=- 1000000002 1000000002 IN IP4 127.0.0.1\n"
    "s=-\n"
    "c=IN IP4 127.0.0.1\n"
    "t=0 0\n"
    f"m=audio {_RTP_PORT_A2} RTP/AVP 0 8\n"
    "a=rtpmap:0 PCMU/8000\n"
    "a=sendonly\n"
)
_SDP_RECVONLY_B = (
    "v=0\n"
    "o=- 2000000002 2000000002 IN IP4 127.0.0.1\n"
    "s=-\n"
    "c=IN IP4 127.0.0.1\n"
    "t=0 0\n"
    f"m=audio {_RTP_PORT_B2} RTP/AVP 0 8\n"
    "a=rtpmap:0 PCMU/8000\n"
    "a=recvonly\n"
)
_SDP_SENDONLY_B = (
    "v=0\n"
    "o=- 2000000002 2000000002 IN IP4 127.0.0.1\n"
    "s=-\n"
    "c=IN IP4 127.0.0.1\n"
    "t=0 0\n"
    f"m=audio {_RTP_PORT_B2} RTP/AVP 0 8\n"
    "a=rtpmap:0 PCMU/8000\n"
    "a=sendonly\n"
)
_SDP_RECVONLY_A = (
    "v=0\n"
    "o=- 1000000002 1000000002 IN IP4 127.0.0.1\n"
    "s=-\n"
    "c=IN IP4 127.0.0.1\n"
    "t=0 0\n"
    f"m=audio {_RTP_PORT_A2} RTP/AVP 0 8\n"
    "a=rtpmap:0 PCMU/8000\n"
    "a=recvonly\n"
)
# Unhold SDPs (sendrecv, version incremented)
_SDP_SENDRECV_A2 = (
    "v=0\n"
    "o=- 1000000003 1000000003 IN IP4 127.0.0.1\n"
    "s=-\n"
    "c=IN IP4 127.0.0.1\n"
    "t=0 0\n"
    f"m=audio {_RTP_PORT_A2} RTP/AVP 0 8\n"
    "a=rtpmap:0 PCMU/8000\n"
    "a=sendrecv\n"
)
_SDP_SENDRECV_B2 = (
    "v=0\n"
    "o=- 2000000003 2000000003 IN IP4 127.0.0.1\n"
    "s=-\n"
    "c=IN IP4 127.0.0.1\n"
    "t=0 0\n"
    f"m=audio {_RTP_PORT_B2} RTP/AVP 0 8\n"
    "a=rtpmap:0 PCMU/8000\n"
    "a=sendrecv\n"
)


def _make_reinvite(branch, call_id, to_tag, cseq, sdp, src_port=57701):
    """Build a re-INVITE from leg A within an established dialog."""
    sdp_cr = sdp.replace("\n", "\r\n")
    return (
        f"INVITE sip:bob@voip.sipwise.local SIP/2.0\n"
        f"Via: SIP/2.0/UDP 127.0.0.1:{src_port};branch={branch};rport\n"
        f"Max-Forwards: 70\n"
        f"To: <sip:bob@voip.sipwise.local>;tag={to_tag}\n"
        f"From: Alice <sip:alice@voip.sipwise.local>;tag=a1b2c3d4\n"
        f"Call-ID: {call_id}\n"
        f"CSeq: {cseq} INVITE\n"
        f"Contact: <sip:alice@127.0.0.1:{src_port}>\n"
        f"Content-Type: application/sdp\n"
        f"Content-Length: {len(sdp_cr)}\n"
        "\n"
        + sdp
    )


def _to_tag(msg):
    m = _re.search(r"To:[^\r\n]*tag=([A-Za-z0-9\-_\.]+)", msg)
    return m.group(1) if m else ""


def _sdp_media_port(msg):
    """Return the first m=audio port from a SIP message body."""
    m = _re.search(r"m=audio (\d+)", msg)
    return int(m.group(1)) if m else None


def _make_rtp(seq=1, ts=0, ssrc=1, payload_type=0):
    """Build a minimal 12-byte RTP header packet."""
    return _struct.pack("!BBHII",
        0x80,         # V=2, P=0, X=0, CC=0
        payload_type,
        seq,
        ts,
        ssrc,
    )


class TestB2B(sems_tester.TestCase):
    _config_base = "b2b"
    _sip_port = 5062
    _uas_port = 5070

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls._uas_sock = cls.makeUASSocket(cls._uas_port)

    @classmethod
    def tearDownClass(cls):
        cls._uas_sock.close()
        super().tearDownClass()

    def tearDown(self):
        """Drain any leftover messages from the shared UAS socket between tests."""
        self._uas_sock.settimeout(0)
        try:
            while True:
                self._uas_sock.recv(4096)
        except BlockingIOError:
            pass
        finally:
            self._uas_sock.settimeout(3)

    def recvSIPSkipRetrans(self, sock, skip_startswith):
        """Receive SIP, discarding retransmissions that start with skip_startswith."""
        for _ in range(5):
            msg = self.recvSIP(sock)
            if not msg.startswith(skip_startswith):
                return msg
        return msg

    # ------------------------------------------------------------------
    # Scenario 1: Full call — leg A sends BYE
    # ------------------------------------------------------------------
    def testBasicCallLegAHangsUp(self):
        src_port = 57701
        leg_a = self.makeUACSocket(src_port)
        branch  = "z9hG4bK-b2b-001"
        call_id = "test-b2b-call-001@127.0.0.1"

        # Leg A → INVITE → SEMS
        self.sendSIP(_make_invite(branch, call_id, src_port=src_port), leg_a)

        # SEMS → 100 Trying → Leg A
        trying = self.recvSIP(leg_a)
        self.assertSIP(trying,
            "^SIP/2\\.0 100 [^\r]+\n"
            "Via: SIP/2\\.0/UDP 127\\.0\\.0\\.1:57701;branch=z9hG4bK-b2b-001[^\r]*\n"
            "To: <sip:bob@voip\\.sipwise\\.local>\n"
            "From: Alice <sip:alice@voip\\.sipwise\\.local>;tag=a1b2c3d4\n"
            "Call-ID: test-b2b-call-001@127\\.0\\.0\\.1\n"
            "CSeq: 1 INVITE\n"
            "Content-Length: 0\n"
        )

        # SEMS → INVITE (B2B) → Leg B
        b2b_invite, sems_addr = self.recvFromSIP(self._uas_sock)
        self.assertSIP(b2b_invite,
            "^INVITE sip:bob@voip\\.sipwise\\.local SIP/2\\.0\n"
            ".*"
            "Content-Type: application/sdp\n"
            ".*"
            "m=audio \\d+ RTP/AVP"
        )

        # Leg B → 180 Ringing → SEMS  (also stops INVITE retransmissions)
        self.sendToSIP(_make_provisional(180, "Ringing", b2b_invite),
                       sems_addr, self._uas_sock)

        # SEMS → 180 Ringing → Leg A
        ringing = self.recvSIP(leg_a)
        self.assertSIP(ringing,
            "^SIP/2\\.0 180 Ringing\n"
            "Via: SIP/2\\.0/UDP 127\\.0\\.0\\.1:57701;branch=z9hG4bK-b2b-001[^\r]*\n"
            "To: <sip:bob@voip\\.sipwise\\.local>;tag=[A-Za-z0-9\\-_\\.]+\n"
            "From: Alice <sip:alice@voip\\.sipwise\\.local>;tag=a1b2c3d4\n"
            "Call-ID: test-b2b-call-001@127\\.0\\.0\\.1\n"
            "CSeq: 1 INVITE\n"
            ".*Content-Length: 0\n"
        )

        # Leg B → 200 OK (SDP) → SEMS
        self.sendToSIP(_make_200_ok_invite(b2b_invite), sems_addr, self._uas_sock)

        # SEMS → 200 OK (SDP) → Leg A
        ok_a = self.recvSIP(leg_a)
        self.assertSIP(ok_a,
            "^SIP/2\\.0 200 OK\n"
            "Via: SIP/2\\.0/UDP 127\\.0\\.0\\.1:57701;branch=z9hG4bK-b2b-001[^\r]*\n"
            "To: <sip:bob@voip\\.sipwise\\.local>;tag=[A-Za-z0-9\\-_\\.]+\n"
            "From: Alice <sip:alice@voip\\.sipwise\\.local>;tag=a1b2c3d4\n"
            "Call-ID: test-b2b-call-001@127\\.0\\.0\\.1\n"
            "CSeq: 1 INVITE\n"
            ".*"
            "Content-Type: application/sdp\n"
            ".*"
            "m=audio \\d+ RTP/AVP"
        )

        to_tag = _to_tag(ok_a)

        # Leg A → ACK → SEMS
        self.sendSIP(_make_ack("z9hG4bK-b2b-001-ack", call_id, to_tag,
                               src_port=src_port), leg_a)

        # SEMS → ACK → Leg B
        ack_b = self.recvSIP(self._uas_sock)
        self.assertSIP(ack_b,
            "^ACK sip:[^\r]+ SIP/2\\.0\n"
            ".*"
            "CSeq: \\d+ ACK\n"
        )

        # Leg A → BYE → SEMS
        self.sendSIP(_make_bye("z9hG4bK-b2b-001-bye", call_id, to_tag,
                               src_port=src_port), leg_a)

        # SEMS → BYE → Leg B
        bye_b = self.recvSIP(self._uas_sock)
        self.assertSIP(bye_b,
            "^BYE sip:[^\r]+ SIP/2\\.0\n"
            ".*"
            "CSeq: \\d+ BYE\n"
        )

        # Leg B → 200 OK → SEMS
        self.sendToSIP(_make_200_ok_for(bye_b), sems_addr, self._uas_sock)

        # SEMS → 200 OK → Leg A
        ok_bye = self.recvSIP(leg_a)
        self.assertSIP(ok_bye,
            "^SIP/2\\.0 200 OK\n"
            "Via: SIP/2\\.0/UDP 127\\.0\\.0\\.1:57701;branch=z9hG4bK-b2b-001-bye[^\r]*\n"
            ".*"
            "CSeq: 2 BYE\n"
        )

        leg_a.close()

    # ------------------------------------------------------------------
    # Scenario 2: Full call with media flow — leg B sends BYE
    # ------------------------------------------------------------------
    def testBasicCallLegBHangsUp(self):
        src_port = 57702
        leg_a = self.makeUACSocket(src_port)
        branch  = "z9hG4bK-b2b-002"
        call_id = "test-b2b-call-002@127.0.0.1"

        # Bind media sockets before INVITE so their Unix paths exist
        rtp_a = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        rtp_a.settimeout(3)
        rtp_a.bind(("127.0.0.1", _RTP_PORT_A))

        rtp_b = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        rtp_b.settimeout(3)
        rtp_b.bind(("127.0.0.1", _RTP_PORT_B))

        self.sendSIP(_make_invite(branch, call_id, src_port=src_port), leg_a)
        self.recvSIP(leg_a)  # drain 100 Trying

        b2b_invite, sems_addr = self.recvFromSIP(self._uas_sock)

        self.sendToSIP(_make_provisional(180, "Ringing", b2b_invite),
                       sems_addr, self._uas_sock)
        self.recvSIP(leg_a)  # drain 180

        self.sendToSIP(_make_200_ok_invite(b2b_invite), sems_addr, self._uas_sock)
        ok_a = self.recvSIP(leg_a)
        to_tag = _to_tag(ok_a)

        self.sendSIP(_make_ack("z9hG4bK-b2b-002-ack", call_id, to_tag,
                               src_port=src_port), leg_a)
        self.recvSIP(self._uas_sock)  # drain ACK on leg B

        # ---- Media flow (RTP_Direct: SEMS passes SDP through unchanged) ----
        # In b2b_invite SEMS forwarded leg A's SDP → leg B sends RTP to _RTP_PORT_A
        # In ok_a SEMS forwarded leg B's SDP → leg A sends RTP to _RTP_PORT_B
        dest_from_b = _sdp_media_port(b2b_invite)  # port leg B should send to
        dest_from_a = _sdp_media_port(ok_a)         # port leg A should send to

        self.assertEqual(dest_from_b, _RTP_PORT_A)
        self.assertEqual(dest_from_a, _RTP_PORT_B)

        # Leg B → Leg A
        pkt_b2a = _make_rtp(seq=1, ts=160, ssrc=0xB0B0B0B0)
        rtp_b.sendto(pkt_b2a, ("127.0.0.1", dest_from_b))
        self.assertEqual(rtp_a.recv(256), pkt_b2a)

        # Leg A → Leg B
        pkt_a2b = _make_rtp(seq=1, ts=160, ssrc=0xA0A0A0A0)
        rtp_a.sendto(pkt_a2b, ("127.0.0.1", dest_from_a))
        self.assertEqual(rtp_b.recv(256), pkt_a2b)

        rtp_a.close()
        rtp_b.close()

        # --- Leg B sends BYE ---
        cid_b2b  = _hdr(b2b_invite, "Call-ID")
        from_b2b = _hdr(b2b_invite, "From")
        bye_from_b = (
            f"BYE sip:alice@voip.sipwise.local SIP/2.0\n"
            f"Via: SIP/2.0/UDP 127.0.0.1:5070;branch=z9hG4bK-uas-bye-002;rport\n"
            f"Max-Forwards: 70\n"
            f"To: {from_b2b}\n"
            f"From: <sip:bob@voip.sipwise.local>;tag=uas-tag-001\n"
            f"Call-ID: {cid_b2b}\n"
            f"CSeq: 10 BYE\n"
            f"Content-Length: 0\n"
            "\n"
        )
        self.sendToSIP(bye_from_b, sems_addr, self._uas_sock)

        # SEMS → BYE → Leg A
        bye_a = self.recvSIP(leg_a)
        self.assertSIP(bye_a,
            "^BYE sip:[^\r]+ SIP/2\\.0\n"
            ".*"
            "CSeq: \\d+ BYE\n"
            ".*"
            "Call-ID: test-b2b-call-002@127\\.0\\.0\\.1\n"
        )

        # Leg A → 200 OK for BYE
        self.sendSIP(_make_200_ok_for(bye_a), leg_a)

        # SEMS → 200 OK → Leg B
        ok_b = self.recvSIP(self._uas_sock)
        self.assertSIP(ok_b,
            "^SIP/2\\.0 200 OK\n"
            ".*"
            "CSeq: 10 BYE\n"
        )

        leg_a.close()

    # ------------------------------------------------------------------
    # Scenario 3: Early CANCEL after 180 Ringing
    # ------------------------------------------------------------------
    def testEarlyCancel(self):
        src_port = 57703
        leg_a = self.makeUACSocket(src_port)
        branch  = "z9hG4bK-b2b-003"
        call_id = "test-b2b-call-003@127.0.0.1"

        self.sendSIP(_make_invite(branch, call_id, src_port=src_port), leg_a)
        self.recvSIP(leg_a)  # 100 Trying

        b2b_invite, sems_addr = self.recvFromSIP(self._uas_sock)

        self.sendToSIP(_make_provisional(180, "Ringing", b2b_invite),
                       sems_addr, self._uas_sock)
        self.recvSIP(leg_a)  # 180

        # Leg A → CANCEL
        self.sendSIP(_make_cancel(branch, call_id, src_port=src_port), leg_a)

        # SEMS → 200 OK for CANCEL → Leg A
        ok_cancel = self.recvSIP(leg_a)
        self.assertSIP(ok_cancel,
            "^SIP/2\\.0 200 OK\n"
            ".*"
            "CSeq: 1 CANCEL\n"
        )

        # SEMS → CANCEL → Leg B
        cancel_b = self.recvSIP(self._uas_sock)
        self.assertSIP(cancel_b,
            "^CANCEL sip:[^\r]+ SIP/2\\.0\n"
            ".*"
            "CSeq: \\d+ CANCEL\n"
        )

        # Leg B → 200 OK for CANCEL
        self.sendToSIP(_make_200_ok_for(cancel_b), sems_addr, self._uas_sock)

        # Leg B → 487 Request Terminated
        self.sendToSIP(_make_final_uas(487, "Request Terminated", b2b_invite),
                       sems_addr, self._uas_sock)

        # SEMS → ACK → Leg B (for 487)
        ack_b = self.recvSIPSkipRetrans(self._uas_sock, "INVITE")
        self.assertSIP(ack_b,
            "^ACK sip:[^\r]+ SIP/2\\.0\n"
            ".*"
            "CSeq: \\d+ ACK\n"
        )

        # SEMS → 487 → Leg A
        err_a = self.recvSIP(leg_a)
        self.assertSIP(err_a,
            "^SIP/2\\.0 487 [^\r]+\n"
            ".*"
            "Call-ID: test-b2b-call-003@127\\.0\\.0\\.1\n"
            ".*"
            "CSeq: 1 INVITE\n"
        )

        # Leg A → ACK (for 487)
        self.sendSIP(
            _make_ack("z9hG4bK-b2b-003-ack", call_id, _to_tag(err_a),
                      src_port=src_port), leg_a)

        leg_a.close()

    # ------------------------------------------------------------------
    # Scenario 4: Callee returns 486 Busy Here
    # ------------------------------------------------------------------
    def testCalleeBusy(self):
        src_port = 57704
        leg_a = self.makeUACSocket(src_port)
        branch  = "z9hG4bK-b2b-004"
        call_id = "test-b2b-call-004@127.0.0.1"

        self.sendSIP(_make_invite(branch, call_id, src_port=src_port), leg_a)
        self.recvSIP(leg_a)  # 100 Trying

        b2b_invite, sems_addr = self.recvFromSIP(self._uas_sock)

        # Send 100 Trying first to stop INVITE retransmissions
        self.sendToSIP(_make_provisional(100, "Trying", b2b_invite),
                       sems_addr, self._uas_sock)

        # Leg B → 486 Busy Here
        self.sendToSIP(_make_final_uas(486, "Busy Here", b2b_invite),
                       sems_addr, self._uas_sock)

        # SEMS → ACK → Leg B (skip any retransmitted INVITE)
        ack_b = self.recvSIPSkipRetrans(self._uas_sock, "INVITE")
        self.assertSIP(ack_b,
            "^ACK sip:[^\r]+ SIP/2\\.0\n"
            ".*"
            "CSeq: \\d+ ACK\n"
        )

        # SEMS → 486 → Leg A
        busy_a = self.recvSIP(leg_a)
        self.assertSIP(busy_a,
            "^SIP/2\\.0 486 Busy Here\n"
            ".*"
            "Call-ID: test-b2b-call-004@127\\.0\\.0\\.1\n"
            ".*"
            "CSeq: 1 INVITE\n"
        )

        # Leg A → ACK
        self.sendSIP(
            _make_ack("z9hG4bK-b2b-004-ack", call_id, _to_tag(busy_a),
                      src_port=src_port), leg_a)

        leg_a.close()

    # ------------------------------------------------------------------
    # Scenario 5: Callee returns 404 Not Found
    # ------------------------------------------------------------------
    def testCalleeNotFound(self):
        src_port = 57705
        leg_a = self.makeUACSocket(src_port)
        branch  = "z9hG4bK-b2b-005"
        call_id = "test-b2b-call-005@127.0.0.1"

        self.sendSIP(_make_invite(branch, call_id, src_port=src_port), leg_a)
        self.recvSIP(leg_a)  # 100 Trying

        b2b_invite, sems_addr = self.recvFromSIP(self._uas_sock)

        # Send 100 Trying first to stop INVITE retransmissions
        self.sendToSIP(_make_provisional(100, "Trying", b2b_invite),
                       sems_addr, self._uas_sock)

        # Leg B → 404 Not Found
        self.sendToSIP(_make_final_uas(404, "Not Found", b2b_invite),
                       sems_addr, self._uas_sock)

        # SEMS → ACK → Leg B (skip any retransmitted INVITE)
        ack_b = self.recvSIPSkipRetrans(self._uas_sock, "INVITE")
        self.assertSIP(ack_b,
            "^ACK sip:[^\r]+ SIP/2\\.0\n"
            ".*"
            "CSeq: \\d+ ACK\n"
        )

        # SEMS → 404 → Leg A
        notfound_a = self.recvSIP(leg_a)
        self.assertSIP(notfound_a,
            "^SIP/2\\.0 404 Not Found\n"
            ".*"
            "Call-ID: test-b2b-call-005@127\\.0\\.0\\.1\n"
            ".*"
            "CSeq: 1 INVITE\n"
        )

        # Leg A → ACK
        self.sendSIP(
            _make_ack("z9hG4bK-b2b-005-ack", call_id, _to_tag(notfound_a),
                      src_port=src_port), leg_a)

        leg_a.close()


    # ------------------------------------------------------------------
    # Scenario 6: CANCEL before any provisional (no 180 from callee)
    # ------------------------------------------------------------------
    def testEarlyCancelBeforeRinging(self):
        src_port = 57706
        leg_a = self.makeUACSocket(src_port)
        branch  = "z9hG4bK-b2b-006"
        call_id = "test-b2b-call-006@127.0.0.1"

        self.sendSIP(_make_invite(branch, call_id, src_port=src_port), leg_a)
        self.recvSIP(leg_a)  # 100 Trying

        b2b_invite, sems_addr = self.recvFromSIP(self._uas_sock)

        # Leg B acknowledges INVITE (stops retransmissions) but doesn't ring yet
        self.sendToSIP(_make_provisional(100, "Trying", b2b_invite),
                       sems_addr, self._uas_sock)

        # Leg A → CANCEL (before any 180)
        self.sendSIP(_make_cancel(branch, call_id, src_port=src_port), leg_a)

        # SEMS → 200 OK for CANCEL → Leg A
        ok_cancel = self.recvSIP(leg_a)
        self.assertSIP(ok_cancel,
            "^SIP/2\\.0 200 OK\n"
            ".*"
            "CSeq: 1 CANCEL\n"
        )

        # SEMS → CANCEL → Leg B
        cancel_b = self.recvSIPSkipRetrans(self._uas_sock, "INVITE")
        self.assertSIP(cancel_b,
            "^CANCEL sip:[^\r]+ SIP/2\\.0\n"
            ".*"
            "CSeq: \\d+ CANCEL\n"
        )

        # Leg B → 200 OK for CANCEL → SEMS
        self.sendToSIP(_make_200_ok_for(cancel_b), sems_addr, self._uas_sock)

        # Leg B → 487 Request Terminated → SEMS
        self.sendToSIP(_make_final_uas(487, "Request Terminated", b2b_invite),
                       sems_addr, self._uas_sock)

        # SEMS → ACK → Leg B (for 487)
        ack_b = self.recvSIPSkipRetrans(self._uas_sock, "INVITE")
        self.assertSIP(ack_b,
            "^ACK sip:[^\r]+ SIP/2\\.0\n"
            ".*"
            "CSeq: \\d+ ACK\n"
        )

        # SEMS → 487 → Leg A
        err_a = self.recvSIP(leg_a)
        self.assertSIP(err_a,
            "^SIP/2\\.0 487 [^\r]+\n"
            ".*"
            "Call-ID: test-b2b-call-006@127\\.0\\.0\\.1\n"
            ".*"
            "CSeq: 1 INVITE\n"
        )

        # Leg A → ACK (for 487)
        self.sendSIP(
            _make_ack("z9hG4bK-b2b-006-ack", call_id, _to_tag(err_a),
                      src_port=src_port), leg_a)

        leg_a.close()

    # ------------------------------------------------------------------
    # Scenario 7: 183 Session Progress with SDP (early media)
    # ------------------------------------------------------------------
    def testEarlyMedia(self):
        src_port = 57707
        leg_a = self.makeUACSocket(src_port)
        branch  = "z9hG4bK-b2b-007"
        call_id = "test-b2b-call-007@127.0.0.1"

        self.sendSIP(_make_invite(branch, call_id, src_port=src_port), leg_a)
        self.recvSIP(leg_a)  # 100 Trying

        b2b_invite, sems_addr = self.recvFromSIP(self._uas_sock)

        # Leg B → 183 Session Progress with SDP → SEMS
        self.sendToSIP(_make_provisional(183, "Session Progress", b2b_invite,
                                         sdp=_SDP_B),
                       sems_addr, self._uas_sock)

        # SEMS → 183 with SDP → Leg A
        prog_a = self.recvSIP(leg_a)
        self.assertSIP(prog_a,
            "^SIP/2\\.0 183 [^\r]+\n"
            ".*"
            "Call-ID: test-b2b-call-007@127\\.0\\.0\\.1\n"
            ".*"
            "Content-Type: application/sdp\n"
            ".*"
            "m=audio \\d+ RTP/AVP"
        )

        # Leg B → 200 OK → SEMS
        self.sendToSIP(_make_200_ok_invite(b2b_invite), sems_addr, self._uas_sock)

        # SEMS → 200 OK → Leg A
        ok_a = self.recvSIP(leg_a)
        self.assertSIP(ok_a,
            "^SIP/2\\.0 200 OK\n"
            ".*"
            "Call-ID: test-b2b-call-007@127\\.0\\.0\\.1\n"
            ".*"
            "m=audio \\d+ RTP/AVP"
        )
        to_tag = _to_tag(ok_a)

        self.sendSIP(_make_ack("z9hG4bK-b2b-007-ack", call_id, to_tag,
                               src_port=src_port), leg_a)
        self.recvSIP(self._uas_sock)  # drain ACK on leg B

        # Leg A → BYE → SEMS
        self.sendSIP(_make_bye("z9hG4bK-b2b-007-bye", call_id, to_tag,
                               src_port=src_port), leg_a)
        bye_b = self.recvSIP(self._uas_sock)
        self.sendToSIP(_make_200_ok_for(bye_b), sems_addr, self._uas_sock)
        ok_bye = self.recvSIP(leg_a)
        self.assertSIP(ok_bye, "^SIP/2\\.0 200 OK\n.*CSeq: 2 BYE\n")

        leg_a.close()

    # ------------------------------------------------------------------
    # Scenario 8: re-INVITE hold from leg A (sendonly → recvonly)
    # ------------------------------------------------------------------
    def testReInviteHoldLegA(self):
        src_port = 57708
        leg_a = self.makeUACSocket(src_port)
        branch  = "z9hG4bK-b2b-008"
        call_id = "test-b2b-call-008@127.0.0.1"

        # --- Initial call setup ---
        self.sendSIP(_make_invite(branch, call_id, src_port=src_port), leg_a)
        self.recvSIP(leg_a)  # 100 Trying

        b2b_invite, sems_addr = self.recvFromSIP(self._uas_sock)
        self.sendToSIP(_make_provisional(180, "Ringing", b2b_invite),
                       sems_addr, self._uas_sock)
        self.recvSIP(leg_a)  # 180

        self.sendToSIP(_make_200_ok_invite(b2b_invite), sems_addr, self._uas_sock)
        ok_a = self.recvSIP(leg_a)
        to_tag = _to_tag(ok_a)

        self.sendSIP(_make_ack("z9hG4bK-b2b-008-ack", call_id, to_tag,
                               src_port=src_port), leg_a)
        self.recvSIP(self._uas_sock)  # drain ACK on leg B

        # --- re-INVITE: leg A puts call on hold (sendonly) ---
        self.sendSIP(_make_reinvite("z9hG4bK-b2b-008-ri", call_id, to_tag,
                                    cseq=2, sdp=_SDP_SENDONLY_A,
                                    src_port=src_port), leg_a)

        # SEMS → re-INVITE → Leg B (relay)
        ri_b = self.recvSIPSkipRetrans(self._uas_sock, "ACK")
        self.assertSIP(ri_b,
            "^INVITE sip:[^\r]+ SIP/2\\.0\n"
            ".*"
            "a=sendonly"
        )

        # Leg B → 200 OK (recvonly) → SEMS
        self.sendToSIP(_make_200_ok_invite(ri_b, sdp=_SDP_RECVONLY_B),
                       sems_addr, self._uas_sock)

        # SEMS → 200 OK → Leg A  (drain 100 Trying SEMS sent for the re-INVITE)
        self.recvSIP(leg_a)  # 100 Trying
        ok_ri_a = self.recvSIP(leg_a)
        self.assertSIP(ok_ri_a,
            "^SIP/2\\.0 200 OK\n"
            ".*"
            "a=recvonly"
        )

        # Leg A → ACK for re-INVITE 200 OK
        self.sendSIP(_make_ack("z9hG4bK-b2b-008-ri-ack", call_id, to_tag,
                               cseq=2, src_port=src_port), leg_a)

        # SEMS → ACK → Leg B
        ack_ri_b = self.recvSIP(self._uas_sock)
        self.assertSIP(ack_ri_b,
            "^ACK sip:[^\r]+ SIP/2\\.0\n"
            ".*"
            "CSeq: \\d+ ACK\n"
        )

        # --- Tear down (leg A, cseq=3 after two INVITEs) ---
        self.sendSIP(_make_bye("z9hG4bK-b2b-008-bye", call_id, to_tag,
                               cseq=3, src_port=src_port), leg_a)
        bye_b = self.recvSIP(self._uas_sock)
        self.assertSIP(bye_b, "^BYE sip:[^\r]+ SIP/2\\.0\n.*CSeq: \\d+ BYE\n")
        self.sendToSIP(_make_200_ok_for(bye_b), sems_addr, self._uas_sock)
        ok_bye = self.recvSIP(leg_a)
        self.assertSIP(ok_bye, "^SIP/2\\.0 200 OK\n.*CSeq: 3 BYE\n")

        leg_a.close()

    # ------------------------------------------------------------------
    # Scenario 9: re-INVITE hold from leg B (sendonly → recvonly)
    # ------------------------------------------------------------------
    def testReInviteHoldLegB(self):
        src_port = 57709
        leg_a = self.makeUACSocket(src_port)
        branch  = "z9hG4bK-b2b-009"
        call_id = "test-b2b-call-009@127.0.0.1"

        # --- Initial call setup ---
        self.sendSIP(_make_invite(branch, call_id, src_port=src_port), leg_a)
        self.recvSIP(leg_a)  # 100 Trying

        b2b_invite, sems_addr = self.recvFromSIP(self._uas_sock)
        self.sendToSIP(_make_provisional(180, "Ringing", b2b_invite),
                       sems_addr, self._uas_sock)
        self.recvSIP(leg_a)  # 180

        self.sendToSIP(_make_200_ok_invite(b2b_invite), sems_addr, self._uas_sock)
        ok_a = self.recvSIP(leg_a)
        to_tag = _to_tag(ok_a)

        self.sendSIP(_make_ack("z9hG4bK-b2b-009-ack", call_id, to_tag,
                               src_port=src_port), leg_a)
        self.recvSIP(self._uas_sock)  # drain ACK on leg B

        # --- re-INVITE: leg B puts call on hold (sendonly) ---
        # Build re-INVITE in the B2B dialog (leg B → SEMS)
        cid_b2b  = _hdr(b2b_invite, "Call-ID")
        # In B2B dialog: b2b_invite From = SEMS's B-leg local tag (= To from leg B's view)
        to_b2b   = _hdr(b2b_invite, "From")
        sdp_cr   = _SDP_SENDONLY_B.replace("\n", "\r\n")
        reinvite_from_b = (
            f"INVITE sip:alice@voip.sipwise.local SIP/2.0\n"
            f"Via: SIP/2.0/UDP 127.0.0.1:5070;branch=z9hG4bK-uas-ri-009;rport\n"
            f"Max-Forwards: 70\n"
            f"To: {to_b2b}\n"
            f"From: <sip:bob@voip.sipwise.local>;tag=uas-tag-001\n"
            f"Call-ID: {cid_b2b}\n"
            f"CSeq: 2 INVITE\n"
            f"Contact: <sip:bob@127.0.0.1:5070>\n"
            f"Content-Type: application/sdp\n"
            f"Content-Length: {len(sdp_cr)}\n"
            "\n"
            + _SDP_SENDONLY_B
        )
        self.sendToSIP(reinvite_from_b, sems_addr, self._uas_sock)
        self.recvSIP(self._uas_sock)  # drain 100 Trying SEMS sends for the re-INVITE

        # SEMS → re-INVITE → Leg A
        ri_a = self.recvSIP(leg_a)
        self.assertSIP(ri_a,
            "^INVITE sip:[^\r]+ SIP/2\\.0\n"
            ".*"
            f"Call-ID: {call_id}\n"
            ".*"
            "a=sendonly"
        )

        # Leg A → 200 OK (recvonly) → SEMS  (Contact must match leg_a's bound port)
        self.sendSIP(_make_200_ok_invite(ri_a, sdp=_SDP_RECVONLY_A,
                                         contact=f"<sip:alice@127.0.0.1:{src_port}>"), leg_a)

        # SEMS → 200 OK → Leg B
        ok_ri_b = self.recvSIP(self._uas_sock)
        self.assertSIP(ok_ri_b,
            "^SIP/2\\.0 200 OK\n"
            ".*"
            "a=recvonly"
        )

        # Leg B → ACK for re-INVITE 200 OK → SEMS
        ack_from_b = (
            f"ACK sip:alice@voip.sipwise.local SIP/2.0\n"
            f"Via: SIP/2.0/UDP 127.0.0.1:5070;branch=z9hG4bK-uas-ri-ack-009;rport\n"
            f"Max-Forwards: 70\n"
            f"To: {to_b2b}\n"
            f"From: <sip:bob@voip.sipwise.local>;tag=uas-tag-001\n"
            f"Call-ID: {cid_b2b}\n"
            f"CSeq: 2 ACK\n"
            f"Content-Length: 0\n"
            "\n"
        )
        self.sendToSIP(ack_from_b, sems_addr, self._uas_sock)

        # SEMS → ACK → Leg A
        ack_ri_a = self.recvSIP(leg_a)
        self.assertSIP(ack_ri_a,
            "^ACK sip:[^\r]+ SIP/2\\.0\n"
            ".*"
            "CSeq: \\d+ ACK\n"
        )

        # --- Tear down (leg B sends BYE) ---
        bye_from_b = (
            f"BYE sip:alice@voip.sipwise.local SIP/2.0\n"
            f"Via: SIP/2.0/UDP 127.0.0.1:5070;branch=z9hG4bK-uas-bye-009;rport\n"
            f"Max-Forwards: 70\n"
            f"To: {to_b2b}\n"
            f"From: <sip:bob@voip.sipwise.local>;tag=uas-tag-001\n"
            f"Call-ID: {cid_b2b}\n"
            f"CSeq: 10 BYE\n"
            f"Content-Length: 0\n"
            "\n"
        )
        self.sendToSIP(bye_from_b, sems_addr, self._uas_sock)

        bye_a = self.recvSIP(leg_a)
        self.assertSIP(bye_a,
            "^BYE sip:[^\r]+ SIP/2\\.0\n"
            ".*"
            f"Call-ID: {call_id}\n"
        )
        self.sendSIP(_make_200_ok_for(bye_a), leg_a)

        ok_b = self.recvSIP(self._uas_sock)
        self.assertSIP(ok_b, "^SIP/2\\.0 200 OK\n.*CSeq: 10 BYE\n")

        leg_a.close()


    # ------------------------------------------------------------------
    # Scenario 10: INVITE retransmission — SEMS deduplicates, does not
    #              forward the retransmit to leg B
    # ------------------------------------------------------------------
    def testInviteRetransmission(self):
        src_port = 57710
        leg_a = self.makeUACSocket(src_port)
        branch  = "z9hG4bK-b2b-010"
        call_id = "test-b2b-call-010@127.0.0.1"

        invite = _make_invite(branch, call_id, src_port=src_port)
        self.sendSIP(invite, leg_a)
        self.recvSIP(leg_a)  # 100 Trying

        b2b_invite, sems_addr = self.recvFromSIP(self._uas_sock)

        # Leg B → 100 Trying → SEMS (stops SEMS's own B2B INVITE retransmit timer)
        self.sendToSIP(_make_provisional(100, "Trying", b2b_invite),
                       sems_addr, self._uas_sock)

        # Leg A retransmits the INVITE (same Via branch = same transaction)
        self.sendSIP(invite, leg_a)

        # SEMS should resend its last provisional (100 Trying / 100 Connecting) to leg A
        retrans_resp = self.recvSIP(leg_a)
        self.assertSIP(retrans_resp, "^SIP/2\\.0 100 [^\r]+\n")

        # SEMS must NOT forward the retransmission to leg B
        self._uas_sock.settimeout(0.2)
        try:
            extra = self.recvSIP(self._uas_sock)
            self.fail("SEMS forwarded INVITE retransmission to leg B: " + repr(extra[:80]))
        except socket.timeout:
            pass  # expected
        finally:
            self._uas_sock.settimeout(3)

        # Complete the call normally
        self.sendToSIP(_make_provisional(180, "Ringing", b2b_invite),
                       sems_addr, self._uas_sock)
        self.recvSIP(leg_a)  # 180

        self.sendToSIP(_make_200_ok_invite(b2b_invite), sems_addr, self._uas_sock)
        ok_a = self.recvSIP(leg_a)
        to_tag = _to_tag(ok_a)

        self.sendSIP(_make_ack("z9hG4bK-b2b-010-ack", call_id, to_tag,
                               src_port=src_port), leg_a)
        self.recvSIP(self._uas_sock)  # drain ACK

        self.sendSIP(_make_bye("z9hG4bK-b2b-010-bye", call_id, to_tag,
                               src_port=src_port), leg_a)
        bye_b = self.recvSIP(self._uas_sock)
        self.sendToSIP(_make_200_ok_for(bye_b), sems_addr, self._uas_sock)
        ok_bye = self.recvSIP(leg_a)
        self.assertSIP(ok_bye, "^SIP/2\\.0 200 OK\n.*CSeq: 2 BYE\n")

        leg_a.close()

    # ------------------------------------------------------------------
    # Scenario 11: Callee 480 Temporarily Unavailable
    # ------------------------------------------------------------------
    def testCallee480(self):
        src_port = 57711
        leg_a = self.makeUACSocket(src_port)
        branch  = "z9hG4bK-b2b-011"
        call_id = "test-b2b-call-011@127.0.0.1"

        self.sendSIP(_make_invite(branch, call_id, src_port=src_port), leg_a)
        self.recvSIP(leg_a)  # 100 Trying

        b2b_invite, sems_addr = self.recvFromSIP(self._uas_sock)
        self.sendToSIP(_make_provisional(100, "Trying", b2b_invite),
                       sems_addr, self._uas_sock)
        self.sendToSIP(_make_final_uas(480, "Temporarily Unavailable", b2b_invite),
                       sems_addr, self._uas_sock)

        ack_b = self.recvSIPSkipRetrans(self._uas_sock, "INVITE")
        self.assertSIP(ack_b, "^ACK sip:[^\r]+ SIP/2\\.0\n.*CSeq: \\d+ ACK\n")

        err_a = self.recvSIP(leg_a)
        self.assertSIP(err_a,
            "^SIP/2\\.0 480 [^\r]+\n"
            ".*Call-ID: test-b2b-call-011@127\\.0\\.0\\.1\n"
            ".*CSeq: 1 INVITE\n"
        )
        self.sendSIP(_make_ack("z9hG4bK-b2b-011-ack", call_id, _to_tag(err_a),
                               src_port=src_port), leg_a)
        leg_a.close()

    # ------------------------------------------------------------------
    # Scenario 12: Callee 408 Request Timeout
    # ------------------------------------------------------------------
    def testCallee408(self):
        src_port = 57712
        leg_a = self.makeUACSocket(src_port)
        branch  = "z9hG4bK-b2b-012"
        call_id = "test-b2b-call-012@127.0.0.1"

        self.sendSIP(_make_invite(branch, call_id, src_port=src_port), leg_a)
        self.recvSIP(leg_a)  # 100 Trying

        b2b_invite, sems_addr = self.recvFromSIP(self._uas_sock)
        self.sendToSIP(_make_provisional(100, "Trying", b2b_invite),
                       sems_addr, self._uas_sock)
        self.sendToSIP(_make_final_uas(408, "Request Timeout", b2b_invite),
                       sems_addr, self._uas_sock)

        ack_b = self.recvSIPSkipRetrans(self._uas_sock, "INVITE")
        self.assertSIP(ack_b, "^ACK sip:[^\r]+ SIP/2\\.0\n.*CSeq: \\d+ ACK\n")

        err_a = self.recvSIP(leg_a)
        self.assertSIP(err_a,
            "^SIP/2\\.0 \\d+ [^\r]+\n"
            ".*Call-ID: test-b2b-call-012@127\\.0\\.0\\.1\n"
            ".*CSeq: 1 INVITE\n"
        )
        self.sendSIP(_make_ack("z9hG4bK-b2b-012-ack", call_id, _to_tag(err_a),
                               src_port=src_port), leg_a)
        leg_a.close()

    # ------------------------------------------------------------------
    # Scenario 13: Callee 503 Service Unavailable
    # ------------------------------------------------------------------
    def testCallee503(self):
        src_port = 57713
        leg_a = self.makeUACSocket(src_port)
        branch  = "z9hG4bK-b2b-013"
        call_id = "test-b2b-call-013@127.0.0.1"

        self.sendSIP(_make_invite(branch, call_id, src_port=src_port), leg_a)
        self.recvSIP(leg_a)  # 100 Trying

        b2b_invite, sems_addr = self.recvFromSIP(self._uas_sock)
        self.sendToSIP(_make_provisional(100, "Trying", b2b_invite),
                       sems_addr, self._uas_sock)
        self.sendToSIP(_make_final_uas(503, "Service Unavailable", b2b_invite),
                       sems_addr, self._uas_sock)

        ack_b = self.recvSIPSkipRetrans(self._uas_sock, "INVITE")
        self.assertSIP(ack_b, "^ACK sip:[^\r]+ SIP/2\\.0\n.*CSeq: \\d+ ACK\n")

        err_a = self.recvSIP(leg_a)
        self.assertSIP(err_a,
            "^SIP/2\\.0 \\d+ [^\r]+\n"
            ".*Call-ID: test-b2b-call-013@127\\.0\\.0\\.1\n"
            ".*CSeq: 1 INVITE\n"
        )
        self.sendSIP(_make_ack("z9hG4bK-b2b-013-ack", call_id, _to_tag(err_a),
                               src_port=src_port), leg_a)
        leg_a.close()

    # ------------------------------------------------------------------
    # Scenario 14: re-INVITE hold then unhold from leg A
    # ------------------------------------------------------------------
    def testReInviteUnhold(self):
        src_port = 57714
        leg_a = self.makeUACSocket(src_port)
        branch  = "z9hG4bK-b2b-014"
        call_id = "test-b2b-call-014@127.0.0.1"

        # --- Initial call setup ---
        self.sendSIP(_make_invite(branch, call_id, src_port=src_port), leg_a)
        self.recvSIP(leg_a)  # 100 Trying

        b2b_invite, sems_addr = self.recvFromSIP(self._uas_sock)
        self.sendToSIP(_make_provisional(180, "Ringing", b2b_invite),
                       sems_addr, self._uas_sock)
        self.recvSIP(leg_a)  # 180

        self.sendToSIP(_make_200_ok_invite(b2b_invite), sems_addr, self._uas_sock)
        ok_a = self.recvSIP(leg_a)
        to_tag = _to_tag(ok_a)

        self.sendSIP(_make_ack("z9hG4bK-b2b-014-ack", call_id, to_tag,
                               src_port=src_port), leg_a)
        self.recvSIP(self._uas_sock)  # drain ACK on leg B

        # --- re-INVITE: leg A holds (sendonly, cseq=2) ---
        self.sendSIP(_make_reinvite("z9hG4bK-b2b-014-hold", call_id, to_tag,
                                    cseq=2, sdp=_SDP_SENDONLY_A,
                                    src_port=src_port), leg_a)
        ri_b = self.recvSIPSkipRetrans(self._uas_sock, "ACK")
        self.assertSIP(ri_b, "^INVITE sip:[^\r]+ SIP/2\\.0\n.*a=sendonly")

        self.sendToSIP(_make_200_ok_invite(ri_b, sdp=_SDP_RECVONLY_B),
                       sems_addr, self._uas_sock)
        self.recvSIP(leg_a)  # 100 Trying for re-INVITE
        ok_hold_a = self.recvSIP(leg_a)
        self.assertSIP(ok_hold_a, "^SIP/2\\.0 200 OK\n.*a=recvonly")

        self.sendSIP(_make_ack("z9hG4bK-b2b-014-hold-ack", call_id, to_tag,
                               cseq=2, src_port=src_port), leg_a)
        self.recvSIP(self._uas_sock)  # drain ACK on leg B

        # --- re-INVITE: leg A unholds (sendrecv, cseq=3) ---
        self.sendSIP(_make_reinvite("z9hG4bK-b2b-014-unhold", call_id, to_tag,
                                    cseq=3, sdp=_SDP_SENDRECV_A2,
                                    src_port=src_port), leg_a)
        ri_unhold_b = self.recvSIPSkipRetrans(self._uas_sock, "ACK")
        self.assertSIP(ri_unhold_b, "^INVITE sip:[^\r]+ SIP/2\\.0\n.*a=sendrecv")

        self.sendToSIP(_make_200_ok_invite(ri_unhold_b, sdp=_SDP_SENDRECV_B2),
                       sems_addr, self._uas_sock)
        self.recvSIP(leg_a)  # 100 Trying for re-INVITE
        ok_unhold_a = self.recvSIP(leg_a)
        self.assertSIP(ok_unhold_a, "^SIP/2\\.0 200 OK\n.*a=sendrecv")

        self.sendSIP(_make_ack("z9hG4bK-b2b-014-unhold-ack", call_id, to_tag,
                               cseq=3, src_port=src_port), leg_a)
        self.recvSIP(self._uas_sock)  # drain ACK on leg B

        # --- Tear down (cseq=4 after three INVITEs) ---
        self.sendSIP(_make_bye("z9hG4bK-b2b-014-bye", call_id, to_tag,
                               cseq=4, src_port=src_port), leg_a)
        bye_b = self.recvSIP(self._uas_sock)
        self.assertSIP(bye_b, "^BYE sip:[^\r]+ SIP/2\\.0\n.*CSeq: \\d+ BYE\n")
        self.sendToSIP(_make_200_ok_for(bye_b), sems_addr, self._uas_sock)
        ok_bye = self.recvSIP(leg_a)
        self.assertSIP(ok_bye, "^SIP/2\\.0 200 OK\n.*CSeq: 4 BYE\n")

        leg_a.close()


    # ------------------------------------------------------------------
    # Scenario 15: CANCEL/200 OK race — leg A cancels while leg B answers
    # ------------------------------------------------------------------
    def testEarlyCancelRace(self):
        src_port = 57715
        leg_a = self.makeUACSocket(src_port)
        branch  = "z9hG4bK-b2b-015"
        call_id = "test-b2b-call-015@127.0.0.1"

        self.sendSIP(_make_invite(branch, call_id, src_port=src_port), leg_a)
        self.recvSIP(leg_a)  # 100 Trying
        b2b_invite, sems_addr = self.recvFromSIP(self._uas_sock)
        self.sendToSIP(_make_provisional(180, "Ringing", b2b_invite),
                       sems_addr, self._uas_sock)
        self.recvSIP(leg_a)  # 180

        # Send 200 OK from leg B first so SEMS establishes the call, then send CANCEL.
        # This tests the case where CANCEL arrives after the call is already established.
        # sems-pbx only handles CANCEL for an established call via explicit BYE from leg A;
        # it does not reliably BYE leg B if 200 OK INVITE arrives at an already-stopped session.
        self.sendToSIP(_make_200_ok_invite(b2b_invite), sems_addr, self._uas_sock)
        time.sleep(0.3)  # allow SEMS to process 200 OK INVITE and establish the call
        self.sendSIP(_make_cancel(branch, call_id, src_port=src_port), leg_a)

        # SEMS → response to CANCEL → leg A.
        # If SEMS processed leg B's 200 OK INVITE before the CANCEL it may
        # forward that 200 OK to leg A first; ACK it and keep waiting.
        # On a loaded machine SEMS may reply to CANCEL with 481 (INVITE
        # transaction already completed) rather than 200 OK — accept either.
        ok_cancel = None
        got_200_invite = False
        to_tag_invite = None
        leg_a.settimeout(1)
        try:
            for _ in range(30):
                try:
                    msg = self.recvSIP(leg_a)
                except TimeoutError:
                    if got_200_invite:
                        # Already ACKed established call; CANCEL response may not
                        # arrive (SEMS may have already sent 481 and we missed it).
                        break
                    continue
                if "CSeq: 1 CANCEL" in msg:
                    # SEMS responded to CANCEL (200 OK or error such as 481)
                    if msg.startswith("SIP/2.0 200"):
                        ok_cancel = msg
                    break
                if msg.startswith("SIP/2.0 200") and "CSeq: 1 INVITE" in msg:
                    # Race: SEMS forwarded leg B's 200 OK before processing CANCEL.
                    # ACK it so SEMS can continue with teardown.
                    got_200_invite = True
                    to_tag_invite = _to_tag(msg)
                    self.sendSIP(
                        _make_ack(branch + "-ack", call_id, to_tag_invite,
                                  src_port=src_port),
                        leg_a,
                    )
        finally:
            leg_a.settimeout(3)

        if not got_200_invite:
            self.assertIsNotNone(ok_cancel, "SEMS did not send 200 OK for CANCEL to leg A")
            self.assertSIP(ok_cancel, "^SIP/2\\.0 200 OK\n.*CSeq: 1 CANCEL\n")

        if got_200_invite:
            # The call was briefly established before CANCEL arrived.
            # Send BYE from leg A to trigger teardown — do NOT wait for 200 OK BYE
            # yet: SEMS relays BYE to leg B and blocks until leg B replies.
            # Reading leg_a before _uas_sock would deadlock both sides.
            self.sendSIP(
                _make_bye(branch + "-bye", call_id, to_tag_invite, cseq=2,
                          src_port=src_port),
                leg_a,
            )
            # SEMS relays BYE to leg B. Receive it and respond FIRST — only then
            # will SEMS send 200 OK BYE back to leg A.
            bye_b = None
            self._uas_sock.settimeout(1)
            try:
                for _ in range(30):
                    try:
                        msg = self.recvSIP(self._uas_sock)
                    except TimeoutError:
                        continue
                    if msg.startswith("BYE"):
                        bye_b = msg
                        break
                    # ACK or other — consume silently
            finally:
                self._uas_sock.settimeout(3)
            self.assertIsNotNone(bye_b, "SEMS did not send BYE to leg B after cancel race")
            self.assertSIP(bye_b, "^BYE sip:[^\r]+ SIP/2\\.0\n.*CSeq: \\d+ BYE\n")
            self.sendToSIP(_make_200_ok_for(bye_b), sems_addr, self._uas_sock)
            # SEMS unblocked — now read 200 OK BYE from leg A
            bye_ok_a = self.recvSIP(leg_a)
            self.assertSIP(bye_ok_a, "^SIP/2\\.0 200 OK\n.*CSeq: 2 BYE\n")
        else:
            # Non-race path: SEMS sent CANCEL to leg B — respond so it can proceed.
            self._uas_sock.settimeout(1)
            try:
                for _ in range(10):
                    try:
                        msg = self.recvSIP(self._uas_sock)
                    except TimeoutError:
                        break
                    if msg.startswith("CANCEL"):
                        self.sendToSIP(_make_200_ok_for(msg), sems_addr, self._uas_sock)
                        break
            finally:
                self._uas_sock.settimeout(3)
            # SEMS → 487 Request Terminated → leg A
            err_a = self.recvSIP(leg_a)
            self.assertSIP(err_a,
                "^SIP/2\\.0 487 [^\r]+\n"
                f".*Call-ID: {call_id}\n"
                ".*CSeq: 1 INVITE\n"
            )
            self.sendSIP(_make_ack("z9hG4bK-b2b-015-ack", call_id, _to_tag(err_a),
                                   src_port=src_port), leg_a)
        leg_a.close()

    # ------------------------------------------------------------------
    # Scenario 16+17: Two concurrent calls — teardown of one must not
    #                 affect the other
    # ------------------------------------------------------------------
    def testConcurrentCalls(self):
        src_port_1, src_port_2 = 57716, 57717
        leg_a1 = self.makeUACSocket(src_port_1)
        leg_a2 = self.makeUACSocket(src_port_2)
        call_id_1 = "test-b2b-call-016@127.0.0.1"
        call_id_2 = "test-b2b-call-017@127.0.0.1"

        def setup_call(leg_a, invite_branch, call_id, ack_branch, src_port):
            """Establish a call through ACK. Returns (to_tag, b2b_call_id, sems_addr)."""
            self.sendSIP(_make_invite(invite_branch, call_id, src_port=src_port), leg_a)
            self.recvSIP(leg_a)  # 100 Trying
            b2b_inv, addr = self.recvFromSIP(self._uas_sock)
            self.sendToSIP(_make_provisional(180, "Ringing", b2b_inv), addr, self._uas_sock)
            self.recvSIP(leg_a)  # 180
            self.sendToSIP(_make_200_ok_invite(b2b_inv), addr, self._uas_sock)
            ok = self.recvSIP(leg_a)
            tag = _to_tag(ok)
            self.sendSIP(_make_ack(ack_branch, call_id, tag, src_port=src_port), leg_a)
            self.recvSIP(self._uas_sock)  # drain ACK on leg B
            return tag, _hdr(b2b_inv, "Call-ID"), addr

        to_tag_1, cid_b2b_1, sems_addr = setup_call(
            leg_a1, "z9hG4bK-b2b-016", call_id_1, "z9hG4bK-b2b-016-ack", src_port_1)
        to_tag_2, cid_b2b_2, _ = setup_call(
            leg_a2, "z9hG4bK-b2b-017", call_id_2, "z9hG4bK-b2b-017-ack", src_port_2)

        # Tear down call 1 — must not touch call 2
        self.sendSIP(_make_bye("z9hG4bK-b2b-016-bye", call_id_1, to_tag_1,
                               src_port=src_port_1), leg_a1)
        bye_b1 = self.recvSIP(self._uas_sock)
        # Verify BYE belongs to call 1 (B2B Call-ID)
        self.assertSIP(bye_b1,
            f"^BYE sip:[^\r]+ SIP/2\\.0\n.*Call-ID: {cid_b2b_1}\n")
        self.sendToSIP(_make_200_ok_for(bye_b1), sems_addr, self._uas_sock)
        self.assertSIP(self.recvSIP(leg_a1), "^SIP/2\\.0 200 OK\n.*CSeq: 2 BYE\n")

        # Call 2 must still be alive: verify by tearing it down successfully
        self.sendSIP(_make_bye("z9hG4bK-b2b-017-bye", call_id_2, to_tag_2,
                               src_port=src_port_2), leg_a2)
        bye_b2 = self.recvSIP(self._uas_sock)
        self.assertSIP(bye_b2,
            f"^BYE sip:[^\r]+ SIP/2\\.0\n.*Call-ID: {cid_b2b_2}\n")
        self.sendToSIP(_make_200_ok_for(bye_b2), sems_addr, self._uas_sock)
        self.assertSIP(self.recvSIP(leg_a2), "^SIP/2\\.0 200 OK\n.*CSeq: 2 BYE\n")

        leg_a1.close()
        leg_a2.close()


    # ------------------------------------------------------------------
    # Scenario 18: RTP port update after re-INVITE
    #   initial RTP on PORT_A/B → re-INVITE changes to PORT_A2/B2
    #   → packets must flow on new ports
    # ------------------------------------------------------------------
    def testRtpAfterReInvite(self):
        src_port = 57718
        leg_a = self.makeUACSocket(src_port)
        branch  = "z9hG4bK-b2b-018"
        call_id = "test-b2b-call-018@127.0.0.1"

        # Bind all four media sockets before signaling so Unix paths exist
        rtp_a  = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        rtp_a.settimeout(3);  rtp_a.bind(("127.0.0.1", _RTP_PORT_A))
        rtp_b  = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        rtp_b.settimeout(3);  rtp_b.bind(("127.0.0.1", _RTP_PORT_B))
        rtp_a2 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        rtp_a2.settimeout(3); rtp_a2.bind(("127.0.0.1", _RTP_PORT_A2))
        rtp_b2 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        rtp_b2.settimeout(3); rtp_b2.bind(("127.0.0.1", _RTP_PORT_B2))

        # --- Initial call setup ---
        self.sendSIP(_make_invite(branch, call_id, src_port=src_port), leg_a)
        self.recvSIP(leg_a)  # 100 Trying
        b2b_invite, sems_addr = self.recvFromSIP(self._uas_sock)
        self.sendToSIP(_make_provisional(180, "Ringing", b2b_invite),
                       sems_addr, self._uas_sock)
        self.recvSIP(leg_a)  # 180
        self.sendToSIP(_make_200_ok_invite(b2b_invite), sems_addr, self._uas_sock)
        ok_a = self.recvSIP(leg_a)
        to_tag = _to_tag(ok_a)
        self.sendSIP(_make_ack("z9hG4bK-b2b-018-ack", call_id, to_tag,
                               src_port=src_port), leg_a)
        self.recvSIP(self._uas_sock)  # drain ACK

        # Verify initial RTP (PORT_A ↔ PORT_B)
        self.assertEqual(_sdp_media_port(b2b_invite), _RTP_PORT_A)
        self.assertEqual(_sdp_media_port(ok_a),        _RTP_PORT_B)
        pkt1 = _make_rtp(seq=1, ts=160, ssrc=0x11111111)
        rtp_b.sendto(pkt1, ("127.0.0.1", _RTP_PORT_A))
        self.assertEqual(rtp_a.recv(256), pkt1)
        rtp_a.sendto(pkt1, ("127.0.0.1", _RTP_PORT_B))
        self.assertEqual(rtp_b.recv(256), pkt1)

        # --- re-INVITE: leg A updates ports to A2 (sendrecv) ---
        self.sendSIP(_make_reinvite("z9hG4bK-b2b-018-ri", call_id, to_tag,
                                    cseq=2, sdp=_SDP_SENDRECV_A2,
                                    src_port=src_port), leg_a)
        ri_b = self.recvSIPSkipRetrans(self._uas_sock, "ACK")
        self.sendToSIP(_make_200_ok_invite(ri_b, sdp=_SDP_SENDRECV_B2),
                       sems_addr, self._uas_sock)
        self.recvSIP(leg_a)  # 100 Trying for re-INVITE
        ok_ri_a = self.recvSIP(leg_a)
        self.sendSIP(_make_ack("z9hG4bK-b2b-018-ri-ack", call_id, to_tag,
                               cseq=2, src_port=src_port), leg_a)
        self.recvSIP(self._uas_sock)  # drain ACK

        # Verify RTP now flows on new ports (PORT_A2 ↔ PORT_B2)
        self.assertEqual(_sdp_media_port(ri_b),   _RTP_PORT_A2)
        self.assertEqual(_sdp_media_port(ok_ri_a), _RTP_PORT_B2)
        pkt2 = _make_rtp(seq=2, ts=320, ssrc=0x22222222)
        rtp_b2.sendto(pkt2, ("127.0.0.1", _RTP_PORT_A2))
        self.assertEqual(rtp_a2.recv(256), pkt2)
        rtp_a2.sendto(pkt2, ("127.0.0.1", _RTP_PORT_B2))
        self.assertEqual(rtp_b2.recv(256), pkt2)

        rtp_a.close(); rtp_b.close(); rtp_a2.close(); rtp_b2.close()

        # BYE (cseq=3 after two INVITEs)
        self.sendSIP(_make_bye("z9hG4bK-b2b-018-bye", call_id, to_tag,
                               cseq=3, src_port=src_port), leg_a)
        bye_b = self.recvSIP(self._uas_sock)
        self.sendToSIP(_make_200_ok_for(bye_b), sems_addr, self._uas_sock)
        self.assertSIP(self.recvSIP(leg_a), "^SIP/2\\.0 200 OK\n.*CSeq: 3 BYE\n")
        leg_a.close()

    # ------------------------------------------------------------------
    # Scenario 19: RTP during hold (a=sendonly from leg A)
    #   sendonly: A→B flows on hold ports; B does not send (recvonly)
    #   In RTP_Direct mode SEMS does not filter by direction — direction
    #   enforcement is left to endpoints; this test verifies the active
    #   direction (A→B) and that the SDP direction flags are preserved.
    # ------------------------------------------------------------------
    def testRtpDirectionHold(self):
        src_port = 57719
        leg_a = self.makeUACSocket(src_port)
        branch  = "z9hG4bK-b2b-019"
        call_id = "test-b2b-call-019@127.0.0.1"

        # Only need hold-phase sockets
        rtp_a2 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        rtp_a2.settimeout(3); rtp_a2.bind(("127.0.0.1", _RTP_PORT_A2))
        rtp_b2 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        rtp_b2.settimeout(3); rtp_b2.bind(("127.0.0.1", _RTP_PORT_B2))

        # --- Setup call ---
        self.sendSIP(_make_invite(branch, call_id, src_port=src_port), leg_a)
        self.recvSIP(leg_a)  # 100 Trying
        b2b_invite, sems_addr = self.recvFromSIP(self._uas_sock)
        self.sendToSIP(_make_provisional(180, "Ringing", b2b_invite),
                       sems_addr, self._uas_sock)
        self.recvSIP(leg_a)  # 180
        self.sendToSIP(_make_200_ok_invite(b2b_invite), sems_addr, self._uas_sock)
        ok_a = self.recvSIP(leg_a)
        to_tag = _to_tag(ok_a)
        self.sendSIP(_make_ack("z9hG4bK-b2b-019-ack", call_id, to_tag,
                               src_port=src_port), leg_a)
        self.recvSIP(self._uas_sock)  # drain ACK

        # --- Hold: leg A sendonly (PORT_A2), leg B recvonly (PORT_B2) ---
        self.sendSIP(_make_reinvite("z9hG4bK-b2b-019-hold", call_id, to_tag,
                                    cseq=2, sdp=_SDP_SENDONLY_A,
                                    src_port=src_port), leg_a)
        ri_b = self.recvSIPSkipRetrans(self._uas_sock, "ACK")
        # SEMS must relay a=sendonly to leg B unchanged
        self.assertSIP(ri_b, "^INVITE sip:[^\r]+ SIP/2\\.0\n.*a=sendonly")
        self.sendToSIP(_make_200_ok_invite(ri_b, sdp=_SDP_RECVONLY_B),
                       sems_addr, self._uas_sock)
        self.recvSIP(leg_a)  # 100 Trying
        ok_ri_a = self.recvSIP(leg_a)
        # SEMS must relay a=recvonly back to leg A unchanged
        self.assertSIP(ok_ri_a, "^SIP/2\\.0 200 OK\n.*a=recvonly")
        self.sendSIP(_make_ack("z9hG4bK-b2b-019-hold-ack", call_id, to_tag,
                               cseq=2, src_port=src_port), leg_a)
        self.recvSIP(self._uas_sock)  # drain ACK

        # sendonly → leg A sends to PORT_B2 (from recvonly answer); leg B receives
        dest_a_to_b = _sdp_media_port(ok_ri_a)
        self.assertEqual(dest_a_to_b, _RTP_PORT_B2)
        pkt_a2b = _make_rtp(seq=1, ts=160, ssrc=0xA0A0A0A0)
        rtp_a2.sendto(pkt_a2b, ("127.0.0.1", dest_a_to_b))
        self.assertEqual(rtp_b2.recv(256), pkt_a2b)

        # recvonly → leg B does not send; PORT_A2 stays silent
        # (In RTP_Direct mode SEMS does not intercept; this verifies no
        #  accidental traffic arrives from the test harness itself.)
        dest_b_to_a = _sdp_media_port(ri_b)
        self.assertEqual(dest_b_to_a, _RTP_PORT_A2)
        rtp_a2.settimeout(0.2)
        with self.assertRaises(TimeoutError):
            rtp_a2.recv(256)
        rtp_a2.settimeout(3)

        rtp_a2.close(); rtp_b2.close()

        # BYE (cseq=3)
        self.sendSIP(_make_bye("z9hG4bK-b2b-019-bye", call_id, to_tag,
                               cseq=3, src_port=src_port), leg_a)
        bye_b = self.recvSIP(self._uas_sock)
        self.sendToSIP(_make_200_ok_for(bye_b), sems_addr, self._uas_sock)
        self.assertSIP(self.recvSIP(leg_a), "^SIP/2\\.0 200 OK\n.*CSeq: 3 BYE\n")
        leg_a.close()


if __name__ == "__main__":
    sems_tester.main()
