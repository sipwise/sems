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

#include "AmSipDialog.h"
#include "AmConfig.h"
#include "AmSession.h"
#include "AmUtils.h"
#include "AmSipHeaders.h"
#include "SipCtrlInterface.h"
#include "sems.h"

#include "sip/parse_route.h"
#include "sip/parse_uri.h"
#include "sip/parse_next_hop.h"

#include "AmB2BMedia.h" // just because of statistics

#include "global_defs.h"

#define GET_CALL_ID() (getCallid().c_str())

//
// helper functions
//

static void addTranscoderStats(string &hdrs)
{
  // add transcoder statistics into request/reply headers
  if (!AmConfig::TranscoderOutStatsHdr.empty()) {
    string usage;
    B2BMediaStatistics::instance()->reportCodecWriteUsage(usage);

    hdrs += AmConfig::TranscoderOutStatsHdr + ": ";
    hdrs += usage;
    hdrs += CRLF;
  }
  if (!AmConfig::TranscoderInStatsHdr.empty()) {
    string usage;
    B2BMediaStatistics::instance()->reportCodecReadUsage(usage);

    hdrs += AmConfig::TranscoderInStatsHdr + ": ";
    hdrs += usage;
    hdrs += CRLF;
  }
}

static bool isDSMEarlyAnnounceForced(const std::string &hdrs)
{
    string announce = getHeader(hdrs, SIP_HDR_P_DSM_APP);
    string p_dsm_app_param = get_header_param(announce, DSM_PARAM_EARLY_AN);
    return p_dsm_app_param == DSM_VALUE_FORCE;
}

static bool isDSMPlaybackFinished(const std::string &hdrs)
{
  /** TODO: for the future, we might also want to check
   *  particular DSM applications, for now plays no role.
   */
  string p_dsm_app = getHeader(hdrs, SIP_HDR_P_DSM_APP, true);
  string p_dsm_app_param = get_header_param(p_dsm_app, DSM_PARAM_PLAYBACK);
  return p_dsm_app_param == DSM_VALUE_FINISHED;
}

AmSipDialog::AmSipDialog(AmSipDialogEventHandler* h)
  : AmBasicSipDialog(h),oa(this),rel100(this,h),
    offeranswer_enabled(true),
    early_session_started(false),session_started(false),
    pending_invites(0),
    sdp_local(), sdp_remote(), faked_183_as_200(false), force_early_announce(false)
{
}

AmSipDialog::~AmSipDialog()
{
}

bool AmSipDialog::onRxReqSanity(const AmSipRequest& req)
{
  if (req.method == SIP_METH_ACK) {
    if(onRxReqStatus(req) && hdl)
      hdl->onSipRequest(req);
    return false;
  }

  if (req.method == SIP_METH_CANCEL) {

    if (uas_trans.find(req.cseq) == uas_trans.end()) {
      reply_error(req,481,SIP_REPLY_NOT_EXIST);
      return false;
    }

    if(onRxReqStatus(req) && hdl)
      hdl->onSipRequest(req);

    return false;
  }

  if(!AmBasicSipDialog::onRxReqSanity(req))
    return false;

  if (req.method == SIP_METH_INVITE) {
    bool pending = pending_invites;
    if (offeranswer_enabled) {
      // not sure this is needed here: could be in AmOfferAnswer as well
      pending |= ((oa.getState() != AmOfferAnswer::OA_None) &&
		  (oa.getState() != AmOfferAnswer::OA_Completed));
    }

    if (pending) {
      reply_error(req, 491, SIP_REPLY_PENDING,
		  SIP_HDR_COLSP(SIP_HDR_RETRY_AFTER) 
		  + int2str(get_random() % 10) + CRLF);
      return false;
    }

    pending_invites++;
  }

  return rel100.onRequestIn(req);
}

bool AmSipDialog::onRxReqStatus(const AmSipRequest& req)
{
  switch(status){
  case Disconnected:
    if(req.method == SIP_METH_INVITE)
      setStatus(Trying);
    break;
  case Connected:
    if(req.method == SIP_METH_BYE)
      setStatus(Disconnecting);
    break;

  case Trying:
  case Proceeding:
  case Early:
    if(req.method == SIP_METH_BYE)
      setStatus(Disconnecting);
    else if(req.method == SIP_METH_CANCEL){
      setStatus(Cancelling);
      reply(req,200,"OK");
    }
    break;

  default: break;
  }

  bool cont = true;
  if (offeranswer_enabled) {
    cont = (oa.onRequestIn(req) == 0);
  }

  return cont;
}

int AmSipDialog::onSdpCompleted()
{
  if(!hdl) return 0;

  int ret = ((AmSipDialogEventHandler*)hdl)->
    onSdpCompleted(oa.getLocalSdp(), oa.getRemoteSdp());

  if(!ret) {
    sdp_local = oa.getLocalSdp();
    sdp_remote = oa.getRemoteSdp();

    if((getStatus() == Early) && !early_session_started) {
      ((AmSipDialogEventHandler*)hdl)->onEarlySessionStart();
      early_session_started = true;
    }

    if((getStatus() == Connected) && !session_started) {
      ((AmSipDialogEventHandler*)hdl)->onSessionStart();
      session_started = true;
    }
  }
  else {
    oa.clear();
  }

  return ret;
}

void AmSipDialog::onSdpReceived(bool is_offer)
{
  if (!hdl) return;

  ((AmSipDialogEventHandler*)hdl)->onSdpReceived(oa.getRemoteSdp(), is_offer);
}

bool AmSipDialog::getSdpOffer(AmSdp& offer)
{
  if(!hdl) return false;
  return ((AmSipDialogEventHandler*)hdl)->getSdpOffer(offer);
}

bool AmSipDialog::getSdpAnswer(const AmSdp& offer, AmSdp& answer)
{
  if(!hdl) return false;
  return ((AmSipDialogEventHandler*)hdl)->getSdpAnswer(offer,answer);
}

AmOfferAnswer::OAState AmSipDialog::getOAState() {
  return oa.getState();
}

void AmSipDialog::setOAState(AmOfferAnswer::OAState n_st) {
  oa.setState(n_st);
}

/** are we expecting to receive an SDP offer? */
bool AmSipDialog::oaExpectingOffer() {
  return oa.getState() == AmOfferAnswer::OA_None ||
    oa.getState() == AmOfferAnswer::OA_Completed;
}

void AmSipDialog::setRel100State(Am100rel::State rel100_state) {
  ILOG_DLG(L_DBG, "setting 100rel state for '%s' to %i\n", local_tag.c_str(), rel100_state);
  rel100.setState(rel100_state);
}

bool AmSipDialog::willBeReliable(const AmSipReply& reply)
{
  return rel100.willBeReliable(reply);
}

void AmSipDialog::setOAEnabled(bool oa_enabled) {
  ILOG_DLG(L_DBG, "%sabling offer_answer on SIP dialog '%s'\n",
      oa_enabled?"en":"dis", local_tag.c_str());
  offeranswer_enabled = oa_enabled;
}

int AmSipDialog::onTxRequest(AmSipRequest& req, int& flags)
{
  rel100.onRequestOut(req);

  if (offeranswer_enabled && oa.onRequestOut(req))
    return -1;

  if(AmBasicSipDialog::onTxRequest(req,flags) < 0)
    return -1;

  // add transcoder statistics into request headers
  addTranscoderStats(req.hdrs);

  if((req.method == SIP_METH_INVITE) && (status == Disconnected)){
    setStatus(Trying);
  }
  else if((req.method == SIP_METH_BYE) && (status != Disconnecting)){
    setStatus(Disconnecting);
  }

  if ((req.method == SIP_METH_BYE) || (req.method == SIP_METH_CANCEL)) {
    flags |= SIP_FLAGS_NOCONTACT;
  }

  return 0;
}

// UAS behavior for locally sent replies
int AmSipDialog::onTxReply(const AmSipRequest& req, AmSipReply& reply, int& flags)
{
  if (offeranswer_enabled) {
    AmMimeBody sdp_body;
    if(oa.onReplyOut(reply, flags, sdp_body, req.body.empty()) < 0)
      return -1;

    /* if generated by OA, save it */
    if (!sdp_body.empty()) {
      ILOG_DLG(L_DBG, "OA generated an SDP body, saving as established_body.\n");
      established_body = sdp_body;
    }
  }

  rel100.onReplyOut(reply);

  // update Dialog status
  switch(status){

  case Connected:
  case Disconnected:
    break;

  case Cancelling:
    if( (reply.cseq_method == SIP_METH_INVITE) &&
	(reply.code < 200) ) {
      // refuse local provisional replies 
      // when state is Cancelling
      ILOG_DLG(L_ERR, "refuse local provisional replies when state is Cancelling\n");
      return -1;
    }
    // else continue with final
    // reply processing
  case Proceeding:
  case Trying:
  case Early:
    if(reply.cseq_method == SIP_METH_INVITE){
      if(reply.code < 200) {
	setStatus(Early);
      }
      else if(reply.code < 300)
	setStatus(Connected);
      else
	setStatus(Disconnected);
    }
    break;

  case Disconnecting:
    if(reply.cseq_method == SIP_METH_BYE){

      // Only reason for refusing a BYE: 
      //  authentication (NYI at this place)
      // Also: we should not send provisionnal replies to a BYE
      if(reply.code >= 200)
	setStatus(Disconnected);
    }
    break;

  default:
    assert(0);
    break;
  }

  // add transcoder statistics into reply headers
  addTranscoderStats(reply.hdrs);

  // target-refresh requests and their replies need to contain Contact (1xx
  // replies only those establishing dialog, take care about them?)
  if(reply.cseq_method != SIP_METH_INVITE && 
     reply.cseq_method != SIP_METH_UPDATE) {
    
    flags |= SIP_FLAGS_NOCONTACT;
  }

  return AmBasicSipDialog::onTxReply(req,reply,flags);
}

void AmSipDialog::onReplyTxed(const AmSipRequest& req, const AmSipReply& reply)
{
  AmBasicSipDialog::onReplyTxed(req,reply);

  if (offeranswer_enabled) {
    oa.onReplySent(reply);
  }

  if (reply.code >= 200) {
    if(reply.cseq_method == SIP_METH_INVITE)
	pending_invites--;
  }
}

void AmSipDialog::onRequestTxed(const AmSipRequest& req)
{
  AmBasicSipDialog::onRequestTxed(req);

  if (offeranswer_enabled) {
    oa.onRequestSent(req);
  }
}

bool AmSipDialog::onRxReplySanity(const AmSipReply& reply)
{
  if(!getRemoteTag().empty()
     && reply.to_tag != getRemoteTag()) {

    if(status == Early) {
      if(reply.code < 200 && !reply.to_tag.empty()) {
        return false;// DROP
      }
    }
    else {
      // DROP
      return false;
    }
  }

  return true;
}

bool AmSipDialog::onRxReplyStatus(const AmSipReply& reply)
{
  /* rfc3261 12.1
     Dialog established only by 101-199 or 2xx
     responses to INVITE */

  ILOG_DLG(L_DBG, "onRxReplyStatus: reply.code = <%d>, reply.route = <%s>, status = <%d>\n",
      reply.code, reply.route.c_str(), status);

  /* INVITE */
  if (reply.cseq_method == SIP_METH_INVITE) {

    switch (status) {

      case Trying:
      case Proceeding:

        ILOG_DLG(L_DBG, "This is the Proceeding stage of the dialog.\n");

        /* 100-199 */
        if (reply.code < 200) {
          setForcedEarlyAnnounce(isDSMEarlyAnnounceForced(reply.hdrs));

          if (reply.code == 100 || reply.to_tag.empty()) {
            setStatus(Proceeding);
          } else {
            setStatus(Early);
            setRemoteTag(reply.to_tag);
            setRouteSet(reply.route);
          }

          /* we should always keep Route set for this leg updated in case
             the provisional response updates the list of routes for any reason */
          if ((reply.code == 180 || reply.code == 183) && !reply.route.empty()) {
            ILOG_DLG(L_DBG, "<%d> Response code is processed, reset the Route set for the leg.\n",
                reply.code);
            setRouteSet(reply.route);
          }

          /* exceptionally treat 183 with the 'P-DSM-App: <app-name>;early-announce=force',
             similarly to the 200OK response, this will properly update the caller
             with the late SDP capabilities (an early announcement),
             which has been put on hold during the transfer

             And furthermore will give the possibility to receive and forward BYE.

             DSM applications using it:
             - early-dbprompt
             - pre-announce
             - play-last-caller
             - office-hours */
          if (reply.code == 183 && getForcedEarlyAnnounce()) {
            ILOG_DLG(L_DBG, "This is 183 with <;%s=%s>, treated exceptionally as 200OK.\n", DSM_PARAM_EARLY_AN, DSM_VALUE_FORCE);

            setStatus(Connected);
            setFaked183As200(true); /* remember that this is a faked 200OK, indeed 183 */

            if (reply.to_tag.empty()) {
              ILOG_DLG(L_DBG, "received 2xx reply without to-tag (callid=%s): sending BYE\n",
                  reply.callid.c_str());
              sendRequest(SIP_METH_BYE);
            }	else {
              setRemoteTag(reply.to_tag);
            }
          }

        /* 200-299 */
        } else if(reply.code < 300) {
          setStatus(Connected);
          setRouteSet(reply.route);

          if (reply.to_tag.empty()){
            ILOG_DLG(L_DBG, "received 2xx reply without to-tag (callid=%s): sending BYE\n",
                reply.callid.c_str());
            send_200_ack(reply.cseq);
            sendRequest(SIP_METH_BYE);
          } else {
            setRemoteTag(reply.to_tag);
          }

        /* 300-699 */
        } else {
          setStatus(Disconnected);
          setRemoteTag(reply.to_tag);
        }
        break;

      case Early:

        ILOG_DLG(L_DBG, "This is the Early stage of the dialog.\n");

        /* 100-199 */
        if (reply.code < 200) {

          setForcedEarlyAnnounce(isDSMEarlyAnnounceForced(reply.hdrs));

          /* we should always keep Route set for this leg updated in case
             the provisional response updates the list of routes for any reason */
          if ((reply.code == 180 || reply.code == 183) && !reply.route.empty()) {
            ILOG_DLG(L_DBG, "<%d> Response code is processed, reset the Route set for the leg.\n",
                reply.code);
            setRouteSet(reply.route);
          }

          /* exceptionally treat 183 with the 'P-DSM-App: <app-name>;early-announce=force',
             similarly to the 200OK response, this will properly update the caller
             with the late SDP capabilities (an early announcement),
             which has been put on hold during the transfer

             And furthermore will give the possibility to receive and forward BYE.

             DSM applications using it:
             - early-dbprompt
             - pre-announce
             - play-last-caller
             - office-hours */
          if (reply.code == 183 && getForcedEarlyAnnounce()) {
            ILOG_DLG(L_DBG, "This is 183 with <;%s=%s>, treated exceptionally as 200OK.\n", DSM_PARAM_EARLY_AN, DSM_VALUE_FORCE);

            setStatus(Connected);
            setFaked183As200(true); /* remember that this is a faked 200OK, indeed 183 */

            if (reply.to_tag.empty()) {
              ILOG_DLG(L_DBG, "received 2xx reply without to-tag (callid=%s): sending BYE\n",
                  reply.callid.c_str());
              sendRequest(SIP_METH_BYE);
            }	else {
              setRemoteTag(reply.to_tag);
            }
          }

        /* 200-299 */
        } else if(reply.code < 300) {
          setStatus(Connected);
          setRouteSet(reply.route);

          /* reset faked 183, if was previously set and this is 200OK received in this leg */
          if (getFaked183As200())
            setFaked183As200(false);

          if (reply.to_tag.empty()) {
            ILOG_DLG(L_DBG, "received 2xx reply without to-tag (callid=%s): sending BYE\n",
                reply.callid.c_str());
            sendRequest(SIP_METH_BYE);
          } else {
            setRemoteTag(reply.to_tag);
          }

        /* 300-699 */
        } else {
          setStatus(Disconnected);
          setRemoteTag(reply.to_tag);
        }

        break;

      case Cancelling:

        if (reply.code >= 300) { /* CANCEL accepted */
          ILOG_DLG(L_DBG, "CANCEL accepted, status -> Disconnected\n");
          setStatus(Disconnected);

        } else if(reply.code < 300) { /* CANCEL rejected */
          ILOG_DLG(L_DBG, "CANCEL rejected/too late - bye()\n");
          setRemoteTag(reply.to_tag);
          setStatus(Connected);
          bye();
          /* if for any reason BYE could not be sent,
          there is nothing we can do anymore */
        }

        break;

      /* TODO: if reply.to_tag != getRemoteTag()
       *       -> ACK + BYE (+absorb answer) */
      case Connected:

        ILOG_DLG(L_DBG, "This is the Connected stage of the dialog.\n");

        /* treat 4XX class of responses for the faked connected state of the dlg
         * as those which finilize a DSM playback (check additionally P-DSM-App header)
         */
        if ((reply.code > 400 && reply.code < 500) &&
            getFaked183As200() && isDSMPlaybackFinished(reply.hdrs))
        {
          setStatus(Disconnected);
        }

        break;

      default:
        break;
    }

  /* PRACK */
  } else if (reply.cseq_method == SIP_METH_PRACK) {
    /* do not update call leg status for transactions not involving INVITE.
     * In this case just update the to-tag and route set */
    if (!reply.to_tag.empty()) {
      ILOG_DLG(L_DBG, "Updating remote tag (to tag) to: '%s'.\n", reply.to_tag.c_str());
      setRemoteTag(reply.to_tag);
    }
    if (!reply.route.empty()) {
      ILOG_DLG(L_DBG, "Updating route set to: '%s'.\n", reply.route.c_str());
      setRouteSet(reply.route);
    }
  }

  if (status == Disconnecting || status == Cancelling) {
    ILOG_DLG(L_DBG, "%s: cseq_method = %s; code = %i\n",
        status == Disconnecting ? "Disconnecting" : "Cancelling",
        reply.cseq_method.c_str(), reply.code);

    if (((status == Disconnecting && reply.cseq_method == SIP_METH_BYE) ||
         (status == Cancelling && reply.cseq_method == SIP_METH_CANCEL)) &&
        (reply.code >= 200))
    {
      /* TODO: support the auth case here (401/403) */
      setStatus(Disconnected);
    }
  }

  if (offeranswer_enabled) {
    oa.onReplyIn(reply);
  }

  bool cont = true;

  /* For those exceptional 183 with the 'P-DSM-App: <app-name>;early-announce=force'
     we don't want to fully imitate 200OK processing, and send ACK
     further processing with ACK is only applied to real 200OK responses */
  if ( (reply.code >= 200) && (reply.code < 300) &&
       (reply.cseq_method == SIP_METH_INVITE) ) {

    if(hdl) ((AmSipDialogEventHandler*)hdl)->onInvite2xx(reply);

  } else {
    cont = AmBasicSipDialog::onRxReplyStatus(reply);
  }

  return cont && rel100.onReplyIn(reply);
}

void AmSipDialog::uasTimeout(AmSipTimeoutEvent* to_ev)
{
  assert(to_ev);

  switch(to_ev->type){
  case AmSipTimeoutEvent::noACK:
    ILOG_DLG(L_DBG, "Timeout: missing ACK\n");
    if (offeranswer_enabled) {
      oa.onNoAck(to_ev->cseq);
    }
    if(hdl) ((AmSipDialogEventHandler*)hdl)->onNoAck(to_ev->cseq);
    break;

  case AmSipTimeoutEvent::noPRACK:
    ILOG_DLG(L_DBG, "Timeout: missing PRACK\n");
    rel100.onTimeout(to_ev->req, to_ev->rpl);
    break;

  case AmSipTimeoutEvent::_noEv:
  default:
    break;
  };
  
  to_ev->processed = true;
}

bool AmSipDialog::getUACInvTransPending() {
  for (TransMap::iterator it=uac_trans.begin();
       it != uac_trans.end(); it++) {
    if (it->second.method == SIP_METH_INVITE)
      return true;
  }
  return false;
}

AmSipRequest* AmSipDialog::getUASPendingInv()
{
  for (TransMap::iterator it=uas_trans.begin();
       it != uas_trans.end(); it++) {
    if (it->second.method == SIP_METH_INVITE)
      return &(it->second);
  }
  return NULL;
}

int AmSipDialog::bye(const string& hdrs, int flags)
{
    switch (status) {

    case Disconnecting:
    case Connected:
    {
      /* collect INVITE UAC transactions */
      vector<unsigned int> ack_trans;
      for (TransMap::iterator it = uac_trans.begin();
          it != uac_trans.end();
          it++)
      {
        if (it->second.method == SIP_METH_INVITE) {
        ack_trans.push_back(it->second.cseq);
        }
      }

      /* finish any UAC transaction before sending BYE */
      for (vector<unsigned int>::iterator it = ack_trans.begin();
          it != ack_trans.end();
          it++)
      {
        send_200_ack(*it);
      }

      /* handle case with force early announce (leg is in pseudo-connected state),
       * but indeed on the SIP level, leg has never seen 2XX response */
      if (getFaked183As200()) {
        setFaked183As200(false); /* reset faked state */

        for (TransMap::reverse_iterator t = uac_trans.rbegin();
            t != uac_trans.rend();
            t++)
        {
          if (t->second.method == SIP_METH_INVITE || t->second.method == SIP_METH_BYE) {
            setStatus(Cancelling);
            return SipCtrlInterface::cancel(&t->second.tt, local_tag,
                                            t->first, t->second.hdrs + hdrs);
          }
        }
        /* if faked state doesn't find required transaction,
         * it's going to a usual way of handling with BYE */
      }

      /* usual handling */
      if (status != Disconnecting) {
        setStatus(Disconnected);
        return sendRequest(SIP_METH_BYE, NULL, hdrs, flags);
      } else {
        return 0;
      }
    }

    case Trying:
    case Proceeding:
    case Early:
      if (getUACInvTransPending()) {
        return cancel();
      } else {
        for (TransMap::iterator it = uas_trans.begin();
             it != uas_trans.end();
             it++)
        {
          if (it->second.method == SIP_METH_INVITE) {
            /* let quit this call by sending final reply */
            return reply(it->second, 487, "Request terminated");
          }
        }

        /* missing AmSipRequest to be able
         * to send the reply on behalf of the app. */
        ILOG_DLG(L_ERR, "ignoring bye() in %s state: "
              "no UAC transaction to cancel or UAS transaction to reply.\n",
              getStatusStr());
        setStatus(Disconnected);
      }
      return 0;

    case Cancelling:
      for (TransMap::iterator it = uas_trans.begin();
          it != uas_trans.end();
          it++)
      {
        if (it->second.method == SIP_METH_INVITE){
          /* let's quit this call by sending final reply */
          return reply(it->second, 487,"Request terminated");
        }
      }

      /* missing AmSipRequest to be able
       * to send the reply on behalf of the app. */
      ILOG_DLG(L_DBG, "ignoring bye() in %s state: no UAS transaction to reply", getStatusStr());
      setStatus(Disconnected);
      return 0;

    default:
      ILOG_DLG(L_DBG, "bye(): we are not connected "
          "(status=%s). do nothing!\n", getStatusStr());
      return 0;
    }
}

int AmSipDialog::reinvite(const string& hdrs,  
			  const AmMimeBody* body,
			  int flags)
{
  if(getStatus() == Connected) {
    return sendRequest(SIP_METH_INVITE, body, hdrs, flags);
  }
  else {
    ILOG_DLG(L_DBG, "reinvite(): we are not connected "
	"(status=%s). do nothing!\n",getStatusStr());
  }

  return -1;
}

int AmSipDialog::invite(const string& hdrs,  
			const AmMimeBody* body)
{
  if(getStatus() == Disconnected) {
    int res = sendRequest(SIP_METH_INVITE, body, hdrs);
    ILOG_DLG(L_DBG, "TODO: is status already 'trying'? status=%s\n",getStatusStr());
    //status = Trying;
    return res;
  }
  else {
    ILOG_DLG(L_DBG, "invite(): we are already connected "
	"(status=%s). do nothing!\n",getStatusStr());
  }

  return -1;
}

int AmSipDialog::update(const AmMimeBody* body, 
                        const string &hdrs)
{
  switch(getStatus()){
  case Connected://if Connected, we should send a re-INVITE instead...
    ILOG_DLG(L_DBG, "re-INVITE should be used instead (see RFC3311, section 5.1)\n");
  case Trying:
  case Proceeding:
  case Early:
    return sendRequest(SIP_METH_UPDATE, body, hdrs);

  default:
  case Cancelling:
  case Disconnected:
  case Disconnecting:
    ILOG_DLG(L_DBG, "update(): dialog not connected "
	"(status=%s). do nothing!\n",getStatusStr());
  }

  return -1;
}

int AmSipDialog::refer(const string& refer_to,
		       int expires,
		       const string& referred_by)
{
  if(getStatus() == Connected) {
    string hdrs = SIP_HDR_COLSP(SIP_HDR_REFER_TO) + refer_to + CRLF;
    if (expires>=0) 
      hdrs+= SIP_HDR_COLSP(SIP_HDR_EXPIRES) + int2str(expires) + CRLF;
    if (!referred_by.empty())
      hdrs+= SIP_HDR_COLSP(SIP_HDR_REFERRED_BY) + referred_by + CRLF;

    return sendRequest("REFER", NULL, hdrs);
  }
  else {
    ILOG_DLG(L_DBG, "refer(): we are not Connected."
	"(status=%s). do nothing!\n",getStatusStr());

    return 0;
  }	
}

int AmSipDialog::info(const string& hdrs, const AmMimeBody* body)
{
  if(getStatus() == Connected) {
    return sendRequest("INFO", body, hdrs);
  } else {
    ILOG_DLG(L_DBG, "info(): we are not Connected."
	"(status=%s). do nothing!\n", getStatusStr());
    return 0;
  }
}    

// proprietary
int AmSipDialog::transfer(const string& target)
{
  if(getStatus() == Connected){

    setStatus(Disconnecting);
		
    string      hdrs = "";
    AmSipDialog tmp_d(*this);
		
    tmp_d.route = "";
    // TODO!!!
    //tmp_d.contact_uri = SIP_HDR_COLSP(SIP_HDR_CONTACT) 
    //  "<" + tmp_d.remote_uri + ">" CRLF;
    tmp_d.remote_uri = target;
		
    string r_set;
    if(!route.empty()){
			
      hdrs = PARAM_HDR ": " "Transfer-RR=\"" + route + "\""+CRLF;
    }
				
    int ret = tmp_d.sendRequest("REFER",NULL,hdrs);
    if(!ret){
      uac_trans.insert(tmp_d.uac_trans.begin(),
		       tmp_d.uac_trans.end());
      cseq = tmp_d.cseq;
    }
		
    return ret;
  }
	
  ILOG_DLG(L_DBG, "transfer(): we are not connected "
      "(status=%i). do nothing!\n",status);
    
  return 0;
}

int AmSipDialog::prack(const AmSipReply &reply1xx,
                       const AmMimeBody* body, 
                       const string &hdrs)
{
  switch(getStatus()) {
  case Trying:
  case Proceeding:
  case Cancelling:
  case Early:
  case Connected:
    break;
  case Disconnected:
  case Disconnecting:
      ILOG_DLG(L_ERR, "can not send PRACK while dialog is in state '%d'.\n", status);
      return -1;
  default:
      ILOG_DLG(L_ERR, "BUG: unexpected dialog state '%d'.\n", status);
      return -1;
  }
  string h = hdrs +
          SIP_HDR_COLSP(SIP_HDR_RACK) + 
          int2str(reply1xx.rseq) + " " + 
          int2str(reply1xx.cseq) + " " + 
          reply1xx.cseq_method + CRLF;
  return sendRequest(SIP_METH_PRACK, body, h);
}

int AmSipDialog::cancel()
{
    for(TransMap::reverse_iterator t = uac_trans.rbegin();
	t != uac_trans.rend(); t++) {
	
	if(t->second.method == SIP_METH_INVITE){

	  if(getStatus() != Cancelling){
	    setStatus(Cancelling);
	    return SipCtrlInterface::cancel(&t->second.tt, local_tag,
					    t->first, t->second.hdrs);
	  }
	  else {
	    ILOG_DLG(L_ERR, "INVITE transaction has already been cancelled\n");
	    return -1;
	  }
	}
    }
    
    ILOG_DLG(L_ERR, "could not find INVITE transaction to cancel\n");
    return -1;
}

int AmSipDialog::drop()
{	
  setStatus(Disconnected);
  return 1;
}

int AmSipDialog::send_200_ack(unsigned int inv_cseq,
			      const AmMimeBody* body,
			      const string& hdrs,
			      int flags)
{
  // TODO: implement missing pieces from RFC 3261:
  // "The ACK MUST contain the same credentials as the INVITE.  If
  // the 2xx contains an offer (based on the rules above), the ACK MUST
  // carry an answer in its body.  If the offer in the 2xx response is not
  // acceptable, the UAC core MUST generate a valid answer in the ACK and
  // then send a BYE immediately."

  TransMap::iterator inv_it = uac_trans.find(inv_cseq);
  if (inv_it == uac_trans.end()) {
    ILOG_DLG(L_ERR, "trying to ACK a non-existing transaction (cseq=%i;local_tag=%s)\n",
	  inv_cseq,local_tag.c_str());
    return -1;
  }

  AmSipRequest req;

  req.method = SIP_METH_ACK;
  req.r_uri = remote_uri;

  req.from = SIP_HDR_COLSP(SIP_HDR_FROM) + local_party;
  if(!ext_local_tag.empty())
    req.from += ";tag=" + ext_local_tag;
  else if(!local_tag.empty())
    req.from += ";tag=" + local_tag;
    
  req.to = SIP_HDR_COLSP(SIP_HDR_TO) + remote_party;
  if(!remote_tag.empty()) 
    req.to += ";tag=" + remote_tag;
    
  req.cseq = inv_cseq;// should be the same as the INVITE
  req.callid = callid;
  req.contact = getContactHdr();
    
  req.route = getRoute();

  req.max_forwards = inv_it->second.max_forwards;

  if(body != NULL)
    req.body = *body;

  if(onTxRequest(req,flags) < 0)
    return -1;

  if (!(flags&SIP_FLAGS_VERBATIM)) {
    // add Signature
    if (AmConfig::Signature.length())
      req.hdrs += SIP_HDR_COLSP(SIP_HDR_USER_AGENT) + AmConfig::Signature + CRLF;
  }

  int res = SipCtrlInterface::send(req, local_tag,
				   remote_tag.empty() || !next_hop_1st_req ? 
				   next_hop : "",
				   outbound_interface, 0, logger);
  if (res)
    return res;

  onRequestTxed(req);
  return 0;
}


/** EMACS **
 * Local variables:
 * mode: c++
 * c-basic-offset: 2
 * End:
 */
