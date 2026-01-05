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

#ifndef _AmRtpSocketPair_h_
#define _AmRtpSocketPair_h_

#include "AmRtpUdpSocket.h"

#include <netinet/in.h>
#include <string>
#include <vector>
#include <set>
#include <memory>

using std::string;
using std::vector;
using std::set;
using std::shared_ptr;

class AmRtpTransport;
class AmSdp;
class SdpMedia;
class msg_logger;

class AmRtpSocketPair
{
  // RTP/RTCP sockets
  AmRtpSocket* rtp_socket;
  AmRtpSocket* rtcp_socket;

  shared_ptr<msg_logger> logger;

  AmRtpTransport* transport;

  bool raw_relay;
  string ice_foundation;

public:
  void addToReceiver();
  void removeFromReceiver();

  void setRtcpMux();

  int getLocalRtpPort() const;
  int getLocalRtcpPort() const;

  int getRemoteRtpPort() const;
  int getRemoteRtcpPort() const;

  void setRemoteRtpAddress(const string& addr, unsigned short port);
  void setRemoteRtcpAddress(const string& addr, unsigned short port);

  const string getRemoteAddress();

  sockaddr_storage* getRemoteRtpSocket();

  // ICE
  void getLocalIceDescription(SdpMedia& m);

  // Raw Relay
  inline void setRawRelay() { raw_relay = true; };

  // Send RTP
  int sendRtp(AmRtpPacket* rp);

  // Send RTCP
  int sendRtcp(unsigned char* buffer, int len);

  // Receive Data
  void recvData(AmRtpSocket* socket, unsigned char* buffer, size_t len,
                sockaddr_storage& addr);

  // Receive RTP
  void recvRtp(AmRtpSocket* socket, unsigned char* buffer, size_t size,
                sockaddr_storage& addr);

  // Receive RTCP
  void recvRtcp(AmRtpSocket* socket, unsigned char* buffer, size_t size,
                sockaddr_storage& addr);

  /** set destination for logging all received/sent RTP and RTCP packets */
  void setLogger(const shared_ptr<msg_logger>& _logger);

  AmRtpSocketPair(AmRtpTransport* transport,
                  int interface,
                  const string& ip,
                  bool rtcp=false,
                  unsigned int port=0);

  ~AmRtpSocketPair();
};

#endif
