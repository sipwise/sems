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

#ifndef _AmRtpSocket_h_
#define _AmRtpSocket_h_

#include "sip/ip_util.h"

#include <event2/event.h>
#include <netinet/in.h>
#include <string>
#include <memory>

using std::string;
using std::shared_ptr;

class AmRtpSocketPair;
class AmRtpPacket;
class msg_logger;

/*
 * Base class for AmRtpUdpSocket and AmRtpTcpSocket.
 */
class AmRtpSocket
{
  struct event_base* ev_base;
  struct event*      ev_read;

  bool isAttachedToReceiver();

  /* libev read callback */
  static void read_cb(evutil_socket_t sd, short what, void* arg);

protected:
  shared_ptr<msg_logger> logger;

  AmRtpSocketPair* listener;

  sockaddr_storage l_saddr;
  sockaddr_storage r_saddr;

  /**
   * Local interface used for this stream
   * (index into @AmConfig::Ifs)
   */
  int l_if;

  /* System network interface associated with the local interface */
  int sys_if;

  /* Local Socket */
  int l_sd;

  bool is_udp;

  int send(unsigned char* buffer, size_t len);

  void logReceived(const unsigned char* buffer, size_t size, sockaddr_storage *remote);
  void logSent(const unsigned char* buffer, size_t size, sockaddr_storage *remote);

public:
  // Pure virtual methods to be defined by the subclass
  virtual void recvData(unsigned char* buffer, size_t size,
                          sockaddr_storage& addr) = 0;

  virtual int sendRtp(AmRtpPacket* rp) = 0;
  virtual int sendRtcp(unsigned char* buffer, int len) = 0;

  virtual void onStunConnected(sockaddr_storage* addr) = 0;

  virtual int send(unsigned char* buffer, size_t len, sockaddr_storage* addr) = 0;

public:

  inline bool isUdp() { return is_udp; };

  void addToReceiver();
  void removeFromReceiver();

  void recv();

  // TODO: Do we really need these twoo??? Try to remove them!
  inline sockaddr_storage* getRemoteSocket() { return &r_saddr; };

  void setRemoteAddress(const string& addr, unsigned short port);

  inline const string getRemoteAddress() { return am_inet_ntop(&r_saddr); };
  inline int getRemotePort() { return am_get_port(&r_saddr); };

  inline const string getLocalAddress() { return am_inet_ntop(&l_saddr); };
  inline int getLocalPort() { return am_get_port(&l_saddr); };

  /** set destination for logging all received/sent RTP and RTCP packets */
  void setLogger(const shared_ptr<msg_logger>& _logger);

  AmRtpSocket(AmRtpSocketPair* listener, int interface, const string& ip,
              unsigned int port);
  virtual ~AmRtpSocket();
};

#endif
