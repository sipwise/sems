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

#include "AmRtpTransport.h"
#include "sip/msg_logger.h"
#include "AmSdp.h"

using std::set;

AmRtpTransport::AmRtpTransport(int interface, const string& ip,
                               bool rtcp, bool ice, unsigned int port)
  : logger(NULL),
    rtcp_mux(!rtcp),
    has_ice(ice)
{
  socket = new AmRtpSocketPair(this, interface, ip, rtcp, port);

  if (ice)
    initStun();
}

AmRtpTransport::~AmRtpTransport()
{
  rtp_streams.clear();

  delete socket;

  DBG("AmRtpTransport destructor finished");
}

/* Private methods */

void AmRtpTransport::addToReceiver()
{
  socket->addToReceiver();
}

void AmRtpTransport::removeFromReceiver()
{
  socket->removeFromReceiver();
}


/* \brief Get the corresponding rtp stream given the incoming RTP packet.
 * Demux following the next priorities:
 * - SSRC
 * - Payload Type
 * - RTP Header extension
 */
AmRtpStream* AmRtpTransport::getStream(rtp_hdr_t* hdr)
{
  uint32_t hdr_ssrc = ntohl(hdr->ssrc);

  for (set<AmRtpStream*>::iterator it = rtp_streams.begin();
       it!= rtp_streams.end(); ++it) {

    // Check if the remote ssrc was signaled
    uint32_t ssrc = (*it)->getRemoteSSRC();

    if (ssrc) {
      if (ssrc  == hdr_ssrc) {
        return (*it);
      }
    } else {
      /* Check if the payload type belongs to the stream.
       * If so, also set the remote SSRC into the stream.
       */
      if ((*it)->offered_payloads.find(hdr->pt) != (*it)->offered_payloads.end()) {
        (*it)->setRemoteSSRC(ntohl(hdr->ssrc));
        return (*it);
      }
    }
  }

  ERROR("NO Stream found with ssrc: %u", hdr_ssrc);
  return NULL;
}

/* \brief Get the corresponding rtp stream given the incoming RTCP packet.
 * Demux based on the SSRC (for SR and RR packets)
 */
AmRtpStream* AmRtpTransport::getStream(const uint32_t ssrc)
{
  for (set<AmRtpStream*>::iterator it = rtp_streams.begin();
       it!= rtp_streams.end(); ++it) {

    // Check if the remote ssrc was signaled
    uint32_t it_ssrc = (*it)->getRemoteSSRC();

    if (it_ssrc) {
      if (it_ssrc  == ssrc) {
        return (*it);
      }
    }
  }
  return NULL;
}

void AmRtpTransport::setRtcpMux() {
  rtcp_mux = true;
  socket->setRtcpMux();
}

int AmRtpTransport::getLocalRtpPort() const {
  return socket->getLocalRtpPort();
}

int AmRtpTransport::getLocalRtcpPort() const {
  return socket->getLocalRtcpPort();
}

int AmRtpTransport::getRemoteRtpPort() const {
  return socket->getRemoteRtpPort();
}

int AmRtpTransport::getRemoteRtcpPort() const {
  return socket->getRemoteRtcpPort();
}

const string AmRtpTransport::getRemoteAddress() {
  return socket->getRemoteAddress();
}

void AmRtpTransport::setRemoteRtpAddress(const string& addr,
                                                unsigned short port) {
  DBG("Setting RTP remote address from signalling");
  socket->setRemoteRtpAddress(addr, port);
}

void AmRtpTransport::setRemoteRtcpAddress(const string& addr,
                                                 unsigned short port) {
  DBG("Setting RTCP remote address from signalling");
  socket->setRemoteRtcpAddress(addr, port);
}

bool AmRtpTransport::hasStream(AmRtpStream* stream)
{
  return rtp_streams.find(stream) != rtp_streams.end();
}

void AmRtpTransport::addStream(AmRtpStream* stream)
{
  /* Motivation for removing temporarily from the event loop:
   *
   * The event_del() should block if the event's callback is running in another
   * thread. That way, we can be sure that event_del() has canceled the
   * callback (if the callback hadn't started running yet), or has waited
   * for the callback to finish.
   *
   */
  if (!hasStream(stream)) {
      removeFromReceiver();
      rtp_streams.insert(stream);
      addToReceiver();
      DBG("New Stream [%p] added to the RTP transport [%p]",stream, this);
  }

  if (stream->isRawRelayed())
    socket->setRawRelay();
}

void AmRtpTransport::removeStream(AmRtpStream* stream)
{
  /* Motivation for removing temporarily from the event loop:
   *
   * The event_del() should block if the event's callback is running in another
   * thread. That way, we can be sure that event_del() has canceled the
   * callback (if the callback hadn't started running yet), or has waited
   * for the callback to finish.
   *
   */
  if (rtp_streams.find(stream) != rtp_streams.end()) {
    removeFromReceiver();
    rtp_streams.erase(stream);
    addToReceiver();
    DBG("Deleted stream [%p] from the RTP transport [%p]",stream, this);
  }
}

sockaddr_storage* AmRtpTransport::getRemoteRtpSocket()
{
  return socket->getRemoteRtpSocket();
}

// Send RTP
int AmRtpTransport::sendRtp(AmRtpPacket* rp) {
  return socket->sendRtp(rp);
}

// Send RTCP
int AmRtpTransport::sendRtcp(unsigned char* buffer, int len) {
  return socket->sendRtcp(buffer, (size_t)len);
}

void AmRtpTransport::recvRtp(unsigned char* buffer, size_t len,
                                    sockaddr_storage& addr) {

  if (!rtp_streams.empty()) {

    // If holding a single RtpStream, don't check the SSRC
    if (rtp_streams.size() == 1) {
      std::set<AmRtpStream*>::iterator stream_it = rtp_streams.begin();
      (*stream_it)->recvRtpPacket(buffer, len, addr);
    }

    else {
      rtp_hdr_t* hdr = (rtp_hdr_t*)buffer;
      AmRtpStream* stream = getStream(hdr);

      if (stream)
        stream->recvRtpPacket(buffer, len, addr);
    }
  }
}

void AmRtpTransport::recvRtcp(unsigned char* buffer, size_t len,
                                    sockaddr_storage& addr) {

  if (!rtp_streams.empty()) {

    // If holding a single RtpStream, don't check the SSRC
    if (rtp_streams.size() == 1) {
      std::set<AmRtpStream*>::iterator stream_it = rtp_streams.begin();
      (*stream_it)->recvRtcpPacket(buffer, len, addr);
    }

    else {
      rtcp_hdr_t* hdr = (rtcp_hdr_t*)buffer;
      rtp_ssrc_t* ssrc = (rtp_ssrc_t*) (buffer + sizeof(rtcp_hdr_t));

      AmRtpStream* stream = getStream(ntohl(ssrc->value));

      if (stream)
        stream->recvRtcpPacket(buffer, len, addr);
    }
  }
}

void AmRtpTransport::recvUnknown(unsigned char* buffer, size_t len,
                                 sockaddr_storage& addr) {
  // Check whether the stream is being relayed raw
  if (rtp_streams.size() == 1) {
    std::set<AmRtpStream*>::iterator it = rtp_streams.begin();
    if ((*it)->isRawRelayed()) {
       (*it)->recvRtpPacket(buffer, len, addr);
    }
  }
}

// Description
void AmRtpTransport::getDescription(SdpMedia& m)
{
  m.port = getLocalRtpPort();

  if (rtcp_mux)
   m.addAttribute(SdpAttribute(string("rtcp-mux")));

  if (hasIce())
    getLocalIceDescription(m);
}

void AmRtpTransport::setLogger(const shared_ptr<msg_logger>& _logger)
{
  logger = _logger;

  socket->setLogger(logger);
}

void AmRtpTransport::getLocalIceDescription(SdpMedia& m)
{

  // Add ice-ufrag and ice-pwd media attributes
  m.ice_username = getLocalStunUsername();
  m.ice_password = getLocalStunPassword();

  socket->getLocalIceDescription(m);
}
