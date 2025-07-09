/*
 * Copyright (C) 2010-2011 Stefan Sayer
 * Copyright (C) 2012-2013 FRAFOS GmbH
 *
 * This file is part of SEMS, a free SIP media server.
 *
 * SEMS is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * For a license to use the SEMS software under conditions
 * other than those described here, or to purchase support for this
 * software, please contact iptel.org by e-mail at the following addresses:
 *    info@iptel.org
 *
 * SEMS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "CallLeg.h"
#include "AmSessionContainer.h"
#include "AmConfig.h"
#include "ampi/MonitoringAPI.h"
#include "AmSipHeaders.h"
#include "AmUtils.h"
#include "AmRtpReceiver.h"
#include "SBCCallRegistry.h"

#include "global_defs.h"

#define GET_CALL_ID() (dlg->getCallid().c_str())

// helper functions

static const char *callStatus2str(const CallLeg::CallStatus state)
{
  static const char *disconnected = "Disconnected";
  static const char *disconnecting = "Disconnecting";
  static const char *noreply = "NoReply";
  static const char *ringing = "Ringing";
  static const char *connected = "Connected";
  static const char *unknown = "???";

  switch (state) {
    case CallLeg::Disconnected: return disconnected;
    case CallLeg::Disconnecting: return disconnecting;
    case CallLeg::NoReply: return noreply;
    case CallLeg::Ringing: return ringing;
    case CallLeg::Connected: return connected;
  }

  return unknown;
}

ReliableB2BEvent::~ReliableB2BEvent()
{
  DBG("reliable event was %sprocessed, sending %p to %s\n",
      processed ? "" : "NOT ",
      processed ? processed_reply : unprocessed_reply,
      sender.c_str());
  if (processed) {
    if (unprocessed_reply) delete unprocessed_reply;
    if (processed_reply) AmSessionContainer::instance()->postEvent(sender, processed_reply);
  }
  else {
    if (processed_reply) delete processed_reply;
    if (unprocessed_reply) AmSessionContainer::instance()->postEvent(sender, unprocessed_reply);
  }
}

static const string sendonly("sendonly");
static const string recvonly("recvonly");
static const string sendrecv("sendrecv");
static const string inactive("inactive");
static const string zero_connection("0.0.0.0");

////////////////////////////////////////////////////////////////////////////////
// helper functions

/** returns true if connection is avtive.
 * Returns given default_value if the connection address is empty to cope with
 * connection address set globaly and not per media stream */
static bool connectionActive(const SdpConnection &connection, bool default_value)
{
  if (connection.address.empty()) return default_value;
  if (connection.address == zero_connection) return false;
  return true;
}

enum MediaActivity { Inactive, Sendonly, Recvonly, Sendrecv };

/** Returns true if there is no direction=inactione or sendonly attribute in
 * given media stream. It doesn't check the connection address! */
static MediaActivity getMediaActivity(const vector<SdpAttribute> &attrs, MediaActivity default_value)
{
  // go through attributes and try to find sendonly/recvonly/sendrecv/inactive
  for (std::vector<SdpAttribute>::const_iterator a = attrs.begin(); 
      a != attrs.end(); ++a)
  {
    if (a->attribute == sendonly) return Sendonly;
    if (a->attribute == inactive) return Inactive;
    if (a->attribute == recvonly) return Recvonly;
    if (a->attribute == sendrecv) return Sendrecv;
  }

  return default_value; // none of the attributes given, return (session) default
}

static MediaActivity getMediaActivity(const SdpMedia &m, MediaActivity default_value)
{
  if (m.send) {
    if (m.recv) return Sendrecv;
    else return Sendonly;
  }
  else {
    if (m.recv) return Recvonly;
  }
  return Inactive;
}

static MediaActivity getMediaActivity(const SdpMedia &m)
{
  if (m.send) {
    if (m.recv) return Sendrecv;
    else return Sendonly;
  }
  else {
    if (m.recv) return Recvonly;
  }
  return Inactive;
}

/**
 * Checks, whether SDP of given type has sendonly / inactive
 */
static bool isSDPBodyHold(const AmSdp &sdp)
{
  /* if no meidas present, take into consideration the session level */
  if (sdp.media.empty())
  {
      MediaActivity session_activity = getMediaActivity(sdp.attributes, Sendrecv);
      if (session_activity != Sendrecv && session_activity != Recvonly)
        return true;
  }

  for (std::vector<SdpMedia>::const_iterator m = sdp.media.begin();
        m != sdp.media.end(); ++m)
  {
    /* only resume audio streams */
    if (m->isAudio()) {
      MediaActivity media_activity = getMediaActivity(*m);
      if (media_activity != Sendrecv && media_activity != Recvonly)
        return true;
    }
  }

  return false;
}

static bool isDSMEarlyAnnounceForced(const std::string &hdrs)
{
  string announce = getHeader(hdrs, SIP_HDR_P_DSM_APP);
  string p_dsm_app_param = get_header_param(announce, DSM_PARAM_EARLY_AN);
  return p_dsm_app_param == DSM_VALUE_FORCE;
}

////////////////////////////////////////////////////////////////////////////////

// callee
CallLeg::CallLeg(const CallLeg* caller, AmSipDialog* p_dlg, AmSipSubscription* p_subs)
  : AmB2BSession(caller->getLocalTag(),p_dlg,p_subs),
    call_status(Disconnected),
    on_hold(false),
    hold(PreserveHoldStatus),
    hold_type_requested(NonHold)
{
  a_leg = !caller->a_leg; // we have to be the complement

  set_sip_relay_only(false); // will be changed later on (for now we have no peer so we can't relay)

  // enable OA for the purpose of hold request detection
  if (dlg) {
    dlg->setOAEnabled(true);
    dlg->setOAForceSDP(false);
  } else {
    WARN("can't enable OA!\n");
  }

  // code below taken from createCalleeSession

  const AmSipDialog* caller_dlg = caller->dlg;

  dlg->setLocalTag(AmSession::getNewId());
  dlg->setCallid(AmSession::getNewId());

  // take important data from A leg
  dlg->setLocalParty(caller_dlg->getRemoteParty());
  dlg->setRemoteParty(caller_dlg->getLocalParty());
  dlg->setRemoteUri(caller_dlg->getLocalUri());

/*  if (AmConfig::LogSessions) {
    ILOG_DLG(L_INFO, "Starting B2B callee session %s\n",
	 getLocalTag().c_str());
  }

  MONITORING_LOG4(other_id.c_str(), 
		  "dir",  "out",
		  "from", dlg->local_party.c_str(),
		  "to",   dlg->remote_party.c_str(),
		  "ruri", dlg->remote_uri.c_str());
*/

  // copy common RTP relay settings from A leg
  //initRTPRelay(caller);
  vector<SdpPayload> lowfi_payloads;
  setRtpRelayMode(caller->getRtpRelayMode());
  setEnableDtmfTranscoding(caller->getEnableDtmfTranscoding());
  caller->getLowFiPLs(lowfi_payloads);
  setLowFiPLs(lowfi_payloads);

 
  // A->B
  SBCCallRegistry::addCall(caller_dlg->getLocalTag(),
			   SBCCallRegistryEntry(dlg->getCallid(), dlg->getLocalTag(), ""));
  // B->A
  SBCCallRegistry::addCall(dlg->getLocalTag(),
			   SBCCallRegistryEntry(caller_dlg->getCallid(), caller_dlg->getLocalTag(), caller_dlg->getRemoteTag()));

}

// caller
CallLeg::CallLeg(AmSipDialog* p_dlg, AmSipSubscription* p_subs)
  : AmB2BSession("",p_dlg,p_subs),
    call_status(Disconnected),
    on_hold(false),
    hold(PreserveHoldStatus),
    hold_type_requested(NonHold)
{
  a_leg = true;

  // At least in the first version we start relaying after the call is fully
  // established.  This is because of forking possibility - we can't simply
  // relay if we have one A leg and multiple B legs.
  // It is possible to start relaying before call is established if we have
  // exactly one B leg (i.e. no parallel fork happened).
  set_sip_relay_only(false);

  // enable OA for the purpose of hold request detection
  if (dlg) {
    dlg->setOAEnabled(true);
    dlg->setOAForceSDP(false);
  } else {
    WARN("can't enable OA!\n");
  }
}

CallLeg::~CallLeg()
{
  // do necessary cleanup (might be needed if the call leg is destroyed other
  // way then expected)
  for (vector<OtherLegInfo>::iterator i = other_legs.begin(); i != other_legs.end(); ++i) {
    i->releaseMediaSession();
  }

  while (!pending_updates.empty()) {
    SessionUpdate *u = pending_updates.front();
    pending_updates.pop_front();
    delete u;
  }

  SBCCallRegistry::removeCall(getLocalTag());
}

bool CallLeg::isHoldRequest(const AmSdp &sdp, holdMethod &method)
{
  /* set defaults from session parameters and attributes
   * inactive/sendonly/sendrecv/recvonly may be given as session attributes,
   * connection can be given for session as well */
  bool connection_active = connectionActive(sdp.conn, false /* empty connection like inactive? */);
  MediaActivity session_activity = getMediaActivity(sdp.attributes, Sendrecv);
  for (std::vector<SdpMedia>::const_iterator m = sdp.media.begin();
      m != sdp.media.end(); ++m)
  {
    if (m->port == 0) continue; /* this stream is disabled, handle like inactive (?) */
    if (!connectionActive(m->conn, connection_active)) {
      method = ZeroedConnection;
      continue;
    }
    switch (getMediaActivity(*m)) {
      case Sendonly:
        method = SendonlyStream;
        continue;
      case Inactive:
        method = InactiveStream;
        continue;
      case Recvonly:
        method = RecvonlyStream;
        return false; /* recvonly cannot provide moh */
      case Sendrecv:
        method = None;
        return false; /* media stream is active */
    }
  }
  if (sdp.media.empty()) {
    /* no streams in the SDP, needed to set the method somehow */
    if (!connection_active) method = ZeroedConnection;
    else {
      switch (session_activity) {
        case Sendonly:
          method = SendonlyStream;
          break;
        case Inactive:
          method = InactiveStream;
          break;
        case Recvonly:
          method = RecvonlyStream;
          return false; /* recvonly cannot provide moh */
        case Sendrecv:
          method = None;
          return false; /* media stream is active */
      }
    }
  }
  return true;
}

void CallLeg::terminateOtherLeg()
{
  if (call_status != Connected) {
    ILOG_DLG(L_DBG, "trying to terminate other leg in %s state -> terminating the others as well\n", callStatus2str(call_status));
    // FIXME: may happen when for example reply forward fails, do we want to terminate
    // all other legs in such case?
    terminateNotConnectedLegs(); // terminates all except the one identified by other_id
  }
  
  AmB2BSession::terminateOtherLeg();

  // remove this one from the list of other legs
  for (vector<OtherLegInfo>::iterator i = other_legs.begin(); i != other_legs.end(); ++i) {
    if (i->id == getOtherId()) {
      i->releaseMediaSession();
      other_legs.erase(i);
      break;
    }
  }

  // FIXME: call disconnect if connected (to put remote on hold)?
  if (getCallStatus() != Disconnected) updateCallStatus(Disconnected); // no B legs should be remaining
}

void CallLeg::terminateNotConnectedLegs()
{
  bool found = false;
  OtherLegInfo b;

  for (vector<OtherLegInfo>::iterator i = other_legs.begin(); i != other_legs.end(); ++i) {
    if (i->id != getOtherId()) {
      i->releaseMediaSession();
      AmSessionContainer::instance()->postEvent(i->id, new B2BEvent(B2BTerminateLeg));
    }
    else {
      found = true; // other_id is there
      b = *i;
    }
  }

  // quick hack to remove all terminated entries from the list
  other_legs.clear();
  if (found) other_legs.push_back(b);
}

void CallLeg::removeOtherLeg(const string &id)
{
  if (getOtherId() == id) AmB2BSession::clear_other();

  // remove the call leg from list of B legs
  for (vector<OtherLegInfo>::iterator i = other_legs.begin(); i != other_legs.end(); ++i) {
    if (i->id == id) {
      i->releaseMediaSession();
      other_legs.erase(i);
      break;
    }
  }

  /*if (terminate) AmSessionContainer::instance()->postEvent(id, new B2BEvent(B2BTerminateLeg));*/
}

// composed for caller and callee already
void CallLeg::onB2BEvent(B2BEvent* ev)
{
  switch (ev->event_id) {

    case B2BSipReply:
      onB2BReply(dynamic_cast<B2BSipReplyEvent*>(ev));
      break;

    case ConnectLeg:
      onB2BConnect(dynamic_cast<ConnectLegEvent*>(ev));
      break;

    case ReconnectLeg:
      onB2BReconnect(dynamic_cast<ReconnectLegEvent*>(ev));
      break;

    case ReplaceLeg:
      onB2BReplace(dynamic_cast<ReplaceLegEvent*>(ev));
      break;

    case ReplaceInProgress:
      onB2BReplaceInProgress(dynamic_cast<ReplaceInProgressEvent*>(ev));
      break;

    case DisconnectLeg:
      {
        DisconnectLegEvent *dle = dynamic_cast<DisconnectLegEvent*>(ev);
        if (dle) disconnect(dle->put_remote_on_hold, dle->preserve_media_session);
      }
      break;

    case ResumeHeldLeg:
      {
        ResumeHeldEvent *e = dynamic_cast<ResumeHeldEvent*>(ev);
        if (e) resumeHeld();
      }
      break;

    case ChangeRtpModeEventId:
      {
        ChangeRtpModeEvent *e = dynamic_cast<ChangeRtpModeEvent*>(ev);
        if (e) changeRtpMode(e->new_mode, e->media);
      }
      break;

      case ApplyPendingUpdatesEventId:
        if (dynamic_cast<ApplyPendingUpdatesEvent*>(ev)) applyPendingUpdate();
        break;

    case B2BSipRequest: {
      B2BSipRequestEvent *req_ev = dynamic_cast<B2BSipRequestEvent*>(ev);
      if (!sip_relay_only) {
        /** disable forwarding of relayed request if we are not connected [yet]
         *  (only we known that, the B leg has just delayed information about being
         *  connected to us and thus it can't set)
         *  Need not to be done if we have only one possible B leg so instead of
         *  checking call_status we can check if sip_relay_only is set or not
         */
        if (req_ev) req_ev->forward = false;
      } else {
        if (req_ev && req_ev->forward) {
          /* in case of already ongoing negotiation in the other leg */
          if (req_ev->req.method == SIP_METH_INVITE && dlg->getUACInvTransPending()) {
            /* if P-Force-491 is present, then it always marks a skip of 491 usage
             * for overlapping invite transactions (for the same other leg).
             * It's in use by the pick-up functionality, where two competing transactions
             * are sent towards caller to update his media capabilities */
            string p_force_491 = getHeader(req_ev->req.hdrs, SIP_HDR_P_FORCE_491, true);

            if (AmConfig::send_491_on_pending_session_leg && p_force_491 != "0") {
              /** do not send right away one more re-INVITE towards it,
               *  just delay the current leg, which sent us re-INVITE, with with a 491 response.
               */
              ILOG_DLG(L_DBG, "Cannot forward INVITE into another leg, already present pending session with it!\n");
              AmB2BSession::relayError(req_ev->req.method, req_ev->req.cseq, true, 491, SIP_REPLY_PENDING);
              return;
            } else {
              /** send a fake 200OK to the one who initiated media re-negotiation,
               *  in order to let it be sure the re-negotiation is completed, and then
               *  after a while, when an opposite leg is done with its pending transaction(s),
               *  propose it new media attributes. This behavior, however, can trigger issues with non
               *  matched ACK for the faked 200OK, in case B2B establishes other transactions
               *  within the period till ACK is received (for the fake 200OK).
               */
              ILOG_DLG(L_DBG, "Pending UAC INVITE transaction, planning session update (Reinvite) for later.\n");
              pending_updates.push_back(new Reinvite(req_ev->req.hdrs,
                                          req_ev->req.body, /* establishing = */ false,
                                          /* relayed */ false, /* r_cseq */ 0));
              ILOG_DLG(L_DBG, "For now replying with fake 200 OK.\n");
              acceptPendingInviteB2B(req_ev->req);
              return;
            }
          }
        }
      }
      // continue handling in AmB2bSession
      AmB2BSession::onB2BEvent(ev);
    } break;

    default:
      AmB2BSession::onB2BEvent(ev);
  }
}

int CallLeg::relaySipReply(AmSipReply &reply)
{
  std::map<int,AmSipRequest>::iterator t_req = recvd_req.find(reply.cseq);

  if (t_req == recvd_req.end()) {
    ILOG_DLG(L_ERR, "Request with CSeq %u not found in recvd_req.\n", reply.cseq);
    return 0; // ignore?
  }

  int res;
  AmSipRequest req(t_req->second);

  if ((reply.code >= 300) && (reply.code <= 305) && !reply.contact.empty()) {
    // relay with Contact in 300 - 305 redirect messages
    AmSipReply n_reply(reply);
    n_reply.hdrs += SIP_HDR_COLSP(SIP_HDR_CONTACT) + reply.contact + CRLF;

    res = relaySip(req, n_reply);
  }
  else res = relaySip(req, reply); // relay response directly

  return res;
}

bool CallLeg::setOther(const string &id, bool forward)
{
  if (getOtherId() == id) return true; // already set (needed when processing 2xx after 1xx)
  for (vector<OtherLegInfo>::iterator i = other_legs.begin(); i != other_legs.end(); ++i) {
    if (i->id == id) {
      setOtherId(id);
      clearRtpReceiverRelay(); // release old media session if set
      setMediaSession(i->media_session);
      if (forward && dlg->getOAState() == AmOfferAnswer::OA_Completed) {
        // reset OA state to offer_recived if already completed to accept new
        // B leg's SDP
        dlg->setOAState(AmOfferAnswer::OA_OfferRecved);
      }
      if (i->media_session) {
        ILOG_DLG(L_DBG, "connecting media session: %s to %s\n", 
            dlg->getLocalTag().c_str(), getOtherId().c_str());
        i->media_session->changeSession(a_leg, this);
      }
      else {
        // media session not set, set direct mode if not set already
        if (rtp_relay_mode != AmB2BSession::RTP_Direct) setRtpRelayMode(AmB2BSession::RTP_Direct);
      }
      set_sip_relay_only(true); // relay only from now on
      return true;
    }
  }
  ILOG_DLG(L_ERR, "%s is not in the list of other leg IDs!\n", id.c_str());
  return false; // something wrong?
}

void CallLeg::b2bInitial1xx(AmSipReply& reply, bool forward)
{
  // stop processing of 100 reply here or add Trying state to handle it without
  // remembering other_id (for now, the 100 won't get here, but to be sure...)
  // Warning: 100 reply may have to tag but forward is explicitly set to false,
  // so it can't be used to check whether it is related to a forwarded request
  // or not!
  if (reply.to_tag.empty() || reply.code == 100) return;

  if (call_status == NoReply) {
    ILOG_DLG(L_DBG, "1xx reply with to-tag received in NoReply state,"
        " changing status to Ringing and remembering the"
        " other leg ID (%s)\n", getOtherId().c_str());
    if (setOther(reply.from_tag, forward)) {
      updateCallStatus(Ringing, &reply);
      if (forward && relaySipReply(reply) != 0) stopCall(StatusChangeCause::InternalError);
    }
  }
  else {
    if (getOtherId() == reply.from_tag) {
      // we can relay this reply because it is from the same B leg from which
      // we already relayed something
      if (forward && relaySipReply(reply) != 0) stopCall(StatusChangeCause::InternalError);
    }
    else {
      // in Ringing state but the reply comes from another B leg than
      // previous 1xx reply => do not relay or process other way
      ILOG_DLG(L_DBG, "1xx reply received in %s state from another B leg, ignoring\n", callStatus2str(call_status));
    }
  }
}

void CallLeg::b2bInitial2xx(AmSipReply& reply, bool forward)
{
  if (!setOther(reply.from_tag, forward)) {
    /* ignore reply which comes from non-our-peer leg? */
    ILOG_DLG(L_DBG, "2xx reply received from unknown B leg, ignoring\n");
    return;
  }

  ILOG_DLG(L_DBG, "setting call status to connected with leg %s\n", getOtherId().c_str());

  /* terminate all other legs than the connected one (determined by other_id) */
  terminateNotConnectedLegs();

  /* connect media with the other leg if RTP relay is enabled */
  if (!other_legs.empty())
    other_legs.begin()->releaseMediaSession(); /* remove reference hold by OtherLegInfo */
  other_legs.clear(); /* no need to remember the connected leg here */

  onCallConnected(reply);

  if (!forward) {
    /* we need to generate re-INVITE based on received SDP
       but only if this is not previously faked 183 as 200OK, TT#187351 */
    saveSessionDescription(reply.body);
    sendEstablishedReInvite();
    if (dlg->getFaked183As200()) {
      ILOG_DLG(L_DBG, "Re-INVITE will be not send to update the leg, because there was a faked 183 as 200OK.\n");
      //dlg->setFaked183As200(false); // should we reset upon media re-negotiation in both legs?
                                      // but then it breaks the BYE -> CANCEL conversion.
    } else {
      ILOG_DLG(L_DBG, "Re-INVITE will be send to update the leg with the last media capabilities.\n");
      sendEstablishedReInvite();
    }

  } else if (relaySipReply(reply) != 0) {
    stopCall(StatusChangeCause::InternalError);
    return;
  }

  /* TT#187351, do not set the leg going towards sems DSM applications
   * to the connected state, if this has been previously faked (183 considered as 200OK)
   * otherwise call cancelation/termination will not work properly for this leg.
   */
  if (!dlg->getFaked183As200() && !dlg->getForcedEarlyAnnounce())
    updateCallStatus(Connected, &reply);
}

void CallLeg::onInitialReply(B2BSipReplyEvent *e)
{
  /* 100-199 */
  if (e->reply.code < 200) {
    dlg->setForcedEarlyAnnounce(isDSMEarlyAnnounceForced(e->reply.hdrs));

    /* exceptionally treat 183 with the 'P-DSM-App: <app-name>;early-announce=force',
       similarly to the 200OK response, this will properly update the caller
       with the late SDP capabilities (an early announcement),
       which has been put on hold during the transfer

       DSM applications using it:
       - early-dbprompt (early_announce)
       - pre-announce
       - play-last-caller
       - office-hours */
    if (e->reply.code == 183 && dlg->getForcedEarlyAnnounce()) {
      b2bInitial2xx(e->reply, e->forward);
    } else {
      b2bInitial1xx(e->reply, e->forward);
    }
  }

  /* 200-299 */
  else if (e->reply.code < 300) {
    b2bInitial2xx(e->reply, e->forward);
  }

  /* 300-699 */
  else {
    b2bInitialErr(e->reply, e->forward);
  }
}

void CallLeg::b2bInitialErr(AmSipReply& reply, bool forward)
{
  if (getCallStatus() == Ringing && getOtherId() != reply.from_tag) {
    removeOtherLeg(reply.from_tag); // we don't care about this leg any more
    onBLegRefused(reply); // new B leg(s) may be added
    ILOG_DLG(L_DBG, "dropping non-ok reply, it is not from current peer\n");
    return;
  }

  ILOG_DLG(L_DBG, "clean-up after non-ok reply (reply: %d, status %s, other: %s)\n", 
      reply.code, callStatus2str(getCallStatus()),
      getOtherId().c_str());
  clearRtpReceiverRelay();
  removeOtherLeg(reply.from_tag); // we don't care about this leg any more
  updateCallStatus(NoReply, &reply);
  onBLegRefused(reply); // possible serial fork here
  set_sip_relay_only(false);

  // there are other B legs for us => wait for their responses and do not
  // relay current response
  if (!other_legs.empty()) return;

  onCallFailed(CallRefused, &reply);
  if (forward) relaySipReply(reply);

  // no other B legs, terminate
  updateCallStatus(Disconnected, &reply);
  stopCall(&reply);
}

// was for caller only
void CallLeg::onB2BReply(B2BSipReplyEvent *ev)
{
  if (!ev) {
    ILOG_DLG(L_ERR, "BUG: invalid argument given\n");
    return;
  }

  AmSipReply& reply = ev->reply;

  ILOG_DLG(L_DBG, "%s: B2B SIP reply %d/%d %s received in %s state\n",
      getLocalTag().c_str(),
      reply.code, reply.cseq, reply.cseq_method.c_str(),
      callStatus2str(call_status));

  // FIXME: testing est_invite_cseq is wrong! (checking in what direction or
  // what role would be needed)
  bool initial_reply = (reply.cseq_method == SIP_METH_INVITE &&
      (call_status == NoReply || call_status == Ringing) &&
      ((reply.cseq == est_invite_cseq && ev->forward) || // related to initial INVITE at our side
       (!ev->forward))); // connect not related to initial INVITE at our side

  if (initial_reply) {
    // handle relayed initial replies (replies to initiating INVITE at the other
    // side, note that this need not to be initiating INVITE at our side)

    ILOG_DLG(L_DBG, "established CSeq: %d, forward: %s\n", est_invite_cseq, ev->forward ? "yes": "no");

    onInitialReply(ev);
  }
  else {
    // handle non-initial replies

    // reply not from our peer (might be one of the discarded ones)
    if (getOtherId() != ev->sender_ltag && getOtherId() != reply.from_tag) {
      ILOG_DLG(L_DBG, "ignoring reply from %s in %s state, other_id = '%s'\n",
	    reply.from_tag.c_str(), callStatus2str(call_status), getOtherId().c_str());
      return;
    }

    // handle replies to other requests than the initial one
    ILOG_DLG(L_DBG, "handling reply via AmB2BSession\n");
    AmB2BSession::onB2BEvent(ev);
  }
}

// TODO: original callee's version, update
void CallLeg::onB2BConnect(ConnectLegEvent* co_ev)
{
  if (!co_ev) {
    ILOG_DLG(L_ERR, "BUG: invalid argument given\n");
    return;
  }

  if (call_status != Disconnected) {
    ILOG_DLG(L_ERR, "BUG: ConnectLegEvent received in %s state\n", callStatus2str(call_status));
    return;
  }

  MONITORING_LOG3(getLocalTag().c_str(), 
		  "b2b_leg", getOtherId().c_str(),
		  "to", dlg->getRemoteParty().c_str(),
		  "ruri", dlg->getRemoteUri().c_str());

  // This leg is marked as 'relay only' since the beginning because it might
  // need not to know on time that it is connected and thus should relay.
  //
  // For example: B leg received 2xx reply, relayed it to A leg and is
  // immediatelly processing in-dialog request which should be relayed, but
  // A leg didn't have chance to process the relayed reply so the B leg is not
  // connected to the A leg yet when handling the in-dialog request.
  set_sip_relay_only(true); // we should relay everything to the other leg from now

  AmMimeBody body(co_ev->body);
  try {
    updateLocalBody(body);
  } catch (const string& s) {
    relayError(SIP_METH_INVITE, co_ev->r_cseq, true, 500, SIP_REPLY_SERVER_INTERNAL_ERROR);
    throw;
  }

  int res = dlg->sendRequest(SIP_METH_INVITE, &body,
      co_ev->hdrs, SIP_FLAGS_VERBATIM);
  if (res < 0) {
    ILOG_DLG(L_DBG, "sending INVITE failed, relaying back error reply\n");
    relayError(SIP_METH_INVITE, co_ev->r_cseq, true, res);

    stopCall(StatusChangeCause::InternalError);
    return;
  }

  updateCallStatus(NoReply);

  if (co_ev->relayed_invite) {
    AmSipRequest fake_req;
    fake_req.method = SIP_METH_INVITE;
    fake_req.cseq = co_ev->r_cseq;
    relayed_req[dlg->cseq - 1] = fake_req;
    est_invite_other_cseq = co_ev->r_cseq;
  }
  else est_invite_other_cseq = 0;

  if (!co_ev->body.empty()) {
    saveSessionDescription(co_ev->body);
  }

  // save CSeq of establising INVITE
  est_invite_cseq = dlg->cseq - 1;
}

void CallLeg::onB2BReconnect(ReconnectLegEvent* ev)
{
  if (!ev) {
    ILOG_DLG(L_ERR, "BUG: invalid argument given\n");
    return;
  }
  ILOG_DLG(L_DBG, "handling ReconnectLegEvent, other: %s, connect to %s\n", 
	getOtherId().c_str(), ev->session_tag.c_str());

  ev->markAsProcessed();

  // release old signaling and media session
  clear_other();
  clearRtpReceiverRelay();
  relayed_req.clear();

  // check if we aren't processing INVITE now (BLF ringing call pickup)
  AmSipRequest *invite = dlg->getUASPendingInv();
  bool is_pending_call = (NULL != invite);
  if (is_pending_call) {
    // the Re-INVITE to be used
    string hdrs = ev->hdrs;
    if (ev->relayed_invite)
      hdrs += SIP_HDR_COLSP(SIP_HDR_P_FORCE_491) "0" CRLF;
    SessionUpdate *u = new Reinvite(hdrs, ev->body, true, ev->relayed_invite, ev->r_cseq);

    ILOG_DLG(L_DBG, "INVITE pending - planning session update with SDP from INVITE+replaces for later for ltag %s",
	  getLocalTag().c_str());

    /* we aren't ready to relay to the other leg yet (to the originator of reconnection) */
    set_sip_relay_only(false);

    pending_updates.push_back(u);
    ILOG_DLG(L_DBG, "INVITE pending - accepting with fake SDP\n");
    // remember SDP origin of the other side for our requests
    AmMimeBody *sdp = ev->body.hasContentType(SIP_APPLICATION_SDP);
    if (sdp) {
      AmSdp parsed_sdp;
      if (parsed_sdp.parse((const char*)sdp->getPayload()) == 0) {
        saveLocalSdpOrigin(parsed_sdp);
      }
    }
    acceptPendingInvite(invite);
  }

  setOtherId(ev->session_tag);
  if (ev->role == ReconnectLegEvent::A) a_leg = true;
  else a_leg = false;
  // FIXME: What about calling SBC CC modules in this case? Original CC
  // interface is called from A leg only and it might happen that we were call
  // leg A before.

  set_sip_relay_only(true); // we should relay everything to the other leg from now
  updateCallStatus(NoReply);

  // use new media session if given
  setRtpRelayMode(ev->rtp_mode);
  if (ev->media) {
    setMediaSession(ev->media);
    getMediaSession()->changeSession(a_leg, this);
  }

  MONITORING_LOG3(getLocalTag().c_str(),
		  "b2b_leg", getOtherId().c_str(),
		  "to", dlg->getRemoteParty().c_str(),
		  "ruri", dlg->getRemoteUri().c_str());

  if (!is_pending_call) {
    ILOG_DLG(L_DBG, "updating session with SDP from INVITE+replaces for ltag %s", getLocalTag().c_str());
    // the Re-INVITE to be used
    SessionUpdate *u = new Reinvite(ev->hdrs, ev->body, true, ev->relayed_invite, ev->r_cseq);
    updateSession(u);
  }
}

void CallLeg::onB2BReplace(ReplaceLegEvent *e)
{
  if (!e) {
    ILOG_DLG(L_ERR, "BUG: invalid argument given\n");
    return;
  }
  e->markAsProcessed();

  ReconnectLegEvent *reconnect = e->getReconnectEvent();
  if (!reconnect) {
    ILOG_DLG(L_ERR, "BUG: invalid ReconnectLegEvent\n");
    return;
  }

  ILOG_DLG(L_DBG, "handling ReplaceLegEvent, other: %s, connect to %s\n", 
	getOtherId().c_str(), reconnect->session_tag.c_str());

  string id(getOtherId());
  if (id.empty()) {
    // try it with the first B leg?
    if (other_legs.empty()) {
      ILOG_DLG(L_ERR, "BUG: there is no B leg to connect our replacement to\n");
      return;
    }
    id = other_legs[0].id;
  }

  // send session ID of the other leg to the originator
  AmSessionContainer::instance()->postEvent(reconnect->session_tag, new ReplaceInProgressEvent(id));

  // send the ReconnectLegEvent to the other leg
  AmSessionContainer::instance()->postEvent(id, reconnect);

  // remove the B leg from our B leg list
  removeOtherLeg(id);

  // commit suicide if our last B leg is stolen
  if (other_legs.empty() && getOtherId().empty()) stopCall(StatusChangeCause::Other /* FIXME? */);
}

void CallLeg::onB2BReplaceInProgress(ReplaceInProgressEvent *e)
{
  for (vector<OtherLegInfo>::iterator i = other_legs.begin(); i != other_legs.end(); ++i) {
    if (i->id.empty()) {
      // replace the temporary (invalid) session with the correct one
      i->id = e->dst_session;
      return;
    }
  }
}

void CallLeg::disconnect(bool hold_remote, bool preserve_media_session)
{
  ILOG_DLG(L_DBG, "disconnecting call leg %s from the other\n", getLocalTag().c_str());

  switch (call_status) {
    case Disconnecting:
    case Disconnected:
      ILOG_DLG(L_DBG, "trying to disconnect already disconnected (or disconnecting) call leg\n");
      return;

    case NoReply:
    case Ringing:
      ILOG_DLG(L_WARN, "trying to disconnect in not connected state, terminating not connected legs in advance (was it intended?)\n");
      terminateNotConnectedLegs();
      // do not break, continue with following state handling!

    case Connected:
      if (!preserve_media_session) {
        // we can't stay connected (at media level) with the other leg
        clearRtpReceiverRelay();
      }
      break; // this is OK
  }

  // create new media session for us if needed
  if (getRtpRelayMode() != RTP_Direct && !preserve_media_session)
    setMediaSession(new AmB2BMedia(a_leg ? this: NULL, a_leg ? NULL : this));

  clear_other();
  set_sip_relay_only(false); // we can't relay once disconnected
  est_invite_cseq = 0; // attempt to invalidate though 0 is valid value
  relayed_req.clear(); // do not forward anything back any more

  if (!hold_remote || isOnHold()) updateCallStatus(Disconnected);
  else {
    updateCallStatus(Disconnecting);
    putOnHold();
  }
}

static void sdp2body(const AmSdp &sdp, AmMimeBody &body)
{
  string body_str;
  sdp.print(body_str);

  AmMimeBody *s = body.hasContentType(SIP_APPLICATION_SDP);
  if (s) s->parse(SIP_APPLICATION_SDP, (const unsigned char*)body_str.c_str(), body_str.length());
  else body.parse(SIP_APPLICATION_SDP, (const unsigned char*)body_str.c_str(), body_str.length());
}

int CallLeg::putOnHoldImpl()
{
  if (on_hold) return -1; // no request went out

  ILOG_DLG(L_DBG, "putting remote on hold\n");
  hold = HoldRequested;

  holdRequested();

  AmSdp sdp;
  createHoldRequest(sdp);
  updateLocalSdp(sdp);
  updateLocalSdpOrigin(sdp);

  AmMimeBody body;
  sdp2body(sdp, body);
  if (dlg->reinvite("", &body, SIP_FLAGS_VERBATIM) != 0) {
    ILOG_DLG(L_ERR, "re-INVITE failed\n");
    offerRejected();
    return -1;
  }
  return dlg->cseq - 1;
}

int CallLeg::resumeHeldImpl()
{
  if (!on_hold) return -1;

  try {
    ILOG_DLG(L_DBG, "resume held remote\n");
    hold = ResumeRequested;

    resumeRequested();

    AmSdp sdp;
    createResumeRequest(sdp);
    if (sdp.media.empty()) {
      ILOG_DLG(L_ERR, "invalid un-hold SDP, can't unhold\n");
      offerRejected();
      return -1;
    }
    updateLocalSdp(sdp);
    updateLocalSdpOrigin(sdp);

    AmMimeBody body(dlg->established_body);
    sdp2body(sdp, body);
    if (dlg->reinvite("", &body, SIP_FLAGS_VERBATIM) != 0) {
      ILOG_DLG(L_ERR, "re-INVITE failed\n");
      offerRejected();
      return -1;
    }
    return dlg->cseq - 1;
  }
  catch (...) {
    offerRejected();
    return -1;
  }
}

void CallLeg::holdAccepted()
{
  ILOG_DLG(L_DBG, "hold accepted on %c leg\n", a_leg?'B':'A');
  if (call_status == Disconnecting) updateCallStatus(Disconnected);
  on_hold = true;
  AmB2BMedia *ms = getMediaSession();
  if (ms) {
    ILOG_DLG(L_DBG, "holdAccepted - mute %c leg\n", a_leg?'B':'A');
    ms->mute(!a_leg); // mute the stream in other (!) leg
  }
}

void CallLeg::holdRejected()
{
  if (call_status == Disconnecting) updateCallStatus(Disconnected);
}

void CallLeg::resumeAccepted()
{
  on_hold = false;
  AmB2BMedia *ms = getMediaSession();
  if (ms) ms->unmute(!a_leg); // unmute the stream in other (!) leg
  ILOG_DLG(L_DBG, "%s: resuming held, unmuting media session %p(%s)\n", getLocalTag().c_str(), ms, !a_leg ? "A" : "B");
}

/* was for caller only */
void CallLeg::onInvite(const AmSipRequest& req)
{
  /* do not call AmB2BSession::onInvite(req); we changed the behavior
   * this method is not called for re-INVITEs because once connected we are in
   * sip_relay_only mode and the re-INVITEs are relayed instead of processing
   * (see AmB2BSession::onSipRequest) */

  if (call_status == Disconnected) { /* for initial INVITE only */
    est_invite_cseq = req.cseq; /* remember initial CSeq */
    /* initialize RTP relay */

    /* relayed INVITE - we need to add the original INVITE to
     * list of received (relayed) requests */
    recvd_req.insert(std::make_pair(req.cseq, req));
  }
}

void CallLeg::onSipRequest(const AmSipRequest& req)
{
  ILOG_DLG(L_DBG, "%s: SIP request %d %s received in %s state\n",
        getLocalTag().c_str(), req.cseq,
        req.method.c_str(), callStatus2str(call_status));

  /* we need to handle cases if there is no other leg (for example call parking)
   * Note that setting sip_relay_only to false in this case doesn't solve the
   * problem because AmB2BSession always tries to relay the request into the
   * other leg. */
  if ((getCallStatus() == Disconnected || getCallStatus() == Disconnecting)
        && getOtherId().empty())
  {
    ILOG_DLG(L_DBG, "handling request %s in disconnected state", req.method.c_str());

    /* this is not correct but what is?
     * handle reINVITEs within B2B call with no other leg */
    if (req.method == SIP_METH_INVITE && dlg->getStatus() == AmBasicSipDialog::Connected) {
      try {
        dlg->reply(req, 500, SIP_REPLY_SERVER_INTERNAL_ERROR);
      }
      catch(...) {
        ILOG_DLG(L_ERR, "exception when handling INVITE in disconnected state");
        dlg->reply(req, 500, SIP_REPLY_SERVER_INTERNAL_ERROR);
        /* stop the call? */
      }
    } else {
      AmSession::onSipRequest(req);
    }

    /* is this needed? */
    if (req.method == SIP_METH_BYE)
      stopCall(&req);

  } else {
    if (getCallStatus() == Ringing && !getOtherId().empty() && req.method == SIP_METH_BYE) {
        dlg->reply(req, 200, "OK");
        stopCall(&req);

    } else if(getCallStatus() == Disconnected && req.method == SIP_METH_BYE) {
      /* seems that we have already sent/received a BYE
       * -> we'd better terminate this ASAP
       *    to avoid other confusions... */
      dlg->reply(req, 200, "OK");

    } else {
      /** only for requests which put the call on hold.
       *  Remember that we have to answer to the one, who puts on hold,
       *  with the 'inactive' back (as soon as the on hold is accepted with the 200OK
       *  by the other side of the call) in case the on hold was requested using 'inactive'
       */
      AmSdp sdp;
      holdMethod hold_method;

      if (req.method == SIP_METH_INVITE && retrieveAmSdp(req.body, sdp)) {

        /* remember the hold status (we want to catch recvonly state of hold),
         * in order to be able to ignore it gracefully during ongoing MoH towards this leg.
         */
        bool already_recvonly_hold = (on_hold && hold_type_requested == NonHold);

        /* in case this INVITE puts on hold, update the method */
        hold_method = updateHoldMethod(sdp);

        /* TODO: do we really need it? There is no MoH handling in this version of SEMS */
      }

      AmB2BSession::onSipRequest(req);
    }
  }
}

void CallLeg::onSipReply(const AmSipRequest& req, const AmSipReply& reply, AmSipDialog::Status old_dlg_status)
{
  TransMap::iterator t = relayed_req.find(reply.cseq);
  bool relayed_request = (t != relayed_req.end());

  ILOG_DLG(L_DBG, "%s: SIP reply %d/%d %s (%s) received in %s state\n",
      getLocalTag().c_str(),
      reply.code, reply.cseq, reply.cseq_method.c_str(),
      (relayed_request ? "to relayed request" : "to locally generated request"),
      callStatus2str(call_status));

#if 0
  if ((oa.hold != OA::PreserveHoldStatus) && (!relayed_request)) {
    ILOG_DLG(L_INFO, "locally generated hold/resume request replied, not handling by B2B\n");
    // local hold/resume request replied, we don't want to relay this reply to the other leg!
    // => do the necessary stuff here (copy & paste from AmB2BSession::onSipReply)
    if (reply.code < 300) {
      const AmMimeBody *sdp_part = reply.body.hasContentType(SIP_APPLICATION_SDP);
      if (sdp_part) {
        AmSdp sdp;
        if (sdp.parse((const char *)sdp_part->getPayload()) == 0) updateRemoteSdp(sdp);
      }
    }
    else if (reply.code >= 300) offerRejected();
    AmSession::onSipReply(req, reply, old_dlg_status);
    return;
  }
#endif
  if (reply.code >= 300 && reply.cseq_method == SIP_METH_INVITE) offerRejected();

  // handle final replies of session updates in progress
  if (!pending_updates.empty() && reply.code >= 200 && pending_updates.front()->hasCSeq(reply.cseq)) {
    if (reply.code == 491) {
      pending_updates.front()->reset();
      double t = get491RetryTime();
      pending_updates_timer.start(getLocalTag(), t);
      ILOG_DLG(L_DBG, "planning to retry update operation in %gs", t);
    }
    else {
      // TODO: 503, ...
      delete pending_updates.front();
      pending_updates.pop_front();
    }
  }

  AmB2BSession::onSipReply(req, reply, old_dlg_status);

  // update internal state and call related callbacks based on received reply
  // (i.e. B leg in case of initial INVITE)
  if (reply.cseq == est_invite_cseq && reply.cseq_method == SIP_METH_INVITE &&
    (call_status == NoReply || call_status == Ringing)) {
    // reply to the initial request
    if ((reply.code > 100) && (reply.code < 200)) {
      if (((call_status == NoReply)) && (!reply.to_tag.empty()))
        updateCallStatus(Ringing, &reply);
    }
    else if ((reply.code >= 200) && (reply.code < 300)) {
      onCallConnected(reply);
      updateCallStatus(Connected, &reply);
    }
    else if (reply.code >= 300) {
      updateCallStatus(Disconnected, &reply);
      terminateLeg(); // commit suicide (don't let the master to kill us)
    }
  }

  // update call registry (unfortunately has to be done always -
  // not possible to determine if learned in this reply (?))
  if (!dlg->getRemoteTag().empty() && reply.code >= 200 && req.method == SIP_METH_INVITE) {
    SBCCallRegistry::updateCall(getOtherId(), dlg->getRemoteTag());
  }

}

// was for caller only
void CallLeg::onInvite2xx(const AmSipReply& reply)
{
  // We don't want to remember reply.cseq as est_invite_cseq, do we? It was in
  // AmB2BCallerSession but we already have initial INVITE cseq remembered and
  // we don't need to change it to last reINVITE one, right? Otherwise we should
  // remember UPDATE cseq as well because SDP may change by it as well (used
  // when handling B2BSipReply in AmB2BSession to check if reINVITE should be
  // sent).
  // 
  // est_invite_cseq = reply.cseq;

  // we don't want to handle the 2xx using AmSession so the following may be
  // unwanted for us:
  // 
  AmB2BSession::onInvite2xx(reply);
}

void CallLeg::onCancel(const AmSipRequest& req)
{
  // initial INVITE handling
  if ((call_status == Ringing) || (call_status == NoReply)) {
    if (a_leg) {
      // terminate whole B2B call if the caller receives CANCEL
      onCallFailed(CallCanceled, NULL);
      updateCallStatus(Disconnected, StatusChangeCause::Canceled);
      stopCall(StatusChangeCause::Canceled);
    }
    // else { } ... ignore for B leg
  }
}

void CallLeg::terminateLeg()
{
  AmB2BSession::terminateLeg();
}

// was for caller only
void CallLeg::onRemoteDisappeared(const AmSipReply& reply) 
{
  if (call_status == Connected) {
    // only in case we are really connected
    // (called on timeout or 481 from the remote)

    ILOG_DLG(L_DBG, "remote unreachable, ending B2BUA call\n");
    // FIXME: shouldn't be cleared in AmB2BSession as well?
    clearRtpReceiverRelay(); 
    AmB2BSession::onRemoteDisappeared(reply); // terminates the other leg
    updateCallStatus(Disconnected, &reply);
  }
}

// was for caller only
void CallLeg::onBye(const AmSipRequest& req)
{
  terminateNotConnectedLegs();
  updateCallStatus(Disconnected, &req);
  clearRtpReceiverRelay(); // FIXME: shouldn't be cleared in AmB2BSession as well?
  AmB2BSession::onBye(req);
}

void CallLeg::onOtherBye(const AmSipRequest& req)
{
  updateCallStatus(Disconnected, &req);
  AmB2BSession::onOtherBye(req);
}

void CallLeg::onNoAck(unsigned int cseq)
{
  updateCallStatus(Disconnected, StatusChangeCause::NoAck);
  AmB2BSession::onNoAck(cseq);
}

void CallLeg::onNoPrack(const AmSipRequest &req, const AmSipReply &rpl)
{
  updateCallStatus(Disconnected, StatusChangeCause::NoPrack);
  AmB2BSession::onNoPrack(req, rpl);
}

void CallLeg::onRtpTimeout()
{
  updateCallStatus(Disconnected, StatusChangeCause::RtpTimeout);
  AmB2BSession::onRtpTimeout();
}

void CallLeg::onSessionTimeout()
{
  updateCallStatus(Disconnected, StatusChangeCause::SessionTimeout);
  AmB2BSession::onSessionTimeout();
}
// AmMediaSession interface from AmMediaProcessor
int CallLeg::readStreams(unsigned long long ts, unsigned char *buffer) {
  // skip RTP processing if in Relay mode
  // (but we want to process DTMF thus we may be in media processor)
  if (getRtpRelayMode()==RTP_Relay)
    return 0;
  return AmB2BSession::readStreams(ts, buffer);
}

int CallLeg::writeStreams(unsigned long long ts, unsigned char *buffer) {
  // skip RTP processing if in Relay mode
  // (but we want to process DTMF thus we may be in media processor)
  if (getRtpRelayMode()==RTP_Relay)
    return 0;
  return AmB2BSession::writeStreams(ts, buffer);
}

void CallLeg::addNewCallee(CallLeg *callee, ConnectLegEvent *e,
			   AmB2BSession::RTPRelayMode mode)
{
  OtherLegInfo b;
  b.id = callee->getLocalTag();

  callee->setRtpRelayMode(mode);
  if (mode != RTP_Direct) {
    // do not initialise the media session with A leg to avoid unnecessary A leg
    // RTP stream creation in every B leg's media session
    if (a_leg) b.media_session = new AmB2BMedia(NULL, callee);
    else b.media_session = new AmB2BMedia(callee, NULL);
    b.media_session->addReference(); // new reference for me
    callee->setMediaSession(b.media_session);
  }
  else b.media_session = NULL;
  other_legs.push_back(b);

  if (AmConfig::LogSessions) {
    ILOG_DLG(L_DBG, "Starting B2B callee session %s\n",
	 callee->getLocalTag().c_str()/*, invite_req.cmd.c_str()*/);
  }

  AmSipDialog* callee_dlg = callee->dlg;
  MONITORING_LOG4(b.id.c_str(),
		  "dir",  "out",
		  "from", callee_dlg->getLocalParty().c_str(),
		  "to",   callee_dlg->getRemoteParty().c_str(),
		  "ruri", callee_dlg->getRemoteUri().c_str());

  callee->start();

  AmSessionContainer* sess_cont = AmSessionContainer::instance();
  sess_cont->addSession(b.id, callee);

  // generate connect event to the newly added leg
  // Warning: correct callee's role must be already set (in constructor or so)
  ILOG_DLG(L_DBG, "relaying connect leg event to the new leg\n");
  // other stuff than relayed INVITE should be set directly when creating callee
  // (remote_uri, remote_party is not propagated and thus B2BConnectEvent is not
  // used because it would just overwrite already set things. Note that in many
  // classes derived from AmB2BCaller[Callee]Session was a lot of things set
  // explicitly)
  AmSessionContainer::instance()->postEvent(b.id, e);

  if (call_status == Disconnected) updateCallStatus(NoReply);
}

void CallLeg::setCallStatus(CallStatus new_status)
{
  call_status = new_status;
}

const char* CallLeg::getCallStatusStr() {
  switch(getCallStatus()) {
  case Disconnected : return "Disconnected";
  case NoReply : return "NoReply";
  case Ringing : return "Ringing";
  case Connected : return "Connected";
  case Disconnecting : return "Disconnecting";
  default: return "Unknown";
  };
}

void CallLeg::updateCallStatus(CallStatus new_status, const StatusChangeCause &cause)
{
  if (new_status == Connected)
    ILOG_DLG(L_DBG, "%s leg %s changing status from %s to %s with %s\n",
        a_leg ? "A" : "B",
        getLocalTag().c_str(),
        callStatus2str(call_status),
        callStatus2str(new_status),
        getOtherId().c_str());
  else
    ILOG_DLG(L_DBG, "%s leg %s changing status from %s to %s\n",
        a_leg ? "A" : "B",
        getLocalTag().c_str(),
        callStatus2str(call_status),
        callStatus2str(new_status));

  setCallStatus(new_status);
  onCallStatusChange(cause);
}

CallLeg::holdMethod CallLeg::updateHoldMethod(const AmSdp &sdp)
{
  holdMethod hold_method;

  /* just get the hold method */
  isHoldRequest(sdp, hold_method);

  switch (hold_method)
  {
    case SendonlyStream:
      hold_type_requested = SendonlyHold;
      break;
    case InactiveStream:
      hold_type_requested = InactiveHold;
      break;
    case ZeroedConnection:
      hold_type_requested = ZeroedHold;
      break;
    /* RecvonlyStream, None and sendrecv cases considered as non-holding */
    default:
      hold_type_requested = NonHold;
      break;
  }

  if (hold_type_requested != NonHold) {
    ILOG_DLG(L_DBG, "hold_type_requested is set to: <%d> for LT <%s>\n",
        hold_type_requested, getLocalTag().c_str());
  }
  return hold_method;
}

bool CallLeg::retrieveAmSdp(const AmMimeBody &mSdp, AmSdp &sdp)
{
  AmMimeBody t_sdp_body = mSdp;
  AmMimeBody * sdp_body = t_sdp_body.hasContentType(SIP_APPLICATION_SDP);
  if (!sdp_body) {
    ILOG_DLG(L_DBG, "Failed to parse SDP body while retrieving into AmSdp!\n");
    return false;
  }
  if (sdp.parse((const char *)sdp_body->getPayload())) {
    ILOG_DLG(L_DBG, "Failed to parse SDP body while retrieving into AmSdp!\n");
    return false;
  }
  return true; /* parsed successfully */
}

void CallLeg::addExistingCallee(const string &session_tag, ReconnectLegEvent *ev)
{
  // add existing session as our B leg

  OtherLegInfo b;
  b.id = session_tag;
  if (rtp_relay_mode != RTP_Direct) {
    // do not initialise the media session with A leg to avoid unnecessary A leg
    // RTP stream creation in every B leg's media session
    b.media_session = new AmB2BMedia(NULL, NULL);
    b.media_session->addReference(); // new reference for me
  }
  else b.media_session = NULL;

  // generate connect event to the newly added leg
  ILOG_DLG(L_DBG, "relaying re-connect leg event to the B leg\n");
  ev->setMedia(b.media_session, rtp_relay_mode);
  // TODO: what about the RTP relay and other settings? send them as well?
  if (!AmSessionContainer::instance()->postEvent(session_tag, ev)) {
    // session doesn't exist - can't connect
    ILOG_DLG(L_INFO, "the B leg to connect to (%s) doesn't exist\n", session_tag.c_str());
    if (b.media_session) {
      b.media_session->releaseReference();
      b.media_session = NULL; // ptr may not be valid any more
    }
    return;
  }

  other_legs.push_back(std::move(b));
  if (call_status == Disconnected) updateCallStatus(NoReply);
}

void CallLeg::addCallee(const string &session_tag, const AmSipRequest &relayed_invite)
{
  addExistingCallee(session_tag, new ReconnectLegEvent(getLocalTag(), relayed_invite));
}

void CallLeg::addCallee(CallLeg *callee, const string &hdrs)
{
  if (!non_hold_sdp.media.empty()) {
    // use non-hold SDP if possible
    AmMimeBody body(dlg->established_body);
    sdp2body(non_hold_sdp, body);
    addNewCallee(callee, new ConnectLegEvent(hdrs, body));
  }
  else addNewCallee(callee, new ConnectLegEvent(hdrs, dlg->established_body));
}

/*void CallLeg::addCallee(CallLeg *callee, const string &hdrs, AmB2BSession::RTPRelayMode mode)
{
  addNewCallee(callee, new ConnectLegEvent(hdrs, dlg->established_body), mode);
}*/

void CallLeg::replaceExistingLeg(const string &session_tag, const AmSipRequest &relayed_invite)
{
  // add existing session as our B leg

  OtherLegInfo b;
  b.id.clear(); // this is an invalid local tag (temporarily)
  if (rtp_relay_mode != RTP_Direct) {
    // let the other leg to set its part, we will set our once connected
    b.media_session = new AmB2BMedia(NULL, NULL);
    b.media_session->addReference(); // new reference for me
  }
  else b.media_session = NULL;

  ReplaceLegEvent *ev = new ReplaceLegEvent(getLocalTag(), relayed_invite, b.media_session, rtp_relay_mode);
  // TODO: what about the RTP relay and other settings? send them as well?
  if (!AmSessionContainer::instance()->postEvent(session_tag, ev)) {
    // session doesn't exist - can't connect
    ILOG_DLG(L_INFO, "the call leg to be replaced (%s) doesn't exist\n", session_tag.c_str());
    if (b.media_session) {
      b.media_session->releaseReference();
      b.media_session = NULL;
    }
    return;
  }

  other_legs.push_back(std::move(b));
  if (call_status == Disconnected) updateCallStatus(NoReply); // we are something like connected to another leg
}

void CallLeg::replaceExistingLeg(const string &session_tag, const string &hdrs)
{
  // add existing session as our B leg

  OtherLegInfo b;
  b.id.clear(); // this is an invalid local tag (temporarily)
  if (rtp_relay_mode != RTP_Direct) {
    // let the other leg to set its part, we will set our once connected
    b.media_session = new AmB2BMedia(NULL, NULL);
    b.media_session->addReference(); // new reference for me
  }
  else b.media_session = NULL;

  auto rev = std::make_unique<ReconnectLegEvent>(a_leg ? ReconnectLegEvent::B : ReconnectLegEvent::A, getLocalTag(), hdrs, dlg->established_body);

  rev->setMedia(b.media_session, rtp_relay_mode);

  auto ev = std::make_unique<ReplaceLegEvent>(getLocalTag(), rev.release());

  // TODO: what about the RTP relay and other settings? send them as well?
  if (!AmSessionContainer::instance()->postEvent(session_tag, ev.release())) {
    // session doesn't exist - can't connect
    ILOG_DLG(L_INFO, "the call leg to be replaced (%s) doesn't exist\n", session_tag.c_str());
    if (b.media_session) {
      b.media_session->releaseReference();
      b.media_session = NULL;
    }
    return;
  }

  other_legs.push_back(std::move(b));
  if (call_status == Disconnected) updateCallStatus(NoReply); // we are something like connected to another leg
}

void CallLeg::clear_other()
{
  removeOtherLeg(getOtherId());
  AmB2BSession::clear_other();
}

void CallLeg::stopCall(const StatusChangeCause &cause) {
  if (getCallStatus() != Disconnected) updateCallStatus(Disconnected, cause);
  terminateNotConnectedLegs();
  terminateOtherLeg();
  terminateLeg();
}

void CallLeg::changeRtpMode(RTPRelayMode new_mode)
{
  if (new_mode == rtp_relay_mode) return; // requested mode is set already

  // we don't need to send reINVITE from here, expecting caller knows what is he
  // doing (it is probably processing or generating its own reINVITE)
  // Switch from RTP_Direct to RTP_Relay is safe (no audio loss), the other can
  // be lossy because already existing media object would be destroyed.
  // FIXME: use AmB2BMedia in all RTP relay modes to avoid these problems?

  clearRtpReceiverRelay();
  setRtpRelayMode(new_mode);

  switch (getCallStatus()) {
    case CallLeg::Connected:
    case CallLeg::Disconnecting:
    case CallLeg::Disconnected:
      if (new_mode == RTP_Relay || new_mode == RTP_Transcoding)
        setMediaSession(new AmB2BMedia(a_leg ? this: NULL, a_leg ? NULL : this));
      if (!getOtherId().empty())
        relayEvent(new ChangeRtpModeEvent(new_mode, getMediaSession()));
      break;

    case CallLeg::NoReply:
    case CallLeg::Ringing:
      if (other_legs.empty()) {
        // we will receive our media session from the peer later on
        // WARNING: this means that getMediaSession called before we receive one
        // will give unusable instance (NULL for now)
        if (!getOtherId().empty())
          relayEvent(new ChangeRtpModeEvent(new_mode, getMediaSession()));
      }
      else {
        // we have to release or generate new media sessions for all our B legs
        changeOtherLegsRtpMode(new_mode);
      }
      break;
  }

  switch (dlg->getOAState()) {
    case AmOfferAnswer::OA_Completed:
    case AmOfferAnswer::OA_None:
      // must be followed by OA exchange because we can't updateLocalSdp
      // (reINVITE would be needed)
      break;

    case AmOfferAnswer::OA_OfferSent:
      ILOG_DLG(L_DBG, "changing RTP mode after offer was sent: reINVITE needed\n");
      // TODO: plan a reINVITE
      ILOG_DLG(L_ERR, "not implemented\n");
      break;

    case AmOfferAnswer::OA_OfferRecved:
      ILOG_DLG(L_DBG, "changing RTP mode after offer was received\n");
      break;

    case AmOfferAnswer::__max_OA: break; // grrrr
  }
}

void CallLeg::changeRtpMode(RTPRelayMode new_mode, AmB2BMedia *new_media)
{
  // we need to process regardless old RTP mode (at least new B2B media session
  // has to be used)

  bool mode_changed = (getRtpRelayMode() != new_mode);

  clearRtpReceiverRelay();
  setRtpRelayMode(new_mode);

  switch (getCallStatus()) {
    case CallLeg::Connected:
    case CallLeg::Disconnecting:
    case CallLeg::Disconnected:
      setMediaSession(new_media);
      break;

    case CallLeg::NoReply:
    case CallLeg::Ringing:
      if (other_legs.empty()) {
        // we are not the "A leg", we can use supplied media session
        setMediaSession(new_media);
      }
      else {
        // we have to release or generate new media sessions for all our B legs
        // (ignoring supplied media)
        // WARNING: we will use the same RTP relay mode for all peer legs!
        if (mode_changed) changeOtherLegsRtpMode(new_mode);
      }
      break;
  }

  AmB2BMedia *m = getMediaSession();
  if (m) m->changeSession(a_leg, this);

  switch (dlg->getOAState()) {
    case AmOfferAnswer::OA_Completed:
    case AmOfferAnswer::OA_None:
      // must be followed by OA exchange because we can't updateLocalSdp
      // (reINVITE would be needed)
      break;

    case AmOfferAnswer::OA_OfferSent:
      ILOG_DLG(L_DBG, "changing RTP mode/media session after offer was sent: reINVITE needed\n");
      // TODO: plan a reINVITE
      ILOG_DLG(L_ERR, "%s: not implemented\n", getLocalTag().c_str());
      break;

    case AmOfferAnswer::OA_OfferRecved:
      ILOG_DLG(L_DBG, "changing RTP mode/media session after offer was received\n");
      break;

    case AmOfferAnswer::__max_OA: break; // grrrr
  }

}

void CallLeg::changeOtherLegsRtpMode(RTPRelayMode new_mode)
{
  // change RTP relay mode and media session for all in other_legs
  const string &other = getOtherId();
  for (vector<OtherLegInfo>::iterator i = other_legs.begin(); i != other_legs.end(); ++i) {
    i->releaseMediaSession();

    if (new_mode != RTP_Direct) {
      i->media_session = new AmB2BMedia(NULL, NULL);
      i->media_session->addReference(); // new reference for storage

      if (other == i->id && i->media_session) {
        // if connected already with one of the legs we have to use the same
        // media session for us
        setMediaSession(i->media_session);
        if (i->media_session) i->media_session->changeSession(a_leg, this);
      }
    }

    AmSessionContainer::instance()->postEvent(i->id, new ChangeRtpModeEvent(new_mode, i->media_session));
  }
}

void CallLeg::acceptPendingInvite(AmSipRequest *invite)
{
  // reply the INVITE with fake 200 reply

  AmMimeBody *sdp = invite->body.hasContentType(SIP_APPLICATION_SDP);
  AmSdp s;
  if (!sdp || s.parse((const char*)sdp->getPayload())) {
    // no offer in the INVITE (or can't be parsed), we have to append fake offer
    // into the reply
    s.version = 0;
    s.origin.user = "sems";
    s.sessionName = "sems";
    s.conn.network = NT_IN;
    s.conn.addrType = AT_V4;
    s.conn.address = "0.0.0.0";

    s.media.push_back(SdpMedia());
    SdpMedia &m = s.media.back();
    m.type = MT_AUDIO;
    m.transport = TP_RTPAVP;
    m.send = false;
    m.recv = false;
    m.payloads.push_back(SdpPayload(0));
  }

  if (!s.conn.address.empty()) s.conn.address = "0.0.0.0";
  for (vector<SdpMedia>::iterator i = s.media.begin(); i != s.media.end(); ++i) {
    //i->port = 0;
    if (!i->conn.address.empty()) i->conn.address = "0.0.0.0";
  }

  AmMimeBody body;
  string body_str;
  s.print(body_str);
  body.parse(SIP_APPLICATION_SDP, (const unsigned char*)body_str.c_str(), body_str.length());
  try {
    updateLocalBody(body);
  } catch (...) { /* throw ? */  }

  ILOG_DLG(L_DBG, "replying pending INVITE with body: %s\n", body_str.c_str());
  dlg->reply(*invite, 200, "OK", &body);

  if (getCallStatus() != Connected) updateCallStatus(Connected);
}

int CallLeg::reinvite(const string &hdrs, const AmMimeBody &body, bool relayed, unsigned r_cseq, bool establishing)
{
  int res;
  try {
    AmMimeBody r_body(body);
    updateLocalBody(r_body);
    res = dlg->sendRequest(SIP_METH_INVITE, &r_body, hdrs, SIP_FLAGS_VERBATIM);
  } catch (const string& s) { res = -500; }

  if (res < 0) {
    if (relayed) {
      ILOG_DLG(L_DBG, "sending re-INVITE failed, relaying back error reply\n");
      relayError(SIP_METH_INVITE, r_cseq, true, res);
    }

    ILOG_DLG(L_DBG, "sending re-INVITE failed, terminating the call\n");
    stopCall(StatusChangeCause::InternalError);
    return -1;
  }

  if (relayed) {
    AmSipRequest fake_req;
    fake_req.method = SIP_METH_INVITE;
    fake_req.cseq = r_cseq;
    relayed_req[dlg->cseq - 1] = fake_req;
    est_invite_other_cseq = r_cseq;
  }
  else est_invite_other_cseq = 0;

  saveSessionDescription(body);

  if (establishing) {
    // save CSeq of establishing INVITE
    est_invite_cseq = dlg->cseq - 1;
  }
  return dlg->cseq - 1;
}

void CallLeg::adjustOffer(AmSdp &sdp)
{
  if (hold != PreserveHoldStatus) {
    ILOG_DLG(L_DBG, "local hold/unhold request");
    /* locally generated hold/unhold requests that already contain correct
     * hold/resume bodies and need not to be altered via createHoldRequest
     * hold/resumeRequested is already called */

  } else {
    /* handling B2B SDP, check for hold/unhold */
    holdMethod hm = None;

    /* if hold request, transform to requested kind of hold and remember that hold
     * was requested with this offer */
    if (isHoldRequest(sdp, hm)) {
      ILOG_DLG(L_DBG, "%s: B2b hold request", getLocalTag().c_str());

      holdRequested(); /* it handles only MoH's hook */

      alterHoldRequest(sdp);
      hold = HoldRequested;
    } else {
      if (on_hold || isSDPBodyHold(sdp)) {
        ILOG_DLG(L_DBG, "B2b resume request");
        resumeRequested();
        alterResumeRequest(sdp);
        hold = ResumeRequested;
      }
    }
  }
}

void CallLeg::updateLocalSdp(AmSdp &sdp)
{
  ILOG_DLG(L_DBG, "%s: updateLocalSdp (OA: %d)\n", getLocalTag().c_str(), dlg->getOAState());
  // handle the body based on current offer-answer status
  // (possibly update the body before sending to remote)

  // FIXME: repeated SDP (183, 200) will cause false match in OA_Completed
  // (need not to be expected with re-INVITEs asking for hold)
  if (dlg->oaExpectingOffer()) {
    // handling offer
    adjustOffer(sdp);
  }

  /** make sure to answer with the 'sendonly' / 'inactive' to the one who previously
   *  requested the on-hold, in order to meet RFC requirements:
   *  - 'sendonly' must be faced with the 'recvonly' sent back as an answer
   *  - 'inactive' must be faced with the 'inactive' sent back as an answer
   *  This block is only needed for cases, when MoH is emulated on the SEMS directly.
   */
  else if (hold == PreserveHoldStatus && hold_type_requested != NonHold)
  {
    for (std::vector<SdpMedia>::iterator m = sdp.media.begin();
         m != sdp.media.end(); ++m)
    {
      if (m->isAudio()) {
        switch(hold_type_requested)
        {
          case SendonlyHold:
            m->send = false; /* make sure to answer with the recvonly, if the on hold */
            m->recv = true;  /* was perviously requested using a=sendonly  */
            ILOG_DLG(L_DBG, "On hold has been previously requested and must be held further\n");
            ILOG_DLG(L_DBG, "This SDP is prepared to be sent with the a=recvonly\n");
            break;
          case InactiveHold:
            m->send = false; /* make sure to answer with the inactive, if the on hold */
            m->recv = false; /* was perviously requested using a=inactive */
            ILOG_DLG(L_DBG, "On hold has been previously requested and must be held further\n");
            ILOG_DLG(L_DBG, "This SDP is prepared to be sent with the a=inactive\n");
            break;
          case ZeroedHold:
            ILOG_DLG(L_DBG, "On hold has been previously requested, but will be not held further (zeroed hold)\n");
            break;
          default:
            break; /* do nothing, this handles the compiler's warning that NonHold is not mentioned in switch */
        }
      }
    }
  }

  /* store non-hold SDP to be able to resumeHeld */
  if (hold == PreserveHoldStatus && !on_hold) non_hold_sdp = sdp;

  AmB2BSession::updateLocalSdp(sdp);
}

void CallLeg::offerRejected()
{
  ILOG_DLG(L_DBG, "%s: offer rejected! (hold status: %d)", getLocalTag().c_str(), hold);
  switch (hold) {
    case HoldRequested: holdRejected(); break;
    case ResumeRequested: resumeRejected(); break;
    case PreserveHoldStatus: break;
  }
  hold = PreserveHoldStatus;
}

void CallLeg::createResumeRequest(AmSdp &sdp)
{
  /* use stored non-hold SDP (saved last time when putting on hold,
   * `updateLocalSdp()` takes care of it).
   *
   * Note: this SDP doesn't need to be correct, but established_body need not to
   * be good enough for unholding (might be held already with zero conncetions) */
  /* keep sessV incremented each time sending SDP offer (hold/resume) */
  non_hold_sdp.origin.sessV++;
  ILOG_DLG(L_DBG, "Increasing session version in SDP origin line to %s", uint128ToStr(non_hold_sdp.origin.sessV).c_str());

  if (!non_hold_sdp.media.empty()) {
    sdp = non_hold_sdp;
  } else {
    /* no stored non-hold SDP */
    ILOG_DLG(L_ERR, "no stored non-hold SDP, but local resume requested\n");
    /* TODO: try to use established_body here and mark properly
     * if no established body exist */
    throw string("not implemented");
  }
  /* do not touch the sdp otherwise (use directly B2B SDP) */
}

void CallLeg::debug()
{
  ILOG_DLG(L_DBG, "call leg: %s", getLocalTag().c_str());
  ILOG_DLG(L_DBG, "\tother: %s\n", getOtherId().c_str());
  ILOG_DLG(L_DBG, "\tstatus: %s\n", callStatus2str(getCallStatus()));
  ILOG_DLG(L_DBG, "\tRTP relay mode: %d\n", rtp_relay_mode);
  ILOG_DLG(L_DBG, "\ton hold: %s\n", on_hold ? "yes" : "no");
  ILOG_DLG(L_DBG, "\toffer/answer status: %d, hold: %d\n", dlg->getOAState(), hold);

  AmB2BMedia *ms = getMediaSession();
  if (ms) ms->debug();
}

int CallLeg::onSdpCompleted(const AmSdp& offer, const AmSdp& answer)
{
  ILOG_DLG(L_DBG, "%s: oaCompleted\n", getLocalTag().c_str());
  switch (hold) {
    case HoldRequested: holdAccepted(); break;
    case ResumeRequested: resumeAccepted(); break;
    case PreserveHoldStatus: break;
  }

  hold = PreserveHoldStatus;
  return AmB2BSession::onSdpCompleted(offer, answer);
}

void CallLeg::applyPendingUpdate()
{
  ILOG_DLG(L_DBG, "going to apply pending updates");

  if (pending_updates.empty()) return;

  if (!canUpdateSession()) {
    ILOG_DLG(L_DBG, "can't apply pending updates now");
    return;
  }

  ILOG_DLG(L_DBG, "applying pending updates");

  do {
    SessionUpdate *u = pending_updates.front();
    u->apply(this);
    if (u->hasCSeq()) {
      // SIP transaction started, wait for finishing it
      break;
    }
    else {
      // the update operation hasn't started a SIP transaction so it can be
      // understood as finished
      pending_updates.pop_front();
      delete u;
    }
  } while (!pending_updates.empty());
}

void CallLeg::onTransFinished()
{
  ILOG_DLG(L_DBG, "UAC/UAS transaction finished");
  AmB2BSession::onTransFinished();

  if (pending_updates.empty() || !canUpdateSession()) return; // there is nothing we can do now

  if (pending_updates_timer.started()) {
    ILOG_DLG(L_DBG, "UAC/UAS transaction finished, but waiting for planned updates");
    return; // it is planned to apply the updates later on
  }

  ILOG_DLG(L_DBG, "UAC/UAS transaction finished, try to apply pending updates");
  AmSessionContainer::instance()->postEvent(getLocalTag(), new ApplyPendingUpdatesEvent());
}

void CallLeg::updateSession(SessionUpdate *u)
{
  if (!canUpdateSession() || !pending_updates.empty()) {
    ILOG_DLG(L_DBG, "planning session update for later");
    pending_updates.push_back(u);
  }
  else {
    u->apply(this);

    if (u->hasCSeq()) pending_updates.push_back(u); // store for failover
    else delete u; // finished
  }
}

void CallLeg::putOnHold()
{
  updateSession(new PutOnHold());
}

void CallLeg::resumeHeld()
{
  updateSession(new ResumeHeld());
}
