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

  rtp_socket = new AmRtpUdpSocket(this, interface, ip, port);
  if (rtcp) {
    rtcp_socket = new AmRtpUdpSocket(this, interface, ip,
                                      rtp_socket->getLocalPort()+1);
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
