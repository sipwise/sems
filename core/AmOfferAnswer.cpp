/*
 * Copyright (C) 2010-2011 Raphael Coeffic
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
/** @file AmOfferAnswer.cpp */

#include "AmOfferAnswer.h"
#include "AmSipDialog.h"
#include "AmSipHeaders.h"
#include "log.h"

#include <assert.h>

const char* __dlg_oa_status2str[AmOfferAnswer::__max_OA]  = {
    "None",
    "OfferRecved",
    "OfferSent",
    "Completed"
};

static const char* getOAStateStr(AmOfferAnswer::OAState st) {
  if((st < 0) || (st >= AmOfferAnswer::__max_OA))
    return "Invalid";
  else
    return __dlg_oa_status2str[st];
}


AmOfferAnswer::AmOfferAnswer(AmSipDialog* dlg)
  : state(OA_None), 
    cseq(0),
    sdp_remote(),
    sdp_local(),
    dlg(dlg),
    force_sdp(true)
{
  
}

AmOfferAnswer::OAState AmOfferAnswer::getState()
{
  return state;
}

void AmOfferAnswer::setState(AmOfferAnswer::OAState n_st)
{
  DBG("setting SIP dialog O/A status: %s->%s\n",
      getOAStateStr(state), getOAStateStr(n_st));
  state = n_st;
}

const AmSdp& AmOfferAnswer::getLocalSdp()
{
  return sdp_local;
}

const AmSdp& AmOfferAnswer::getRemoteSdp()
{
  return sdp_remote;
}

/** State maintenance */
void AmOfferAnswer::saveState()
{
  saved_state = state;
}

int AmOfferAnswer::checkStateChange()
{
  int ret = 0;

  if((saved_state != state) &&
     (state == OA_Completed)) {

    ret = dlg->onSdpCompleted();
  }

  return ret;
}

void AmOfferAnswer::clear()
{
  setState(OA_None);
  cseq  = 0;
  sdp_remote.clear();
  sdp_local.clear();
}

void AmOfferAnswer::clearTransitionalState()
{
  if(state != OA_Completed){
    clear();
  }
}

int AmOfferAnswer::onRequestIn(const AmSipRequest& req)
{
  saveState();

  const char* err_txt  = NULL;
  int         err_code = 0;

  if((req.method == SIP_METH_INVITE || 
      req.method == SIP_METH_UPDATE || 
      req.method == SIP_METH_ACK ||
      req.method == SIP_METH_PRACK) &&
     !req.body.empty() )
  {
    const AmMimeBody* sdp_body = req.body.hasContentType(SIP_APPLICATION_SDP);
    const AmMimeBody * csta_body = req.body.hasContentType(SIP_APPLICATION_CSTA_XML);

    /* if application/sdp present */
    if(sdp_body) {
      err_code = onRxSdp(req.cseq,*sdp_body,&err_txt);
    /* if both application/sdp and application/csta+xml are not present */
    } else if (!csta_body) {
      err_code = 400;
      err_txt = "unsupported content type";
    }
  }

  if(checkStateChange()){
    err_code = 500;
    err_txt = "internal error";
  }

  if(err_code){
    if( req.method != SIP_METH_ACK ){ // INVITE || UPDATE || PRACK
      dlg->reply(req,err_code,err_txt);
    }
    else { // ACK
      // TODO: only if reply to initial INVITE (if re-INV, app should decide)
      DBG("error %i with SDP received in ACK request: sending BYE\n",err_code);
      dlg->bye();
    }
  }

  if((req.method == SIP_METH_ACK) &&
     (req.cseq == cseq)){
    // 200 ACK received:
    //  -> reset OA state
    DBG("200 ACK received: resetting OA state");
    clearTransitionalState();
  }

  return err_code ? -1 : 0;
}

int AmOfferAnswer::onReplyIn(const AmSipReply& reply)
{
  const char* err_txt  = NULL;
  int         err_code = 0;

  if((reply.cseq_method == SIP_METH_INVITE || 
      reply.cseq_method == SIP_METH_UPDATE || 
      reply.cseq_method == SIP_METH_PRACK) &&
     !reply.body.empty() )
  {
    const AmMimeBody* sdp_body = reply.body.hasContentType(SIP_APPLICATION_SDP);
    const AmMimeBody* csta_body = reply.body.hasContentType(SIP_APPLICATION_CSTA_XML);

    if (sdp_body || csta_body) {

      if (((state == OA_Completed) ||
          (state == OA_OfferRecved)) &&
          (reply.cseq == cseq))
      {
        DBG("ignoring subsequent SDP reply within the same transaction\n");
        DBG("this usually happens when 183 and 200 have SDP\n");

        /* Make sure that session is started when 200 OK is received */
        if (reply.code == 200) dlg->onSdpCompleted();

      } else {
        saveState();
        err_code = onRxSdp(reply.cseq,reply.body,&err_txt);
        checkStateChange();
      }
    }
  }

  if ((reply.code >= 300) &&
      (reply.cseq == cseq))
  {
    /* final error reply -> cleanup OA state */
    DBG("after %u reply to %s: resetting OA state\n", reply.code, reply.cseq_method.c_str());
    clearTransitionalState();
  }

  if (err_code) {
    /* TODO: only if initial INVITE (if re-INV, app should decide) */
    DBG("error %i (%s) with SDP received in %i reply: sending ACK+BYE\n", err_code, err_txt ? err_txt : "none", reply.code);
    dlg->bye();
  }

  return 0;
}

int AmOfferAnswer::onRxSdp(unsigned int m_cseq, const AmMimeBody& body, const char** err_txt)
{
  DBG("entering onRxSdp(), oa_state=%s\n", getOAStateStr(state));
  OAState old_oa_state = state;

  int err_code = 0;
  assert(err_txt);

  /* check if we have a body */
  const AmMimeBody *sdp_body = body.hasContentType(SIP_APPLICATION_SDP);
  const AmMimeBody *csta_body = body.hasContentType(SIP_APPLICATION_CSTA_XML);

  if (sdp_body == NULL) {
    err_code = 400;
    *err_txt = "sdp body part not found";
  }
 
  if (sdp_remote.parse((const char*)body.getPayload())) {
    err_code = 400;
    *err_txt = "session description parsing failed";
  }
  else if(sdp_remote.media.empty()){
    err_code = 400;
    *err_txt = "no media line found in SDP message";
  }

  if(err_code != 0) {
    sdp_remote.clear();
  }

  if(err_code == 0) {

    switch(state) {
    case OA_None:
    case OA_Completed:
      setState(OA_OfferRecved);
      cseq = m_cseq;
      break;
      
    case OA_OfferSent:
      setState(OA_Completed);
      break;

    case OA_OfferRecved:
      err_code = 400;// TODO: check correct error code
      *err_txt = "pending SDP offer";
      break;

    default:
      assert(0);
      break;
    }
  }

  DBG("oa_state: %s -> %s\n", getOAStateStr(old_oa_state), getOAStateStr(state));

  return err_code;
}

int AmOfferAnswer::onTxSdp(unsigned int m_cseq, const AmMimeBody& body, bool force_no_sdp_update)
{
  DBG("AmOfferAnswer::onTxSdp()\n");

  /* assume that the payload is ok if it is not empty.
   * (do not parse again self-generated SDP) */
  if (body.empty()) {
    DBG("Body is empty, cannot do anything here.\n");
    return -1;
  }

  switch (state) {

    case OA_None:
    case OA_Completed:
      if (!force_no_sdp_update)
        setState(OA_OfferSent);
      cseq = m_cseq;
      break;

    case OA_OfferRecved:
      if (!force_no_sdp_update)
        setState(OA_Completed);
      break;

    case OA_OfferSent:
      /* There is already a pending offer */
      DBG("There is already a pending offer, onTxSdp fails\n");
      return -1;

    default:
      break;
  }

  return 0;
}

int AmOfferAnswer::onRequestOut(AmSipRequest& req)
{
  AmMimeBody* sdp_body = req.body.hasContentType(SIP_APPLICATION_SDP);
  AmMimeBody* csta_body = req.body.hasContentType(SIP_APPLICATION_CSTA_XML);

  bool generate_sdp = sdp_body && !sdp_body->getLen();
  bool has_sdp = sdp_body && sdp_body->getLen();
  bool has_csta = csta_body && csta_body->getLen();

  if ((!sdp_body && !csta_body) &&
      ((req.method == SIP_METH_PRACK) || (req.method == SIP_METH_ACK)) &&
      (state == OA_OfferRecved))
  {
    generate_sdp = true;
    sdp_body = req.body.addPart(SIP_APPLICATION_SDP);
  }

  saveState();

  if (generate_sdp) {
    string sdp_buf;
    if (!getSdpBody(sdp_buf)){
      sdp_body->setPayload((const unsigned char*)sdp_buf.c_str(),
			   sdp_buf.length());
      has_sdp = true;
    }
    else if(force_sdp) {
      return -1;
    }
  } else if (sdp_body && has_sdp) {
    // update local SDP copy
    if (sdp_local.parse((const char*)sdp_body->getPayload())) {
      ERROR("parser failed on Tx SDP: '%s'\n", (const char*)sdp_body->getPayload());
    }
  }

  if ((has_sdp || (has_csta && req.method != SIP_METH_INFO)) &&
      (onTxSdp(req.cseq,req.body) != 0))
  {
    WARN("onTxSdp() failed\n");
    return -1;
  }

  return 0;
}

int AmOfferAnswer::onReplyOut(AmSipReply& reply)
{
  AmMimeBody* sdp_body = reply.body.hasContentType(SIP_APPLICATION_SDP);
  AmMimeBody* csta_body = reply.body.hasContentType(SIP_APPLICATION_CSTA_XML);

  bool force_no_sdp_update = false;   /* for sequential 183 responses with similar SDP body (version)  */
  bool generate_sdp = sdp_body && !sdp_body->getLen();
  bool has_sdp = sdp_body && sdp_body->getLen();
  bool has_csta = csta_body && csta_body->getLen();

  /* check whether 183 has same SDP version it has had before. Then it doesn't change leg's OA state */
  if (has_sdp && !generate_sdp &&
      reply.cseq_method == SIP_METH_INVITE && reply.code == 183)
  {
    AmSdp parser_sdp;
    if (parser_sdp.parse((const char*)sdp_body->getPayload())) {
      WARN("SDP parsing for the coming reply failed (cannot create AmSdp object).\n");
    } else {
      force_no_sdp_update = (sdp_local.origin.sessV == parser_sdp.origin.sessV);
      if (force_no_sdp_update)
        DBG("Forcing no OA state update (no SDP changes, same session version: was <%u>, now is <%u>).\n",
            sdp_local.origin.sessV, parser_sdp.origin.sessV);
    }
  }

  if (!has_sdp && !has_csta && !generate_sdp) {
    /* let's see whether we should force SDP or not. */

    if (reply.cseq_method == SIP_METH_INVITE) {

      /* TT#184101, a sequential 183, which has no SDP body, but the media session has
         already had the local SDP and saved that.
         Re-use it, and do not beget the 183 without SDP body */
      if (reply.code == 183 && !sdp_local.media.empty()) {

        DBG("The 183 with no SDP, but system already has local SDP for this session, re-using it..\n");

        string existing_sdp;
        sdp_local.print(existing_sdp);

        if(!sdp_body){
          if( (sdp_body = reply.body.addPart(SIP_APPLICATION_SDP)) == NULL ) {
            DBG("AmMimeBody::addPart() failed\n");
            return -1;
          }
        }

        sdp_body->setPayload((const unsigned char*)existing_sdp.c_str(), existing_sdp.length());

        has_sdp = true;
        DBG("Now has_sdp has been reset to true.\n");

      /* - 183 reply without SDP body, but is very first one (no SDP saved before)
         - 200-299 responses with no SDP */
      } else if ((reply.code == 183) || ((reply.code >= 200) && (reply.code < 300))) {

        /* either offer received or no offer at all: -> force SDP */
        generate_sdp = (state == OA_OfferRecved)
                        || (state == OA_None)
                        || (state == OA_Completed);
      }

    } else if (reply.cseq_method == SIP_METH_UPDATE) {

      if ((reply.code >= 200) && (reply.code < 300)) {
        /* offer received:
         *  -> force SDP */
        generate_sdp = (state == OA_OfferRecved);
        DBG("Now generate_sdp has been reset to <%c>.\n", generate_sdp ? 't' : 'f');
      }
    }
  }

  if (reply.cseq_method == SIP_METH_INVITE && reply.code < 300) {
    /* ignore SDP repeated in 1xx and 2xx replies (183, 180, ... 2xx) */
    if (has_sdp &&
        (state == OA_Completed || state == OA_OfferSent) &&
        reply.cseq == cseq)
    {
      has_sdp = false;
      DBG("Now has_sdp has been reset to false.\n");
    }
  }

  saveState();

  if (generate_sdp) {
    string sdp_buf;

    if (getSdpBody(sdp_buf)) {
      if (reply.code == 183 && reply.cseq_method == SIP_METH_INVITE) {
        /* just ignore if no SDP is generated (required for B2B) */
      } else if (reply.code == 200 && reply.cseq_method == SIP_METH_INVITE && state == OA_Completed) {
        /* just ignore if no SDP is generated (required for B2B) */
      } else if(force_sdp)
        return -1;

    } else {
      if (!sdp_body) {
        if ((sdp_body = reply.body.addPart(SIP_APPLICATION_SDP)) == NULL ) {
          DBG("AmMimeBody::addPart() failed\n");
          return -1;
        }
      }

      sdp_body->setPayload((const unsigned char*)sdp_buf.c_str(), sdp_buf.length());
      has_sdp = true;
    }

  } else if (sdp_body && has_sdp) {
    /* update local SDP copy */
    if (sdp_local.parse((const char*)sdp_body->getPayload())) {
      ERROR("parser failed on Tx SDP: '%s'\n", (const char*)sdp_body->getPayload());
    }
  }

  if ((has_sdp || (has_csta && reply.cseq_method != SIP_METH_INFO)) &&
      (onTxSdp(reply.cseq, reply.body, force_no_sdp_update) != 0))
  {
    WARN("onTxSdp() failed\n");
    return -1;
  }

  if ((reply.code >= 300) && (reply.cseq == cseq)) {
    /* final error reply -> cleanup OA state */
    DBG("after %u reply to %s: resetting OA state\n", reply.code, reply.cseq_method.c_str());
    clearTransitionalState();
  }

  return 0;
}

int AmOfferAnswer::onRequestSent(const AmSipRequest& req)
{
  int ret = checkStateChange();

  if((req.method == SIP_METH_ACK) &&
     (req.cseq == cseq)) {

    // transaction has been removed:
    //  -> cleanup OA state
    DBG("200 ACK sent: resetting OA state\n");
    clearTransitionalState();
  }

  return ret;
}

int AmOfferAnswer::onReplySent(const AmSipReply& reply)
{
  int ret = checkStateChange();

  // final answer to non-invite req that triggered O/A transaction
  if( (reply.code >= 200) &&
      (reply.cseq_method != SIP_METH_CANCEL) &&
      (reply.cseq == cseq) &&
      (reply.cseq_method != SIP_METH_INVITE) ) {

    // transaction has been removed:
    //  -> cleanup OA state
    DBG("transaction finished by final reply %u: resetting OA state\n", reply.cseq);
    clearTransitionalState();
  }
  
  return ret;
}

int AmOfferAnswer::getSdpBody(string& sdp_body)
{
    switch(state){
    case OA_None:
    case OA_Completed:
      if(dlg->getSdpOffer(sdp_local)){
	sdp_local.print(sdp_body);
      }
      else {
	DBG("No SDP Offer.\n");
	return -1;
      }
      break;
    case OA_OfferRecved:
      if(dlg->getSdpAnswer(sdp_remote,sdp_local)){
	sdp_local.print(sdp_body);
      }
      else {
	DBG("No SDP Answer.\n");
	return -1;
      }
      break;
      
    case OA_OfferSent:
      DBG("Still waiting for a reply\n");
      return -1;

    default: 
      break;
    }

    return 0;
}

void AmOfferAnswer::onNoAck(unsigned int ack_cseq)
{
  if(ack_cseq == cseq){
    DBG("ACK timeout: resetting OA state\n");
    clearTransitionalState();
  }
}
