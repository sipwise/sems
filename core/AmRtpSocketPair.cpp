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

#include "AmRtpSocketPair.h"
#include "AmRtpTransport.h"
#include "AmStunServer.h"
#include "AmUtils.h"
#include "ice_utils.h"
#include "sip/msg_logger.h"

#include <stdexcept>
#include <sys/ioctl.h>


using std::set;

AmRtpSocketPair::AmRtpSocketPair(AmRtpTransport* transport, int interface,
                                 const string& ip, bool rtcp, unsigned int port)
  : logger(NULL),
    transport(transport),
    raw_relay(false),
    rtp_socket(NULL),
    rtcp_socket(NULL)
{
  /* Without rtcp-mux: allocate an even RTP port and then RTCP = RTP+1 (odd).
   * The cyclic RTP counter may hand us a port whose +1 neighbour is still in
   * use by a session that is being torn down (port released at OS level but
   * not yet out of the active-session range).  Retry the whole even/odd pair
   * up to RTP_RTCP_PAIR_RETRIES times before giving up.
   *
   * With rtcp-mux (rtcp == false): only one socket needed; no retry required. */
  static const int RTP_RTCP_PAIR_RETRIES = 5;

  for (int attempt = 0; ; ++attempt) {
    rtp_socket = new AmRtpUdpSocket(this, interface, ip, port);

    if (!rtcp) break;   /* rtcp-mux: single socket, done */

    unsigned int rtcp_port = (unsigned int)rtp_socket->getLocalPort() + 1;
    try {
      rtcp_socket = new AmRtpUdpSocket(this, interface, ip, rtcp_port);
      break;            /* RTP+RTCP pair successfully bound */
    } catch (const std::runtime_error& e) {
      /* RTCP port busy — release the RTP socket and try the next pair. */
      DBG("RTP+RTCP pair failed (RTP=%u, RTCP=%u busy): %s; attempt %d/%d\n",
          rtp_socket->getLocalPort(), rtcp_port, e.what(),
          attempt + 1, RTP_RTCP_PAIR_RETRIES);
      delete rtp_socket;
      rtp_socket = nullptr;
      port = 0;  /* draw a fresh even port from the pool on the next attempt */

      if (attempt >= RTP_RTCP_PAIR_RETRIES - 1)
        throw std::runtime_error("could not find a free RTP+RTCP port pair");
    }
  }

  ice_foundation = createIceFoundation();
}

AmRtpSocketPair::~AmRtpSocketPair()
{
  //Detach the sockets from the loop to avoid receiving data once destructed
  rtp_socket->removeFromReceiver();
  delete rtp_socket;

  if (rtcp_socket) {
    rtcp_socket->removeFromReceiver();
    delete rtcp_socket;
  }
}

/* Public methods */

void AmRtpSocketPair::addToReceiver()
{
  rtp_socket->addToReceiver();
  if (rtcp_socket)
    rtcp_socket->addToReceiver();
}

void AmRtpSocketPair::removeFromReceiver()
{
  rtp_socket->removeFromReceiver();
  if (rtcp_socket)
    rtcp_socket->removeFromReceiver();
}

void AmRtpSocketPair::setRtcpMux() {
  if (rtcp_socket) {
    rtcp_socket->removeFromReceiver();
    delete rtcp_socket;
    rtcp_socket = NULL;
  }
}

// Stream handling

int AmRtpSocketPair::getLocalRtpPort() const {
  return rtp_socket->getLocalPort();
}

int AmRtpSocketPair::getLocalRtcpPort() const {
  if (rtcp_socket)
    return rtcp_socket->getLocalPort();
  else
    return rtp_socket->getLocalPort();
}

int AmRtpSocketPair::getRemoteRtpPort() const {
  return rtp_socket->getRemotePort();
}

int AmRtpSocketPair::getRemoteRtcpPort() const {
  if (rtcp_socket)
    return rtcp_socket->getRemotePort();
  else
    return rtp_socket->getRemotePort();
}

const string AmRtpSocketPair::getRemoteAddress() {
  return rtp_socket->getRemoteAddress();
}

void AmRtpSocketPair::setRemoteRtpAddress(const string& addr,
                                          unsigned short port) {
  rtp_socket->setRemoteAddress(addr, port);
}

void AmRtpSocketPair::setRemoteRtcpAddress(const string& addr,
                                           unsigned short port) {
  if (rtcp_socket)
    rtcp_socket->setRemoteAddress(addr, port);
  else
    rtp_socket->setRemoteAddress(addr, port);
}

void AmRtpSocketPair::getLocalIceDescription(SdpMedia& m)
{
  AmStunServer::getLocalIceDescription(m, ice_foundation, rtp_socket, rtcp_socket);
}

sockaddr_storage* AmRtpSocketPair::getRemoteRtpSocket()
{
  return rtp_socket->getRemoteSocket();
}

// Send RTP
int AmRtpSocketPair::sendRtp(AmRtpPacket* rp) {
  return rtp_socket->sendRtp(rp);
}

// Send RTCP
int AmRtpSocketPair::sendRtcp(unsigned char* buffer, int len) {

  AmRtpSocket* socket = (rtcp_socket)? rtcp_socket : rtp_socket;

  return socket->sendRtcp(buffer, (size_t)len);
}

// Receive Data from underlaying sockets

void AmRtpSocketPair::recvData(AmRtpSocket* socket, unsigned char* buffer,
                              size_t len, sockaddr_storage& addr)
{

  if (raw_relay) {
    transport->recvUnknown(buffer, len, addr);
    return;
  }

  // Check if it is a STUN packet
  if (transport->hasIce()) {
    if (AmStunPacket::isPacketStun(buffer, len)){
      DBG("STUN packet received [%p] from %s:%u", this,
            am_inet_ntop(&addr).c_str(), am_get_port(&addr));
      transport->recvStun(buffer, len, socket, &addr);
      return;
    }
  }

  // We are not demuxing RTP and RTCP in the same port
  if (rtcp_socket) {
    // Everything coming from RTCP socket should be RTCP
    if (socket == rtcp_socket) {
      recvRtcp(socket, buffer, len, addr);
      return;
    }
    // Everything coming from RTP socket should be RTP
    else {
      recvRtp(socket, buffer, len, addr);
      return;
    }
  }

  // We are demuxing RTP and RTCP in the same port
  else {
    // Check if it is a RTCP packet
    if (AmRtpPacket::isPacketRtcp(buffer, len)) {
      recvRtcp(socket, buffer, len, addr);
      return;
    }

    // Should be a RTP packet
    recvRtp(socket, buffer, len, addr);
    return;
  }
}

void AmRtpSocketPair::recvRtp(AmRtpSocket* socket, unsigned char* buffer,
                              size_t len, sockaddr_storage& addr)
{
  transport->recvRtp(buffer, len, addr);
}

void AmRtpSocketPair::recvRtcp(AmRtpSocket* socket, unsigned char* buffer,
                               size_t len, sockaddr_storage& addr)
{
  transport->recvRtcp(buffer, len, addr);
}

void AmRtpSocketPair::setLogger(const shared_ptr<msg_logger>& _logger)
{

  rtp_socket->setLogger(logger);
  if (rtcp_socket)
    rtcp_socket->setLogger(logger);
}
