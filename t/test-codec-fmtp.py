#!/usr/bin/env python3

"""Codec instances initialised from the peer's a=fmtp.

A PCMA-only caller (leg A) is transcoded by the SBC to an iLBC or Opus callee
(leg B), so SEMS runs the callee's codec itself.  On leg B SEMS is the
offerer, so the codec must take its parameters from the callee's answer, and
what SEMS then sends tells which parameters it applied:

  - iLBC: the payload size gives the frame mode, 50 bytes for 30 ms and
    38 bytes for 20 ms.  RFC 3952 §5 has both sides use 30 ms unless offer
    and answer both say mode=20, a missing mode meaning 30.
  - Opus: the TOC byte gives the audio bandwidth the encoder was limited to
    by maxplaybackrate.

The callee also sends iLBC in another mode than the negotiated one, which SEMS
must decode all the same and pass on to the caller."""

import math
import re as _re
import socket
import struct
import time

import sems_tester

# media ports advertised by the test endpoints, outside the SEMS relay range
_RTP_PORT_A = 9302
_RTP_PORT_B = 9304

_PT_PCMA = 8

# ptime of the PCMA stream sent by leg A, and number of packets per call
_PTIME = 0.02
_PACKETS = 50

# iLBC payload size per frame mode
_ILBC_20MS = 38
_ILBC_30MS = 50

# 20 ms iLBC frames (RFC 3951, mode=20) of a 440 Hz tone at amplitude 8000,
# encoded by the iLBC plug-in
_ILBC_20MS_TONE = [bytes.fromhex(f) for f in (
    "5b7597a8d3f001ffc003fc00ff7da56835a95f5415f5aa4e555b2aa4f9323d1b36b1df8f7824",
    "5b759b1cd285e01ffc00ff803fcd92402cabff5e40065ffe780021bff8f1cd4dbf452626e4b8",
    "591d9515f287f807ff803ff007c1fa350286bffa240021bfe890028af66cdd52ad45e1e14574",
    "5b759d25f085803ff801ff007fd97900b6aeea35005cafe8d40186fea2042d49ca33e9e9e5b8",
    "591d9514d2851fe001ff003fe049286fa7940028affae40072bfd75038f1dde5ad457373a3b8",
)]


def _alaw_encode(sample):
    """G.711 A-law code of a 16 bit linear sample."""
    pcm = sample >> 3
    if pcm >= 0:
        mask = 0xD5
    else:
        mask = 0x55
        pcm = -pcm - 1
    seg = 0
    while seg < 8 and pcm > (0x20 << seg) - 1:
        seg += 1
    if seg >= 8:
        return 0x7F ^ mask
    mant = (pcm >> 1) & 0xF if seg < 2 else (pcm >> seg) & 0xF
    return ((seg << 4) | mant) ^ mask


def _alaw_decode(code):
    """16 bit linear sample of a G.711 A-law code."""
    code ^= 0x55
    seg = (code & 0x70) >> 4
    t = (code & 0x0F) << 4
    if seg == 0:
        t += 8
    elif seg == 1:
        t += 0x108
    else:
        t = (t + 0x108) << (seg - 1)
    return t if code & 0x80 else -t


def _tone_alaw(first_sample, count, freq=440, amp=8000):
    return bytes(
        _alaw_encode(int(amp * math.sin(2 * math.pi * freq * (first_sample + i) / 8000)))
        for i in range(count))


def _rms_alaw(payload):
    samples = [_alaw_decode(b) for b in payload]
    return math.sqrt(sum(s * s for s in samples) / len(samples)) if samples else 0


def _rtp(pt, seq, ts, ssrc, payload):
    return struct.pack("!BBHII", 0x80, pt, seq & 0xFFFF, ts & 0xFFFFFFFF, ssrc) + payload


def _payloads(packets, pt):
    """RTP payloads of the given payload type (no CSRC or extensions)."""
    return [p[12:] for p in packets if len(p) > 12 and p[1] & 0x7F == pt]


def _opus_bandwidth(toc):
    """Audio bandwidth of an Opus packet, from its TOC byte (RFC 6716 §3.1)."""
    config = toc >> 3
    if config < 12:
        return ("NB", "MB", "WB")[config // 4]
    if config < 16:
        return ("SWB", "FB")[(config - 12) // 2]
    return ("NB", "WB", "SWB", "FB")[(config - 16) // 4]


def _hdr(msg, name):
    m = _re.search(name + r": ([^\r\n]+)", msg)
    return m.group(1) if m else ""


def _to_tag(msg):
    m = _re.search(r"To:[^\r\n]*tag=([A-Za-z0-9\-_\.]+)", msg)
    return m.group(1) if m else ""


def _media_port(msg):
    m = _re.search(r"m=audio (\d+)", msg)
    return int(m.group(1)) if m else None


def _rtpmap_pt(msg, codec):
    m = _re.search(r"a=rtpmap:(\d+) " + codec + "/", msg, _re.IGNORECASE)
    return int(m.group(1)) if m else None


def _fmtp(msg, pt):
    m = _re.search(rf"a=fmtp:{pt} ([^\r\n]+)", msg)
    return m.group(1) if m else None


def _sdp(origin, port, pt, rtpmap, fmtp=None, extra_pt=None):
    """SDP with one codec, optionally preceded by the static extra_pt."""
    pts = f"{extra_pt} {pt}" if extra_pt is not None else f"{pt}"
    return (
        "v=0\n"
        f"o=- {origin} {origin} IN IP4 127.0.0.1\n"
        "s=-\n"
        "c=IN IP4 127.0.0.1\n"
        "t=0 0\n"
        f"m=audio {port} RTP/AVP {pts}\n"
        + (f"a=rtpmap:{extra_pt} PCMA/8000\n" if extra_pt is not None else "")
        + f"a=rtpmap:{pt} {rtpmap}\n"
        + (f"a=fmtp:{pt} {fmtp}\n" if fmtp else "")
        + "a=sendrecv\n"
    )


# PCMA only - SEMS adds iLBC and Opus towards leg B without any a=fmtp
_SDP_A_PCMA = _sdp(1000000001, _RTP_PORT_A, _PT_PCMA, "PCMA/8000")

# PCMA and iLBC mode=20 - SEMS passes the mode=20 on in its offer to leg B
_SDP_A_ILBC20 = _sdp(1000000002, _RTP_PORT_A, 97, "iLBC/8000", "mode=20",
                     extra_pt=_PT_PCMA)


def _make_invite(branch, call_id, sdp, src_port):
    sdp_cr = sdp.replace("\n", "\r\n")
    return (
        f"INVITE sip:bob@voip.sipwise.local SIP/2.0\n"
        f"Via: SIP/2.0/UDP 127.0.0.1:{src_port};branch={branch};rport\n"
        f"Max-Forwards: 70\n"
        f"To: <sip:bob@voip.sipwise.local>\n"
        f"From: Alice <sip:alice@voip.sipwise.local>;tag=fmtp-tag-1\n"
        f"Call-ID: {call_id}\n"
        f"CSeq: 1 INVITE\n"
        f"Contact: <sip:alice@127.0.0.1:{src_port}>\n"
        f"Content-Type: application/sdp\n"
        f"Content-Length: {len(sdp_cr)}\n"
        "\n" + sdp
    )


def _make_in_dialog(method, branch, call_id, to_tag, cseq, src_port):
    return (
        f"{method} sip:bob@voip.sipwise.local SIP/2.0\n"
        f"Via: SIP/2.0/UDP 127.0.0.1:{src_port};branch={branch};rport\n"
        f"Max-Forwards: 70\n"
        f"To: <sip:bob@voip.sipwise.local>;tag={to_tag}\n"
        f"From: Alice <sip:alice@voip.sipwise.local>;tag=fmtp-tag-1\n"
        f"Call-ID: {call_id}\n"
        f"CSeq: {cseq} {method}\n"
        f"Content-Length: 0\n"
        "\n"
    )


def _make_200_ok(request_msg, sdp=None):
    to = _hdr(request_msg, "To")
    body = sdp.replace("\n", "\r\n") if sdp else ""
    return (
        f"SIP/2.0 200 OK\n"
        f"Via: {_hdr(request_msg, 'Via')}\n"
        f"To: {to if ';tag=' in to else to + ';tag=uas-fmtp-001'}\n"
        f"From: {_hdr(request_msg, 'From')}\n"
        f"Call-ID: {_hdr(request_msg, 'Call-ID')}\n"
        f"CSeq: {_hdr(request_msg, 'CSeq')}\n"
        f"Contact: <sip:bob@127.0.0.1:5074>\n"
        + ("Content-Type: application/sdp\n" if sdp else "")
        + f"Content-Length: {len(body)}\n"
        "\n" + (sdp or "")
    )


class TestCodecFmtp(sems_tester.TestCase):
    _config_base = "codec-fmtp"
    _sip_port = 5064
    _uas_port = 5074

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

    def _call(self, call_id, src_port, codec, rtpmap, fmtp,
              sdp_a=_SDP_A_PCMA, frames_b=None):
        """Run a call in which leg B answers with codec only (rtpmap, fmtp),
        leg A sends a PCMA tone and, when frames_b is given, leg B sends these
        payloads in turn.  Returns the SDP offered to leg B, the payload type
        of codec in it, and the RTP packets received by legs A and B."""
        rtp_a = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        rtp_a.bind(("127.0.0.1", _RTP_PORT_A))
        rtp_a.settimeout(0)
        rtp_b = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        rtp_b.bind(("127.0.0.1", _RTP_PORT_B))
        rtp_b.settimeout(0)
        leg_a = self.makeUACSocket(src_port)
        try:
            self.sendSIP(_make_invite("z9hG4bK-fmtp-1", call_id, sdp_a, src_port), leg_a)
            self.recvSIP(leg_a)  # 100 Trying

            invite_b, sems_addr = self.recvB2BINVITE(self._uas_sock)
            b2b_cid = _hdr(invite_b, "Call-ID")
            pt = _rtpmap_pt(invite_b, codec)
            self.assertIsNotNone(pt, f"no {codec} offered to leg B")

            self.sendToSIP(_make_200_ok(invite_b,
                                        _sdp(2000000001, _RTP_PORT_B, pt, rtpmap, fmtp)),
                           sems_addr, self._uas_sock)
            ok_a = self.recvSIP(leg_a)
            to_tag = _to_tag(ok_a)
            self.sendSIP(_make_in_dialog("ACK", "z9hG4bK-fmtp-1-ack", call_id, to_tag, 1,
                                         src_port), leg_a)
            self.recvSIPForCall(self._uas_sock, b2b_cid)  # ACK

            sems_a = ("127.0.0.1", _media_port(ok_a))
            sems_b = ("127.0.0.1", _media_port(invite_b))
            recv_a, recv_b = [], []
            for seq in range(_PACKETS):
                rtp_a.sendto(_rtp(_PT_PCMA, seq, seq * 160, 0xA1A1A1A1,
                                  _tone_alaw(seq * 160, 160)), sems_a)
                if frames_b:
                    rtp_b.sendto(_rtp(pt, seq, seq * 160, 0xB2B2B2B2,
                                      frames_b[seq % len(frames_b)]), sems_b)
                time.sleep(_PTIME)
                for sock, received in ((rtp_a, recv_a), (rtp_b, recv_b)):
                    try:
                        while True:
                            received.append(sock.recv(2048))
                    except BlockingIOError:
                        pass

            self.sendSIP(_make_in_dialog("BYE", "z9hG4bK-fmtp-1-bye", call_id, to_tag, 2,
                                         src_port), leg_a)
            bye_b = self.recvSIPForCall(self._uas_sock, b2b_cid)
            self.sendToSIP(_make_200_ok(bye_b), sems_addr, self._uas_sock)
            self.recvSIP(leg_a)  # 200 OK for BYE

            return invite_b, pt, recv_a, recv_b
        finally:
            leg_a.close()
            rtp_a.close()
            rtp_b.close()

    def assertIlbcSent(self, packets, pt, size):
        """SEMS sent leg B iLBC, in frames of the given size only."""
        payloads = _payloads(packets, pt)
        self.assertGreater(len(payloads), _PACKETS // 4, "too little iLBC received")
        self.assertEqual({len(p) for p in payloads}, {size})

    def opusBandwidths(self, packets, pt):
        payloads = _payloads(packets, pt)
        self.assertGreater(len(payloads), _PACKETS // 4, "too little Opus received")
        return {_opus_bandwidth(p[0]) for p in payloads}

    # ------------------------------------------------------------------
    # iLBC frame mode (RFC 3952 §5)
    # ------------------------------------------------------------------
    def testIlbcAnswerMode20ToOfferWithoutMode(self):
        """Our offer has no mode, i.e. 30 ms: the peer's mode=20 does not
        lower it."""
        offer_b, pt, _, recv_b = self._call(
            "test-fmtp-ilbc-001@127.0.0.1", 57901, "iLBC", "iLBC/8000", "mode=20")
        self.assertIsNone(_fmtp(offer_b, pt))
        self.assertIlbcSent(recv_b, pt, _ILBC_30MS)

    def testIlbcAnswerWithoutMode(self):
        offer_b, pt, _, recv_b = self._call(
            "test-fmtp-ilbc-002@127.0.0.1", 57902, "iLBC", "iLBC/8000", None)
        self.assertIlbcSent(recv_b, pt, _ILBC_30MS)

    def testIlbcMode20OnBothSides(self):
        """Offer and answer both say mode=20: 20 ms frames are sent."""
        offer_b, pt, _, recv_b = self._call(
            "test-fmtp-ilbc-003@127.0.0.1", 57903, "iLBC", "iLBC/8000", "mode=20",
            sdp_a=_SDP_A_ILBC20)
        self.assertEqual(_fmtp(offer_b, pt), "mode=20")
        self.assertIlbcSent(recv_b, pt, _ILBC_20MS)

    def testIlbcOfferMode20AnswerMode30(self):
        offer_b, pt, _, recv_b = self._call(
            "test-fmtp-ilbc-004@127.0.0.1", 57904, "iLBC", "iLBC/8000", "mode=30",
            sdp_a=_SDP_A_ILBC20)
        self.assertEqual(_fmtp(offer_b, pt), "mode=20")
        self.assertIlbcSent(recv_b, pt, _ILBC_30MS)

    def testIlbcDecodesOtherModeThanNegotiated(self):
        """30 ms are negotiated, but leg B sends 20 ms frames: they are
        decoded and reach leg A as the tone they carry, not as silence."""
        _, pt, recv_a, recv_b = self._call(
            "test-fmtp-ilbc-005@127.0.0.1", 57905, "iLBC", "iLBC/8000", None,
            frames_b=_ILBC_20MS_TONE)
        pcma = _payloads(recv_a, _PT_PCMA)
        self.assertGreater(len(pcma), _PACKETS // 4, "too little PCMA received")
        loudest = max(_rms_alaw(p) for p in pcma)
        self.assertGreater(loudest, 2000, "leg A only gets silence")

        # SEMS itself keeps sending the negotiated mode
        self.assertIlbcSent(recv_b, pt, _ILBC_30MS)

    # ------------------------------------------------------------------
    # Opus maxplaybackrate
    # ------------------------------------------------------------------
    def testOpusAnswerWithoutFmtp(self):
        """Reference: without maxplaybackrate the encoder goes beyond
        narrowband, so that the tests below tell a limit applied."""
        _, pt, _, recv_b = self._call(
            "test-fmtp-opus-001@127.0.0.1", 57911, "opus", "opus/48000/2", None)
        self.assertNotEqual(self.opusBandwidths(recv_b, pt), {"NB"})

    def testOpusAnswerMaxPlaybackRate(self):
        _, pt, _, recv_b = self._call(
            "test-fmtp-opus-002@127.0.0.1", 57912, "opus", "opus/48000/2",
            "maxplaybackrate=8000")
        self.assertEqual(self.opusBandwidths(recv_b, pt), {"NB"})

    def testOpusAnswerMaxPlaybackRateNotFirst(self):
        """maxplaybackrate is applied after other parameters, too."""
        _, pt, _, recv_b = self._call(
            "test-fmtp-opus-003@127.0.0.1", 57913, "opus", "opus/48000/2",
            "minptime=10;useinbandfec=1; maxplaybackrate=8000")
        self.assertEqual(self.opusBandwidths(recv_b, pt), {"NB"})


if __name__ == "__main__":
    sems_tester.main()
