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

#ifndef _AmRtpTransport_h_
#define _AmRtpTransport_h_

#include "AmRtpStream.h"
#include "AmStunServer.h"
#include "AmRtpSocketPair.h"

#include <string>
#include <vector>

using std::string;

class AmRtpPacket;
class SdpMedia;
class msg_logger;

class AmRtpTransport:
    public AmStunServer
{
  AmRtpSocketPair* socket;

  shared_ptr<msg_logger> logger;

  // RTP Stream collection
  std::set<AmRtpStream*> rtp_streams;

  // Is RTCP being multiplexed?
  bool rtcp_mux;

  // Is ICE being used?
  bool has_ice;

  // Rtp Stream helpers
  AmRtpStream* getStream(rtp_hdr_t* hdr);
  AmRtpStream* getStream(const uint32_t ssrc);

  // Avoid collisions with other threads when altering the Stream collection
  void addToReceiver();
  void removeFromReceiver();

public:

  inline bool isRtcpMux() { return rtcp_mux; };

  void setRtcpMux();

  int getLocalRtpPort() const;
  int getLocalRtcpPort() const;

  int getRemoteRtpPort() const;
  int getRemoteRtcpPort() const;

  void setRemoteRtpAddress(const string& addr, unsigned short port);
  void setRemoteRtcpAddress(const string& addr, unsigned short port);

  const string getRemoteAddress();

  bool hasStream(AmRtpStream* stream);

  void addStream(AmRtpStream* stream);
  void removeStream(AmRtpStream* stream);

  void getLocalIceDescription(SdpMedia& m);

  sockaddr_storage* getRemoteRtpSocket();
  sockaddr_storage* getRemoteRtcpSocket();

  // STUN/ICE
  inline bool hasIce() { return has_ice; };

  // Send RTP
  int sendRtp(AmRtpPacket* rp);

  // Send RTCP
  int sendRtcp(unsigned char* buffer, int len);

  // Receive RTP
  void recvRtp(unsigned char* buffer, size_t len, sockaddr_storage& addr);

  // Receive RTCP
  void recvRtcp(unsigned char* buffer, size_t len, sockaddr_storage& addr);

  // Receive Unknown data
  void recvUnknown(unsigned char* buffer, size_t len, sockaddr_storage& addr);

  // Description
  void getDescription(SdpMedia& m);

  /** set destination for logging all received/sent RTP and RTCP packets */
  void setLogger(const shared_ptr<msg_logger>& _logger);

  AmRtpTransport(int interface, const string& ip, bool rtcp=false, bool ice=false, unsigned int port=0);
  ~AmRtpTransport();
};

typedef vector<AmRtpTransport*> RtpTransportVector;
typedef RtpTransportVector::iterator RtpTransportIterator;

#endif
