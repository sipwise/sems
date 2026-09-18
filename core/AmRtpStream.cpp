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
#include "AmArg.h"

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

  if (AmConfig::RtcpSendInterval)
    update_sender_stats(rp);

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
    rtcp_first_report(true),
    rtcp_prev_tx_pkt(0),
    rtcp_prev_rx_pkt(0),
    rtcp_last_tx_rtp_ts(0),
    rtcp_own_media(false),
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
    rtp_timer(this),
    rtcp_report_timer(this)
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

  if (AmConfig::RtcpSendInterval)
    AmAppTimer::instance()->removeTimer(&rtcp_report_timer);

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

  /* init prepared RTCP reports (SR/RR + SDES CNAME) for this stream.
     CNAME is limited to INET6_ADDRSTRLEN bytes (see RtcpSdesData). */
  {
    string cname;
    char host[256];
    if (gethostname(host, sizeof(host)) == 0) {
      host[sizeof(host) - 1] = '\0';
      cname = host;
    }
    if (cname.empty() || cname.size() > INET6_ADDRSTRLEN) {
      char b[16];
      snprintf(b, sizeof(b), "%08x", l_ssrc);
      cname = b;
    }
    rtcp_reports.init(l_ssrc, cname);
    r_ssrc_i = false;
    rtcp_first_report = true;
    rtcp_prev_tx_pkt = 0;
    rtcp_prev_rx_pkt = 0;
    rtcp_last_tx_rtp_ts = 0;
    rtcp_own_media = false;

    DBG("stream [%p] RTCP prepared: l_ssrc 0x%08x, CNAME '%s', "
        "rtcp_send_interval %u, rtcp_mode %s",
        this, l_ssrc, cname.c_str(), AmConfig::RtcpSendInterval,
        AmConfig::RtcpMode == AmConfig::RtcpGenerateMode
          ? "generate"
          : (AmConfig::RtcpMode == AmConfig::RtcpPassthruMode ? "passthru" : "auto"));
  }

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

    if (session) {
      AmDtmfEvent * dtmf_ptr = new AmRtpDtmfEvent(dpl, getLocalTelephoneEventRate(), p->timestamp);
      if (!session->postDtmfEvent(dtmf_ptr))
      {
        WARN("Unable to post DTMF event. Release it.\n");
        delete dtmf_ptr;
      }
    }
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

    if (AmConfig::RtcpSendInterval) {
      gettimeofday(&p->recv_time, NULL);
      update_receiver_stats(*p);
    }
  }

  bufferPacket(p, recv_addr);
}

void AmRtpStream::recvRtcpPacket(unsigned char* buffer, int recved_bytes, sockaddr_storage& recv_addr)
{
  // Parse compound RTCP (SR/RR/SDES), update per-stream statistics.
  // NOTE: AmRtpTransport::recvRtcp() already demuxed this packet to us by SSRC.
  // Incoming RTCP is always parsed: rtcp_send_interval only controls
  // generation of our own reports, not accounting of the remote ones.
  struct timeval recv_time;
  gettimeofday(&recv_time, NULL);
  rtcp_parse_update_stats(buffer, recved_bytes, recv_time, rtp_stats);

  if (AmConfig::RtcpSendInterval && AmConfig::RtcpMode == AmConfig::RtcpGenerateMode)
    // own reports are generated for this stream: don't relay incoming RTCP,
    // to keep a single source of reports towards the remote side
    return;

  if(!relay_enabled || !relay_stream)
    return;

  if(!relay_stream->rtp_transport)
    return;

  // clear RTP timer
  clearRTPTimeout();

  if (passive_rtcp)
    handleSymmetricRtp(&recv_addr,true);

  if (relay_stream->getOnHold())
    // our media towards the destination leg is held: the leg no longer
    // receives the original stream, so reports describing it are not
    // forwarded; the held leg is served by our own reports instead
    // (see rtcp_generate_enabled())
    return;

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
    if (!relay_raw && AmConfig::RtcpSendInterval)
      update_sender_stats(*p);
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

  if (AmConfig::RtcpSendInterval)
    AmAppTimer::instance()->removeTimer(&rtcp_report_timer);

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

  if (AmConfig::RtcpSendInterval)
    AmAppTimer::instance()->setTimer(&rtcp_report_timer,
                                     rtcp_report_interval_sec());
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
    << line_prefix << "local RTCP port: " << getLocalRtcpPort() << std::endl
    << line_prefix << "RTCP SR sent/recv: " << rtp_stats.rtcp_sr_sent << "/" << rtp_stats.rtcp_sr_recv << std::endl
    << line_prefix << "RTCP RR sent/recv: " << rtp_stats.rtcp_rr_sent << "/" << rtp_stats.rtcp_rr_recv << std::endl
    << line_prefix << "RTCP own tx pkt/bytes: " << rtp_stats.tx.pkt << "/" << rtp_stats.tx.bytes << std::endl
    << line_prefix << "RTCP tx loss: " << rtp_stats.tx.loss << std::endl
    << line_prefix << "RTCP relay tx pkt/bytes: " << rtp_stats.relay_tx_pkt << "/" << rtp_stats.relay_tx_bytes << std::endl
    << line_prefix << "RTCP own media active: " << BOOL_STR(rtcp_own_media) << std::endl;


#undef BOOL_STR
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////
// RTCP support

double AmRtpStream::rtcp_report_interval_sec()
{
  // RFC 3550, section 6.2: to avoid bursts and to prevent synchronization
  // of reporting sources, each reporting interval is scaled by a random
  // factor of [0.5 .. 1.5]. The very first interval is additionally
  // halved, since we know nothing about other session members.
  double interval = AmConfig::RtcpSendInterval;
  if (rtcp_first_report)
    interval /= 2.0;

  return interval * (5000.0 + (get_random() % 10001)) / 10000.0;
}

bool AmRtpStream::rtcp_generate_enabled()
{
  if (!AmConfig::RtcpSendInterval)
    return false;

  switch (AmConfig::RtcpMode) {
  case AmConfig::RtcpGenerateMode:
    return true;
  case AmConfig::RtcpPassthruMode:
    return false;
  default:
    // auto: on a relayed stream the relayed reports cover the original
    // sources; we report on our own SSRC while SEMS supplies the media
    // (MoH, announcement, relay with SSRC rewrite), and on a stream held
    // by us: once its media is muted the transparent chain is broken by
    // us, so SEMS is the remote side's media peer and reporting source
    // (RFC 3550 sec. 6.1: a participant that keeps receiving RTP must
    // keep sending RTCP).
    if (!(relay_enabled && relay_stream))
      return true;
    return rtcp_own_media || hold;
  }
}

void AmRtpStream::on_rtcp_timeout()
{
  // reports are driven by our own timer, independent of the media pump:
  // a participant keeps reporting while it is in the session, even when
  // its media is muted or held (RFC 3550 sec. 6.1)
  if (rtcp_generate_enabled()) {
    rtcp_first_report = false;
    rtcp_send_report();
  }

  if (AmConfig::RtcpSendInterval)
    AmAppTimer::instance()->setTimer(&rtcp_report_timer,
                                     rtcp_report_interval_sec());
}

void AmRtpStream::rtcp_send_report()
{
  unsigned char buf[RTCP_REPORT_MAX_LEN];
  unsigned int  len = 0;
  struct timeval now;

  // no hold check here: RTCP is not part of the media plane, a held
  // stream keeps sending receiver reports while it receives RTP
  if (!rtp_transport)
    return;

  if (!rtp_transport->getLocalRtpPort() || !rtp_transport->getRemoteRtpPort())
    return;

  // no RTCP destination advertised (rejected stream, m=audio 0): nothing to send to
  if (!rtp_transport->getRemoteRtcpPort())
    return;

  gettimeofday(&now, nullptr);

  rtp_stats.lock();

  unsigned long rx_pkt = 0;
  for (const auto& rx_it : rtp_stats.rx)
    rx_pkt += rx_it.second.pkt;

  bool tx_changed = rtp_stats.tx.pkt != rtcp_prev_tx_pkt;
  // no traffic-based suppression here: RFC 3550 sec. 6.1 lets a
  // participant cease sending RTCP only if it expects no further RTP,
  // i.e. once it leaves the session. While our SIP dialog is up we are a
  // session member (and, for IMS, under peer media-plane RTCP monitoring,
  // 3GPP TS 26.114), so we keep reporting on every reporting interval
  // even when the stream is silent in both directions. Streams that
  // really left the session are destroyed, which disarms the report
  // timer (see stopReceiving/~AmRtpStream).
  rtcp_prev_tx_pkt = rtp_stats.tx.pkt;
  rtcp_prev_rx_pkt = rx_pkt;

  // we are a sender only while our own-SSRC counters are growing; once
  // sending stops (held or finished announcement) we are no longer a
  // sender, so reports degrade to RR without stale sender info.
  // on a relayed stream in auto mode a stable tx counter also means our
  // SSRC went idle: generation stops with the last report.
  bool own_sender = tx_changed;
  if (!tx_changed && relay_stream
      && AmConfig::RtcpMode == AmConfig::RtcpAutoMode) {
    rtcp_own_media = false;
  }

  const void* report;
  if (own_sender) {
    if (rtp_stats.current_rx && rtp_stats.current_rx->pkt
        && !(relay_stream && AmConfig::RtcpMode == AmConfig::RtcpAutoMode)) {
      // SR with RR data
      fill_sender_report(rtcp_reports.sr.sr.sender, now, rtcp_last_tx_rtp_ts);
      fill_receiver_report(rtcp_reports.sr.sr.receiver, now);
      report = &rtcp_reports.sr;
      len    = rtcp_reports.sr.packet_length;
    } else {
      // SR without RR data
      fill_sender_report(rtcp_reports.sr_empty.sr.sender, now, rtcp_last_tx_rtp_ts);
      report = &rtcp_reports.sr_empty;
      len    = rtcp_reports.sr_empty.packet_length;
    }
  } else { // no data sent: send RR with receiver info of the current source
    if (rtp_stats.current_rx && rtp_stats.current_rx->pkt)
      fill_receiver_report(rtcp_reports.rr.rr.receiver, now);
    report = &rtcp_reports.rr;
    len    = rtcp_reports.rr.packet_length;
  }

  if (len > sizeof(buf))
    len = 0;
  else
    memcpy(buf, report, len);

  rtp_stats.unlock();

  if (!len)
    return;

  // send outside of rtp_stats lock: the report is already copied,
  // the syscall must not block the RTP receive thread
  if (rtp_transport->sendRtcp(buf, len) < 0) {
    DBG("stream [%p] failed to send RTCP report: %s", this, strerror(errno));
    return;
  }

  DBG("stream [%p] sent RTCP %s+SDES report (%u bytes): tx pkt %llu, rx pkt %lu",
      this,
      (report == &rtcp_reports.sr || report == &rtcp_reports.sr_empty) ? "SR" : "RR",
      len, rtcp_prev_tx_pkt, rtcp_prev_rx_pkt);
}

void AmRtpStream::update_sender_stats(const AmRtpPacket &p)
{
  lock_guard<AmMutex> l(rtp_stats);

  rtp_stats.tx.pkt++;
  rtp_stats.tx.bytes += p.getDataSize();
  rtcp_last_tx_rtp_ts = p.timestamp;
  rtcp_own_media = true;
}

void AmRtpStream::update_relay_tx_stats(const AmRtpPacket &p)
{
  lock_guard<AmMutex> l(rtp_stats);

  rtp_stats.relay_tx_pkt++;
  rtp_stats.relay_tx_bytes += p.getDataSize();
}

void AmRtpStream::fill_sender_report(RtcpSenderReportHeader &s, struct timeval &now, unsigned int user_ts)
{
  uint64_t i;

  rtp_stats.rtcp_sr_sent++;

  s.sender_pcount = htonl(rtp_stats.tx.pkt);
  s.sender_bcount = htonl(rtp_stats.tx.bytes);
  s.rtp_ts        = htonl(user_ts);

  i = now.tv_usec;
  i <<= 32;
  i /= 1000000;
  s.ntp_frac = htonl(i);

  i = now.tv_sec;
  i += NTP_TIME_OFFSET;
  s.ntp_sec = htonl(i);
}

void AmRtpStream::init_receiver_info(const AmRtpPacket &p)
{
  r_ssrc = p.ssrc;
  rtcp_reports.update(r_ssrc);
  r_ssrc_i = true;

  rtp_stats.probation = MIN_SEQUENTIAL;
  rtp_stats.init_seq(p.ssrc, p.sequence);
}

void AmRtpStream::update_receiver_stats(const AmRtpPacket &p)
{
  lock_guard<AmMutex> l(rtp_stats);

  if ((!r_ssrc_i) || (p.ssrc != r_ssrc)) {
    if (rtp_stats.current_rx)
      rtp_stats.current_rx->loss += rtp_stats.total_lost;
    init_receiver_info(p);
  }

  if (rtp_stats.current_rx) {
    rtp_stats.current_rx->pkt++;
    rtp_stats.current_rx->bytes += p.getDataSize();
  }

  if (!rtp_stats.update_seq(p.ssrc, p.sequence)) {
    /* skip jitter measurement
       for duplicated/reordered/unexpected sequence packets */
    return;
  }

  // https://tools.ietf.org/html/rfc3550#appendix-A.8
  uint64_t recv_time_msec = p.recv_time.tv_sec * 1000 + p.recv_time.tv_usec / 1000;
  int      transit        = (recv_time_msec << 3) - p.timestamp;
  if (rtp_stats.transit) {
    int d = rtp_stats.transit - transit;
    if (d < 0)
      d = -d;
    if (rtp_stats.current_rx)
      rtp_stats.current_rx->rtcp_jitter += d - ((rtp_stats.current_rx->rtcp_jitter + 8) >> 4);
  }
  rtp_stats.transit = transit;

  if (timerisset(&rtp_stats.rx_recv_time)) {
    timeval diff;
    timersub(&p.recv_time, &rtp_stats.rx_recv_time, &diff);
    if (rtp_stats.current_rx) {
      MathStat<long> &rx_delta = rtp_stats.current_rx->rx_delta;
      rx_delta.update((diff.tv_sec * 1000000) + diff.tv_usec);
      if (rx_delta.n && (0 == rx_delta.n % 250)) {
        // update jitter every 250 packets (5 seconds)
        rtp_stats.current_rx->jitter_usec.update(rx_delta.sd());
      }
    }
  }
  rtp_stats.rx_recv_time = p.recv_time;
}

void AmRtpStream::fill_receiver_report(RtcpReceiverReportHeader &r, struct timeval &now)
{
  struct timeval delay;

  rtp_stats.rtcp_rr_sent++;

  rtp_stats.update_lost();

  r.total_lost_2 = (rtp_stats.total_lost >> 16) & 0xff;
  r.total_lost_1 = (rtp_stats.total_lost >> 8) & 0xff;
  r.total_lost_0 = rtp_stats.total_lost & 0xff;

  r.fract_lost = rtp_stats.fraction_lost;

  r.last_seq = ((rtp_stats.cycles << 16) | (rtp_stats.max_seq & 0xffff));
  r.last_seq = htonl(r.last_seq);

  if (rtp_stats.sr_lsr) {
    r.lsr = htonl(rtp_stats.sr_lsr);

    timersub(&now, &rtp_stats.sr_recv_time, &delay);
    r.dlsr = (delay.tv_sec << 16);
    r.dlsr |= (uint16_t)(delay.tv_usec * 65536 / 1e6);
    r.dlsr = htonl(r.dlsr);
  } else {
    r.lsr  = 0;
    r.dlsr = 0;
  }

  if (rtp_stats.current_rx) {
    uint32_t jitter = rtp_stats.current_rx->rtcp_jitter >> 4;
    r.jitter = htonl(jitter);

    // update stats
    rtp_stats.current_rx->rtcp_jitter_usec.update(jitter);
  } else {
    r.jitter = 0;
  }
}

template <typename T>
static void rtcp_math_stat_to_arg(AmArg& dst, const MathStat<T>& s)
{
  dst["n"]    = (int)s.n;
  dst["min"]  = (long long)s.min;
  dst["max"]  = (long long)s.max;
  dst["last"] = (long long)s.last;
  dst["mean"] = (double)s.mean;
  dst["sd"]   = (double)s.sd();
}

static void rtcp_unidir_stat_to_arg(AmArg& dst, const RtcpUnidirectionalStat& s)
{
  dst["pkt"]     = (long long)s.pkt;
  dst["bytes"]   = (long long)s.bytes;
  dst["loss"]    = (long long)s.loss;
  dst["reorder"] = (long long)s.reorder;
  dst["dup"]     = (long long)s.dup;

  rtcp_math_stat_to_arg(dst["rx_delta"], s.rx_delta);
  rtcp_math_stat_to_arg(dst["jitter_usec"], s.jitter_usec);
  rtcp_math_stat_to_arg(dst["rtcp_jitter_usec"], s.rtcp_jitter_usec);
}

void AmRtpStream::get_rtcp_stats(AmArg& dst)
{
  lock_guard<AmMutex> l(rtp_stats);

  dst["l_ssrc"] = (long long)l_ssrc;
  dst["r_ssrc"] = (long long)r_ssrc;

  dst["sr_sent"] = (long long)rtp_stats.rtcp_sr_sent;
  dst["sr_recv"] = (long long)rtp_stats.rtcp_sr_recv;
  dst["rr_sent"] = (long long)rtp_stats.rtcp_rr_sent;
  dst["rr_recv"] = (long long)rtp_stats.rtcp_rr_recv;

  dst["fraction_lost"] = (int)rtp_stats.fraction_lost;
  dst["total_lost"]    = (long long)rtp_stats.total_lost;

  AmArg& tx = dst["tx"]; // own SSRC only, not the relayed traffic
  tx["pkt"]   = (long long)rtp_stats.tx.pkt;
  tx["bytes"] = (long long)rtp_stats.tx.bytes;
  // packet loss reported by the remote side in its RR packets
  tx["loss"]  = (long long)rtp_stats.tx.loss;

  dst["relay_tx_pkt"]   = (long long)rtp_stats.relay_tx_pkt;
  dst["relay_tx_bytes"] = (long long)rtp_stats.relay_tx_bytes;

  rtcp_math_stat_to_arg(dst["rtt"], rtp_stats.rtt);
  rtcp_math_stat_to_arg(dst["remote_jitter"], rtp_stats.rtcp_remote_jitter);

  AmArg& rx = dst["rx"];
  for (const auto& rx_it : rtp_stats.rx) {
    AmArg item;
    item["ssrc"] = (long long)rx_it.first;
    rtcp_unidir_stat_to_arg(item, rx_it.second);
    rx.push(item);
  }
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
