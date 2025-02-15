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

#include "AmSession.h"
#include "AmSdp.h"
#include "AmConfig.h"
#include "AmUtils.h"
#include "AmPlugIn.h"
#include "AmApi.h"
#include "AmSessionContainer.h"
#include "AmSessionProcessor.h"
#include "AmMediaProcessor.h"
#include "AmDtmfDetector.h"
#include "AmPlayoutBuffer.h"
#include "AmAppTimer.h"

#ifdef WITH_ZRTP
#include "AmZRTP.h"
#endif

#include "log.h"

#include <algorithm>

#include <unistd.h>
#include <assert.h>
#include <sys/time.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>

volatile unsigned int AmSession::session_num = 0;
AmMutex AmSession::session_num_mut;
volatile unsigned int AmSession::max_session_num = 0;
volatile unsigned long long AmSession::avg_session_num = 0;

struct timeval get_now() {
  struct timeval res;
  gettimeofday(&res, NULL);
  return res;
}
struct timeval avg_last_timestamp = get_now();
struct timeval avg_first_timestamp = avg_last_timestamp;

// AmSession methods

AmSession::AmSession(AmSipDialog* p_dlg)
  : AmEventQueue(this), dlg(p_dlg),
    input(NULL), output(NULL),
    sess_stopped(false),
    m_dtmfDetector(this), m_dtmfEventQueue(&m_dtmfDetector),
    m_dtmfDetectionEnabled(true),
    accept_early_session(false),
    rtp_interface(-1),
    refresh_method(REFRESH_UPDATE_FB_REINV),
    processing_status(SESSION_PROCESSING_EVENTS)
#ifdef WITH_ZRTP
  , enable_zrtp(AmConfig::enable_zrtp)
#endif

#ifdef SESSION_THREADPOOL
  , _pid(this)
#endif
{
  DBG("dlg = %p",dlg);
  if(!dlg) dlg = new AmSipDialog(this);
  else dlg->setEventhandler(this);
}

AmSession::~AmSession()
{
  for(vector<AmSessionEventHandler*>::iterator evh = ev_handlers.begin();
      evh != ev_handlers.end(); evh++) {
    
    if((*evh)->destroy)
      delete *evh;
  }

  delete dlg;

  DBG("AmSession destructor finished\n");
}

AmSipDialog* AmSession::createSipDialog()
{
  return new AmSipDialog(this);
}

void AmSession::setCallgroup(const string& cg) {
  callgroup = cg;
}

string AmSession::getCallgroup() {
  return callgroup;
}

void AmSession::changeCallgroup(const string& cg) {
  callgroup = cg;
  AmMediaProcessor::instance()->changeCallgroup(this, cg);
}

void AmSession::startMediaProcessing() 
{
  if(getStopped() || isProcessingMedia())
    return;

  if(isAudioSet()) {
    AmMediaProcessor::instance()->addSession(this, callgroup);
  }
  else {
    DBG("no audio input and output set. "
	"Session will not be attached to MediaProcessor.\n");
  }
}

void AmSession::stopMediaProcessing() 
{
  if(!isProcessingMedia())
    return;

  AmMediaProcessor::instance()->removeSession(this);
}

void AmSession::addHandler(AmSessionEventHandler* sess_evh)
{
  if (sess_evh != NULL)
    ev_handlers.push_back(sess_evh);
}

void AmSession::setInput(AmAudio* in)
{
  lockAudio();
  input = in;
  unlockAudio();
}

void AmSession::setOutput(AmAudio* out)
{
  lockAudio();
  output = out;
  unlockAudio();
}

void AmSession::setInOut(AmAudio* in,AmAudio* out)
{
  lockAudio();
  input = in;
  output = out;
  unlockAudio();
}
  
bool AmSession::isAudioSet()
{
  lockAudio();
  bool set = input || output;
  unlockAudio();
  return set;
}

void AmSession::lockAudio()
{ 
  audio_mut.lock();
}

void AmSession::unlockAudio()
{
  audio_mut.unlock();
}

const string& AmSession::getCallID() const
{ 
  return dlg->getCallid();
}

const string& AmSession::getRemoteTag() const
{ 
  return dlg->getRemoteTag();
}

const string& AmSession::getLocalTag() const
{
  return dlg->getLocalTag();
}

const string& AmSession::getFirstBranch() const
{
  return dlg->get1stBranch();
}

void AmSession::setUri(const string& uri)
{
  DBG("AmSession::setUri(%s)\n",uri.c_str());
  /* TODO: sdp.uri = uri;*/
}

void AmSession::setLocalTag()
{
  if (dlg->getLocalTag().empty()) {
    string new_id = getNewId();
    dlg->setLocalTag(new_id);
    DBG("AmSession::setLocalTag() - session id set to %s\n", new_id.c_str());
  }
}

void AmSession::setLocalTag(const string& tag)
{
  DBG("AmSession::setLocalTag(%s)\n",tag.c_str());
  dlg->setLocalTag(tag);
}

const vector<SdpPayload*>& AmSession::getPayloads()
{
  return m_payloads;
}

int AmSession::getRPort()
{
  return RTPStream()->getRPort();
}

#ifdef SESSION_THREADPOOL
void AmSession::start() {
  AmSessionProcessorThread* processor_thread = 
    AmSessionProcessor::getProcessorThread();
  if (NULL == processor_thread) 
    throw string("no processing thread available");

  // have the thread register and start us
  processor_thread->startSession(this);
}

bool AmSession::is_stopped() {
  return processing_status == SESSION_ENDED_DISCONNECTED;
}
#else
// in this case every session has its own thread 
// - this is the main processing loop
void AmSession::run() {
  DBG("startup session\n");
  if (!startup())
    return;

  DBG("running session event loop\n");
  while (true) {
    waitForEvent();
    if (!processingCycle())
      break;
  }

  DBG("session event loop ended, finalizing session\n");
  finalize();
}
#endif

bool AmSession::startup() {
  session_started();

  try {
    try {

      onStart();

#ifdef WITH_ZRTP
      if (enable_zrtp) {
	if (zrtp_session_state.initSession(this)) {
	  ERROR("initializing ZRTP session\n");
	  throw AmSession::Exception(500, SIP_REPLY_SERVER_INTERNAL_ERROR);
	}
	DBG("initialized ZRTP session context OK\n");
      }
#endif

    } 
    catch(const AmSession::Exception& e){ throw e; }
    catch(const string& str){
      ERROR("%s\n",str.c_str());
      throw AmSession::Exception(500,"unexpected exception.");
    }
    catch(...){
      throw AmSession::Exception(500,"unexpected exception.");
    }
    
  } catch(const AmSession::Exception& e){
    ERROR("%i %s\n",e.code,e.reason.c_str());
    onBeforeDestroy();
    destroy();
    
    session_stopped();

    return false;
  }

  return true;
}

bool AmSession::processEventsCatchExceptions() {
  try {
    try {	
      processEvents();
    } 
    catch(const AmSession::Exception& e){ throw e; }
    catch(const string& str){
      ERROR("%s\n",str.c_str());
      throw AmSession::Exception(500,"unexpected exception.");
    } 
    catch(...){
      throw AmSession::Exception(500,"unexpected exception.");
    }    
  } catch(const AmSession::Exception& e){
    ERROR("%i %s\n",e.code,e.reason.c_str());
    return false;
  }
  return true;
}

/** one cycle of the event processing loop. 
    this should be called until it returns false. */
bool AmSession::processingCycle() {

  DBG("vv S [%s|%s] %s, %s, %i UACTransPending, %i usages vv\n",
      dlg->getCallid().c_str(),getLocalTag().c_str(),
      dlg->getStatusStr(),
      sess_stopped.get()?"stopped":"running",
      dlg->getUACTransPending(),
      dlg->getUsages());

  switch (processing_status) {
  case SESSION_PROCESSING_EVENTS: 
    {
      if (!processEventsCatchExceptions()) {
	// exception occured, stop processing
	processing_status = SESSION_ENDED_DISCONNECTED;
	return false;
      }
      
      AmSipDialog::Status dlg_status = dlg->getStatus();
      bool s_stopped = sess_stopped.get();
      
      DBG("^^ S [%s|%s] %s, %s, %i UACTransPending, %i usages ^^\n",
	  dlg->getCallid().c_str(),getLocalTag().c_str(),
	  AmBasicSipDialog::getStatusStr(dlg_status),
	  s_stopped?"stopped":"running",
	  dlg->getUACTransPending(),
	  dlg->getUsages());
      
      // session running?
      if (!s_stopped || (dlg_status == AmSipDialog::Disconnecting)
	  || dlg->getUsages())
	return true;
      
      // session stopped?
      if (s_stopped &&
	  (dlg_status == AmSipDialog::Disconnected)) {
	processing_status = SESSION_ENDED_DISCONNECTED;
	return false;
      }
      
      // wait for session's status to be disconnected
      // todo: set some timer to tear down the session anyway,
      //       or react properly on negative reply to BYE (e.g. timeout)
      processing_status = SESSION_WAITING_DISCONNECTED;
      
      if ((dlg_status != AmSipDialog::Disconnected) &&
	  (dlg_status != AmSipDialog::Cancelling)) {
	DBG("app did not send BYE - do that for the app\n");
	if (dlg->bye() != 0) {
	  processing_status = SESSION_ENDED_DISCONNECTED;
	  // BYE sending failed - don't wait for dlg status to go disconnected
	  return false;
	}
      }
      
      return true;
      
    } break;
    
  case SESSION_WAITING_DISCONNECTED: {
    // processing events until dialog status is Disconnected 
    
    if (!processEventsCatchExceptions()) {
      processing_status = SESSION_ENDED_DISCONNECTED;
      return false; // exception occured, stop processing
    }

    bool res = dlg->getStatus() != AmSipDialog::Disconnected;
    if (!res)
      processing_status = SESSION_ENDED_DISCONNECTED;

    DBG("^^ S [%s|%s] %s, %s, %i UACTransPending, %i usages ^^\n",
	dlg->getCallid().c_str(),getLocalTag().c_str(),
	dlg->getStatusStr(),
	sess_stopped.get()?"stopped":"running",
	dlg->getUACTransPending(),
	dlg->getUsages());

    return res;
  }; break;

  default: {
    ERROR("unknown session processing state\n");
    return false; // stop processing      
  }
  }
}

void AmSession::finalize()
{
  DBG("running finalize sequence...\n");
  dlg->finalize();
  
#ifdef WITH_ZRTP
  if (enable_zrtp) {
    zrtp_session_state.freeSession();
  }
#endif

  onBeforeDestroy();
  destroy();
  
  session_stopped();

  DBG("session is stopped.\n");
}
#ifndef SESSION_THREADPOOL
  void AmSession::on_stop() 
#else
  void AmSession::stop()
#endif  
{
  DBG("AmSession::stop()\n");

  if (!isDetached())
    AmMediaProcessor::instance()->clearSession(this);
  else
    clearAudio();
}

void AmSession::setStopped(bool wakeup) {
  if (!sess_stopped.get()) {
    sess_stopped.set(true); 
    onStop();
  }
  if (wakeup) 
    AmSessionContainer::instance()->postEvent(getLocalTag(), 
					      new AmEvent(0));
}

string AmSession::getAppParam(const string& param_name) const
{
  map<string,string>::const_iterator param_it;
  param_it = app_params.find(param_name);
  if(param_it != app_params.end())
    return param_it->second;
  else
    return "";
}

void AmSession::destroy() {
  DBG("AmSession::destroy()\n");
  AmSessionContainer::instance()->destroySession(this);
}

string AmSession::getNewId() {
  struct timeval t;
  gettimeofday(&t,NULL);

  string id = "";

  id += int2hex(get_random()) + "-";
  id += int2hex(t.tv_sec) + int2hex(t.tv_usec) + "-";
  id += int2hex((unsigned int)((unsigned long)pthread_self()));

  return id;
}
/* bookkeeping functions - TODO: move to monitoring */
static inline void __update_session_count(unsigned int num) {
  int fd = open("/proc/ngcp/check", O_RDONLY);
  if (fd >= 0) {
      // set calls usage
      ioctl(fd, TCXONC, (int) num);
      close(fd);
  }
}
void AmSession::session_started() {
  struct timeval now, delta;

  session_num_mut.lock();
  //avg session number
  gettimeofday(&now, NULL);
  timersub(&now, &avg_last_timestamp, &delta);
  avg_session_num += session_num * (delta.tv_sec * 1000000ULL + delta.tv_usec);
  avg_last_timestamp = now;

  //current session number
  session_num = session_num + 1;

  //maximum session number
  if(session_num > max_session_num) max_session_num = session_num;

  session_num_mut.unlock();

  __update_session_count(getSessionNum());
}

void AmSession::session_stopped() {
  struct timeval now, delta;
  session_num_mut.lock();
  //avg session number
  gettimeofday(&now, NULL);
  timersub(&now, &avg_last_timestamp, &delta);
  avg_session_num += session_num * (delta.tv_sec * 1000000ULL + delta.tv_usec);
  avg_last_timestamp = now;
  //current session number
  session_num = session_num - 1;
  session_num_mut.unlock();

  __update_session_count(getSessionNum());
}

unsigned int AmSession::getSessionNum() {
  unsigned int res = 0;
  session_num_mut.lock();
  res = session_num;
  session_num_mut.unlock();
  return res;
}

unsigned int AmSession::getMaxSessionNum() {
  unsigned int res = 0;
  session_num_mut.lock();
  res = max_session_num;
  max_session_num = session_num;
  session_num_mut.unlock();
  return res;
}

unsigned int AmSession::getAvgSessionNum() {
  unsigned int res = 0;
  struct timeval now, delta;
  session_num_mut.lock();
  gettimeofday(&now, NULL);
  timersub(&now, &avg_last_timestamp, &delta);
  avg_session_num += session_num * (delta.tv_sec * 1000000ULL + delta.tv_usec);
  timersub(&now, &avg_first_timestamp, &delta);
  unsigned long long d_usec = delta.tv_sec * 1000000ULL + delta.tv_usec;
  if (!d_usec) {
    res = 0;
    WARN("zero delta!\n");
  } else {
    //Round up
    res = (unsigned int)((avg_session_num + d_usec - 1) / d_usec);
  }
  avg_session_num = 0;
  avg_last_timestamp = now;
  avg_first_timestamp = now;
  session_num_mut.unlock();
  return res;
}

void AmSession::setInbandDetector(Dtmf::InbandDetectorType t)
{ 
  m_dtmfDetector.setInbandDetector(t, RTPStream()->getSampleRate()); 
}

void AmSession::postDtmfEvent(AmDtmfEvent *evt)
{
  if (m_dtmfDetectionEnabled)
    {
      if (dynamic_cast<AmSipDtmfEvent *>(evt) ||
	  dynamic_cast<AmRtpDtmfEvent *>(evt))
        {   
	  // this is a raw event from sip info or rtp
	  m_dtmfEventQueue.postEvent(evt);
        }
      else 
        {
	  // this is an aggregated event, 
	  // post it into our event queue
	  postEvent(evt);
        }
    }
}

void AmSession::processDtmfEvents()
{
  if (m_dtmfDetectionEnabled)
    {
      m_dtmfEventQueue.processEvents();
    }
}

void AmSession::putDtmfAudio(const unsigned char *buf, int size, unsigned long long system_ts)
{
  m_dtmfEventQueue.putDtmfAudio(buf, size, system_ts);
}

void AmSession::sendDtmf(int event, unsigned int duration_ms) {
  RTPStream()->sendDtmf(event, duration_ms);
}


void AmSession::onDtmf(int event, int duration_msec)
{
  DBG("AmSession::onDtmf(%i,%i)\n",event,duration_msec);
}

void AmSession::clearAudio()
{
  lockAudio();

  if (input) {
    input->close();
    input = NULL;
  }
  if (output) {
    output->close();
    output = NULL;
  }

  unlockAudio();
  DBG("Audio cleared !!!\n");
  postEvent(new AmAudioEvent(AmAudioEvent::cleared));
}

void AmSession::process(AmEvent* ev)
{
  CALL_EVENT_H(process,ev);

  DBG("AmSession processing event\n");

  if (ev->event_id == E_SYSTEM) {
    AmSystemEvent* sys_ev = dynamic_cast<AmSystemEvent*>(ev);
    if(sys_ev){	
      DBG("Session received system Event\n");
      onSystemEvent(sys_ev);
      return;
    }
  }

  AmSipEvent* sip_ev = dynamic_cast<AmSipEvent*>(ev);
  if(sip_ev){
    (*sip_ev)(dlg);
    return;
  }

  AmAudioEvent* audio_ev = dynamic_cast<AmAudioEvent*>(ev);
  if(audio_ev){
    onAudioEvent(audio_ev);
    return;
  }

  AmDtmfEvent* dtmf_ev = dynamic_cast<AmDtmfEvent*>(ev);
  if (dtmf_ev) {
    DBG("Session received DTMF, event = %d, duration = %d\n", 
	dtmf_ev->event(), dtmf_ev->duration());
    onDtmf(dtmf_ev->event(), dtmf_ev->duration());
    return;
  }

  AmRtpTimeoutEvent* timeout_ev = dynamic_cast<AmRtpTimeoutEvent*>(ev);
  if(timeout_ev){
    onRtpTimeout();
    return;
  }

#ifdef WITH_ZRTP
  AmZRTPProtocolEvent* zrtp_p_ev = dynamic_cast<AmZRTPProtocolEvent*>(ev);
  if(zrtp_p_ev){
    onZRTPProtocolEvent((zrtp_protocol_event_t)zrtp_p_ev->event_id, zrtp_p_ev->stream_ctx);
    return;
  }

  AmZRTPSecurityEvent* zrtp_s_ev = dynamic_cast<AmZRTPSecurityEvent*>(ev);
  if(zrtp_s_ev){
    onZRTPSecurityEvent((zrtp_security_event_t)zrtp_s_ev->event_id, zrtp_s_ev->stream_ctx);
    return;
  }
#endif
}

void AmSession::onSipRequest(const AmSipRequest& req)
{
  CALL_EVENT_H(onSipRequest,req);

  DBG("onSipRequest: method = %s\n",req.method.c_str());

  updateRefreshMethod(req.hdrs);

  if(req.method == SIP_METH_INVITE){

    try {
      onInvite(req);
    }
    catch(const string& s) {
      ERROR("%s\n",s.c_str());
      setStopped();
      dlg->reply(req, 500, SIP_REPLY_SERVER_INTERNAL_ERROR);
    }
    catch(const AmSession::Exception& e) {
      ERROR("%i %s\n",e.code,e.reason.c_str());
      setStopped();
      dlg->reply(req, e.code, e.reason, NULL, e.hdrs);
    }
  }
  else if(req.method == SIP_METH_ACK){
    return;
  }
  else if( req.method == SIP_METH_BYE ){
    dlg->reply(req,200,"OK");
    onBye(req);
  }
  else if( req.method == SIP_METH_CANCEL ){
    onCancel(req);
  } 
  else if( req.method == SIP_METH_INFO ){

    const AmMimeBody* dtmf_body = 
      req.body.hasContentType("application/dtmf-relay");

    if (dtmf_body) {
      string dtmf_body_str((const char*)dtmf_body->getPayload(),
			   dtmf_body->getLen());
      postDtmfEvent(new AmSipDtmfEvent(dtmf_body_str));
      dlg->reply(req, 200, "OK");
    } else {
      dlg->reply(req, 415, "Unsupported Media Type");
    }
  } else if (req.method == SIP_METH_PRACK) {
    // TODO: SDP
    dlg->reply(req, 200, "OK");
    // TODO: WARN: only include latest SDP if req.rseq == dlg->rseq (latest 1xx)
  }
  else {
    dlg->reply(req, 501, "Not implemented");
  }
}

void AmSession::onSipReply(const AmSipRequest& req, const AmSipReply& reply,
			   AmBasicSipDialog::Status old_dlg_status)
{
  CALL_EVENT_H(onSipReply, req, reply, old_dlg_status);

  updateRefreshMethod(reply.hdrs);

  if (dlg->getStatus() < AmSipDialog::Connected &&
      reply.code == 180) {
    onRinging(reply);
  }

  if (old_dlg_status != dlg->getStatus()) {
    DBG("Dialog status changed %s -> %s (stopped=%s) \n", 
	AmBasicSipDialog::getStatusStr(old_dlg_status), 
	dlg->getStatusStr(),
	sess_stopped.get() ? "true" : "false");
  } else {
    DBG("Dialog status stays %s (stopped=%s)\n", 
	AmBasicSipDialog::getStatusStr(old_dlg_status), 
	sess_stopped.get() ? "true" : "false");
  }
}



void AmSession::onInvite2xx(const AmSipReply& reply)
{
  dlg->send_200_ack(reply.cseq);
}

void AmSession::onRemoteDisappeared(const AmSipReply&) {
  // see 3261 - 12.2.1.2: should end dialog on 408/481
  DBG("Remote end unreachable - ending session\n");
  dlg->bye();
  setStopped();
}

void AmSession::onNoAck(unsigned int cseq)
{
  if (dlg->getStatus() == AmSipDialog::Connected)
    dlg->bye();
  setStopped();
}

void AmSession::onNoPrack(const AmSipRequest &req, const AmSipReply &rpl)
{
  dlg->reply(req, 504, "Server Time-out");
  // TODO: handle forking case (when more PRACKs are sent, out of which some
  // might time-out/fail).
  if (dlg->getStatus() < AmSipDialog::Connected)
    setStopped();
}

void AmSession::onAudioEvent(AmAudioEvent* audio_ev)
{
  if (audio_ev->event_id == AmAudioEvent::cleared)
    setStopped();
}

void AmSession::onInvite(const AmSipRequest& req)
{
  dlg->reply(req,200,"OK");
}

void AmSession::onBye(const AmSipRequest& req)
{
  setStopped();
}

void AmSession::onCancel(const AmSipRequest& cancel)
{
  dlg->bye();
  setStopped();
}

void AmSession::onSystemEvent(AmSystemEvent* ev) {
  if (ev->sys_event == AmSystemEvent::ServerShutdown) {
    setStopped();
    return;
  }
}

void AmSession::onSendRequest(AmSipRequest& req, int& flags)
{
  CALL_EVENT_H(onSendRequest,req,flags);
}

void AmSession::onSendReply(const AmSipRequest& req, AmSipReply& reply, int& flags)
{
  CALL_EVENT_H(onSendReply,req,reply,flags);
}

/** Hook called when an SDP offer is required */
bool AmSession::getSdpOffer(AmSdp& offer)
{
  DBG("AmSession::getSdpOffer(...) ...\n");

  offer.version = 0;
  offer.origin.user = "sems";
  //offer.origin.sessId = 1;
  //offer.origin.sessV = 1;
  offer.sessionName = "sems";
  offer.conn.network = NT_IN;
  offer.conn.addrType = AT_V4;
  offer.conn.address = advertisedIP();

  // TODO: support mutiple media types (needs multiples RTP streams)
  // TODO: support update instead of clearing everything

  if(RTPStream()->getSdpMediaIndex() < 0)
    offer.media.clear();

  unsigned int media_idx = 0;
  if(!offer.media.size()) {
    offer.media.push_back(SdpMedia());
    offer.media.back().type=MT_AUDIO;
  }
  else {
    media_idx = RTPStream()->getSdpMediaIndex();
  }

  RTPStream()->setLocalIP(localMediaIP());
  RTPStream()->getSdpOffer(media_idx,offer.media.back());
  
  return true;
}

struct codec_priority_cmp
{
public:
  codec_priority_cmp() {}

  bool operator()(const SdpPayload& left, const SdpPayload& right)
  {
    for (vector<string>::iterator it = AmConfig::CodecOrder.begin(); it != AmConfig::CodecOrder.end(); it++) {
      if (strcasecmp(left.encoding_name.c_str(),it->c_str())==0 && strcasecmp(right.encoding_name.c_str(), it->c_str())!=0)
	return true;
      if (strcasecmp(right.encoding_name.c_str(),it->c_str())==0)
	return false;
    }

    return false;
  }
};

/** Hook called when an SDP answer is required */
bool AmSession::getSdpAnswer(const AmSdp& offer, AmSdp& answer)
{
  DBG("AmSession::getSdpAnswer(...) ...\n");

  answer.version = 0;
  answer.origin.user = "sems";
  //answer.origin.sessId = 1;
  //answer.origin.sessV = 1;
  answer.sessionName = "sems";
  answer.conn.network = NT_IN;
  if (offer.conn.address.empty()) answer.conn.addrType = AT_V4; // or use first stream connection?
  else answer.conn.addrType = offer.conn.addrType;
  answer.conn.address = advertisedIP(answer.conn.addrType);
  answer.media.clear();
  
  bool audio_1st_stream = true;
  unsigned int media_index = 0;
  for(vector<SdpMedia>::const_iterator m_it = offer.media.begin();
      m_it != offer.media.end(); ++m_it) {

    answer.media.push_back(SdpMedia());
    SdpMedia& answer_media = answer.media.back();

    if( m_it->type == MT_AUDIO
	&& m_it->transport == TP_RTPAVP
        && audio_1st_stream 
        && (m_it->port != 0) ) {

      RTPStream()->setLocalIP(localMediaIP(answer.conn.addrType));
      RTPStream()->getSdpAnswer(media_index,*m_it,answer_media);
      if(answer_media.payloads.empty() ||
	 ((answer_media.payloads.size() == 1) &&
	  (answer_media.payloads[0].encoding_name == "telephone-event"))
	 ){
	// no compatible media found
	throw Exception(488,"no compatible payload");
      }
      audio_1st_stream = false;
    }
    else {
      
      answer_media.type = m_it->type;
      answer_media.port = 0;
      answer_media.nports = 0;
      answer_media.transport = m_it->transport;
      answer_media.send = false;
      answer_media.recv = false;
      answer_media.payloads.clear();
      if(!m_it->payloads.empty()) {
	SdpPayload dummy_pl = m_it->payloads.front();
	dummy_pl.encoding_name.clear();
	dummy_pl.sdp_format_parameters.clear();
	answer_media.payloads.push_back(dummy_pl);
      }
      answer_media.attributes.clear();
    }

    // sort payload type in the answer according to the priority given in the codec_order configuration key
    std::stable_sort(answer_media.payloads.begin(),answer_media.payloads.end(),codec_priority_cmp());

    media_index++;
  }

  return true;
}

int AmSession::onSdpCompleted(const AmSdp& local_sdp, const AmSdp& remote_sdp)
{
  DBG("AmSession::onSdpCompleted(...) ...\n");

  if(local_sdp.media.empty() || remote_sdp.media.empty()) {

    ERROR("Invalid SDP");

    string debug_str;
    local_sdp.print(debug_str);
    ERROR("Local SDP:\n%s",
	  debug_str.empty() ? "<empty>"
	  : debug_str.c_str());
    
    remote_sdp.print(debug_str);
    ERROR("Remote SDP:\n%s",
	  debug_str.empty() ? "<empty>"
	  : debug_str.c_str());

    return -1;
  }

  bool set_on_hold = false;
  if (!remote_sdp.media.empty()) {
    vector<SdpAttribute>::const_iterator pos =
      std::find(remote_sdp.media[0].attributes.begin(), remote_sdp.media[0].attributes.end(), SdpAttribute("sendonly"));
    set_on_hold = pos != remote_sdp.media[0].attributes.end();
  }

  lockAudio();

  // TODO: 
  //   - get the right media ID
  //   - check if the stream coresponding to the media ID 
  //     should be created or updated   
  //
  int ret = 0;

  try {
    ret = RTPStream()->init(local_sdp, remote_sdp, AmConfig::ForceSymmetricRtp);
  } catch (const string& s) {
    ERROR("Error while initializing RTP stream: '%s'\n", s.c_str());
    ret = -1;
  } catch (...) {
    ERROR("Error while initializing RTP stream (unknown exception in AmRTPStream::init)\n");
    ret = -1;
  }
  unlockAudio();

  if (!isProcessingMedia()) {
    setInbandDetector(AmConfig::DefaultDTMFDetector);
  }

  return ret;
}

void AmSession::onEarlySessionStart()
{
  startMediaProcessing();
}

void AmSession::onSessionStart()
{
  startMediaProcessing();
}

void AmSession::onRtpTimeout()
{
  DBG("RTP timeout, stopping Session\n");
  dlg->bye();
  setStopped();
}

void AmSession::onSessionTimeout() {
  DBG("Session Timer: Timeout, ending session.\n");
  dlg->bye();
  setStopped();
}

void AmSession::updateRefreshMethod(const string& headers) {
  if (refresh_method == REFRESH_UPDATE_FB_REINV) {
    if (key_in_list(getHeader(headers, SIP_HDR_ALLOW),
		    SIP_METH_UPDATE)) {
      DBG("remote allows UPDATE, using UPDATE for session refresh.\n");
      refresh_method = REFRESH_UPDATE;
    }
  }
}

bool AmSession::refresh(int flags) {
  // no session refresh if not connected
  if (dlg->getStatus() != AmSipDialog::Connected)
    return false;

  if (refresh_method == REFRESH_UPDATE) {
    DBG("Refreshing session with UPDATE\n");
    return sendUpdate( NULL, "") == 0;
  } else {

    if (dlg->getUACInvTransPending()) {
      DBG("INVITE transaction pending - not refreshing now\n");
      return false;
    }

    DBG("Refreshing session with re-INVITE\n");
    return sendReinvite(true, "", flags) == 0;
  }
}

int AmSession::sendUpdate(const AmMimeBody* body,
			  const string &hdrs)
{
  return dlg->update(body, hdrs);
}

void AmSession::onInvite1xxRel(const AmSipReply &reply)
{
  // TODO: SDP
  if (dlg->prack(reply, NULL, /*headers*/"") < 0)
    ERROR("failed to send PRACK request in session '%s'.\n",sid4dbg().c_str());
}

void AmSession::onPrack2xx(const AmSipReply &reply)
{
  /* TODO: SDP */
}

string AmSession::sid4dbg()
{
  string dbg;
  dbg = dlg->getCallid() + "/" + dlg->getLocalTag() + "/" 
    + dlg->getRemoteTag() + "/" 
    + int2str(RTPStream()->getLocalPort()) + "/" 
    + RTPStream()->getRHost() + ":" + int2str(RTPStream()->getRPort());
  return dbg;
}

int AmSession::sendReinvite(bool updateSDP, const string& headers, int flags) 
{
  if(updateSDP){
    // Forces SDP offer/answer 
    AmMimeBody sdp;
    sdp.addPart(SIP_APPLICATION_SDP);
    return dlg->reinvite(headers, &sdp, flags);
  }
  else {
    return dlg->reinvite(headers, NULL, flags);
  }
}

int AmSession::sendInvite(const string& headers) 
{
  onOutgoingInvite(headers);

  // Forces SDP offer/answer
  AmMimeBody sdp;
  sdp.addPart(SIP_APPLICATION_SDP);
  return dlg->invite(headers, &sdp);
}

void AmSession::setOnHold(bool hold)
{
  lockAudio();
  bool old_hold = RTPStream()->getOnHold();
  RTPStream()->setOnHold(hold);
  if (hold != old_hold) 
    sendReinvite();
  unlockAudio();
}

void AmSession::onFailure()
{
  // switch (cause) {
  //   case FAIL_REL100_421:
  //   case FAIL_REL100_420:
  //     if (rpl) {
  //       dlg.cancel();
  //       if (dlg.getStatus() < AmSipDialog::Connected)
  //         setStopped();
  //     } else if (req) {
  //       if (cause == FAIL_REL100_421) {
  //         dlg.reply(*req, 421, SIP_REPLY_EXTENSION_REQUIRED, NULL,
  //             SIP_HDR_COLSP(SIP_HDR_REQUIRE) SIP_EXT_100REL CRLF);
  //       } else {
  //         dlg.reply(*req, 420, SIP_REPLY_BAD_EXTENSION, NULL,
  //             SIP_HDR_COLSP(SIP_HDR_UNSUPPORTED) SIP_EXT_100REL CRLF);
  //       }
  //       /* finally, stop session if running */
  //       if (dlg.getStatus() < AmSipDialog::Connected)
  //         setStopped();
  //     }
  //     break;
  //   default:
  //     break;
  // }
}


int AmSession::getRtpInterface()
{
  if(rtp_interface < 0){
    // TODO: get default media interface for signaling IF instead
    rtp_interface = AmConfig::SIP_Ifs[dlg->getOutboundIf()].RtpInterface;
    if(rtp_interface < 0) {
      DBG("No media interface for signaling interface:\n");
      DBG("Using default media interface instead.\n");
      rtp_interface = 0;
    }
  }
  return rtp_interface;
}

void AmSession::setRtpInterface(int _rtp_interface) {
  DBG("setting media interface to %d\n", _rtp_interface);
  rtp_interface = _rtp_interface;
}

string AmSession::localMediaIP(int addrType)
{
  // sets rtp_interface if not initialized
  getRtpInterface();
  
  assert(rtp_interface >= 0);
  assert((unsigned int)rtp_interface < AmConfig::RTP_Ifs.size());

  string set_ip = "";
  for (size_t i = rtp_interface; i < AmConfig::RTP_Ifs.size(); i++) {
    set_ip = AmConfig::RTP_Ifs[i].LocalIP; // "media_ip" parameter.
    if ((addrType == AT_NONE) ||
	((addrType == AT_V4) && (set_ip.find(".") != std::string::npos)) ||
	((addrType == AT_V6) && (set_ip.find(":") != std::string::npos)))
      return set_ip;
  }
  return set_ip;
}

// Utility for basic NAT handling: allow the config file to specify the IP
// address to use in SDP bodies 
string AmSession::advertisedIP(int addrType)
{
  // sets rtp_interface if not initialized
  getRtpInterface();
  
  assert(rtp_interface >= 0);
  assert((unsigned int)rtp_interface < AmConfig::RTP_Ifs.size());

  string set_ip = "";
  for (size_t i = rtp_interface; i < AmConfig::RTP_Ifs.size(); i++) {
    set_ip = AmConfig::RTP_Ifs[i].getIP(); // "media_ip" parameter.
    if ((addrType == AT_NONE) ||
	((addrType == AT_V4) && (set_ip.find(".") != std::string::npos)) ||
	((addrType == AT_V6) && (set_ip.find(":") != std::string::npos)))
      return set_ip;
  }
  return set_ip;
}  

bool AmSession::timersSupported() {
  WARN("this function is deprecated; application timers are always supported\n");
  return true;
}

bool AmSession::setTimer(int timer_id, double timeout) {
  if (timeout <= 0.005) {
    DBG("setting timer %d with immediate timeout - posting Event\n", timer_id);
    AmTimeoutEvent* ev = new AmTimeoutEvent(timer_id);
    postEvent(ev);
    return true;
  }

  DBG("setting timer %d with timeout %f\n", timer_id, timeout);
  AmAppTimer::instance()->setTimer(getLocalTag(), timer_id, timeout);

  return true;
}

bool AmSession::removeTimer(int timer_id) {

  DBG("removing timer %d\n", timer_id);
  AmAppTimer::instance()->removeTimer(getLocalTag(), timer_id);

  return true;
}

bool AmSession::removeTimers() {

  DBG("removing timers\n");
  AmAppTimer::instance()->removeTimers(getLocalTag());

  return true;
}

 
#ifdef WITH_ZRTP

void AmSession::onZRTPProtocolEvent(zrtp_protocol_event_t event, zrtp_stream_t *stream_ctx) {
  DBG("AmSession::onZRTPProtocolEvent: %s\n", zrtp_protocol_event_desc(event));

  if (event==ZRTP_EVENT_IS_SECURE) {
      INFO("ZRTP_EVENT_IS_SECURE \n");
      //         info->is_verified  = ctx->_session_ctx->secrets.verifieds & ZRTP_BIT_RS0;
 
      // zrtp_session_t *session = stream_ctx->_session_ctx;
 
      // if (ZRTP_SAS_BASE32 == session->sas_values.rendering) {
      // 	DBG("Got SAS value <<<%.4s>>>\n", session->sas_values.str1.buffer);
      // } else {
      // 	DBG("Got SAS values SAS1 '%s' and SAS2 '%s'\n", 
      // 	    session->sas_values.str1.buffer,
      // 	    session->sas_values.str2.buffer);
      // }
  }
 
    // case ZRTP_EVENT_IS_PENDINGCLEAR:
    //   INFO("ZRTP_EVENT_IS_PENDINGCLEAR\n");
    //   INFO("other side requested goClear. Going clear.\n\n");
    //   //      zrtp_clear_stream(zrtp_audio);
    //   break;
  
}

void AmSession::onZRTPSecurityEvent(zrtp_security_event_t event, zrtp_stream_t *stream_ctx) {
  DBG("AmSession::onZRTPSecurityEvent: %s\n", zrtp_security_event_desc(event));
}
 
#endif

int AmSession::readStreams(unsigned long long ts, unsigned char *buffer) 
{ 
  int res = 0;
  lockAudio();

  AmRtpAudio *stream = RTPStream();
  unsigned int f_size = stream->getFrameSize();
  if (stream->checkInterval(ts)) {
    int got = stream->get(ts, buffer, stream->getSampleRate(), f_size);
    if (got < 0) res = -1;
    if (got > 0) {
      if (isDtmfDetectionEnabled())
        putDtmfAudio(buffer, got, ts);

      if (input) res = input->put(ts, buffer, stream->getSampleRate(), got);
    }
  }
  
  unlockAudio();
  return res;
}

int AmSession::writeStreams(unsigned long long ts, unsigned char *buffer) 
{ 
  int res = 0;
  lockAudio();

  AmRtpAudio *stream = RTPStream();
  if (stream->sendIntReached()) { // FIXME: shouldn't depend on checkInterval call before!
    unsigned int f_size = stream->getFrameSize();
    int got = 0;
    if (output) got = output->get(ts, buffer, stream->getSampleRate(), f_size);
    if (got < 0) res = -1;
    if (got > 0) res = stream->put(ts, buffer, stream->getSampleRate(), got);
  }
  
  unlockAudio();
  return res;

}

/** EMACS **
 * Local variables:
 * mode: c++
 * c-basic-offset: 2
 * End:
 */
