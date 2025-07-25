/*
 * Copyright (C) 2002-2003 Fhg Fokus
 *
 * This file is part of SEMS, a free SIP media server.
 *
 * SEMS is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version. This program is released under
 * the GPL with the additional exemption that compiling, linking,
 * and/or using OpenSSL is allowed.
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
#include "AmB2BSession.h"
#include "AmSessionContainer.h"
#include "AmConfig.h"
#include "ampi/MonitoringAPI.h"
#include "AmSipHeaders.h"
#include "AmUtils.h"
#include "AmRtpReceiver.h"

#include "global_defs.h"

#include <assert.h>

#define GET_CALL_ID() (dlg->getCallid().c_str())

// helpers
static const string sdp_content_type(SIP_APPLICATION_SDP);
static const string empty;

//
// helper functions
//

static void errCode2RelayedReply(AmSipReply &reply, int err_code, unsigned default_code = 500)
{
  // FIXME: use cleaner method to propagate error codes/reasons, 
  // do it everywhere in the code
  if ((err_code < -399) && (err_code > -700)) {
    reply.code = -err_code;
  }
  else reply.code = default_code;

  // TODO: optimize with a table
  switch (reply.code) {
    case 400: reply.reason = "Bad Request"; break;
    case 478: reply.reason = "Unresolvable destination"; break;
    case 488: reply.reason = SIP_REPLY_NOT_ACCEPTABLE_HERE; break;
    default: reply.reason = SIP_REPLY_SERVER_INTERNAL_ERROR;
  }
}

static bool isDSMEarlyAnnounceForced(const std::string &hdrs)
{
  string p_dsm_app = getHeader(hdrs, SIP_HDR_P_DSM_APP, true);
  return get_header_param(p_dsm_app, DSM_PARAM_EARLY_AN) == DSM_VALUE_FORCE;
}

static bool isDSMPlaybackFinished(const std::string &hdrs)
{
  /** TODO: for the future, we might also want to check
   *  particular DSM applications, for now plays no role.
   */
  string p_dsm_app = getHeader(hdrs, SIP_HDR_P_DSM_APP, true);
  return get_header_param(p_dsm_app, DSM_PARAM_PLAYBACK) == DSM_VALUE_FINISHED;
}

static bool isDSMToTagReset(const std::string &hdrs)
{
  string p_dsm_app = getHeader(hdrs, SIP_HDR_P_DSM_APP, true);
  return get_header_param(p_dsm_app, DSM_PARAM_RESET_TOTAG) == DSM_VALUE_IS_SET;
}

//
// AmB2BSession methods
//

AmB2BSession::AmB2BSession(const string& other_local_tag, AmSipDialog* p_dlg,
			   AmSipSubscription* p_subs)
  : AmSession(p_dlg),
    other_id(other_local_tag),
    sip_relay_only(true),
    a_leg(false),
    subs(p_subs),
    rtp_relay_mode(RTP_Direct),
    rtp_relay_force_symmetric_rtp(false),
    enable_dtmf_transcoding(false),
    enable_dtmf_rtp_filtering(false),
    enable_dtmf_rtp_detection(false),
    rtp_relay_transparent_seqno(true), rtp_relay_transparent_ssrc(true),
    est_invite_cseq(0),est_invite_other_cseq(0),
    media_session(NULL),
    previous_origin_sessId(0),
    previous_origin_sessV(0)
{
  if(!subs) subs = new AmSipSubscription(dlg,this);
}

AmB2BSession::~AmB2BSession()
{
  clearRtpReceiverRelay();

  ILOG_DLG(L_DBG, "relayed_req.size() = %zu\n",relayed_req.size());

  map<int,AmSipRequest>::iterator it = recvd_req.begin();
  ILOG_DLG(L_DBG, "recvd_req.size() = %zu\n",recvd_req.size());
  for(;it != recvd_req.end(); ++it){
    ILOG_DLG(L_DBG, "  <%i,%s>\n",it->first,it->second.method.c_str());
  }

  if(subs)
    delete subs;
}

void AmB2BSession::set_sip_relay_only(bool r) { 
  if (!getLocalTag().empty())
    ILOG_DLG(L_DBG, "Set sip_relay_only=%s for local_tag '%s'\n", (r ? "true" : "false"), getLocalTag().c_str());

  sip_relay_only = r;
}

void AmB2BSession::clear_other()
{
  setOtherId("");
}

void AmB2BSession::process(AmEvent* event)
{
  B2BEvent* b2b_e = dynamic_cast<B2BEvent*>(event);
  if(b2b_e){

    onB2BEvent(b2b_e);
    return;
  }

  SingleSubTimeoutEvent* to_ev = dynamic_cast<SingleSubTimeoutEvent*>(event);
  if(to_ev) {
    subs->onTimeout(to_ev->timer_id,to_ev->sub);
    return;
  }

  AmSession::process(event);
}

void AmB2BSession::finalize()
{
  // clean up relayed_req
  if(!other_id.empty()) {
    while(!relayed_req.empty()) {
      TransMap::iterator it = relayed_req.begin();
      const AmSipRequest& req = it->second;
      relayError(req.method,req.cseq,true,481,SIP_REPLY_NOT_EXIST);
      relayed_req.erase(it);
    }
  }

  AmSession::finalize();
}

void AmB2BSession::sl_reply(const string &method, unsigned cseq, bool forward, int sip_code, const char *reason)
{
  if (method != SIP_METH_ACK) {
    AmSipReply n_reply;
    n_reply.code = sip_code;
    n_reply.reason = reason;
    n_reply.cseq = cseq;
    n_reply.cseq_method = method;
    n_reply.from_tag = dlg->getLocalTag();
    ILOG_DLG(L_DBG, "relaying stateless B2B SIP reply %d %s\n", sip_code, reason);
    relayEvent(new B2BSipReplyEvent(n_reply, forward, method, getLocalTag()));
  }
}

void AmB2BSession::relayError(const string &method, unsigned cseq,
			      bool forward, int err_code)
{
  if (method != "ACK") {
    AmSipReply n_reply;
    errCode2RelayedReply(n_reply, err_code, 500);
    n_reply.cseq = cseq;
    n_reply.cseq_method = method;
    n_reply.from_tag = dlg->getLocalTag();
    ILOG_DLG(L_DBG, "relaying B2B SIP error reply %u %s\n", n_reply.code, n_reply.reason.c_str());
    relayEvent(new B2BSipReplyEvent(n_reply, forward, method, getLocalTag()));
  }
}

void AmB2BSession::relayError(const string &method, unsigned cseq, bool forward, int sip_code, const char *reason)
{
  if (method != "ACK") {
    AmSipReply n_reply;
    n_reply.code = sip_code;
    n_reply.reason = reason;
    n_reply.cseq = cseq;
    n_reply.cseq_method = method;
    n_reply.from_tag = dlg->getLocalTag();
    ILOG_DLG(L_DBG, "relaying B2B SIP reply %d %s\n", sip_code, reason);
    relayEvent(new B2BSipReplyEvent(n_reply, forward, method, getLocalTag()));
  }
}

void AmB2BSession::createFakeReply(const AmMimeBody *sdp,   AmMimeBody& reply_body) {

  int rtp_int_tmp = getRtpInterface();
  string rtp_local_ip = AmConfig::RTP_Ifs[rtp_int_tmp].LocalIP;

  AmSdp s;

  if (!sdp || s.parse((const char*)sdp->getPayload())) {
    /* no offer in the INVITE (or can't be parsed), we have to append fake offer
       into the reply */
    s.version = 0;
    s.origin.user = "sems";
    s.sessionName = "sems";
    s.conn.network = NT_IN;
    s.conn.addrType = AT_V4;

    /* MT#55582, removed.
    s.conn.address = "0.0.0.0";
    */

    /* re-fill the empty connection address with the media_ip */
    if (s.conn.address.empty()) s.conn.address = rtp_local_ip;

    s.media.push_back(SdpMedia());
    SdpMedia &m = s.media.back();
    m.type = MT_AUDIO;
    m.transport = TP_RTPAVP;
    m.send = false;
    m.recv = false;
    m.payloads.push_back(SdpPayload(0));
  }

  /* MT#55582, removed, because in case of generating a faked reply
   during the media session refreshment (e.g. call pickup after the AA),
   it leads to a held media session

  if (!s.conn.address.empty()) s.conn.address = "0.0.0.0";
  for (vector<SdpMedia>::iterator i = s.media.begin(); i != s.media.end(); ++i) {
    //i->port = 0;
    if (!i->conn.address.empty()) i->conn.address = "0.0.0.0";
  }*/

  /* re-fill the empty connection address with the media_ip */
  if (s.conn.address.empty()) {
    s.conn.address = rtp_local_ip;
    ILOG_DLG(L_DBG, "RTP Connection address was empty, and has been rested to: <%s>\n", s.conn.address.c_str());
  }

  /* same here, but for the rest media sessions in SDP */
  for (vector<SdpMedia>::iterator i = s.media.begin(); i != s.media.end(); ++i) {
    if (i->conn.address.empty()) i->conn.address = rtp_local_ip;
  }

  string body_str;
  s.print(body_str);
  reply_body.parse(SIP_APPLICATION_SDP, (const unsigned char*)body_str.c_str(), body_str.length());
  try {
    updateLocalBody(reply_body);
  } catch (...) { /* throw ? */  }

  ILOG_DLG(L_DBG, "created pending INVITE reply body: %s\n", body_str.c_str());
}

static void sdp2body(const AmSdp &sdp, AmMimeBody &body)
{
  string body_str;
  sdp.print(body_str);
  AmMimeBody *s = body.hasContentType(SIP_APPLICATION_SDP);
  if (s) s->parse(SIP_APPLICATION_SDP, (const unsigned char*)body_str.c_str(), body_str.length());
  else body.parse(SIP_APPLICATION_SDP, (const unsigned char*)body_str.c_str(), body_str.length());
}

void AmB2BSession::acceptPendingInvite(AmSipRequest *invite)
{
  // reply the INVITE with fake 200 reply

  const AmMimeBody *sdp = invite->body.hasContentType(SIP_APPLICATION_SDP);
  AmMimeBody body;

  createFakeReply(sdp, body);

  ILOG_DLG(L_DBG, "Replying to pending invite with 200 OK\n");
  dlg->reply(*invite, 200, "OK", &body);
}

void AmB2BSession::acceptPendingInviteB2B(const AmSipRequest& invite)
{
  const AmMimeBody *sdp = invite.body.hasContentType(SIP_APPLICATION_SDP);
  AmSipReply n_reply;

  /* local sdp, which was already learned before */
  AmSdp remote_sdp = dlg->getRemoteSdp();

  unsigned int remote_port = 0;

  if (dlg->getRemoteMediaPort() > 0) {
    ILOG_DLG(L_DBG, "Using remotely seen SDP port for faking this reply: '%d'\n", dlg->getRemoteMediaPort());
    remote_port = dlg->getRemoteMediaPort();
  }
  else if (!remote_sdp.media.empty()) {
    /* take first possible media and re-use its port */
    SdpMedia remote_sdp_media = remote_sdp.media.front();
    if (remote_sdp_media.port > 0) {
      ILOG_DLG(L_DBG, "Using remotely seen SDP port for faking this reply: '%d'\n", remote_sdp_media.port);
      remote_port = remote_sdp_media.port;
    }
  }

  if (remote_port > 0) {
    /* create fake AmSdp from AmMimeBody */
    AmSdp fake_sdp;
    AmMimeBody fake_mimebody;
    fake_sdp.parse((const char *)sdp->getPayload());

    ILOG_DLG(L_DBG, "Using local SDP port for faking this reply: '%d'\n", remote_port);
    for (auto it = fake_sdp.media.begin(); it != fake_sdp.media.end(); ++it)
    {
      it->port = remote_port;
    }

    sdp2body(fake_sdp, fake_mimebody);
    createFakeReply(&fake_mimebody, n_reply.body);
  }
  else {
    createFakeReply(sdp, n_reply.body);
  }

  n_reply.code = 200;
  n_reply.reason = "OK";
  n_reply.cseq = invite.cseq;
  n_reply.cseq_method = SIP_METH_INVITE;
  n_reply.from_tag = dlg->getLocalTag();
  ILOG_DLG(L_DBG, "Relaying B2B-event (fake 200 OK)\n");
  relayEvent(new B2BSipReplyEvent(n_reply, true, SIP_METH_INVITE, getLocalTag()));
}

void AmB2BSession::onB2BEvent(B2BEvent* ev)
{
  ILOG_DLG(L_DBG, "AmB2BSession::onB2BEvent\n");

  switch (ev->event_id) {

    case B2BSipRequest:
    {
        B2BSipRequestEvent* req_ev = dynamic_cast<B2BSipRequestEvent*>(ev);
        assert(req_ev);

        ILOG_DLG(L_DBG, "B2BSipRequest: %s (fwd=%s)\n", req_ev->req.method.c_str(), req_ev->forward? "true" : "false");

        if (req_ev->forward) {

          /* Check Max-Forwards first */
          if (req_ev->req.max_forwards == 0) {
            relayError(req_ev->req.method,req_ev->req.cseq, true,483,SIP_REPLY_TOO_MANY_HOPS);
            return;
          }

          if (req_ev->req.method == SIP_METH_INVITE && dlg->getUACInvTransPending()) {
            ILOG_DLG(L_DBG, "There are still pending UAC INVITE transactions, this leg: '%s' (UAC) -> other leg: '%s'\n",
                            dlg->getLocalTag().c_str(),
                            getOtherId().c_str());
            ILOG_DLG(L_DBG, "Sending a fake 200OK to this transaction in the meanwhile (CSeq: %d)\n", req_ev->req.cseq);
            /* UAC side (other side) already has a pending update, this one just overrides it?
            * let the UAC respond, what will trigger a back update again and negotiations should be finished by that
            */
            acceptPendingInviteB2B(req_ev->req);
            return;
          }

          if (req_ev->req.method == SIP_METH_BYE &&
              dlg->getStatus() != AmBasicSipDialog::Connected) {
            ILOG_DLG(L_DBG, "not sip-relaying BYE in not connected dlg, b2b-relaying 200 OK\n");
            relayError(req_ev->req.method, req_ev->req.cseq, true, 200, "OK");
            return;
          }

          /* relay, unless it's a BYE dedicated for other leg with a faked 183 */
          int res = 0;

          if (req_ev->req.method == SIP_METH_BYE && dlg->getFaked183As200()) {
            ILOG_DLG(L_DBG, "This BYE will not forwarded, because other leg is a faked 183 to 200OK. CANCEL required.\n");
            /* for now just answer with 200 OK, later on we must send CANCEL to the Early stage leg */
            sl_reply(req_ev->req.method, req_ev->req.cseq, true, 200, "OK");
          } else {
            res = relaySip(req_ev->req); /* most requests get here */
          }

          if (res < 0) {
            /* reply relayed request internally */
            relayError(req_ev->req.method, req_ev->req.cseq, true, res);
            return;
          }
        }
        
        if (req_ev->req.method == SIP_METH_BYE) {
          /* CANCEL is handled differently: other side has already
            sent a terminate event.
            || (req_ev->req.method == SIP_METH_CANCEL) */

          if (dlg->getFaked183As200()) onOtherCancel();
          else onOtherBye(req_ev->req);
        }
    }
    return;

    case B2BSipReply:
    {
      B2BSipReplyEvent* reply_ev = dynamic_cast<B2BSipReplyEvent*>(ev);
      assert(reply_ev);

      ILOG_DLG(L_DBG, "B2BSipReply: %i %s (fwd=%s)\n", reply_ev->reply.code,
                                          reply_ev->reply.reason.c_str(),
                                          reply_ev->forward? "true" : "false");

      ILOG_DLG(L_DBG, "B2BSipReply: content-type = %s\n", reply_ev->reply.body.getCTStr().c_str());

      if (reply_ev->forward) {
        std::map<int,AmSipRequest>::iterator t_req = recvd_req.find(reply_ev->reply.cseq);

        if (t_req != recvd_req.end()) {

          if ((reply_ev->reply.code >= 300) && (reply_ev->reply.code <= 305) &&
              !reply_ev->reply.contact.empty()) {
            /* relay with Contact in 300 - 305 redirect messages */
            AmSipReply n_reply(reply_ev->reply);
            n_reply.hdrs += SIP_HDR_COLSP(SIP_HDR_CONTACT) + reply_ev->reply.contact + CRLF;

            if(relaySip(t_req->second,n_reply) < 0) {
              terminateOtherLeg();
              terminateLeg();
            }

          } else {
            /* relay response */
            if (relaySip(t_req->second,reply_ev->reply) < 0) {
              terminateOtherLeg();
              terminateLeg();
            }
          }

        } else {
          ILOG_DLG(L_DBG, "Cannot relay reply: request already replied (code=%u;cseq=%u;call-id=%s)",
            reply_ev->reply.code,
            reply_ev->reply.cseq,
            reply_ev->reply.callid.c_str());
        }

      } else {

        /* ensure that 'P-DSM-App: <app-name>;early-announce=force' is not present */
        if (reply_ev->reply.code == 183 && !dlg->getForcedEarlyAnnounce()) {
          dlg->setForcedEarlyAnnounce(isDSMEarlyAnnounceForced(reply_ev->reply.hdrs));
        }

        /* don't forget to reset the force_early_announce, if 200 OK in the same leg received */
        if (SIP_IS_200_CLASS(reply_ev->reply.code) && dlg->getForcedEarlyAnnounce()) {
          dlg->setForcedEarlyAnnounce(false);
        }

        /* check whether not-forwarded (locally initiated)
         * INV/UPD transaction changed session in other leg */
        if (SIP_IS_200_CLASS(reply_ev->reply.code) &&
            (!reply_ev->reply.body.empty()) &&
            (reply_ev->reply.cseq_method == SIP_METH_INVITE ||
            reply_ev->reply.cseq_method == SIP_METH_UPDATE))
        {
          if (updateSessionDescription(reply_ev->reply.body)) {

            if (reply_ev->reply.cseq != est_invite_cseq) {
              if (dlg->getUACInvTransPending()) {
                ILOG_DLG(L_DBG, "changed session, but UAC INVITE trans pending\n");
                /* todo(?): save until trans is finished? */
                return;
              }
              ILOG_DLG(L_DBG, "session description changed - refreshing\n");
              sendEstablishedReInvite();
            } else {
              ILOG_DLG(L_DBG, "reply to establishing INVITE request - not refreshing\n");
            }
          }

        /* 183 - coming from DSM application */
        } else if (reply_ev->reply.code == 183 &&
                   dlg->getForcedEarlyAnnounce() &&
                   !reply_ev->reply.body.empty()) {

          if (updateSessionDescription(reply_ev->reply.body)) {
            if (dlg->getUACInvTransPending()) {
              ILOG_DLG(L_DBG, "changed session, but UAC INVITE trans pending\n");
            } else {
              ILOG_DLG(L_DBG, "Received 183 with <;%s=%s>, refreshing media session.\n", DSM_PARAM_EARLY_AN, DSM_VALUE_FORCE);

              setMute(true);
              AmMediaProcessor::instance()->removeSession(this);

              if (sendEstablishedReInvite() < 0) {
                  ILOG_DLG(L_ERR, "could not re-Invite after locally initiated request"
                        "in B2B leg changed session (this='%s', other='%s')\n",
                        getLocalTag().c_str(), other_id.c_str());
              }
            }
          }

        /* Processing of the playback completion from DSM applications
         * For now we only support: 480 Unavailable and 486 Busy/603 Decline.
         * Exceptionally 487 is added, for cases like:
         * a call to HG, where nobody answers and the timeout triggers a cancelation of all the legs.
         */
        } else if ((reply_ev->reply.code == 480 ||
                    reply_ev->reply.code == 486 ||
                    reply_ev->reply.code == 487 ||
                    reply_ev->reply.code == 603)
                    && dlg->getStatus() == AmSipDialog::Connected) {

          /* TT#188800, if this is a completion of the playback of one of the DSM applications,
            (office hours, play last caller, pre announce, early dbprompt)
            in the session, which has had AA or a transfer before going to this DSM application,
            then a caller is now likely in the connected state, and requires BYE, not 480 */
          if (isDSMPlaybackFinished(reply_ev->reply.hdrs)) {
            ILOG_DLG(L_DBG, "This is the end of DSM playback, the caller is in the connected state.\n");
            ILOG_DLG(L_DBG, "Terminating the original leg with BYE, instead of 480.\n");
            terminateLeg();
          }
        }
      }
    }
    return;

    case B2BTerminateLeg:
      ILOG_DLG(L_DBG, "terminateLeg()\n");
      terminateLeg();
      break;
  }

  /* ILOG_DLG(L_ERR, "unknown event caught\n"); */
}

bool AmB2BSession::getMappedReferID(unsigned int refer_id, 
				    unsigned int& mapped_id) const
{
  map<unsigned int, unsigned int>::const_iterator id_it =
    refer_id_map.find(refer_id);
  if(id_it != refer_id_map.end()) {
    mapped_id = id_it->second;
    return true;
  }

  return false;
}

void AmB2BSession::insertMappedReferID(unsigned int refer_id,
				       unsigned int mapped_id)
{
  refer_id_map[refer_id] = mapped_id;
}

void AmB2BSession::onSipRequest(const AmSipRequest& req)
{
  bool fwd = sip_relay_only &&
    (req.method != SIP_METH_CANCEL);

  if( ((req.method == SIP_METH_SUBSCRIBE) ||
       (req.method == SIP_METH_NOTIFY) ||
       (req.method == SIP_METH_REFER))
      && !subs->onRequestIn(req) ) {
    return;
  }

  if(!fwd)
    AmSession::onSipRequest(req);
  else {
    updateRefreshMethod(req.hdrs);

    if(req.method == SIP_METH_BYE)
      onBye(req);
  }

  B2BSipRequestEvent* r_ev = new B2BSipRequestEvent(req,fwd);

  if (fwd) {
    ILOG_DLG(L_DBG, "relaying B2B SIP request (fwd) %s %s\n", r_ev->req.method.c_str(), r_ev->req.r_uri.c_str());

    if(r_ev->req.method == SIP_METH_NOTIFY) {

      string event = getHeader(r_ev->req.hdrs,SIP_HDR_EVENT,true);
      string id = get_header_param(event,"id");
      event = strip_header_params(event);

      if(event == "refer" && !id.empty()) {

	int id_int=0;
	if(str2int(id,id_int)) {

	  unsigned int mapped_id=0;
	  if(getMappedReferID(id_int,mapped_id)) {

	    removeHeader(r_ev->req.hdrs,SIP_HDR_EVENT);
	    r_ev->req.hdrs += SIP_HDR_COLSP(SIP_HDR_EVENT) "refer;id=" 
	      + int2str(mapped_id) + CRLF;
	  }
	}
      }
    }

    int res = relayEvent(r_ev);
    if (res == 0) {
      // successfuly relayed, store the request
      if(req.method != SIP_METH_ACK)
        recvd_req.insert(std::make_pair(req.cseq,req));
    }
    else {
      // relay failed, generate error reply
      ILOG_DLG(L_DBG, "relay failed, replying error\n");
      AmSipReply n_reply;
      errCode2RelayedReply(n_reply, res, 500);
      dlg->reply(req, n_reply.code, n_reply.reason);
    }

    return;
  }

  ILOG_DLG(L_DBG, "relaying B2B SIP request %s %s\n", r_ev->req.method.c_str(), r_ev->req.r_uri.c_str());
  relayEvent(r_ev);
}

void AmB2BSession::onRequestSent(const AmSipRequest& req)
{
  if( ((req.method == SIP_METH_SUBSCRIBE) ||
       (req.method == SIP_METH_NOTIFY) ||
       (req.method == SIP_METH_REFER)) ) {
    subs->onRequestSent(req);
  }

  AmSession::onRequestSent(req);
}

void AmB2BSession::updateLocalSdp(AmSdp &sdp)
{
  if (rtp_relay_mode == RTP_Direct) return; // nothing to do

  if (!media_session) {
    // report missing media session (here we get for rtp_relay_mode == RTP_Relay)
    ILOG_DLG(L_ERR, "BUG: media session is missing, can't update local SDP\n");
    return; // FIXME: throw an exception here?
  }

  media_session->replaceConnectionAddress(sdp, a_leg, localMediaIP(), advertisedIP());
}

void AmB2BSession::saveLocalSdpOrigin(const AmSdp& sdp)
{
  if (sdp_origin.conn.address.empty()) {
    // remember this origin for whole dialog lifetime
    sdp_origin = sdp.origin;
    previous_sdp = sdp;
    previous_origin_sessId = sdp.origin.sessId;
    previous_origin_sessV = sdp.origin.sessV;
    ILOG_DLG(L_DBG, "Remembering initial SDP Origin (Id %s V %s)\n",
        uint128ToStr(sdp.origin.sessId).c_str(), uint128ToStr(sdp.origin.sessV).c_str());
  }
}

void AmB2BSession::updateLocalSdpOrigin(AmSdp& sdp) {
  // fix SDP origin
  if (sdp_origin.conn.address.empty()) {
    saveLocalSdpOrigin(sdp);
  }
  else {
    bool sdp_changed = false;
    // check if Origin Id/Version has changed
    if ((sdp.origin.sessV != previous_origin_sessV) ||
       (sdp.origin.sessId != previous_origin_sessId)){
      sdp_changed = true;
      // remember for next time
      previous_origin_sessId = sdp.origin.sessId;
      previous_origin_sessV = sdp.origin.sessV;
    }
    // use remembered SDP origin
    sdp.origin = sdp_origin;
    // check if SDP has changed (apart from origin)
    if (!sdp_changed) {
      // comparing the AmSdp objects may be unsafe (intialized members...),
      // so comparing resulting SDP string
      string s_sdp; string s_previous_sdp;
      sdp.print(s_sdp); previous_sdp.print(s_previous_sdp);
      if (!(s_sdp == s_previous_sdp)) {
       sdp_changed = true;
      }
    }
    // ...and increase version if changed
    if (sdp_changed) {
      // increase version
      sdp_origin.sessV++;
      // update origin
      sdp.origin = sdp_origin;
      // remember the current SDP for the next time
      previous_sdp = sdp;
      ILOG_DLG(L_DBG, "SDP changed; updating Origin (Id %s V %s)\n",
          uint128ToStr(sdp.origin.sessId).c_str(), uint128ToStr(sdp.origin.sessV).c_str());
    } else {
      ILOG_DLG(L_DBG, "SDP unchanged; keeping Origin (Id %s V %s)\n",
          uint128ToStr(sdp.origin.sessId).c_str(), uint128ToStr(sdp.origin.sessV).c_str());
    }
  }
}

void AmB2BSession::updateLocalBody(AmMimeBody& body)
{
  AmMimeBody *sdp = body.hasContentType(SIP_APPLICATION_SDP);
  if (!sdp) return;

  AmSdp parser_sdp;
  if (parser_sdp.parse((const char*)sdp->getPayload())) {
    ILOG_DLG(L_DBG, "SDP parsing failed!\n");
    return; // FIXME: throw an exception here?
  }

  updateLocalSdp(parser_sdp);
  updateLocalSdpOrigin(parser_sdp);

  // regenerate SDP
  string n_body;
  parser_sdp.print(n_body);
  sdp->parse(sdp->getCTStr(), (const unsigned char*)n_body.c_str(), n_body.length());
}

void AmB2BSession::updateUACTransCSeq(unsigned int old_cseq, unsigned int new_cseq) {
  if (old_cseq == new_cseq)
    return;

  TransMap::iterator t = relayed_req.find(old_cseq);
  if (t != relayed_req.end()) {
    relayed_req[new_cseq] = t->second;
    relayed_req.erase(t);
    ILOG_DLG(L_DBG, "updated relayed_req (UAC trans): CSeq %u -> %u\n", old_cseq, new_cseq);
  }

  if (est_invite_cseq == old_cseq) {
    est_invite_cseq = new_cseq;
    ILOG_DLG(L_DBG, "updated est_invite_cseq: CSeq %u -> %u\n", old_cseq, new_cseq);
  }
}


void AmB2BSession::onSipReply(const AmSipRequest& req, const AmSipReply& reply,
			      AmBasicSipDialog::Status old_dlg_status)
{
  TransMap::iterator t = relayed_req.find(reply.cseq);
  bool fwd = (t != relayed_req.end()) && (reply.code != 100);
  bool to_tag_reset = isDSMToTagReset(reply.hdrs); /* check P-DSM-App: <app-name>;reset-to-tag=1 */

  ILOG_DLG(L_DBG, "onSipReply: %s -> %i %s (fwd=%s), c-t=%s\n",
      reply.cseq_method.c_str(), reply.code,reply.reason.c_str(),
      fwd? "true" : "false", reply.body.getCTStr().c_str());

  /* update last reply for further usage with header getters */
  last_200_reply = reply;

  if (to_tag_reset && !dlg->getRemoteTag().empty() && reply.code >= 180 && reply.code <= 183 ) {
    ILOG_DLG(L_DBG, "onSipReply: sess %p received %i reply with remote-tag %s", this, reply.code, reply.to_tag.c_str());
    ILOG_DLG(L_DBG, "dlg->getRemoteTag(%s)\n", dlg->getRemoteTag().c_str());
    ILOG_DLG(L_DBG, "dlg->setRemoteTag(%s)\n", reply.to_tag.c_str());

    /* Overwrite the existing to RemoteTag with the received one in order to store always the last */
    dlg->setRemoteTag(reply.to_tag.c_str());
  } else if(!dlg->getRemoteTag().empty() && dlg->getRemoteTag() != reply.to_tag) {
    ILOG_DLG(L_DBG, "sess %p received %i reply with != to-tag: %s (remote-tag:%s)",
        this, reply.code, reply.to_tag.c_str(),dlg->getRemoteTag().c_str());
    return; /* drop packet */
  }

  if( ((reply.cseq_method == SIP_METH_SUBSCRIBE) ||
       (reply.cseq_method == SIP_METH_NOTIFY) ||
       (reply.cseq_method == SIP_METH_REFER)) &&
       !subs->onReplyIn(req,reply) )
  {
    ILOG_DLG(L_DBG, "subs.onReplyIn returned false\n");
    return;
  }

  if (fwd) {
    updateRefreshMethod(reply.hdrs);
    AmSipReply n_reply = reply;
    n_reply.cseq = t->second.cseq;

    ILOG_DLG(L_DBG, "relaying B2B SIP reply %u %s\n", n_reply.code, n_reply.reason.c_str());
    relayEvent(new B2BSipReplyEvent(n_reply, true, t->second.method, getLocalTag()));

    if (reply.code >= 200) {
      if ((reply.code < 300) && (t->second.method == SIP_METH_INVITE)) {
        ILOG_DLG(L_DBG, "not removing relayed INVITE transaction yet...\n");
      } else {
        /* grab cseq-mqpping in case of REFER */
        if ((reply.code < 300) && (reply.cseq_method == SIP_METH_REFER)) {
          if (subs->subscriptionExists(SingleSubscription::Subscriber, "refer",int2str(reply.cseq))) {
            /* remember mapping for refer event package event-id */
            insertMappedReferID(reply.cseq,t->second.cseq);
          }
        }
        relayed_req.erase(t);
      }
    }

  } else {
    AmSession::onSipReply(req, reply, old_dlg_status);
    AmSipReply n_reply = reply;

    if (est_invite_cseq == reply.cseq) {
      n_reply.cseq = est_invite_other_cseq;
    } else {
      /* correct CSeq for 100 on relayed request (FIXME: why not relayed above?) */
      if (t != relayed_req.end()) {
        n_reply.cseq = t->second.cseq;
      } else {
        /* the reply here will not have the proper cseq for the other side.
         * We should avoid collisions of CSeqs - painful in comparsions with
         * est_invite_cseq where are compared CSeq numbers in different
         * directions. Under presumption that 0 is not used we can use it
         * as 'unspecified cseq' (according to RFC 3261 this seems to be valid
         * value so it need not to work always) */
        n_reply.cseq = 0;
      }
    }

    ILOG_DLG(L_DBG, "relaying B2B SIP reply %u %s\n", n_reply.code, n_reply.reason.c_str());
    relayEvent(new B2BSipReplyEvent(n_reply, false, reply.cseq_method, getLocalTag()));
  }
}

void AmB2BSession::onReplySent(const AmSipRequest& req, const AmSipReply& reply)
{
  if( ((reply.cseq_method == SIP_METH_SUBSCRIBE) ||
       (reply.cseq_method == SIP_METH_NOTIFY) ||
       (reply.cseq_method == SIP_METH_REFER)) ) {
    subs->onReplySent(req,reply);
  }
  
  if(reply.code >= 200 && reply.cseq_method != SIP_METH_CANCEL){
    if((req.method == SIP_METH_INVITE) && (reply.code >= 300)) {
      ILOG_DLG(L_DBG, "relayed INVITE failed with %u %s\n", reply.code, reply.reason.c_str());
    }
    ILOG_DLG(L_DBG, "recvd_req.erase(<%u,%s>)\n", req.cseq, req.method.c_str());
    recvd_req.erase(reply.cseq);
  } 

  AmSession::onReplySent(req,reply);
}

void AmB2BSession::onInvite2xx(const AmSipReply& reply)
{
  TransMap::iterator it = relayed_req.find(reply.cseq);
  bool req_fwded = it != relayed_req.end();

  /* update last reply for further usage with header getters */
  last_200_reply = reply;

  if(!req_fwded) {
    ILOG_DLG(L_DBG, "req not fwded\n");
    AmSession::onInvite2xx(reply);
  } else {
    ILOG_DLG(L_DBG, "no 200 ACK now: waiting for the 200 ACK from the other side...\n");
  }
}

int AmB2BSession::onSdpCompleted(const AmSdp& local_sdp, const AmSdp& remote_sdp)
{
  if (rtp_relay_mode != RTP_Direct) {
    if (!media_session) {
      // report missing media session (here we get for rtp_relay_mode == RTP_Relay)
      ILOG_DLG(L_ERR, "BUG: media session is missing, can't update SDP\n");
    }
    else {
      media_session->updateStreams(a_leg, local_sdp, remote_sdp, this);
    }
  }

  if(hasRtpStream() && RTPStream()->getSdpMediaIndex() >= 0) {
    if(!sip_relay_only){
      return AmSession::onSdpCompleted(local_sdp,remote_sdp);
    }
    ILOG_DLG(L_DBG, "sip_relay_only = true: doing nothing!\n");
  }

  return 0;
}

int AmB2BSession::relayEvent(AmEvent* ev)
{
  ILOG_DLG(L_DBG, "AmB2BSession::relayEvent: to other_id='%s'\n",
      other_id.c_str());

  if(!other_id.empty()) {
    if (!AmSessionContainer::instance()->postEvent(other_id,ev))
      return -1;
  } else {
    delete ev;
  }

  return 0;
}

void AmB2BSession::onOtherBye(const AmSipRequest& req)
{
  ILOG_DLG(L_DBG, "onOtherBye()\n");
  terminateLeg();
}

void AmB2BSession::onOtherCancel()
{
  ILOG_DLG(L_DBG, "The other leg will be canceled, because still in the Early stage.\n");

  setStopped();
  clearRtpReceiverRelay();
  dlg->cancel();
}

bool AmB2BSession::onOtherReply(const AmSipReply& reply)
{
  if(reply.code >= 300) 
    setStopped();
  
  return false;
}

void AmB2BSession::terminateLeg()
{
  setStopped();

  clearRtpReceiverRelay();

  dlg->bye("", SIP_FLAGS_VERBATIM);
}

void AmB2BSession::terminateOtherLeg()
{
  if (!other_id.empty())
    relayEvent(new B2BEvent(B2BTerminateLeg));
}

void AmB2BSession::onRtpTimeout() {
  ILOG_DLG(L_DBG, "RTP Timeout, ending other leg\n");
  terminateOtherLeg();
  AmSession::onRtpTimeout();
}

void AmB2BSession::onSessionTimeout() {
  ILOG_DLG(L_DBG, "Session Timer: Timeout, ending other leg\n");
  terminateOtherLeg();
  AmSession::onSessionTimeout();
}

void AmB2BSession::onRemoteDisappeared(const AmSipReply& reply) {
  if (dlg && dlg->getStatus() == AmBasicSipDialog::Connected) {
    ILOG_DLG(L_DBG, "%c leg: remote unreachable, ending other leg\n", a_leg?'A':'B');
    terminateOtherLeg();
    AmSession::onRemoteDisappeared(reply);
  }
}

void AmB2BSession::onNoAck(unsigned int cseq)
{
  ILOG_DLG(L_DBG, "OnNoAck(%u): terminate other leg.\n",cseq);
  terminateOtherLeg();
  AmSession::onNoAck(cseq);
}

bool AmB2BSession::saveSessionDescription(const AmMimeBody& body) {

  const AmMimeBody* sdp_body = body.hasContentType(SIP_APPLICATION_SDP);
  if(!sdp_body)
    return false;

  ILOG_DLG(L_DBG, "saving session description (%s, %.*s...)\n",
      sdp_body->getCTStr().c_str(), 50, sdp_body->getPayload());

  dlg->established_body = *sdp_body;

  const char* cmp_body_begin = (const char*)sdp_body->getPayload();
  size_t cmp_body_length = sdp_body->getLen();

#define skip_line						\
    while (cmp_body_length && *cmp_body_begin != '\n') {	\
      cmp_body_begin++;						\
      cmp_body_length--;					\
    }								\
    if (cmp_body_length) {					\
      cmp_body_begin++;						\
      cmp_body_length--;					\
    }

  if (cmp_body_length) {
  // for SDP, skip v and o line
  // (o might change even if SDP unchanged)
  skip_line;
  skip_line;
  }

  body_hash = hashlittle(cmp_body_begin, cmp_body_length, 0);
  return true;
}

bool AmB2BSession::updateSessionDescription(const AmMimeBody& body) {

  const AmMimeBody* sdp_body = body.hasContentType(SIP_APPLICATION_SDP);
  if(!sdp_body)
    return false;

  const char* cmp_body_begin = (const char*)sdp_body->getPayload();
  size_t cmp_body_length = sdp_body->getLen();
  if (cmp_body_length) {
    // for SDP, skip v and o line
    // (o might change even if SDP unchanged)
    skip_line;
    skip_line;
  }

#undef skip_line

  uint32_t new_body_hash = hashlittle(cmp_body_begin, cmp_body_length, 0);

  if (body_hash != new_body_hash) {
    ILOG_DLG(L_DBG, "session description changed - saving (%s, %.*s...)\n",
	sdp_body->getCTStr().c_str(), 50, sdp_body->getPayload());
    body_hash = new_body_hash;
    dlg->established_body = body;
    return true;
  }

  return false;
}

int AmB2BSession::sendEstablishedReInvite(const std::string &hdrs) {
  if (dlg->established_body.empty()) {
    ILOG_DLG(L_ERR, "trying to re-INVITE with saved description, but none saved\n");
    return -1;
  }

  ILOG_DLG(L_DBG, "sending re-INVITE with saved session description\n");

  try {
    AmMimeBody body(dlg->established_body); // contains only SDP
    updateLocalBody(body);
    return dlg->reinvite(hdrs, &body, SIP_FLAGS_VERBATIM);
  } catch (const string& s) {
    ILOG_DLG(L_ERR, "sending established SDP reinvite: %s\n", s.c_str());
  }
  return -1;
}

bool AmB2BSession::refresh(int flags) {
  // no session refresh if not connected
  if (dlg->getStatus() != AmSipDialog::Connected)
    return false;

  ILOG_DLG(L_DBG, " AmB2BSession::refresh: refreshing session\n");
  // not in B2B mode
  if (other_id.empty() ||
      // UPDATE as refresh handled like normal session
      refresh_method == REFRESH_UPDATE) {
    return AmSession::refresh(SIP_FLAGS_VERBATIM);
  }

  // refresh with re-INVITE
  if (dlg->getUACInvTransPending()) {
    ILOG_DLG(L_DBG, "INVITE transaction pending - not refreshing now\n");
    return false;
  }
  return sendEstablishedReInvite() == 0;
}

int AmB2BSession::relaySip(const AmSipRequest& req)
{
  AmMimeBody body(req.body);

  if ((req.method == SIP_METH_INVITE ||
       req.method == SIP_METH_UPDATE ||
       req.method == SIP_METH_ACK ||
       req.method == SIP_METH_PRACK))
  {
    updateLocalBody(body);
  }

  /* all methods apart ACK */
  if (req.method != "ACK") {
    relayed_req[dlg->cseq] = req;

    const string* hdrs = &req.hdrs;
    string m_hdrs;

    /* translate RAck for PRACK */
    if (req.method == SIP_METH_PRACK && req.rseq) {
      TransMap::iterator t;

      for (t=relayed_req.begin(); t != relayed_req.end(); t++)
      {
        if (t->second.cseq == req.rack_cseq) {
          m_hdrs = req.hdrs +
                   SIP_HDR_COLSP(SIP_HDR_RACK) +
                   int2str(req.rseq) +
                   " " +
                   int2str(t->first) +
                   " " +
                   req.rack_method +
                   CRLF;
          hdrs = &m_hdrs;
          break;
        }
      }

      if (t==relayed_req.end()) {
        ILOG_DLG(L_WARN, "Transaction with CSeq %d not found for translating RAck cseq\n", req.rack_cseq);
      }
    }

    ILOG_DLG(L_DBG, "relaying SIP request %s %s\n", req.method.c_str(), req.r_uri.c_str());

    int err = dlg->sendRequest(req.method, &body, *hdrs, SIP_FLAGS_VERBATIM);

    if(err < 0){
      ILOG_DLG(L_ERR, "dlg->sendRequest() failed\n");
      return err;
    }

    if ((req.method == SIP_METH_INVITE ||
        req.method == SIP_METH_UPDATE) &&
        !req.body.empty())
    {
      saveSessionDescription(req.body);
    }

  } else {
    /* all other methods (most probably 200OK for ACK) */
    TransMap::iterator t = relayed_req.begin();

    while (t != relayed_req.end())
    {
      if (t->second.cseq == req.cseq)
      break;
      t++;
    }

    if (t == relayed_req.end()) {
      ILOG_DLG(L_ERR, "transaction for ACK not found in relayed requests\n");
      /* FIXME: local body (if updated) should be discarded here */
      return -1;
    }

    ILOG_DLG(L_DBG, "sending relayed 200 ACK\n");

    int err = dlg->send_200_ack(t->first, &body, req.hdrs, SIP_FLAGS_VERBATIM);
    if(err < 0) {
      ILOG_DLG(L_ERR, "dlg->send_200_ack() failed\n");
      return err;
    }

    if (!req.body.empty() &&
        (t->second.method == SIP_METH_INVITE))
    {
      /* delayed SDP negotiation - save SDP */
      saveSessionDescription(req.body);
    }

    relayed_req.erase(t);
  }

  return 0;
}

int AmB2BSession::relaySip(const AmSipRequest& orig, const AmSipReply& reply)
{
  const string* hdrs = &reply.hdrs;
  string m_hdrs;
  const string method(orig.method);

  if (reply.rseq != 0) {
    m_hdrs = reply.hdrs +
      SIP_HDR_COLSP(SIP_HDR_RSEQ) + int2str(reply.rseq) + CRLF;
    hdrs = &m_hdrs;
  }

  AmMimeBody body(reply.body);
  if ((orig.method == SIP_METH_INVITE ||
       orig.method == SIP_METH_UPDATE ||
       orig.method == SIP_METH_ACK ||
       orig.method == SIP_METH_PRACK))
  {
    updateLocalBody(body);
  }

  ILOG_DLG(L_DBG, "relaying SIP reply %u %s\n", reply.code, reply.reason.c_str());

  int flags = SIP_FLAGS_VERBATIM;
  if(reply.to_tag.empty())
    flags |= SIP_FLAGS_NOTAG;

  int err = dlg->reply(orig,reply.code,reply.reason,
		       &body, *hdrs, flags);

  if(err < 0){
    ILOG_DLG(L_ERR, "dlg->reply() failed\n");
    return err;
  }

  if ((method == SIP_METH_INVITE ||
       method == SIP_METH_UPDATE) &&
      !reply.body.empty()) {
    saveSessionDescription(reply.body);
  }

  return 0;
}

void AmB2BSession::setRtpRelayMode(RTPRelayMode mode)
{
  ILOG_DLG(L_DBG, "enabled RTP relay mode for B2B call '%s'\n",
      getLocalTag().c_str());

  rtp_relay_mode = mode;
}

void AmB2BSession::setRtpInterface(int relay_interface) {
  ILOG_DLG(L_DBG, "setting RTP interface for session '%s' to %i\n",
      getLocalTag().c_str(), relay_interface);
  rtp_interface = relay_interface;
}

void AmB2BSession::setRtpRelayForceSymmetricRtp(bool force_symmetric) {
  rtp_relay_force_symmetric_rtp = force_symmetric;
}

void AmB2BSession::setRtpRelayTransparentSeqno(bool transparent) {
  rtp_relay_transparent_seqno = transparent;
}

void AmB2BSession::setRtpRelayTransparentSSRC(bool transparent) {
  rtp_relay_transparent_ssrc = transparent;
}

void AmB2BSession::setEnableDtmfTranscoding(bool enable) {
  enable_dtmf_transcoding = enable;
}

void AmB2BSession::setEnableDtmfRtpFiltering(bool enable) {
  enable_dtmf_rtp_filtering = enable;
}

void AmB2BSession::setEnableDtmfRtpDetection(bool enable) {
  enable_dtmf_rtp_detection = enable;
}

void AmB2BSession::getLowFiPLs(vector<SdpPayload>& lowfi_payloads) const {
  lowfi_payloads = this->lowfi_payloads;
}

void AmB2BSession::setLowFiPLs(const vector<SdpPayload>& lowfi_payloads) {
  this->lowfi_payloads = lowfi_payloads;
}

void AmB2BSession::clearRtpReceiverRelay() {
  switch (rtp_relay_mode) {

    case RTP_Relay:
    case RTP_Transcoding:
      if (media_session) { 
        media_session->stop(a_leg);
        media_session->releaseReference();
        media_session = NULL;
      }
      break;

    case RTP_Direct:
      // nothing to do
      break;
  }
}

void AmB2BSession::computeRelayMask(const SdpMedia &m, bool &enable, PayloadMask &mask)
{
  int te_pl = -1;
  enable = false;

  mask.clear();

  // walk through the media lines and find the telephone-event payload
  for (std::vector<SdpPayload>::const_iterator i = m.payloads.begin();
      i != m.payloads.end(); ++i)
  {
    // do not mark telephone-event payload for relay
    if(!strcasecmp("telephone-event",i->encoding_name.c_str())){
      te_pl = i->payload_type;
    }
    else {
      enable = true;
    }
  }

  if(!enable)
    return;

  if(te_pl > 0) {
    ILOG_DLG(L_DBG, "unmarking telephone-event payload %d for relay\n", te_pl);
    mask.set(te_pl);
  }

  ILOG_DLG(L_DBG, "marking all other payloads for relay\n");
  mask.invert();
}


// 
// AmB2BCallerSession methods
//

AmB2BCallerSession::AmB2BCallerSession()
  : AmB2BSession(),
    callee_status(None), sip_relay_early_media_sdp(false)
{
  a_leg = true;
}

AmB2BCallerSession::~AmB2BCallerSession()
{
}

void AmB2BCallerSession::set_sip_relay_early_media_sdp(bool r)
{
  sip_relay_early_media_sdp = r; 
}

void AmB2BCallerSession::terminateLeg()
{
  AmB2BSession::terminateLeg();
}

void AmB2BCallerSession::terminateOtherLeg()
{
  AmB2BSession::terminateOtherLeg();
  callee_status = None;
}

void AmB2BCallerSession::onB2BEvent(B2BEvent* ev)
{
  bool processed = false;

  if (ev->event_id == B2BSipReply) {

    AmSipReply& reply = ((B2BSipReplyEvent*)ev)->reply;

    /* update last reply for further usage with header getters */
    last_200_reply = reply;

    if (getOtherId().empty()) {
      ILOG_DLG(L_DBG, "B2BSipReply: other_id empty (reply code=%i; method=%s; callid=%s; from_tag=%s; to_tag=%s; cseq=%i)\n",
        reply.code,reply.cseq_method.c_str(),reply.callid.c_str(),reply.from_tag.c_str(),
        reply.to_tag.c_str(),reply.cseq);

    } else if (getOtherId() != reply.from_tag) { /* was: local_tag */
      ILOG_DLG(L_DBG, "Dialog mismatch! (oi=%s;ft=%s)\n", getOtherId().c_str(), reply.from_tag.c_str());
      return;
    }

    ILOG_DLG(L_DBG, "%u %s reply received from other leg\n", reply.code, reply.reason.c_str());
      
    switch (callee_status) {
      case NoReply:
      case Ringing:
        if (reply.cseq == invite_req.cseq) {

          /* get possibly passed headers for updates towards caller */
          string hdrs;
          map<string,string>::const_iterator hdrs_it;
          hdrs_it = ((B2BSipReplyEvent*)ev)->params.find("hdrs");
          if (hdrs_it != ((B2BSipReplyEvent*)ev)->params.end()) {
            hdrs = hdrs_it->second;
            ILOG_DLG(L_DBG, "Got some headers, which can later be used for re-inviting the caller: '%s'\n", hdrs.c_str());
          }

          if (reply.code < 200) {
            if ((!sip_relay_only) &&
                (reply.code>=180 && reply.code<=183 && (!reply.body.empty()))) {

              /* save early media SDP */
              updateSessionDescription(reply.body);

              if (sip_relay_early_media_sdp) {
                if (reinviteCaller(reply, hdrs)) {
                  ILOG_DLG(L_ERR, "re-INVITEing caller for early session failed - stopping this and other leg\n");
                  terminateOtherLeg();
                  terminateLeg();
                  break;
                }
              }
            }

            callee_status = Ringing;

          } else if(reply.code < 300) {

            callee_status  = Connected;
            ILOG_DLG(L_DBG, "setting callee status to connected\n");

            if (!sip_relay_only) {
              ILOG_DLG(L_DBG, "received 200 class reply to establishing INVITE: "
                  "switching to SIP relay only mode, sending re-INVITE to caller\n");

              sip_relay_only = true;

              if (reinviteCaller(reply, hdrs)) {
                ILOG_DLG(L_ERR, "re-INVITEing caller failed - stopping this and other leg\n");
                terminateOtherLeg();
                terminateLeg();
              }
            }

          } else {
            /* TODO: terminated my own leg instead? (+ clear_other()) */
            terminateOtherLeg();
          }

          processed = onOtherReply(reply);
        }
        break;
	
      default:
        ILOG_DLG(L_DBG, "reply from callee: %u %s\n",reply.code,reply.reason.c_str());
        break;
    }
  }

  if (!processed)
    AmB2BSession::onB2BEvent(ev);
}

int AmB2BCallerSession::relayEvent(AmEvent* ev)
{
  if(getOtherId().empty() && !getStopped()){

    bool create_callee = false;
    B2BSipEvent* sip_ev = dynamic_cast<B2BSipEvent*>(ev);
    if (sip_ev && sip_ev->forward)
      create_callee = true;
    else
      create_callee = dynamic_cast<B2BConnectEvent*>(ev) != NULL;

    if (create_callee) {
      createCalleeSession();
      if (getOtherId().length()) {
	MONITORING_LOG(getLocalTag().c_str(), "b2b_leg", getOtherId().c_str());
      }
    }

  }

  return AmB2BSession::relayEvent(ev);
}

void AmB2BCallerSession::onInvite(const AmSipRequest& req)
{
  invite_req = req;
  est_invite_cseq = req.cseq;

  AmB2BSession::onInvite(req);
}

void AmB2BCallerSession::onInviteKeepSDP(const AmSipRequest& req)
{
  /* save SDP body to re-use if newer request has no SDP */
  AmMimeBody previous_body(invite_req.body);
  invite_req = req;

  if (invite_req.body.empty() && !previous_body.empty()) {
     invite_req.body = previous_body;
     ILOG_DLG(L_DBG, "Currently processed INVITE has no SDP body, use the one from previous offer.\n");
  }

  est_invite_cseq = req.cseq;

  AmB2BSession::onInvite(req);
}

void AmB2BCallerSession::onInvite2xx(const AmSipReply& reply)
{
  invite_req.cseq = reply.cseq;
  est_invite_cseq = reply.cseq;

  AmB2BSession::onInvite2xx(reply);
}

void AmB2BCallerSession::onCancel(const AmSipRequest& req)
{
  terminateOtherLeg();
  terminateLeg();
}

void AmB2BCallerSession::onSystemEvent(AmSystemEvent* ev) {
  if (ev->sys_event == AmSystemEvent::ServerShutdown) {
    terminateOtherLeg();
  }

  AmB2BSession::onSystemEvent(ev);
}

void AmB2BCallerSession::onRemoteDisappeared(const AmSipReply& reply) {
  ILOG_DLG(L_DBG, "remote unreachable, ending B2BUA call\n");
  clearRtpReceiverRelay();

  AmB2BSession::onRemoteDisappeared(reply);
}

void AmB2BCallerSession::onBye(const AmSipRequest& req)
{
  clearRtpReceiverRelay();
  AmB2BSession::onBye(req);
}

void AmB2BCallerSession::connectCallee(const string& remote_party,
				       const string& remote_uri,
				       bool relayed_invite)
{
  if(callee_status != None)
    terminateOtherLeg();

  clear_other();

  if (relayed_invite) {
    // relayed INVITE - we need to add the original INVITE to
    // list of received (relayed) requests
    recvd_req.insert(std::make_pair(invite_req.cseq,invite_req));

    // in SIP relay mode from the beginning
    sip_relay_only = true;
  }

  B2BConnectEvent* ev = new B2BConnectEvent(remote_party,remote_uri);

  ev->body         = invite_req.body;
  ev->hdrs         = invite_req.hdrs;
  ev->relayed_invite = relayed_invite;
  ev->r_cseq       = invite_req.cseq;

  ILOG_DLG(L_DBG, "relaying B2B connect event to %s\n", remote_uri.c_str());
  relayEvent(ev);
  callee_status = NoReply;
}

int AmB2BCallerSession::reinviteCaller(const AmSipReply& callee_reply, const string& hdrs)
{
  return dlg->sendRequest(SIP_METH_INVITE,
			 &callee_reply.body,
			 hdrs, SIP_FLAGS_VERBATIM);
}

void AmB2BCallerSession::createCalleeSession() {
  AmB2BCalleeSession* callee_session = newCalleeSession();  
  if (NULL == callee_session) 
    return;

  AmSipDialog* callee_dlg = callee_session->dlg;

  setOtherId(AmSession::getNewId());
  
  callee_dlg->setLocalTag(getOtherId());
  if (callee_dlg->getCallid().empty())
    callee_dlg->setCallid(AmSession::getNewId());

  callee_dlg->setLocalParty(dlg->getRemoteParty());
  callee_dlg->setRemoteParty(dlg->getLocalParty());
  callee_dlg->setRemoteUri(dlg->getLocalUri());

  if (AmConfig::LogSessions) {
    ILOG_DLG(L_INFO, "Starting B2B callee session %s\n",
	 callee_session->getLocalTag().c_str());
  }

  MONITORING_LOG4(getOtherId().c_str(), 
		  "dir",  "out",
		  "from", callee_dlg->getLocalParty().c_str(),
		  "to",   callee_dlg->getRemoteParty().c_str(),
		  "ruri", callee_dlg->getRemoteUri().c_str());

  try {
    initializeRTPRelay(callee_session);
  } catch (...) {
    delete callee_session;
    throw;
  }

  AmSessionContainer* sess_cont = AmSessionContainer::instance();
  sess_cont->addSession(getOtherId(),callee_session);

  callee_session->start();
}

AmB2BCalleeSession* AmB2BCallerSession::newCalleeSession()
{
  return new AmB2BCalleeSession(this);
}
    
void AmB2BSession::setMediaSession(AmB2BMedia *new_session) 
{ 
  // FIXME: ignore old media_session? can it be already set here?
  if (media_session) ILOG_DLG(L_ERR, "BUG: non-empty media session overwritten\n");
  media_session = new_session; 
  if (media_session)
    media_session->addReference(); // new reference for me
}

void AmB2BCallerSession::initializeRTPRelay(AmB2BCalleeSession* callee_session) {
  if (!callee_session) return;
  
  callee_session->setRtpRelayMode(rtp_relay_mode);
  callee_session->setEnableDtmfTranscoding(enable_dtmf_transcoding);
  callee_session->setEnableDtmfRtpFiltering(enable_dtmf_rtp_filtering);
  callee_session->setEnableDtmfRtpDetection(enable_dtmf_rtp_detection);
  callee_session->setLowFiPLs(lowfi_payloads);

  if ((rtp_relay_mode == RTP_Relay) || (rtp_relay_mode == RTP_Transcoding)) {
    setMediaSession(new AmB2BMedia(this, callee_session)); // we need to add our reference
    callee_session->setMediaSession(getMediaSession());
  }
}

AmB2BCalleeSession::AmB2BCalleeSession(const string& other_local_tag)
  : AmB2BSession(other_local_tag)
{
  a_leg = false;
}

AmB2BCalleeSession::AmB2BCalleeSession(const AmB2BCallerSession* caller)
  : AmB2BSession(caller->getLocalTag())
{
  a_leg = false;
  rtp_relay_mode = caller->getRtpRelayMode();
  rtp_relay_force_symmetric_rtp = caller->getRtpRelayForceSymmetricRtp();
}

AmB2BCalleeSession::~AmB2BCalleeSession() {
}

void AmB2BCalleeSession::onB2BEvent(B2BEvent* ev)
{
  if(ev->event_id == B2BConnectLeg){
    B2BConnectEvent* co_ev = dynamic_cast<B2BConnectEvent*>(ev);
    if (!co_ev)
      return;

    MONITORING_LOG3(getLocalTag().c_str(), 
		    "b2b_leg", getOtherId().c_str(),
		    "to", co_ev->remote_party.c_str(),
		    "ruri", co_ev->remote_uri.c_str());


    dlg->setRemoteParty(co_ev->remote_party);
    dlg->setRemoteUri(co_ev->remote_uri);

    if (co_ev->relayed_invite) {
      AmSipRequest fake_req;
      fake_req.method = SIP_METH_INVITE;
      fake_req.cseq = co_ev->r_cseq;
      relayed_req[dlg->cseq] = fake_req;
    }

    AmMimeBody body(co_ev->body);
    try {
      updateLocalBody(body);
    } catch (const string& s) {
      relayError(SIP_METH_INVITE, co_ev->r_cseq, co_ev->relayed_invite, 500, 
          SIP_REPLY_SERVER_INTERNAL_ERROR);
      throw;
    }

    int res = dlg->sendRequest(SIP_METH_INVITE, &body,
			co_ev->hdrs, SIP_FLAGS_VERBATIM);
    if (res < 0) {
      ILOG_DLG(L_DBG, "sending INVITE failed, relaying back error reply\n");
      relayError(SIP_METH_INVITE, co_ev->r_cseq, co_ev->relayed_invite, res);

      if (co_ev->relayed_invite)
	relayed_req.erase(dlg->cseq);

      setStopped();
      return;
    }

    saveSessionDescription(co_ev->body);

    // save CSeq of establising INVITE
    est_invite_cseq = dlg->cseq - 1;
    est_invite_other_cseq = co_ev->r_cseq;

    return;
  }    

  AmB2BSession::onB2BEvent(ev);
}

/** EMACS **
 * Local variables:
 * mode: c++
 * c-basic-offset: 2
 * End:
 */
