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

#include "AmStunServer.h"
#include "AmRtpSocket.h"
#include "sip/ip_util.h"
#include "log.h"

#include <netinet/in.h>
#include <string>
#include <vector>

using std::string;
using std::vector;

void AmStunServer::recvStun(unsigned char* buffer, size_t len,
                      AmRtpSocket* socket, sockaddr_storage* addr)
{
  AmStunPacket *stun = AmStunPacket::parse(buffer, len);

  AmStunPacket::Type type = stun->getType();
  AmStunPacket::Method method = stun->getMethod();

  // If it is a binding request
  if (type==AmStunPacket::Request && method==AmStunPacket::Binding){
    DWORD len = 0;
    // Create response
    AmStunPacket* resp = stun->createResponse();
    // Add received xor mapped addres
    resp->addXorAddressAttribute(addr);
    // TODO: Check incoming request username attribute value starts with l_ice_username+":"
    // Create  response
    DWORD size = resp->getSize();
    unsigned char* aux = (unsigned char*)malloc(size);
    memset(aux, 0, size);

    // Check if we have local password
    if (!l_ice_password.empty())
      len = resp->authenticatedFingerPrint(aux, size, l_ice_password.c_str());
    else
      //Do not authenticate
      len = resp->nonAuthenticatedFingerPrint(aux, size);

    // Send it
    DBG("Sending STUN packet to %s:%u",
          am_inet_ntop(addr).c_str(), am_get_port(addr));
    socket->send(aux, len, addr);

    // Clean memory
    free(aux);
    // Clean response
    delete(resp);

    // Rewrite remote address if necessary
    if (stun->hasAttribute(AmStunPacket::Attribute::UseCandidate)) {
      if (!(am_inet_ntop(addr) == socket->getRemoteAddress() &&
            am_get_port(addr) == socket->getRemotePort())) {
        ERROR("Setting remote address based on STUN");
        socket->setRemoteAddress(am_inet_ntop(addr), am_get_port(addr));
      }
      socket->onStunConnected(addr);
   }
  }

  delete(stun);
}

AmIceCandidate* AmStunServer::getLocalIceCandidates(AmRtpSocket* socket,
                                                    const string& foundation,
                                                    bool rtcp)
{
  return new AmIceCandidate(
      foundation,
      rtcp?2:1,
      socket->isUdp()?"UDP":"TCP",
      socket->getLocalAddress(),
      socket->getLocalPort());
}

//TODO: Don't deal with SDP. Rename to:
// getLocalStunCredentials(string& username, string& password) {
// username = getLocalStunUsername();
// password = getLocalStunPassword();
// return;
//}
void AmStunServer::getLocalIceDescription(SdpMedia& m,
                                          const string& foundation,
                                          AmRtpSocket* rtp_socket,
                                          AmRtpSocket* rtcp_socket)
{
  m.iceCandidates.push_back(getLocalIceCandidates(rtp_socket, foundation,
                                                  false));
  if (rtcp_socket)
    m.iceCandidates.push_back(getLocalIceCandidates(rtcp_socket, foundation,
                                                    true));
}
