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

#ifndef _AmRtpUdpSocket_h_
#define _AmRtpUdpSocket_h_

#include "AmRtpSocket.h"
#include "rtp/rtp.h"

#include <string>

using std::string;

class AmRtpSocketPair;
class AmRtpPacket;

/*
 * Receives traffic from AmRtpSocket:
 * - recvPacket
 *
 * Sends traffic according to the selected sending method:
 * - sendto
 * - sendraw
 * - sendmsg
 */
class AmRtpUdpSocket:
    public AmRtpSocket
{

  typedef int (*sendFunction) (unsigned char* buffer, size_t len,
                               int interface, int sd, sockaddr_storage* l_saddr,
                               sockaddr_storage* r_saddr);
  sendFunction _send;


  int send(unsigned char* buffer, size_t len, int interface, int sd,
	   sockaddr_storage* l_saddr, sockaddr_storage* r_saddr);


  /* Sending Methods */
  static int sendto(unsigned char* buffer, size_t len, int interface,
		    int sd, sockaddr_storage* l_saddr, sockaddr_storage* r_saddr);

  static int sendraw(unsigned char* buffer, size_t len, int interface,
		     int sd, sockaddr_storage* l_saddr, sockaddr_storage* r_saddr);

  static int sendmsg(unsigned char* buffer, size_t len, int interface,
		     int sd, sockaddr_storage* l_saddr, sockaddr_storage* r_saddr);

public:

  // Methods to be overwriten by the child classes
  virtual void recvData(unsigned char* buffer, size_t size,
                        sockaddr_storage& addr);

  virtual int sendRtp(AmRtpPacket* rp);

  virtual int sendRtcp(unsigned char* buffer, int size);

  int send(unsigned char* buffer, size_t len);

  int send(unsigned char* buffer, size_t len, sockaddr_storage* addr);

  void onStunConnected(sockaddr_storage* addr) {};

  AmRtpUdpSocket(AmRtpSocketPair* listener, int interface,
                 const string& ip, unsigned int port);
  ~AmRtpUdpSocket(){};
};

#endif
