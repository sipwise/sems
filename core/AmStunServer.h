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

#ifndef _AmStunServer_h_
#define _AmStunServer_h_

#include "AmStunPacket.h"
#include "AmIceCandidate.h"
#include "AmSdp.h"

#include "stun/stun_utils.h"
#include "ice_utils.h"

#include <netinet/in.h>
#include <string>
#include <vector>

using std::string;
using std::vector;

class AmRtpSocket;

class AmStunServer
{
  /** Local and remote credentials*/
  string          l_ice_username;
  string          l_ice_password;
  string          r_ice_username;
  string          r_ice_password;

  static AmIceCandidate* getLocalIceCandidates(AmRtpSocket* socket,
                                               const string& foundation,
                                               bool rtcp);

public:
  inline void initStun() {
    l_ice_username = createIceUsername();
    l_ice_password = createIcePassword();
  }

  void recvStun(unsigned char* buffer, size_t len,
                      AmRtpSocket* socket, sockaddr_storage* addr);

  inline void setLocalStunCredentials(const string& username,
                                      const string& password)
  {
    l_ice_username = username;
    l_ice_password = password;
  }

  inline void setRemoteStunCredentials(const string& username,
                                       const string& password)
  {
    r_ice_username = username;
    r_ice_password = password;
  }

  inline string getLocalStunUsername() const
  {
    return l_ice_username;
  }

  inline string getLocalStunPassword() const
  {
    return l_ice_password;
  }

  inline string getRemoteStunUsername() const
  {
    return r_ice_username;
  }

  inline string getRemoteStunPassword() const
  {
    return r_ice_password;
  }

  // Description
  static void getLocalIceDescription(SdpMedia& m, const string& foundation,
                                     AmRtpSocket* rtp_socket,
                                     AmRtpSocket* rtcp_socket);

  AmStunServer(){};
  virtual ~AmStunServer() {};
};

#endif
