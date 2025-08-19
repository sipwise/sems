#include "AmBasicSipDialog.h"

#include "AmConfig.h"
#include "AmSipHeaders.h"
#include "SipCtrlInterface.h"
#include "AmSession.h"

#include "sip/parse_route.h"
#include "sip/parse_uri.h"
#include "sip/parse_next_hop.h"
#include "sip/msg_logger.h"
#include "sip/sip_parser.h"

#define GET_CALL_ID() (getCallid().c_str())

const char* AmBasicSipDialog::status2str[AmBasicSipDialog::__max_Status] = {
  "Disconnected",
  "Trying",
  "Proceeding",
  "Cancelling",
  "Early",
  "Connected",
  "Disconnecting"
};

AmBasicSipDialog::AmBasicSipDialog(AmBasicSipEventHandler* h)
  : status(Disconnected),
    cseq(10),r_cseq_i(false),hdl(h),
    logger(0),
    outbound_proxy(AmConfig::OutboundProxy),
    force_outbound_proxy(AmConfig::ForceOutboundProxy),
    next_hop(AmConfig::NextHop),
    next_hop_1st_req(AmConfig::NextHop1stReq),
    patch_ruri_next_hop(false),
    next_hop_fixed(false),
    outbound_interface(-1),
    nat_handling(AmConfig::SipNATHandling),
    usages(0)
{
  //assert(h);
}

AmBasicSipDialog::~AmBasicSipDialog()
{
  termUasTrans();
  termUacTrans();
  dump();
}

AmSipRequest* AmBasicSipDialog::getUACTrans(unsigned int t_cseq)
{
  TransMap::iterator it = uac_trans.find(t_cseq);
  if(it == uac_trans.end())
    return NULL;
  
  return &(it->second);
}

AmSipRequest* AmBasicSipDialog::getUASTrans(unsigned int t_cseq)
{
  TransMap::iterator it = uas_trans.find(t_cseq);
  if(it == uas_trans.end())
    return NULL;
  
  return &(it->second);
}

string AmBasicSipDialog::getUACTransMethod(unsigned int t_cseq)
{
  AmSipRequest* req = getUACTrans(t_cseq);
  if(req != NULL)
    return req->method;

  return string();
}

bool AmBasicSipDialog::getUACTransPending()
{
  return !uac_trans.empty();
}

void AmBasicSipDialog::setStatus(Status new_status) 
{
  ILOG_DLG(L_DBG, "setting SIP dialog status: %s->%s. Local_tag: <%s>\n",
      getStatusStr(), getStatusStr(new_status), getLocalTag().c_str());
  status = new_status;
}

const char* AmBasicSipDialog::getStatusStr(AmBasicSipDialog::Status st)
{
  if((st < 0) || (st >= __max_Status))
    return "Invalid";
  else
    return status2str[st];
}

const char* AmBasicSipDialog::getStatusStr()
{
  return getStatusStr(status);
}

string AmBasicSipDialog::getContactHdr() {
  return
    SIP_HDR_COLSP(SIP_HDR_CONTACT) "<"+ getContactUri() += ">" CRLF;
}


string AmBasicSipDialog::getContactUri()
{
  string contact_uri = "sip:";

  if(!ext_local_tag.empty()) {
    contact_uri += local_tag + "@";
  }

  int oif = getOutboundIf();
  assert(oif >= 0);
  assert(oif < (int)AmConfig::SIP_Ifs.size());
  
  contact_uri += AmConfig::SIP_Ifs[oif].getIP();
  contact_uri += ":" + int2str(AmConfig::SIP_Ifs[oif].LocalPort);

  if(!contact_params.empty()) {
    contact_uri += ";" + contact_params;
  }

  return contact_uri;
}

string AmBasicSipDialog::getRoute() 
{
  string res;
  string route_first_element;

  if (!route.empty()) {
    route_first_element = route.substr(0, route.find(','));
  }

  // Do no add outbound_proxy if it's already the top most Route
  if(!outbound_proxy.empty() &&
    (force_outbound_proxy || remote_tag.empty()) &&
    route_first_element.find(outbound_proxy) == std::string::npos)
  {
    res += "<" + outbound_proxy + ";lr>";

    if(!route.empty()) {
      res += ",";
    }
  }

  res += route;

  if(!res.empty()) {
    res = SIP_HDR_COLSP(SIP_HDR_ROUTE) + res + CRLF;
  }

  return res;
}

void AmBasicSipDialog::setOutboundInterface(int interface_id) {
  ILOG_DLG(L_DBG, "setting outbound interface to %i\n",  interface_id);
  outbound_interface = interface_id;
}

/** 
 * Computes, set and return the outbound interface
 * based on remote_uri, next_hop_ip, outbound_proxy, route.
 */
int AmBasicSipDialog::getOutboundIf()
{
  if (outbound_interface >= 0)
    return outbound_interface;

  if(AmConfig::SIP_Ifs.size() == 1){
    return (outbound_interface = 0);
  }

  // Destination priority:
  // 1. next_hop
  // 2. outbound_proxy (if 1st req or force_outbound_proxy)
  // 3. first route
  // 4. remote URI
  
  string dest_uri;
  string dest_ip;
  string local_ip;
  multimap<string,unsigned short>::iterator if_it;

  list<sip_destination> ip_list;
  if(!next_hop.empty() && 
     !parse_next_hop(stl2cstr(next_hop),ip_list) &&
     !ip_list.empty()) {

    dest_ip = c2stlstr(ip_list.front().host);
  }
  else if(!outbound_proxy.empty() &&
	  (remote_tag.empty() || force_outbound_proxy)) {
    dest_uri = outbound_proxy;
  }
  else if(!route.empty()){
    // parse first route
    sip_header fr;
    fr.value = stl2cstr(route);
    sip_uri* route_uri = get_first_route_uri(&fr);
    if(!route_uri){
      ILOG_DLG(L_ERR, "Could not parse route (local_tag='%s';route='%s')",
	    local_tag.c_str(),route.c_str());
      goto error;
    }

    dest_ip = c2stlstr(route_uri->host);
  }
  else {
    dest_uri = remote_uri;
  }

  if(dest_uri.empty() && dest_ip.empty()) {
    ILOG_DLG(L_ERR, "No destination found (local_tag='%s')",local_tag.c_str());
    goto error;
  }
  
  if(!dest_uri.empty()){
    sip_uri d_uri;
    if(parse_uri(&d_uri,dest_uri.c_str(),dest_uri.length()) < 0){
      ILOG_DLG(L_ERR, "Could not parse destination URI (local_tag='%s';dest_uri='%s')",
	    local_tag.c_str(),dest_uri.c_str());
      goto error;
    }

    dest_ip = c2stlstr(d_uri.host);
  }

  if(get_local_addr_for_dest(dest_ip,local_ip) < 0){
    ILOG_DLG(L_ERR, "No local address for dest '%s' (local_tag='%s')",dest_ip.c_str(),local_tag.c_str());
    goto error;
  }

  if_it = AmConfig::LocalSIPIP2If.find(local_ip);
  if(if_it == AmConfig::LocalSIPIP2If.end()){
    ILOG_DLG(L_ERR, "Could not find a local interface for resolved local IP (local_tag='%s';local_ip='%s')",
	  local_tag.c_str(), local_ip.c_str());
    goto error;
  }

  setOutboundInterface(if_it->second);
  return if_it->second;

 error:
  ILOG_DLG(L_WARN, "Error while computing outbound interface: default interface will be used instead.");
  setOutboundInterface(0);
  return 0;
}

void AmBasicSipDialog::resetOutboundIf()
{
  setOutboundInterface(-1);
}

/**
 * Update dialog status from UAC Request that we send.
 */
void AmBasicSipDialog::initFromLocalRequest(const AmSipRequest& req)
{
  setRemoteUri(req.r_uri);

  user         = req.user;
  domain       = req.domain;

  setCallid(      req.callid   );
  setLocalTag(    req.from_tag );
  setLocalUri(    req.from_uri );
  setRemoteParty( req.to       );
  setLocalParty(  req.from     );
}

bool AmBasicSipDialog::onRxReqSanity(const AmSipRequest& req)
{
  // Sanity checks
  if(!remote_tag.empty() && !req.from_tag.empty() &&
     (req.from_tag != remote_tag)){
    ILOG_DLG(L_DBG, "remote_tag = '%s'; req.from_tag = '%s'\n",
	remote_tag.c_str(), req.from_tag.c_str());
    reply_error(req, 481, SIP_REPLY_NOT_EXIST);
    return false;
  }

  if (r_cseq_i && req.cseq <= r_cseq){

    if (req.method == SIP_METH_NOTIFY) {
      if (!AmConfig::IgnoreNotifyLowerCSeq) {
	// clever trick to not break subscription dialog usage
	// for implementations which follow 3265 instead of 5057
	string hdrs = SIP_HDR_COLSP(SIP_HDR_RETRY_AFTER)  "0"  CRLF;

	ILOG_DLG(L_INFO, "remote cseq lower than previous ones - refusing request\n");
	// see 12.2.2
	reply_error(req, 500, SIP_REPLY_SERVER_INTERNAL_ERROR, hdrs);
	return false;
      }
    }
    else {
      ILOG_DLG(L_INFO, "remote cseq lower than previous ones - refusing request\n");
      // see 12.2.2
      reply_error(req, 500, SIP_REPLY_SERVER_INTERNAL_ERROR);
      return false;
    }
  }

  r_cseq = req.cseq;
  r_cseq_i = true;

  return true;
}

void AmBasicSipDialog::onRxRequest(const AmSipRequest& req)
{
  ILOG_DLG(L_DBG, "AmBasicSipDialog::onRxRequest(req = %s)\n", req.method.c_str());

  if (logger && (req.method != SIP_METH_ACK)) {
    /* log only non-initial received requests, the initial one is already logged
     * or will be logged at application level (problem with SBCSimpleRelay) */
    if (!callid.empty()) req.log(logger);
  }

  if (!onRxReqSanity(req))
    return;

  uas_trans[req.cseq] = req;

  /* target refresh requests */
  if (req.from_uri.length() &&
      (remote_uri.empty() ||
        (req.method == SIP_METH_INVITE ||
         req.method == SIP_METH_UPDATE ||
         req.method == SIP_METH_SUBSCRIBE ||
         req.method == SIP_METH_NOTIFY)
      )
     ) {

    /* refresh the target */
    if (remote_uri != req.from_uri) {
      setRemoteUri(req.from_uri);

      if (nat_handling && req.first_hop) {
        string nh = req.remote_ip + ":"
                    + int2str(req.remote_port)
                    + "/" + req.trsp;
        setNextHop(nh);
        setNextHop1stReq(false);
      }
    }

    string ua = getHeader(req.hdrs, SIP_HDR_USER_AGENT);
    setRemoteUA(ua);
  }

  /* Dlg not yet initialized? */
  if (callid.empty()) {

    user         = req.user;
    domain       = req.domain;

    setCallid(      req.callid   );
    setRemoteTag(   req.from_tag );
    setLocalUri(    req.r_uri    );
    setRemoteParty( req.from     );
    setLocalParty(  req.to       );
    setRouteSet(    req.route    );
    set1stBranch(   req.via_branch );
    setOutboundInterface( req.local_if );
  }

  if (onRxReqStatus(req) && hdl)
    hdl->onSipRequest(req);
}

bool AmBasicSipDialog::onRxReplyStatus(const AmSipReply& reply)
{
  /**
   * Error code list from RFC 5057:
   * those error codes terminate the dialog
   *
   * Note: 408, 480 should only terminate
   *       the usage according to RFC 5057.
   */
  switch(reply.code){
  case 404:
  case 408:
  case 410:
  case 416:
  case 480:
  case 482:
  case 483:
  case 484:
  case 485:
  case 502:
  case 604:
    if(hdl) hdl->onRemoteDisappeared(reply);
    break;
  }
  
  return true;
}

void AmBasicSipDialog::termUasTrans()
{
  while(!uas_trans.empty()) {

    TransMap::iterator it = uas_trans.begin();
    int req_cseq = it->first;
    const AmSipRequest& req = it->second;
    ILOG_DLG(L_DBG, "terminating UAS transaction (%u %s)",req.cseq,req.cseq_method.c_str());

    reply(req,481,SIP_REPLY_NOT_EXIST);

    it = uas_trans.find(req_cseq);
    if(it != uas_trans.end())
      uas_trans.erase(it);
  }
}

void AmBasicSipDialog::termUacTrans()
{
  while(!uac_trans.empty()) {
    TransMap::iterator it = uac_trans.begin();
    trans_ticket& tt = it->second.tt;

    tt.lock_bucket();
    tt.remove_trans();
    tt.unlock_bucket();

    uac_trans.erase(it);
  }
}

void AmBasicSipDialog::dropTransactions() {
  termUacTrans();
  uas_trans.clear();
}

bool AmBasicSipDialog::onRxReplySanity(const AmSipReply& reply)
{
  if(ext_local_tag.empty()) {
    if(reply.from_tag != local_tag) {
      ILOG_DLG(L_ERR, "received reply with wrong From-tag ('%s' vs. '%s')",
	    reply.from_tag.c_str(), local_tag.c_str());
      throw string("reply has wrong from-tag");
      //return;
    }
  }
  else if(reply.from_tag != ext_local_tag) {
    ILOG_DLG(L_ERR, "received reply with wrong From-tag ('%s' vs. '%s')",
	  reply.from_tag.c_str(), ext_local_tag.c_str());
    throw string("reply has wrong from-tag");
    //return;
  }

  return true;
}

void AmBasicSipDialog::onRxReply(const AmSipReply& reply)
{
  if(!onRxReplySanity(reply))
    return;

  TransMap::iterator t_it = uac_trans.find(reply.cseq);
  if(t_it == uac_trans.end()){
    ILOG_DLG(L_ERR, "could not find any transaction matching reply: %s\n", 
        ((AmSipReply)reply).print().c_str());
    return;
  }

  ILOG_DLG(L_DBG, "onRxReply(rep = %u %s): transaction found!\n",
      reply.code, reply.reason.c_str());

  updateDialogTarget(reply);
  
  Status saved_status = status;
  AmSipRequest orig_req(t_it->second);

  if(onRxReplyStatus(reply) && hdl) {
    hdl->onSipReply(orig_req,reply,saved_status);
  }

  if((reply.code >= 200) && // final reply
     // but not for 2xx INV reply (wait for 200 ACK)
     ((reply.cseq_method != SIP_METH_INVITE) ||
      (reply.code >= 300))) {
       
    uac_trans.erase(reply.cseq);
    if (hdl) hdl->onTransFinished();
  }
}

void AmBasicSipDialog::updateDialogTarget(const AmSipReply& reply)
{
  if( (reply.code > 100) && (reply.code < 300) &&
      !reply.to_uri.empty() &&
      !reply.to_tag.empty() &&
      (remote_uri.empty() ||
       (reply.cseq_method.length()==6 &&
	((reply.cseq_method == SIP_METH_INVITE) ||
	 (reply.cseq_method == SIP_METH_UPDATE) ||
	 (reply.cseq_method == SIP_METH_NOTIFY))) ||
       (reply.cseq_method == SIP_METH_SUBSCRIBE)) ) {
    
    setRemoteUri(reply.to_uri);
    if(!getNextHop().empty()) {
      string nh = reply.remote_ip 
	+ ":" + int2str(reply.remote_port)
	+ "/" + reply.trsp;
      setNextHop(nh);
    }

    string ua = getHeader(reply.hdrs,"Server");
    setRemoteUA(ua);
  }
}

void AmBasicSipDialog::setRemoteTag(const string& new_rt)
{
  if(new_rt != remote_tag){
    remote_tag = new_rt;
  }
}

int AmBasicSipDialog::onTxRequest(AmSipRequest& req, int& flags)
{
  if(hdl) hdl->onSendRequest(req,flags);

  return 0;
}

int AmBasicSipDialog::onTxReply(const AmSipRequest& req, 
				AmSipReply& reply, int& flags)
{
  if(hdl) hdl->onSendReply(req,reply,flags);

  return 0;
}

void AmBasicSipDialog::onReplyTxed(const AmSipRequest& req, 
				   const AmSipReply& reply)
{
  if(hdl) hdl->onReplySent(req, reply);

  /**
   * Error code list from RFC 5057:
   * those error codes terminate the dialog
   *
   * Note: 408, 480 should only terminate
   *       the usage according to RFC 5057.
   */
  switch(reply.code){
  case 404:
  case 408:
  case 410:
  case 416:
  case 480:
  case 482:
  case 483:
  case 484:
  case 485:
  case 502:
  case 604:
    if(hdl) hdl->onLocalTerminate(reply);
    break;
  }

  if ((reply.code >= 200) && 
      (reply.cseq_method != SIP_METH_CANCEL)) {
    
    uas_trans.erase(reply.cseq);
    if (hdl) hdl->onTransFinished();
  }
}

void AmBasicSipDialog::onRequestTxed(const AmSipRequest& req)
{
  if(hdl) hdl->onRequestSent(req);

  if(req.method != SIP_METH_ACK) {
    uac_trans[req.cseq] = req;
    cseq++;
  }
  else {
    uac_trans.erase(req.cseq);
    if (hdl) hdl->onTransFinished();
  }
}

int AmBasicSipDialog::reply(const AmSipRequest& req,
			    unsigned int  code,
			    const string& reason,
			    const AmMimeBody* body,
			    const string& hdrs,
			    int flags)
{
  TransMap::const_iterator t_it = uas_trans.find(req.cseq);

  if (t_it == uas_trans.end()) {
    ILOG_DLG(L_ERR, "could not find any transaction matching request cseq\n");
    ILOG_DLG(L_ERR, "request cseq=%i; reply code=%i; callid=%s; local_tag=%s; "
	  "remote_tag=%s\n",
	  req.cseq,code,callid.c_str(),
	  local_tag.c_str(),remote_tag.c_str());
    log_stacktrace(L_ERR);
    return -1;
  }

  ILOG_DLG(L_DBG, "reply: transaction found!\n");

  AmSipReply reply;

  reply.code = code;
  reply.reason = reason;
  reply.tt = req.tt;
  if((code > 100) && !(flags & SIP_FLAGS_NOTAG)) {
     reply.to_tag = ext_local_tag.empty() ? local_tag : ext_local_tag;
  }
  reply.hdrs = hdrs;
  reply.cseq = req.cseq;
  reply.cseq_method = req.method;

   /* Add Allow header in 200OK with default Allow, if it is empty */
  if (reply.code == 200 && getHeader(reply.hdrs, SIP_HDR_ALLOW).empty()) {
    /* Add default Allow list, supporting all major methods */
    reply.hdrs += SIP_HDR_ALLOW_FULL CRLF;
  }

  if (body != NULL)
    reply.body = *body;

  if (onTxReply(req,reply,flags)) {
    ILOG_DLG(L_DBG, "onTxReply failed\n");
    return -1;
  }

  if (!(flags & SIP_FLAGS_VERBATIM)) {
    /* add Signature */
    if (AmConfig::Signature.length())
      reply.hdrs += SIP_HDR_COLSP(SIP_HDR_SERVER) + AmConfig::Signature + CRLF;
  }

  if ((code > 100 && code < 300) && !(flags & SIP_FLAGS_NOCONTACT)) {
    /* if 300<=code<400, explicit contact setting should be done */
    reply.contact = getContactHdr();
  }

  int ret = SipCtrlInterface::send(reply,local_tag,logger);
  if (ret) {
    ILOG_DLG(L_ERR, "Could not send reply: code=%i; reason='%s'; method=%s; call-id=%s; cseq=%i\n",
          reply.code,reply.reason.c_str(),
          reply.cseq_method.c_str(),
          callid.c_str(),
          reply.cseq);

    return ret;
  } else {
    onReplyTxed(req,reply);
  }

  return ret;
}


/* static */
int AmBasicSipDialog::reply_error(const AmSipRequest& req, unsigned int code,
				  const string& reason, const string& hdrs,
				  const shared_ptr<msg_logger>& logger)
{
  AmSipReply reply;

  reply.code = code;
  reply.reason = reason;
  reply.tt = req.tt;
  reply.hdrs = hdrs;
  reply.to_tag = AmSession::getNewId();

  if (AmConfig::Signature.length())
    reply.hdrs += SIP_HDR_COLSP(SIP_HDR_SERVER) + AmConfig::Signature + CRLF;

  // add transcoder statistics into reply headers
  //addTranscoderStats(reply.hdrs);

  int ret = SipCtrlInterface::send(reply,string(""),logger);
  if(ret){
    ERROR("Could not send reply: code=%i; reason='%s';"
	  " method=%s; call-id=%s; cseq=%i\n",
	  reply.code,reply.reason.c_str(),
	  req.method.c_str(),req.callid.c_str(),req.cseq);
  }

  return ret;
}

int AmBasicSipDialog::sendRequest(const string& method, 
				  const AmMimeBody* body,
				  const string& hdrs,
				  int flags)
{
  AmSipRequest req;

  req.method = method;
  req.r_uri = remote_uri;

  ILOG_DLG(L_DBG, "sendRequest: remote_uri='%s'; callid='%s'; cseq='%i'; from_tag='%s'; to_tag='%s';\n",
      remote_uri.c_str(),
      callid.c_str(),
      cseq,
      (!ext_local_tag.empty() ? ext_local_tag.c_str() : local_tag.c_str()),
      (!remote_tag.empty() ? remote_tag.c_str() : ""));

  req.from = SIP_HDR_COLSP(SIP_HDR_FROM) + local_party;
  if (!ext_local_tag.empty()) {
    req.from += ";tag=" + ext_local_tag;
  } else if(!local_tag.empty()) {
    req.from += ";tag=" + local_tag;
  }

  req.to = SIP_HDR_COLSP(SIP_HDR_TO) + remote_party;
  if (!remote_tag.empty())
    req.to += ";tag=" + remote_tag;

  req.cseq = cseq;
  req.callid = callid;
  req.hdrs = hdrs;
  req.route = getRoute();

  if (body != NULL) {
    req.body = *body;
  }

  if (onTxRequest(req,flags) < 0) {
    return -1;
  }

  if (!(flags & SIP_FLAGS_NOCONTACT)) {
    req.contact = getContactHdr();
  }

  if (!(flags & SIP_FLAGS_VERBATIM)) {
    /* add Signature */
    if (AmConfig::Signature.length())
      req.hdrs += SIP_HDR_COLSP(SIP_HDR_USER_AGENT) + AmConfig::Signature + CRLF;
  }

  int send_flags = 0;
  if (patch_ruri_next_hop && remote_tag.empty()) {
    send_flags |= TR_FLAG_NEXT_HOP_RURI;
  }

  if ((flags & SIP_FLAGS_NOBL) || !remote_tag.empty()) {
    send_flags |= TR_FLAG_DISABLE_BL;
  }

  int res = SipCtrlInterface::send(req, local_tag,
                                    remote_tag.empty() || !next_hop_1st_req ?
                                    next_hop : "",
                                    outbound_interface,
                                    send_flags,logger);

  if (res) {
    ILOG_DLG(L_ERR, "Could not send request: method=%s; call-id=%s; cseq=%i\n",
    req.method.c_str(),req.callid.c_str(),req.cseq);
    return res;
  }

  onRequestTxed(req);
  return 0;
}

void AmBasicSipDialog::dump()
{
  ILOG_DLG(L_DBG, "callid = %s\n",callid.c_str());
  ILOG_DLG(L_DBG, "local_tag = %s\n",local_tag.c_str());
  ILOG_DLG(L_DBG, "uac_trans.size() = %zu\n",uac_trans.size());
  if(uac_trans.size()){
    for(TransMap::iterator it = uac_trans.begin();
	it != uac_trans.end(); it++){
	    
      ILOG_DLG(L_DBG, "    cseq = %i; method = %s\n",it->first,it->second.method.c_str());
    }
  }
  ILOG_DLG(L_DBG, "uas_trans.size() = %zu\n",uas_trans.size());
  if(uas_trans.size()){
    for(TransMap::iterator it = uas_trans.begin();
	it != uas_trans.end(); it++){
 
      ILOG_DLG(L_DBG, "    cseq = %i; method = %s\n",it->first,it->second.method.c_str());
    }
  }
}

void AmBasicSipDialog::setMsgLogger(const shared_ptr<msg_logger>& logger)
{
  this->logger = logger;
}
