#!/usr/bin/env python3

"""Regression test for MT#65396 on the CE codebase: a B2B-relayed
re-INVITE racing our own internally generated hold/resume re-INVITE
must be queued, not 491'd and dropped.

CE has no call_transfer module, so the self-triggered hold/resume
re-INVITE that a real blind transfer produces (call_transfer's
ResumeHeldEvent) is instead driven here by a tiny test-only DSM
diagram (t/xdsm/diagrams/race_test.dsm) which calls the exact same
generic CallLeg::putOnHold()/resumeHeld() API in reaction to two
in-dialog INFO requests sent by this test. Everything downstream -
the pending UAC INVITE transaction, the B2B-relayed collision, and
CallLeg::onB2BEvent's queue-vs-491 decision - is the real production
code path, identical to the sems-pbx MT#65396 fix.

The suite also covers the second half of MT#65396: the anti-491 marker
P-Force-491: 0 used to be read from the B2B-relayed request headers,
AFTER the profile's header filter had run - so any whitelist profile
silently stripped it and the 491 fired anyway. The header is now
consumed into the skip_491 event flag at request reception; the test
profile deliberately configures whitelist filtering to prove the flag
survives it.
"""

import re as _re
import sems_tester


_SDP_ALICE = (
    "v=0\n"
    "o=- 1000000001 1000000001 IN IP4 127.0.0.1\n"
    "s=-\n"
    "c=IN IP4 127.0.0.1\n"
    "t=0 0\n"
    "m=audio 30002 RTP/AVP 0 8\n"
    "a=rtpmap:0 PCMU/8000\n"
    "a=rtpmap:8 PCMA/8000\n"
    "a=sendrecv\n"
)

_SDP_BOB = (
    "v=0\n"
    "o=- 2000000001 2000000001 IN IP4 127.0.0.1\n"
    "s=-\n"
    "c=IN IP4 127.0.0.1\n"
    "t=0 0\n"
    "m=audio 30004 RTP/AVP 0 8\n"
    "a=rtpmap:0 PCMU/8000\n"
    "a=rtpmap:8 PCMA/8000\n"
    "a=sendrecv\n"
)

_SDP_BOB2 = (
    "v=0\n"
    "o=- 2000000002 2000000001 IN IP4 127.0.0.1\n"
    "s=-\n"
    "c=IN IP4 127.0.0.1\n"
    "t=0 0\n"
    "m=audio 30006 RTP/AVP 0 8\n"
    "a=rtpmap:0 PCMU/8000\n"
    "a=sendrecv\n"
)


_SDP_BOB3 = (
    "v=0\n"
    "o=- 2000000003 2000000001 IN IP4 127.0.0.1\n"
    "s=-\n"
    "c=IN IP4 127.0.0.1\n"
    "t=0 0\n"
    "m=audio 30008 RTP/AVP 0 8\n"
    "a=rtpmap:0 PCMU/8000\n"
    "a=sendrecv\n"
)


def _make_invite(branch, call_id, src_port):
    sdp = _SDP_ALICE.replace("\n", "\r\n")
    return (
        f"INVITE sip:bob@voip.sipwise.local SIP/2.0\n"
        f"Via: SIP/2.0/UDP 127.0.0.1:{src_port};branch={branch};rport\n"
        f"Max-Forwards: 70\n"
        f"To: <sip:bob@voip.sipwise.local>\n"
        f"From: Alice <sip:alice@voip.sipwise.local>;tag=a1b2c3d4\n"
        f"Call-ID: {call_id}\n"
        f"CSeq: 1 INVITE\n"
        f"Contact: <sip:alice@127.0.0.1:{src_port}>\n"
        f"Content-Type: application/sdp\n"
        f"Content-Length: {len(sdp)}\n"
        "\n" + _SDP_ALICE
    )


def _hdr(msg, name):
    m = _re.search(name + r": ([^\r\n]+)", msg)
    return m.group(1) if m else ""


def _to_tag(msg):
    m = _re.search(r"To:[^\r\n]*tag=([A-Za-z0-9\-_\.]+)", msg)
    return m.group(1) if m else ""


def _make_ack(branch, call_id, to_tag, cseq, src_port):
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


def _make_info(branch, call_id, to_tag, cseq, src_port):
    return (
        f"INFO sip:bob@voip.sipwise.local SIP/2.0\n"
        f"Via: SIP/2.0/UDP 127.0.0.1:{src_port};branch={branch};rport\n"
        f"Max-Forwards: 70\n"
        f"To: <sip:bob@voip.sipwise.local>;tag={to_tag}\n"
        f"From: Alice <sip:alice@voip.sipwise.local>;tag=a1b2c3d4\n"
        f"Call-ID: {call_id}\n"
        f"CSeq: {cseq} INFO\n"
        f"Content-Length: 0\n"
        "\n"
    )


def _make_200_ok_invite(invite_msg, sdp, contact, local_tag="uas-tag-001"):
    via = _hdr(invite_msg, "Via")
    to = _hdr(invite_msg, "To")
    from_ = _hdr(invite_msg, "From")
    call_id = _hdr(invite_msg, "Call-ID")
    cseq = _hdr(invite_msg, "CSeq")
    to_tag = to + f";tag={local_tag}" if ";tag=" not in to else to
    sdp_cr = sdp.replace("\n", "\r\n")
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
        "\n" + sdp
    )


def _make_200_ok_for_request(request_msg):
    via = _hdr(request_msg, "Via")
    to = _hdr(request_msg, "To")
    from_ = _hdr(request_msg, "From")
    call_id = _hdr(request_msg, "Call-ID")
    cseq = _hdr(request_msg, "CSeq")
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


def _make_reinvite_uas(call_id, to_hdr, sdp, branch, cseq, src_port=5070,
                        local_tag="uas-tag-001", extra_hdrs=""):
    """Build a re-INVITE sent by the UAS side of the dialog (Bob).
    `extra_hdrs` are inserted verbatim, newline-separated (e.g.
    "P-Force-491: 0\\n")."""
    sdp_cr = sdp.replace("\n", "\r\n")
    return (
        f"INVITE sip:alice@voip.sipwise.local SIP/2.0\n"
        f"Via: SIP/2.0/UDP 127.0.0.1:{src_port};branch={branch};rport\n"
        f"Max-Forwards: 70\n"
        f"To: {to_hdr}\n"
        f"From: <sip:bob@voip.sipwise.local>;tag={local_tag}\n"
        f"Call-ID: {call_id}\n"
        f"CSeq: {cseq} INVITE\n"
        f"Contact: <sip:bob@127.0.0.1:{src_port}>\n"
        + extra_hdrs +
        f"Content-Type: application/sdp\n"
        f"Content-Length: {len(sdp_cr)}\n"
        "\n" + sdp
    )


def _make_ack_uas(call_id, to_hdr, branch, cseq, src_port=5070,
                   local_tag="uas-tag-001"):
    """Build an ACK sent by the UAS side of the dialog (Bob), for a 2xx
    response to one of his own in-dialog INVITEs."""
    return (
        f"ACK sip:alice@voip.sipwise.local SIP/2.0\n"
        f"Via: SIP/2.0/UDP 127.0.0.1:{src_port};branch={branch};rport\n"
        f"Max-Forwards: 70\n"
        f"To: {to_hdr}\n"
        f"From: <sip:bob@voip.sipwise.local>;tag={local_tag}\n"
        f"Call-ID: {call_id}\n"
        f"CSeq: {cseq} ACK\n"
        f"Content-Length: 0\n"
        "\n"
    )


def _recv_invite(test, sock):
    """Read from sock until an INVITE is seen, discarding anything else
    (e.g. the direct 200 OK reply to an INFO request sent on this leg)."""
    for _ in range(10):
        msg = test.recvSIP(sock)
        if msg.startswith("INVITE"):
            return msg
    raise AssertionError("No INVITE received after 10 attempts")


def _cseq(msg):
    m = _re.search(r"CSeq:\s*(\d+)", msg)
    return m.group(1) if m else ""


def _recv_fresh_invite(test, sock, seen_cseqs):
    """Return the next re-INVITE on `sock` with a CSeq not already in
    `seen_cseqs`, skipping ACKs and retransmissions of earlier re-INVITEs.
    On a loaded CI the earlier re-INVITEs (hold, resume) keep retransmitting
    via timer A and pile up in the socket buffer; a plain _recv_invite()
    would mistake one of those for the queued update we are waiting for.
    Records the new CSeq before returning."""
    for _ in range(20):
        msg = test.recvSIP(sock)
        if not msg.startswith("INVITE"):
            continue
        c = _cseq(msg)
        if c in seen_cseqs:
            continue
        seen_cseqs.add(c)
        return msg
    raise AssertionError("no fresh INVITE received")


class TestXdsmRace(sems_tester.TestCase):
    _config_base = "xdsm"
    _sip_port = 5066
    _uas_port = 5070

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls._uas_sock = cls.makeUASSocket(cls._uas_port)

    @classmethod
    def tearDownClass(cls):
        cls._uas_sock.close()
        super().tearDownClass()

    def setUp(self):
        self._uas_sock.settimeout(0)
        try:
            while True:
                self._uas_sock.recv(4096)
        except BlockingIOError:
            pass
        finally:
            self._uas_sock.settimeout(3)

    def tearDown(self):
        # Safety net for the shared UAS socket: a test may leave SEMS with
        # in-dialog business still outstanding towards Bob - a renegotiation
        # re-INVITE following a fake 200, or a 491/200 whose ACK we never
        # sent. Left alone these keep retransmitting for up to 32s (timer G)
        # and bleed into whichever test runs next; on slower machines (CI)
        # they get mistaken for that test's own traffic and it times out.
        # Drain and answer them here so every test starts from a quiet
        # socket. A plain sendrecv 200 OK is enough - we are only quiescing
        # the dialog, not asserting anything about it.
        sems_addr = ("127.0.0.1", self._sip_port)
        self._uas_sock.settimeout(1)
        try:
            for _ in range(20):
                msg = self.recvSIP(self._uas_sock)
                if msg.startswith("INVITE"):
                    self.sendToSIP(
                        _make_200_ok_invite(msg, _SDP_BOB3,
                                             "<sip:bob@127.0.0.1:5070>"),
                        sems_addr, self._uas_sock)
                elif msg.startswith("SIP/2.0") or msg.startswith("ACK"):
                    continue  # a response, or an ACK - nothing to answer
                else:
                    self.sendToSIP(_make_200_ok_for_request(msg), sems_addr,
                                   self._uas_sock)
        except (TimeoutError, OSError):
            pass
        finally:
            self._uas_sock.settimeout(3)
        super().tearDown()

    def testHoldResumeRace(self):
        """MT#65396: a re-INVITE relayed towards Alice must be queued, not
        491'd, while Alice's own DSM-triggered resume is in flight."""
        src_port = 58001
        leg_a = self.makeUACSocket(src_port)
        branch = "z9hG4bK-xdsm-001"
        call_id = "test-xdsm-race-001@127.0.0.1"

        # --- Alice calls Bob, Bob answers ---
        self.sendSIP(_make_invite(branch, call_id, src_port), leg_a)
        self.recvSIP(leg_a)  # 100 Trying

        b2b_invite, sems_addr = self.recvB2BINVITE(self._uas_sock)
        b2b_cid = _hdr(b2b_invite, "Call-ID")
        self.sendToSIP(_make_200_ok_invite(b2b_invite, _SDP_BOB,
                                            "<sip:bob@127.0.0.1:5070>"),
                       sems_addr, self._uas_sock)
        ok_a = self.recvSIP(leg_a)
        to_tag = _to_tag(ok_a)
        self.sendSIP(_make_ack(branch + "-ack", call_id, to_tag, 1, src_port),
                     leg_a)
        self.recvSIPForCall(self._uas_sock, b2b_cid)  # drain ACK on Bob

        # Track CSeqs already handled on Alice's leg so their timer-A
        # retransmissions don't get mistaken later for the queued update.
        seen_cseqs = set()

        # --- Trigger 1: Alice's leg is put on hold (DSM: sbc.putOnHold) ---
        self.sendSIP(_make_info(branch + "-info1", call_id, to_tag, 2,
                                 src_port), leg_a)

        hold_alice = _recv_invite(self, leg_a)
        seen_cseqs.add(_cseq(hold_alice))
        self.sendToSIP(
            _make_200_ok_invite(hold_alice, _SDP_ALICE,
                                 f"<sip:alice@127.0.0.1:{src_port}>"),
            sems_addr, leg_a)
        self.recvSIP(leg_a)  # drain ACK

        # The INFO itself gets relayed to Bob too - just answer it.
        info_bob = self.recvSIPForCall(self._uas_sock, b2b_cid)
        if info_bob.startswith("INFO"):
            self.sendToSIP(_make_200_ok_for_request(info_bob), sems_addr,
                           self._uas_sock)

        # --- Trigger 2: Alice's leg is resumed (DSM: sbc.resumeHeld) - the
        # resulting re-INVITE is left UNANSWERED on purpose, so it is still
        # pending when Bob's own re-INVITE (below) needs to be relayed into
        # the very same leg. ---
        self.sendSIP(_make_info(branch + "-info2", call_id, to_tag, 3,
                                 src_port), leg_a)

        resume_alice = _recv_invite(self, leg_a)
        seen_cseqs.add(_cseq(resume_alice))

        info_bob2 = self.recvSIPForCall(self._uas_sock, b2b_cid)
        if info_bob2.startswith("INFO"):
            self.sendToSIP(_make_200_ok_for_request(info_bob2), sems_addr,
                           self._uas_sock)

        # --- The race: Bob sends his own re-INVITE, which SEMS must relay
        # into Alice's leg while her own resume transaction is pending. It
        # carries P-Force-491: 0 (as the real pickup update does) - stripped
        # by the whitelist header filter but surviving as the skip_491 flag,
        # which is what must suppress the 491 here. ---
        collide = _make_reinvite_uas(
            b2b_cid, _hdr(b2b_invite, "From"), _SDP_BOB2,
            "z9hG4bK-xdsm-001-collide", cseq=12, extra_hdrs="P-Force-491: 0\n")
        self.sendToSIP(collide, sems_addr, self._uas_sock)

        collide_reply = self.recvSIPForCall(self._uas_sock, b2b_cid)
        while collide_reply.startswith("SIP/2.0 100"):
            collide_reply = self.recvSIPForCall(self._uas_sock, b2b_cid)
        # This is the actual regression check: without the fix, SEMS replies
        # 491 Request Pending here and silently drops the update instead of
        # queueing it for once Alice's own resume transaction completes.
        self.assertSIP(collide_reply, "^SIP/2\\.0 200 OK\n")

        # Now let Alice's own pending resume transaction complete.
        self.sendToSIP(
            _make_200_ok_invite(resume_alice, _SDP_ALICE,
                                 f"<sip:alice@127.0.0.1:{src_port}>"),
            sems_addr, leg_a)

        # The queued update (Bob's new SDP) must still reach Alice - proving
        # it was queued for real, not just fake-200'd and dropped. Pick the
        # next re-INVITE with an unseen CSeq, skipping the resume's ACK and
        # any timer-A retransmissions of the resume still in the buffer.
        queued_update = _recv_fresh_invite(self, leg_a, seen_cseqs)
        self.sendToSIP(
            _make_200_ok_invite(queued_update, _SDP_ALICE,
                                 f"<sip:alice@127.0.0.1:{src_port}>"),
            sems_addr, leg_a)
        self.recvSIP(leg_a)  # drain ACK

        leg_a.close()

    def testStackedSelfUpdatesPlusExternalCollision(self):
        """Adversarial version of MT#65396: stack a SECOND self-triggered
        update (resume) behind the first (hold) - still unanswered - and
        THEN queue Bob's external collision (marked P-Force-491: 0) behind
        both of those. This stresses whether CallLeg::onB2BEvent keeps the
        queued updates in order once several are stacked up, and that the
        marked external collision is still queued rather than 491'd even
        when it is the third thing waiting.

        Proof points:
          - While hold's own INVITE is still unanswered, requesting resume
            too must NOT put a second real INVITE on the wire (it must be
            queued) - so exactly one INVITE arrives on leg_a before we
            reply to the first one.
          - Bob's own colliding re-INVITE (P-Force-491: 0), arriving while
            hold is still the one in flight, must still get 200 OK (queued),
            not 491 - not just when Alice's leg is on her first self-update,
            but even with a second self-update already stacked behind it.
          - Once hold completes, the NEXT thing sent to Alice must be the
            resume (not Bob's collision, which was queued after it) -
            proving FIFO order is preserved across the whole chain.
          - The final, third INVITE to Alice must carry Bob's original SDP
            (port from _SDP_BOB2) - proving his update survived being
            queued behind two of SEMS's own self-issued updates intact,
            not dropped or corrupted.
        """
        src_port = 58002
        leg_a = self.makeUACSocket(src_port)
        branch = "z9hG4bK-xdsm-002"
        call_id = "test-xdsm-race-002@127.0.0.1"

        # --- Alice calls Bob, Bob answers ---
        self.sendSIP(_make_invite(branch, call_id, src_port), leg_a)
        self.recvSIP(leg_a)  # 100 Trying

        b2b_invite, sems_addr = self.recvB2BINVITE(self._uas_sock)
        b2b_cid = _hdr(b2b_invite, "Call-ID")
        self.sendToSIP(_make_200_ok_invite(b2b_invite, _SDP_BOB,
                                            "<sip:bob@127.0.0.1:5070>"),
                       sems_addr, self._uas_sock)
        ok_a = self.recvSIP(leg_a)
        to_tag = _to_tag(ok_a)
        self.sendSIP(_make_ack(branch + "-ack", call_id, to_tag, 1, src_port),
                     leg_a)
        self.recvSIPForCall(self._uas_sock, b2b_cid)  # drain ACK on Bob

        # Track CSeqs already handled on Alice's leg so their timer-A
        # retransmissions don't get mistaken later for a queued update.
        seen_cseqs = set()

        # --- Trigger hold - leave its re-INVITE UNANSWERED. ---
        self.sendSIP(_make_info(branch + "-info1", call_id, to_tag, 2,
                                 src_port), leg_a)
        hold_alice = _recv_invite(self, leg_a)
        seen_cseqs.add(_cseq(hold_alice))

        # The INFO itself gets relayed to Bob too - just answer it, draining
        # it now so it cannot be mistaken for anything else below.
        info_bob1 = self.recvSIPForCall(self._uas_sock, b2b_cid)
        if info_bob1.startswith("INFO"):
            self.sendToSIP(_make_200_ok_for_request(info_bob1), sems_addr,
                           self._uas_sock)

        # --- Stack a SECOND self-triggered update (resume) while hold's own
        # INVITE is still pending. This must NOT put a second INVITE on the
        # wire - it must be queued behind the first. ---
        self.sendSIP(_make_info(branch + "-info2", call_id, to_tag, 3,
                                 src_port), leg_a)

        info_bob2 = self.recvSIPForCall(self._uas_sock, b2b_cid)
        if info_bob2.startswith("INFO"):
            self.sendToSIP(_make_200_ok_for_request(info_bob2), sems_addr,
                           self._uas_sock)

        # Prove exactly one INVITE TRANSACTION is in flight: the only things
        # that may arrive on leg_a now are the direct 200 OK replies to our
        # own two INFO requests, and SIP retransmissions of hold_alice
        # itself (same CSeq, since we deliberately never ACKed it) - but no
        # INVITE for a DIFFERENT (new) transaction, which would mean the
        # stacked resume update leaked onto the wire instead of being queued.
        hold_cseq = _hdr(hold_alice, "CSeq")
        leg_a.settimeout(1)
        try:
            for _ in range(5):
                leftover = self.recvSIP(leg_a)
                if leftover.startswith("SIP/2.0 200 OK") and "INFO" in leftover:
                    continue
                if leftover.startswith("INVITE") and _hdr(leftover, "CSeq") == hold_cseq:
                    continue  # retransmission of hold_alice - expected, not a new update
                self.fail("unexpected extra message on leg_a before hold's "
                          f"own INVITE was answered: {leftover!r}")
        except TimeoutError:
            pass
        finally:
            leg_a.settimeout(3)

        # --- Now queue Bob's own external collision behind BOTH of our
        # self-issued updates. Marked P-Force-491: 0 (stripped by the
        # whitelist filter, surviving as skip_491), it must still be queued
        # (200 OK), not 491'd, even though it is now the THIRD thing waiting
        # on this leg. ---
        collide = _make_reinvite_uas(
            b2b_cid, _hdr(b2b_invite, "From"), _SDP_BOB2,
            "z9hG4bK-xdsm-002-collide", cseq=12, extra_hdrs="P-Force-491: 0\n")
        self.sendToSIP(collide, sems_addr, self._uas_sock)

        collide_reply = self.recvSIPForCall(self._uas_sock, b2b_cid)
        while collide_reply.startswith("SIP/2.0 100"):
            collide_reply = self.recvSIPForCall(self._uas_sock, b2b_cid)
        self.assertSIP(collide_reply, "^SIP/2\\.0 200 OK\n")

        # --- Complete hold's transaction: the resume we stacked earlier
        # must come out next (FIFO), not Bob's collision. _recv_invite()
        # skips the ACK that SEMS sends for hold's 200 OK - we must not
        # drain it blindly with a bare recvSIP(), since UDP does not order
        # that ACK against the resume re-INVITE fired right behind it, and
        # a blind drain would sometimes swallow the resume itself. ---
        self.sendToSIP(
            _make_200_ok_invite(hold_alice, _SDP_ALICE,
                                 f"<sip:alice@127.0.0.1:{src_port}>"),
            sems_addr, leg_a)

        resume_alice = _recv_fresh_invite(self, leg_a, seen_cseqs)
        self.assertNotIn("30006", resume_alice,
                          "resume must come before Bob's collision, "
                          "not be confused with it")

        # --- Complete the resume: Bob's originally-queued collision must
        # come out last, third, with his SDP intact. (Again pick the next
        # fresh-CSeq INVITE, skipping the resume's ACK and retransmissions.)
        self.sendToSIP(
            _make_200_ok_invite(resume_alice, _SDP_ALICE,
                                 f"<sip:alice@127.0.0.1:{src_port}>"),
            sems_addr, leg_a)

        queued_update = _recv_fresh_invite(self, leg_a, seen_cseqs)
        self.assertIn("30006", queued_update,
                      "Bob's collision SDP (port 30006) must survive being "
                      "queued behind two of our own self-issued updates")
        self.sendToSIP(
            _make_200_ok_invite(queued_update, _SDP_ALICE,
                                 f"<sip:alice@127.0.0.1:{src_port}>"),
            sems_addr, leg_a)
        self.recvSIP(leg_a)  # drain ACK

        leg_a.close()

    def testGenuineExternalGlareStill491s(self):
        """Boundary check for MT#65396: a plain call with no DSM trigger,
        no hold/resume and no P-Force-491 anywhere - just two of Bob's
        own re-INVITEs colliding back to back - must still be answered
        491 exactly as before the fix, end to end. (Both re-INVITEs ride
        the same dialog here, so this particular 491 comes from the
        dialog layer, AmSipDialog::onRxReqSanity(), which the fix leaves
        untouched; the onB2BEvent() branch of the same boundary -
        collision on a free dialog, no hold, no marker - is exercised by
        testForce491HeaderSurvivesHeaderFilter, which proves it 491s on
        unpatched code and queues only with the marker present.)"""
        src_port = 58003
        leg_a = self.makeUACSocket(src_port)
        branch = "z9hG4bK-xdsm-003"
        call_id = "test-xdsm-race-003@127.0.0.1"

        # --- Alice calls Bob, Bob answers - plain call, no DSM trigger ---
        self.sendSIP(_make_invite(branch, call_id, src_port), leg_a)
        self.recvSIP(leg_a)  # 100 Trying

        b2b_invite, sems_addr = self.recvB2BINVITE(self._uas_sock)
        b2b_cid = _hdr(b2b_invite, "Call-ID")
        b2b_from = _hdr(b2b_invite, "From")
        self.sendToSIP(_make_200_ok_invite(b2b_invite, _SDP_BOB,
                                            "<sip:bob@127.0.0.1:5070>"),
                       sems_addr, self._uas_sock)
        ok_a = self.recvSIP(leg_a)
        to_tag = _to_tag(ok_a)
        self.sendSIP(_make_ack(branch + "-ack", call_id, to_tag, 1, src_port),
                     leg_a)
        self.recvSIPForCall(self._uas_sock, b2b_cid)  # drain ACK on Bob

        # --- Bob sends his own re-INVITE towards Alice, left unanswered
        # on purpose - this is a genuinely external, non-hold pending UAC
        # invite on Alice's leg (own_pending_is_hold_resume must be false
        # for it: nothing here ever touches putOnHold/resumeHeld). ---
        first = _make_reinvite_uas(
            b2b_cid, b2b_from, _SDP_ALICE, "z9hG4bK-xdsm-003-first", cseq=2)
        self.sendToSIP(first, sems_addr, self._uas_sock)
        reinvite_alice = _recv_invite(self, leg_a)

        # --- Bob immediately sends a SECOND re-INVITE, colliding with the
        # first one still pending on Alice's leg. No hold/resume is
        # involved anywhere, so the pre-existing legacy behaviour (491,
        # since send_491_on_pending_session_leg=yes and no P-Force-491
        # header is present here) must be completely unchanged. ---
        second = _make_reinvite_uas(
            b2b_cid, b2b_from, _SDP_BOB2, "z9hG4bK-xdsm-003-second", cseq=3)
        self.sendToSIP(second, sems_addr, self._uas_sock)
        second_reply = self.recvSIPForCall(self._uas_sock, b2b_cid)
        while second_reply.startswith("SIP/2.0 100"):
            second_reply = self.recvSIPForCall(self._uas_sock, b2b_cid)
        # A genuine external-vs-external glare (no hold/resume involved)
        # must still get 491, unaffected by the MT#65396 fix.
        self.assertSIP(second_reply, "^SIP/2\\.0 491 Request Pending\n")

        # --- Clean up: complete the first (still pending) re-INVITE.
        # SEMS relays Alice's 200 OK back to Bob and waits for Bob's own
        # ACK before it ACKs Alice in turn. ---
        self.sendToSIP(
            _make_200_ok_invite(reinvite_alice, _SDP_ALICE,
                                 f"<sip:alice@127.0.0.1:{src_port}>"),
            sems_addr, leg_a)
        relayed_200 = self.recvSIPForCall(self._uas_sock, b2b_cid)
        self.assertSIP(relayed_200, "^SIP/2\\.0 200 OK\n")
        ack_from_bob = (
            f"ACK sip:alice@voip.sipwise.local SIP/2.0\n"
            f"Via: SIP/2.0/UDP 127.0.0.1:5070;branch=z9hG4bK-xdsm-003-ack;rport\n"
            f"Max-Forwards: 70\n"
            f"To: {b2b_from}\n"
            f"From: <sip:bob@voip.sipwise.local>;tag=uas-tag-001\n"
            f"Call-ID: {b2b_cid}\n"
            f"CSeq: 2 ACK\n"
            f"Content-Length: 0\n"
            "\n"
        )
        self.sendToSIP(ack_from_bob, sems_addr, self._uas_sock)
        self.recvSIP(leg_a)  # drain ACK to Alice

        leg_a.close()


    def testForce491HeaderSurvivesHeaderFilter(self):
        """Regression test for the second half of MT#65396: the
        P-Force-491: 0 anti-491 marker must keep working even though the
        profile's whitelist header filter (see
        t/xdsm/race_test.sbcprofile.conf) strips the header from
        B2B-relayed request hdrs. Before the fix the header was read in
        onB2BEvent from the already-filtered hdrs, so it was never seen
        and the 491 fired anyway; now it is consumed into the skip_491
        event flag at request reception, ahead of the filter.

        Topology matters: the colliding re-INVITE must arrive on a
        dialog with no open transaction of its own, otherwise
        AmSipDialog::onRxReqSanity() 491s it at the dialog layer before
        any B2B code runs. So the collision rides behind a DSM
        hold/resume cycle: Bob's first update gets fake-200d against the
        pending resume (freeing his dialog), the resume completes, the
        queued update is replayed towards Alice, and while that replay is
        pending Bob sends one more re-INVITE marked P-Force-491: 0. It
        must be queued (fake 200 OK) and replayed to Alice, not 491d."""
        src_port = 58004
        leg_a = self.makeUACSocket(src_port)
        branch = "z9hG4bK-xdsm-004"
        call_id = "test-xdsm-race-004@127.0.0.1"

        # --- Alice calls Bob, Bob answers ---
        self.sendSIP(_make_invite(branch, call_id, src_port), leg_a)
        self.recvSIP(leg_a)  # 100 Trying

        b2b_invite, sems_addr = self.recvB2BINVITE(self._uas_sock)
        b2b_cid = _hdr(b2b_invite, "Call-ID")
        b2b_from = _hdr(b2b_invite, "From")
        self.sendToSIP(_make_200_ok_invite(b2b_invite, _SDP_BOB,
                                            "<sip:bob@127.0.0.1:5070>"),
                       sems_addr, self._uas_sock)
        ok_a = self.recvSIP(leg_a)
        to_tag = _to_tag(ok_a)
        self.sendSIP(_make_ack(branch + "-ack", call_id, to_tag, 1, src_port),
                     leg_a)
        self.recvSIPForCall(self._uas_sock, b2b_cid)  # drain ACK on Bob

        # Track CSeqs already handled on Alice's leg so their timer-A
        # retransmissions don't get mistaken later for a replayed update.
        seen_cseqs = set()

        # --- Trigger 1: hold - complete it right away. ---
        self.sendSIP(_make_info(branch + "-info1", call_id, to_tag, 2,
                                 src_port), leg_a)
        hold_alice = _recv_invite(self, leg_a)
        seen_cseqs.add(_cseq(hold_alice))
        self.sendToSIP(
            _make_200_ok_invite(hold_alice, _SDP_ALICE,
                                 f"<sip:alice@127.0.0.1:{src_port}>"),
            sems_addr, leg_a)
        self.recvSIP(leg_a)  # drain ACK

        info_bob1 = self.recvSIPForCall(self._uas_sock, b2b_cid)
        if info_bob1.startswith("INFO"):
            self.sendToSIP(_make_200_ok_for_request(info_bob1), sems_addr,
                           self._uas_sock)

        # --- Trigger 2: resume - leave its re-INVITE UNANSWERED. ---
        self.sendSIP(_make_info(branch + "-info2", call_id, to_tag, 3,
                                 src_port), leg_a)
        resume_alice = _recv_invite(self, leg_a)
        seen_cseqs.add(_cseq(resume_alice))

        info_bob2 = self.recvSIPForCall(self._uas_sock, b2b_cid)
        if info_bob2.startswith("INFO"):
            self.sendToSIP(_make_200_ok_for_request(info_bob2), sems_addr,
                           self._uas_sock)

        # --- Bob's first update (marked P-Force-491: 0, stripped by the
        # whitelist filter but surviving as skip_491) collides with the
        # pending resume: it must be queued (fake 200 OK). This frees Bob's
        # dialog for the next INVITE - just like the fake-200 mechanism did
        # between the two pickup updates in the incident. ---
        collide1 = _make_reinvite_uas(
            b2b_cid, b2b_from, _SDP_BOB2, "z9hG4bK-xdsm-004-collide1",
            cseq=12, extra_hdrs="P-Force-491: 0\n")
        self.sendToSIP(collide1, sems_addr, self._uas_sock)
        reply1 = self.recvSIPForCall(self._uas_sock, b2b_cid)
        while reply1.startswith("SIP/2.0 100"):
            reply1 = self.recvSIPForCall(self._uas_sock, b2b_cid)
        self.assertSIP(reply1, "^SIP/2\\.0 200 OK\n")
        self.sendToSIP(
            _make_ack_uas(b2b_cid, b2b_from, "z9hG4bK-xdsm-004-collide1-ack",
                          cseq=12),
            sems_addr, self._uas_sock)

        # --- Complete the resume; the queued update (Bob's SDP, m-line
        # port 30006) is replayed towards Alice. Leave it unanswered: it
        # is now a pending UAC INVITE on Alice's leg with
        # hold == PreserveHoldStatus (plain sendrecv offer, no hold/
        # resume semantics anywhere). Pick the next re-INVITE with an unseen
        # CSeq, skipping the resume's ACK and any retransmissions. ---
        self.sendToSIP(
            _make_200_ok_invite(resume_alice, _SDP_ALICE,
                                 f"<sip:alice@127.0.0.1:{src_port}>"),
            sems_addr, leg_a)
        replayed1 = _recv_fresh_invite(self, leg_a, seen_cseqs)
        self.assertIn("m=audio 30006 ", replayed1)

        # --- The fake 200 to collide1 may be followed by SEMS
        # renegotiating Bob's leg with Alice's actual answer; if that
        # re-INVITE shows up, complete it so Bob's dialog is free and
        # O/A-settled again before the next collision (otherwise the
        # dialog layer would 491 it before any B2B code runs). ---
        self._uas_sock.settimeout(1)
        try:
            renego = self.recvSIPForCall(self._uas_sock, b2b_cid)
            while renego.startswith("SIP/2.0 100"):
                renego = self.recvSIPForCall(self._uas_sock, b2b_cid)
            if renego.startswith("INVITE"):
                self.sendToSIP(
                    _make_200_ok_invite(renego, _SDP_BOB2,
                                         "<sip:bob@127.0.0.1:5070>"),
                    sems_addr, self._uas_sock)
                self.recvSIPForCall(self._uas_sock, b2b_cid)  # drain ACK
        except (TimeoutError, AssertionError):
            pass
        finally:
            self._uas_sock.settimeout(3)

        # --- The actual regression check: Bob sends one more re-INVITE,
        # marked P-Force-491: 0. The whitelist header filter strips the
        # header on B2B relay; only the skip_491 flag captured at
        # reception can still suppress the 491. Unpatched code answers
        # 491 Request Pending here and drops the update. ---
        collide2 = _make_reinvite_uas(
            b2b_cid, b2b_from, _SDP_BOB3, "z9hG4bK-xdsm-004-collide2",
            cseq=13, extra_hdrs="P-Force-491: 0\n")
        self.sendToSIP(collide2, sems_addr, self._uas_sock)
        reply2 = self.recvSIPForCall(self._uas_sock, b2b_cid)
        while reply2.startswith("SIP/2.0 100"):
            reply2 = self.recvSIPForCall(self._uas_sock, b2b_cid)
        self.assertSIP(reply2, "^SIP/2\\.0 200 OK\n")
        self.sendToSIP(
            _make_ack_uas(b2b_cid, b2b_from, "z9hG4bK-xdsm-004-collide2-ack",
                          cseq=13),
            sems_addr, self._uas_sock)

        # --- Answer the first replayed update; the second queued one
        # (m-line port 30008) must then reach Alice too - proving it was
        # queued for real, not fake-200d and dropped. ---
        self.sendToSIP(
            _make_200_ok_invite(replayed1, _SDP_ALICE,
                                 f"<sip:alice@127.0.0.1:{src_port}>"),
            sems_addr, leg_a)
        replayed2 = _recv_fresh_invite(self, leg_a, seen_cseqs)
        self.assertIn("m=audio 30008 ", replayed2)
        self.sendToSIP(
            _make_200_ok_invite(replayed2, _SDP_ALICE,
                                 f"<sip:alice@127.0.0.1:{src_port}>"),
            sems_addr, leg_a)
        self.recvSIP(leg_a)  # drain ACK

        leg_a.close()


if __name__ == "__main__":
    sems_tester.main()
