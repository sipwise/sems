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

#include "AmRtpStream.h"
#include "AmRtpPacket.h"
#include "AmRtpReceiver.h"
#include "AmRtpTransport.h"
#include "AmConfig.h"
#include "AmPlugIn.h"
#include "AmAudio.h"
#include "AmUtils.h"
#include "AmSession.h"

#include "AmDtmfDetector.h"
#include "rtp/telephone_event.h"
#include "amci/codecs.h"
#include "AmJitterBuffer.h"

#include "sip/resolver.h"
#include "sip/ip_util.h"
#include "sip/raw_sender.h"
#include "sip/msg_logger.h"

#include "log.h"

#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>       
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "rtp/rtp.h"

#include <set>
#include <iostream>

using std::set;

void PayloadMask::clear()
{
  memset(bits, 0, sizeof(bits));
}

void PayloadMask::set_all()
{
  memset(bits, 0xFF, sizeof(bits));
}

void PayloadMask::invert()
{
  // assumes that bits[] contains 128 bits
  unsigned long long* ull = (unsigned long long*)bits;
  ull[0] = ~ull[0];
  ull[1] = ~ull[1];
}

PayloadMask::PayloadMask(const PayloadMask &src)
{
  memcpy(bits, src.bits, sizeof(bits));
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////


/* RFC 6263 Application Mechanism for Keeping Alive the NAT Mappings
 * 4.5 (RTP Packet with Incorrect Version Number)
 */
void AmRtpStream::ping()
{

  if (!rtp_transport) {
    return;
  }

  unsigned char ping_chr[2];

  ping_chr[0] = 0;
  ping_chr[1] = 0;

  AmRtpPacket rp;
  rp.version = 0;
  rp.payload = payload;
  rp.marker = true;
  rp.sequence = sequence++;
  rp.timestamp = 0;   
  rp.ssrc = l_ssrc;

  rp.compile((unsigned char*)ping_chr,2);

  rtp_transport->sendRtp(&rp);
}

int AmRtpStream::compile_and_send(const int payload, bool marker, unsigned int ts, 
				  unsigned char* buffer, unsigned int size) {

  if (!rtp_transport)
    return 0;

  AmRtpPacket rp;
  rp.payload = payload;
  rp.timestamp = ts;
  rp.marker = marker;
  rp.sequence = sequence++;
  rp.ssrc = l_ssrc;
  rp.compile((unsigned char*)buffer,size);

  if (rtp_transport->sendRtp(&rp) < 0)
    return -1;

  return size;
}

void AmRtpStream::generateDtmf(unsigned int ts)
{
  if(remote_telephone_event_pt.get())
    dtmf_sender.sendPacket(ts,remote_telephone_event_pt->payload_type,this);
}

int AmRtpStream::send( unsigned int ts, unsigned char* buffer, unsigned int size )
{
  if ((mute) || (hold))
    return 0;

  generateDtmf(ts);

  if(!size)
    return -1;

  PayloadMappingTable::iterator it = pl_map.find(payload);
  if ((it == pl_map.end()) || (it->second.remote_pt < 0)) {
    ERROR("sending packet with unsupported remote payload type %d\n", payload);
    return -1;
  }
  
  return compile_and_send(it->second.remote_pt, false, ts, buffer, size);
}

int AmRtpStream::send_raw( char* packet, unsigned int length )
{
  if ((mute) || (hold))
    return 0;

  if (!rtp_transport)
    return 0;

  AmRtpPacket rp;
  rp.compile_raw((unsigned char*)packet, length);

  if(rtp_transport->sendRtp(&rp) < 0){
    ERROR("while sending raw RTP packet.\n");
    return -1;
  }

  return length;
}

// returns 
// @param ts              [out] timestamp of the received packet, 
//                              in audio buffer relative time
// @param audio_buffer_ts [in]  current ts at the audio_buffer 

int AmRtpStream::receive( unsigned char* buffer, unsigned int size,
			  unsigned int& ts, int &out_payload)
{
  AmRtpPacket* rp = NULL;
  int err = nextPacket(rp);
    
  if(err <= 0)
    return err;

  if (!rp)
    return 0;

  /* do we have a new talk spurt? */
  begin_talk = ((last_payload == 13) || rp->marker);
  last_payload = rp->payload;

  if(!rp->getDataSize()) {
    mem.freePacket(rp);
    return RTP_EMPTY;
  }

  if (rp->payload == getLocalTelephoneEventPT())
    {
      recvDtmfPacket(rp);
      mem.freePacket(rp);
      return RTP_DTMF;
    }

  assert(rp->getData());
  if(rp->getDataSize() > size){
    ERROR("received too big RTP packet\n");
    mem.freePacket(rp);
    return RTP_BUFFER_SIZE;
  }

  memcpy(buffer,rp->getData(),rp->getDataSize());
  ts = rp->timestamp;
  out_payload = rp->payload;

  int res = rp->getDataSize();
  mem.freePacket(rp);
  return res;
}

AmRtpStream::AmRtpStream(AmSession* _s, int _if) 
  : l_ssrc(0),
    r_ssrc(0),
    r_ssrc_i(false),
    session(_s),
    passive(false),
    passive_rtcp(false),
    offer_answer_used(true),
    active(false), // do not return any data unless something really received
    mute(false),
    hold(false),
    receiving(true),
    monitor_rtp_timeout(true),
    relay_stream(NULL),
    relay_enabled(false),
    relay_raw(false),
    sdp_media_index(-1),
    relay_transparent_ssrc(true),
    relay_transparent_seqno(true),
    relay_filter_dtmf(false),
    force_receive_dtmf(false),
    hook(NULL),
    rtp_transport(NULL),
    rtp_keepalive_freq(0),
    rtp_timeout(0),
    rtp_keepalive_timer(this),
    rtp_timer(this)
{

  l_ssrc = get_random();
  sequence = get_random();
  clearRTPTimeout();

  // by default the system codecs
  payload_provider = AmPlugIn::instance();

  if (session) {
    // RTP Keepalive
    rtp_keepalive_freq = session->rtp_keepalive_freq;

    // RTP Timeout
    rtp_timeout = session->rtp_timeout;
  }
}

AmRtpStream::~AmRtpStream()
{
  if (rtp_keepalive_freq)
    AmAppTimer::instance()->removeTimer(&rtp_keepalive_timer);

  if (rtp_timeout)
    AmAppTimer::instance()->removeTimer(&rtp_timer);

  if (rtp_transport)
    rtp_transport->removeStream(this);
}

int AmRtpStream::getLocalRtpPort()
{
  if (!rtp_transport)
    return 0;
  else
    return rtp_transport->getLocalRtpPort();
}

int AmRtpStream::getLocalRtcpPort()
{
  if (!rtp_transport)
    return 0;
  else
    return rtp_transport->getLocalRtcpPort();
}

int AmRtpStream::getRemoteRtpPort()
{
  if (!rtp_transport)
    return 0;
  else
    return rtp_transport->getRemoteRtpPort();
}

string AmRtpStream::getRemoteAddress()
{
  if (!rtp_transport)
    return string();
  else
    return rtp_transport->getRemoteAddress();
}

void AmRtpStream::handleSymmetricRtp(struct sockaddr_storage* recv_addr, bool rtcp) {

  if (!rtp_transport)
    return;

  if((!rtcp && passive) || (rtcp && passive_rtcp)) {

    struct sockaddr_in* in_recv = (struct sockaddr_in*)recv_addr;
    struct sockaddr_in6* in6_recv = (struct sockaddr_in6*)recv_addr;

    struct sockaddr_in* in_addr = (struct sockaddr_in*)rtp_transport->getRemoteRtpSocket();
    struct sockaddr_in6* in6_addr = (struct sockaddr_in6*)rtp_transport->getRemoteRtpSocket();

    unsigned short port = am_get_port(recv_addr);

    // symmetric RTP
    if ( (!rtcp && (port != rtp_transport->getRemoteRtpPort())) ||
   (rtcp && rtp_transport &&
    (port != rtp_transport->getRemoteRtcpPort())) ||
	 ((recv_addr->ss_family == AF_INET) &&
	  (in_addr->sin_addr.s_addr != in_recv->sin_addr.s_addr)) ||
	 ((recv_addr->ss_family == AF_INET6) &&
	  (memcmp(&in6_addr->sin6_addr,
		      &in6_recv->sin6_addr,
		      sizeof(struct in6_addr))))
	 ) {

      string addr_str = get_addr_str(recv_addr);

      if (rtcp)
        rtp_transport->setRemoteRtcpAddress(addr_str, port);
      else
        rtp_transport->setRemoteRtpAddress(addr_str, port);

      DBG("Symmetric %s: setting new remote address: %s:%i\n",
	  !rtcp ? "RTP" : "RTCP", addr_str.c_str(),port);

    } else {
      const char* prot = rtcp ? "RTCP" : "RTP";
      DBG("Symmetric %s: remote end sends %s from advertised address."
	  " Leaving passive mode.\n",prot,prot);
    }
  }

  if (rtcp)
    passive_rtcp = false;
  else
    passive = false;
}

void AmRtpStream::setPassiveMode(bool p)
{
  passive_rtcp = passive = p;
  if (p) {
    DBG("The other UA is NATed or passive mode forced: switched to passive mode.\n");
  } else {
    DBG("Passive mode not activated.\n");
  }
}

void AmRtpStream::getSdp(SdpMedia& m)
{
  m.nports = 0;
  m.send = !hold;
  m.recv = receiving;
  m.type = MT_AUDIO;

  // direction
  if (AmConfig::SkipGenerateDirectionBoth)
    m.dir = SdpMedia::DirUndefined;
  else
    m.dir = SdpMedia::DirBoth;

  // get Transport description
  if (rtp_transport)
    rtp_transport->getDescription(m);
}

void AmRtpStream::getSdpOffer(unsigned int index, SdpMedia& offer)
{
  DBG("RTP Stream [%p] got media index %u", this, index);
  sdp_media_index = index;
  getSdp(offer);
  offer.payloads.clear();
  payload_provider->getPayloads(offer.payloads);
}

void AmRtpStream::getSdpAnswer(unsigned int index, const SdpMedia& offer, SdpMedia& answer)
{
  DBG("getSdpAnswer() for media index %u\n", index);
  sdp_media_index = index;
  getSdp(answer);
  answer.transport = offer.transport;
  offer.calcAnswer(payload_provider,answer);
}

int AmRtpStream::init(const AmSdp& local,
		      const AmSdp& remote,
                      bool force_passive_mode)
{
  int remote_media_index = sdp_media_index;

  if (!rtp_transport) {
    DBG("No RTP Transport prensent\n");
    return -1;
  }

  if((sdp_media_index < 0) ||
     ((unsigned)sdp_media_index >= local.media.size()) ||
     ((unsigned)sdp_media_index >= remote.media.size()) ) {

    bool fixed = false;

    if (sdp_media_index >= 0 && ((unsigned int)sdp_media_index < local.media.size()) &&
        remote.media.size() != local.media.size())
    {
      WARN("SDP negotiation mismatch - probably remote is violating 3264 §6 (jitsi?); "
           "trying to find matching stream\n");

      /* try to find remote stream that matches up mt/transport */
      for (std::vector<SdpMedia>::const_iterator it=remote.media.begin();
            it != remote.media.end(); it++)
      {
        if (it->type == MT_AUDIO && it->transport == local.media[sdp_media_index].transport) {
          remote_media_index = it-remote.media.begin();
          DBG("fixed remote media_index to %d (local %d)\n", remote_media_index, sdp_media_index);
          fixed  = true;
          break;
        }
      }
    }

    if (!fixed) {
      ERROR("Media index %i is invalid, either within local or remote SDP (or both)",sdp_media_index);
      return -1;
    }
  }

  if (hook)
    hook->initStream(local, remote, sdp_media_index);

  const SdpMedia& local_media = local.media[sdp_media_index];
  const SdpMedia& remote_media = remote.media[remote_media_index];

  DBG("initializing RTP stream (force_passive = %s, index = %u, %zd/%zd l/r payloads)\n",
      force_passive_mode? "true" : "false", sdp_media_index,
      local_media.payloads.size(), remote_media.payloads.size());

  payloads.clear();
  offered_payloads.clear();
  pl_map.clear();
  payloads.resize(local_media.payloads.size());

  int i=0;
  vector<Payload>::iterator p_it = payloads.begin();

  /* first pass on local SDP - fill pl_map with intersection of codecs */
  for (vector<SdpPayload>::const_iterator sdp_it = local_media.payloads.begin();
      sdp_it != local_media.payloads.end(); sdp_it++) {

    offered_payloads[sdp_it->payload_type] = sdp_it->payload_type;

    /* find internal payload type */
    int int_pt;
    if ((local_media.transport == TP_RTPAVP ||
        local_media.transport == TP_RTPSAVP ||
        local_media.transport == TP_RTPSAVPF) && sdp_it->payload_type < 20)
    {
      int_pt = sdp_it->payload_type;
    } else {
      int_pt = payload_provider->
      getDynPayload(sdp_it->encoding_name, sdp_it->clock_rate, sdp_it->encoding_param);
    }

    /* get payload format for type */
    amci_payload_t* a_pl = NULL;
    if(int_pt >= 0) 
      a_pl = payload_provider->payload(int_pt);

    if (a_pl == NULL) {
      /* ignore relay payloads...*/
      if (!relay_payloads.get(sdp_it->payload_type)) {
        DBG("No internal payload corresponding to type %s/%i (ignoring)\n",
              sdp_it->encoding_name.c_str(),
              sdp_it->clock_rate);
        /* and unknown payloads */
      }
      continue;
    }
    
    p_it->pt         = sdp_it->payload_type;
    p_it->name       = sdp_it->encoding_name;
    p_it->codec_id   = a_pl->codec_id;
    p_it->clock_rate = a_pl->sample_rate;
    p_it->advertised_clock_rate = sdp_it->clock_rate;
    p_it->sdp_format_parameters = sdp_it->sdp_format_parameters;

    pl_map[sdp_it->payload_type].index     = i;
    pl_map[sdp_it->payload_type].remote_pt = -1;
    

    ++p_it;
    ++i;
  }

  /* remove payloads which were not initialised (because of unknown payloads
     which are to be relayed) */
  if (p_it != payloads.end())
    payloads.erase(p_it, payloads.end());

  /* second pass on remote SDP - initialize payload IDs used by remote (remote_pt) */
  for (vector<SdpPayload>::const_iterator sdp_it = remote_media.payloads.begin(); sdp_it != remote_media.payloads.end(); sdp_it++) {
    /* TODO: match not only on encoding name
     * but also on parameters, if necessary
     * Some codecs define multiple payloads
     * with different encoding parameters */
    PayloadMappingTable::iterator pmt_it = pl_map.end();

    if (sdp_it->encoding_name.empty() || 
        ((local_media.transport == TP_RTPAVP ||
          local_media.transport == TP_RTPSAVP ||
          local_media.transport == TP_RTPSAVPF) && sdp_it->payload_type < 20))
    {
      /* must be a static payload */
      pmt_it = pl_map.find(sdp_it->payload_type);
    } else {
      for (p_it = payloads.begin(); p_it != payloads.end(); ++p_it)
      {
        if (!strcasecmp(p_it->name.c_str(),sdp_it->encoding_name.c_str()) && 
            (p_it->advertised_clock_rate == (unsigned int)sdp_it->clock_rate)) {
          pmt_it = pl_map.find(p_it->pt);
          break;
        }
      }
    }

    /* TODO: remove following code once proper 
     * payload matching is implemented.
     * initialize remote_pt if not already there */
    if(pmt_it != pl_map.end() && (pmt_it->second.remote_pt < 0)){
      pmt_it->second.remote_pt = sdp_it->payload_type;
    }
  }

  setPassiveMode(remote_media.dir == SdpMedia::DirActive || force_passive_mode);

  /* set remote address - media c-line having precedence over session c-line */
  if (remote.conn.address.empty() && remote_media.conn.address.empty()) {
    WARN("no c= line given globally or in m= section in remote SDP\n");
    return -1;
  }

  if (remote_media.conn.address.empty()) {
    rtp_transport->setRemoteRtpAddress(remote.conn.address, remote_media.port);
    rtp_transport->setRemoteRtcpAddress(remote.conn.address, remote_media.rtcp_address.getPort());
  } else {
    rtp_transport->setRemoteRtpAddress(remote_media.conn.address, remote_media.port);
    rtp_transport->setRemoteRtcpAddress(remote_media.conn.address, remote_media.rtcp_address.getPort());
  }

  if (local_media.payloads.empty()) {
    DBG("local_media.payloads.empty()\n");
    return -1;
  }

  remote_telephone_event_pt.reset(remote.telephoneEventPayload());
  if (remote_telephone_event_pt.get()) {
      DBG("remote party supports telephone events (pt=%i)\n",
	  remote_telephone_event_pt->payload_type);
  } else {
    DBG("remote party doesn't support telephone events\n");
  }

  local_telephone_event_pt.reset(local.telephoneEventPayload());

  if (local_media.recv) {
    resume();
  } else {
    pause();
  }

  SdpConnection conn = remote.conn.address.empty() ? remote_media.conn : remote.conn;

  if (local_media.send && !hold &&
      (remote_media.port != 0) &&
      (((conn.addrType == AT_V4) && (conn.address != "0.0.0.0")) ||
        ((conn.addrType == AT_V6) &&
          (conn.address != "0000:0000:0000:0000:0000:0000:0000:0000") &&
          (conn.address != "::/128") &&
          (conn.address != "::")))
      )
  {
    mute = false;
  } else {
    mute = true;
  }

  payload = getDefaultPT();
  if(payload < 0) {
    DBG("could not set a default payload\n");
    return -1;
  }

  DBG("default payload selected = %i\n",payload);
  last_payload = payload;


  active = false; // mark as nothing received yet

  /* Attach this stream with the corresponding rtp/rtcp transport */
  if (rtp_transport)
    rtp_transport->addStream(this);

  return 0;
}

void AmRtpStream::setReceiving(bool r) {
  DBG("RTP stream instance [%p] set receiving=%s\n", this, r?"true":"false");
  receiving = r;
}

void AmRtpStream::pause()
{
  DBG("RTP Stream instance [%p] pausing (receiving=false)\n", this);
  receiving = false;
}

void AmRtpStream::resume()
{
  DBG("RTP Stream instance [%p] resuming (receiving=true, clearing biffers/TS/TO)\n", this);
  clearRTPTimeout();
  receive_mut.lock();
  mem.clear();
  receive_buf.clear();
  receive_mut.unlock();
  receiving = true;
}

void AmRtpStream::setOnHold(bool on_hold) {
  hold = on_hold;
}

bool AmRtpStream::getOnHold() {
  return hold;
}

void AmRtpStream::recvDtmfPacket(AmRtpPacket* p) {
  if (p->payload == getLocalTelephoneEventPT()) {
    dtmf_payload_t* dpl = (dtmf_payload_t*)p->getData();

    DBG("DTMF: event=%i; e=%i; r=%i; volume=%i; duration=%i; ts=%u session = [%p]\n",
	dpl->event,dpl->e,dpl->r,dpl->volume,ntohs(dpl->duration),p->timestamp, session);
    if (session) 
      session->postDtmfEvent(new AmRtpDtmfEvent(dpl, getLocalTelephoneEventRate(), p->timestamp));
  }
}

void AmRtpStream::bufferPacket(AmRtpPacket* p, sockaddr_storage& recv_addr)
{
  // call hooks for received packet
  if (hook) hook->receivedPacket(p);

  if (!receiving) {

    if (passive) {
      handleSymmetricRtp(&recv_addr,false);
    }

    if (force_receive_dtmf) {
      recvDtmfPacket(p);
    }

    mem.freePacket(p);
    return;
  }

  if (relay_enabled) {
    if (force_receive_dtmf) {
      recvDtmfPacket(p);
    }

    // Relay DTMF packets if current audio payload
    // is also relayed.
    // Else, check whether or not we should relay this payload

    bool is_dtmf_packet = (p->payload == getLocalTelephoneEventPT()); 

      if (relay_raw || (is_dtmf_packet && !active) ||
	  relay_payloads.get(p->payload)) {

      if(active){
	DBG("switching to relay-mode\t(ts=%u;stream=%p)\n",
	    p->timestamp,this);
	active = false;
      }
      handleSymmetricRtp(&recv_addr,false);

      if (NULL != relay_stream &&
	  (!(relay_filter_dtmf && is_dtmf_packet))) {
        relay_stream->relay(p);
      }

      mem.freePacket(p);
      return;
    }
  }

  receive_mut.lock();
  // NOTE: useless, as DTMF events are pushed into 'rtp_ev_qu'
  // free packet on double packet for TS received
  // if(p->payload == getLocalTelephoneEventPT()) {
  //   if (receive_buf.find(p->timestamp) != receive_buf.end()) {
  //     mem.freePacket(receive_buf[p->timestamp]);
  //   }
  // }  

    if(p->payload == getLocalTelephoneEventPT()) {
      rtp_ev_qu.push(p);
    } else {
      if(!receive_buf.insert(ReceiveBuffer::value_type(p->timestamp,p)).second) {
	// insert failed
	mem.freePacket(p);
      }
    }

  receive_mut.unlock();
}

void AmRtpStream::clearRTPTimeout() {
  last_recv_time = AmAppTimer::instance()->unix_clock.get();
}

int AmRtpStream::getDefaultPT()
{
  for(PayloadCollection::iterator it = payloads.begin();
      it != payloads.end(); ++it){

    // skip telephone-events payload
    if(it->codec_id == CODEC_TELEPHONE_EVENT)
      continue;

    // skip incompatible payloads
    PayloadMappingTable::iterator pl_it = pl_map.find(it->pt);
    if ((pl_it == pl_map.end()) || (pl_it->second.remote_pt < 0))
      continue;

    return it->pt;
  }

  return -1;
}

int AmRtpStream::nextPacket(AmRtpPacket*& p)
{
  if (!receiving && !passive)
    return RTP_EMPTY;

  receive_mut.lock();

  if(!rtp_ev_qu.empty()) {
    // first return RTP telephone event payloads
    p = rtp_ev_qu.front();
    rtp_ev_qu.pop();
    receive_mut.unlock();
    return 1;
  }

  if(receive_buf.empty()){
    receive_mut.unlock();
    return RTP_EMPTY;
  }

  p = receive_buf.begin()->second;
  receive_buf.erase(receive_buf.begin());

  receive_mut.unlock();

  return 1;
}

AmRtpPacket *AmRtpStream::reuseBufferedPacket()
{
  AmRtpPacket *p = NULL;

  receive_mut.lock();
  if(!receive_buf.empty()) {
    p = receive_buf.begin()->second;
    receive_buf.erase(receive_buf.begin());
  }
  receive_mut.unlock();
  return p;
}

void AmRtpStream::recvRtpPacket(unsigned char* buffer, int size, sockaddr_storage& recv_addr)
{ 
  AmRtpPacket* p = mem.newPacket();
  if (!p) p = reuseBufferedPacket();
  if (!p) {
    DBG("out of buffers for RTP packets (stream [%p])\n",
	this);
    return;
  }
  
  int parse_res = 0;

  p->compile_raw(buffer, size);

  clearRTPTimeout();
    
  if(!relay_raw) {
    parse_res = p->parse();
    if (parse_res < 0) {
      DBG("error while parsing RTP packet.\n");
      mem.freePacket(p);	  
      return;
    }
  }

  bufferPacket(p, recv_addr);
}

void AmRtpStream::recvRtcpPacket(unsigned char* buffer, int recved_bytes, sockaddr_storage& recv_addr)
{
  static const cstring empty;

  if(!relay_enabled || !relay_stream)
    return;

  if(!relay_stream->rtp_transport)
    return;

  // clear RTP timer
  clearRTPTimeout();

  if (passive_rtcp)
    handleSymmetricRtp(&recv_addr,true);

  int err = relay_stream->rtp_transport->sendRtcp(buffer, recved_bytes);

  if(err < 0){
    ERROR("could not relay RTCP packet: %s\n",strerror(errno));
    return;
  }
}

void AmRtpStream::relay(AmRtpPacket* p)
{
  if (!rtp_transport)
    return;

  // not yet initialized
  // or muted/on-hold
  if (!rtp_transport->getLocalRtpPort() || mute || hold) 
    return;

  generateDtmf(p->timestamp);

  if(session && !session->onBeforeRTPRelay(p,rtp_transport->getRemoteRtpSocket()))
    return;

  rtp_hdr_t* hdr = (rtp_hdr_t*)p->getBuffer();
  if (!relay_raw && !relay_transparent_seqno)
    hdr->seq = htons(sequence++);
  if (!relay_raw && !relay_transparent_ssrc)
    hdr->ssrc = htonl(l_ssrc);

  if (hook) hook->relayedPacket(p);

  if(rtp_transport->sendRtp(p) == 0){
    if(session) session->onAfterRTPRelay(p, rtp_transport->getRemoteRtpSocket());
  }
}

void AmRtpStream::setRemoteSSRC(unsigned int ssrc)
{
  r_ssrc = ssrc;
}

unsigned int AmRtpStream::getRemoteSSRC()
{
  return r_ssrc;
}

int AmRtpStream::getLocalTelephoneEventRate()
{
  if (local_telephone_event_pt.get())
    return local_telephone_event_pt->clock_rate;
  return 0;
}

int AmRtpStream::getLocalTelephoneEventPT()
{
  if(local_telephone_event_pt.get())
    return local_telephone_event_pt->payload_type;
  return -1;
}

void AmRtpStream::setPayloadProvider(AmPayloadProvider* pl_prov)
{
  payload_provider = pl_prov;
}

void AmRtpStream::sendDtmf(int event, unsigned int duration_ms) {
  dtmf_sender.queueEvent(event,duration_ms,getLocalTelephoneEventRate());
}

void AmRtpStream::setRelayStream(AmRtpStream* stream) {
  relay_stream = stream;
  DBG("set relay stream [%p] for RTP instance [%p]\n",
      stream, this);
}

void AmRtpStream::setRelayPayloads(const PayloadMask &_relay_payloads)
{
  relay_payloads = _relay_payloads;
}

void AmRtpStream::enableRtpRelay() {
  DBG("enabled RTP relay for RTP stream instance [%p]\n", this);
  relay_enabled = true;
}

void AmRtpStream::disableRtpRelay() {
  DBG("disabled RTP relay for RTP stream instance [%p]\n", this);
  relay_enabled = false;
}

void AmRtpStream::enableRawRelay()
{
  DBG("enabled RAW relay for RTP stream instance [%p]\n", this);
  relay_raw = true;
}

void AmRtpStream::disableRawRelay()
{
  DBG("disabled RAW relay for RTP stream instance [%p]\n", this);
  relay_raw = false;
}

bool AmRtpStream::isRawRelayed() {
  return relay_raw;
}

void AmRtpStream::setRtpRelayTransparentSeqno(bool transparent) {
  DBG("%sabled RTP relay transparent seqno for RTP stream instance [%p]\n",
      transparent ? "en":"dis", this);
  relay_transparent_seqno = transparent;
}

void AmRtpStream::setRtpRelayTransparentSSRC(bool transparent) {
  DBG("%sabled RTP relay transparent SSRC for RTP stream instance [%p]\n",
      transparent ? "en":"dis", this);
  relay_transparent_ssrc = transparent;
}

void AmRtpStream::setRtpRelayFilterRtpDtmf(bool filter) {
  DBG("%sabled RTP relay filtering of RTP DTMF (2833 / 4733) for RTP stream instance [%p]\n",
      filter ? "en":"dis", this);
  relay_filter_dtmf = filter;
}

void AmRtpStream::stopReceiving()
{
  if (rtp_keepalive_freq)
    AmAppTimer::instance()->removeTimer(&rtp_keepalive_timer);

  if (rtp_timeout)
    AmAppTimer::instance()->removeTimer(&rtp_timer);

  bool onhold = getOnHold();

  if (rtp_transport) {
    DBG("Remove stream [%p] from RTP transport, local: <%d>, remote: <%d>, on hold <%c>\n",
        this, getLocalRtpPort(), getRemoteRtpPort(),
        onhold ? 't' : 'f');

    rtp_transport->removeStream(this);
  }
}

void AmRtpStream::resumeReceiving()
{
  if (rtp_transport){
    DBG("add/resume stream [%p] into RTP transport\n",this);
    rtp_transport->addStream(this);
  }

  if (rtp_keepalive_freq)
    AmAppTimer::instance()->setTimer(&rtp_keepalive_timer,rtp_keepalive_freq);

  if (rtp_timeout)
    AmAppTimer::instance()->setTimer(&rtp_timer,rtp_timeout);
}


void AmRtpStream::changeSession(AmSession *_s)
{
  session = _s;
  if(!_s) {
    // we assume the stream has already been removed from the transport...
    rtp_transport = NULL;

    rtp_keepalive_freq = 0;
    rtp_timeout = 0;
  }
  else {
    // TODO:
    // - create transports
    // - link stream to transport
    // - etc...

    // RTP Keepalive
    rtp_keepalive_freq = session->rtp_keepalive_freq;

    // RTP Timeout
    rtp_timeout = session->rtp_timeout;
  }
}

string AmRtpStream::getPayloadName(int payload_type)
{
  for(PayloadCollection::iterator it = payloads.begin();
      it != payloads.end(); ++it){

    if (it->pt == payload_type) return it->name;
  }

  return string("");
}

PacketMem::PacketMem()
  : cur_idx(0), n_used(0)
{
  memset(used, 0, sizeof(used));
}

inline AmRtpPacket* PacketMem::newPacket() 
{
  if(n_used >= MAX_PACKETS)
    return NULL; // full

  while(used[cur_idx])
    cur_idx = (cur_idx + 1) & MAX_PACKETS_MASK;

  used[cur_idx] = true;
  n_used++;

  AmRtpPacket* p = &packets[cur_idx];
  cur_idx = (cur_idx + 1) & MAX_PACKETS_MASK;

  return p;
}

inline void PacketMem::freePacket(AmRtpPacket* p) 
{
  if (!p)  return;

  int idx = p-packets;
  assert(idx >= 0);
  assert(idx < MAX_PACKETS);

  if(!used[idx]) {
    ERROR("freePacket() double free: n_used = %d, idx = %d",n_used,idx);
    return;
  }

  used[p-packets] = false;
  n_used--;
}

inline void PacketMem::clear() 
{
  memset(used, 0, sizeof(used));
  n_used = cur_idx = 0;
}

void AmRtpStream::setRtpTransport(AmRtpTransport* rtp_transport)
{
  this->rtp_transport = rtp_transport;
}

AmRtpTransport* AmRtpStream::getRtpTransport()
{
  return this->rtp_transport;
}

void AmRtpStream::debug(std::ostream &out, const char *line_prefix)
{
#define BOOL_STR(b) ((b) ? "yes" : "no")
  if (!rtp_transport)
    return;

  bool rtcp_mux = rtp_transport->isRtcpMux();

  if(rtp_transport->getLocalRtpPort()) {
    out << line_prefix << "<" << getLocalRtpPort() << "> <-> <"
      << getRemoteAddress() << ":" << getRemoteRtpPort() << ">" << std::endl;
  } else {
    out << line_prefix << "<unbound> <-> <"
      << getRemoteAddress().c_str() << ':' << getLocalRtpPort() <<  ">" << std::endl;
  }

  if (relay_stream) {
    out << line_prefix << "internal relay to stream " << relay_stream << " (local port "
      << relay_stream->getLocalRtpPort() << ")" << std::endl;
  }
  else out << line_prefix << "no relay" << std::endl;

  u_int64_t now = AmAppTimer::instance()->unix_clock.get();
  u_int64_t diff = now - last_recv_time;

  out << line_prefix << "mute: " << BOOL_STR(mute) << std::endl
    << line_prefix << "hold: " << BOOL_STR(hold) << std::endl
    << line_prefix << "receiving: " << BOOL_STR(receiving) << std::endl
    << line_prefix << "last received packet: " << diff << "s ago" << std::endl
    << line_prefix << "passive: " << BOOL_STR(passive) << std::endl
    << line_prefix << "passive RTCP: " << BOOL_STR(passive_rtcp) << std::endl
    << line_prefix << "rtcp_mux: " << BOOL_STR(rtcp_mux) << std::endl
    << line_prefix << "RTP timeout: " << rtp_timeout << std::endl
    << line_prefix << "RTP keepalive freq: " << rtp_keepalive_freq << std::endl
    << line_prefix << "local RTCP port: " << getLocalRtcpPort() << std::endl;


#undef BOOL_STR
}

void AmRtpStream::onKeepAliveTimeout()
{
  ping();

  AmAppTimer::instance()->setTimer(&rtp_keepalive_timer,rtp_keepalive_freq);
}

void AmRtpStream::onRtpTimeout()
{

  if (!session)
    return;

  if (!rtp_transport || !rtp_transport->getLocalRtpPort() || hold || !receiving) {
    AmAppTimer::instance()->setTimer(&rtp_timer,rtp_timeout);
    return;
  }

  u_int64_t now = AmAppTimer::instance()->unix_clock.get();
  u_int64_t diff;

  receive_mut.lock();

  diff = now - last_recv_time;

  if ((diff > 0) && ((unsigned int)diff > rtp_timeout)) {
   ERROR("RTP Timeout detected. Last packet was received "
     "%i seconds ago [%p]\n",(unsigned int)diff, this);

   receive_mut.unlock();

   session->postEvent(new AmRtpTimeoutEvent());
  } else {
    receive_mut.unlock();
    AmAppTimer::instance()->setTimer(&rtp_timer,rtp_timeout);
  }
}

AmRtpStream::Hook *AmRtpStream::setHook(Hook *h)
{
  Hook *old = hook;
  hook = h;
  return old;
}
