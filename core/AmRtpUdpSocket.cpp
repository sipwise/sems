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

#include "AmRtpUdpSocket.h"
#include "AmRtpTransport.h"
#include "AmRtpPacket.h"
#include "AmStunPacket.h"
#include "AmConfig.h"

#include "sip/raw_sender.h"


AmRtpUdpSocket::AmRtpUdpSocket(AmRtpSocketPair *listener, int interface,
                               const string& ip, unsigned int port=0)
  : AmRtpSocket(listener, interface, ip, port)
{
  is_udp = true;

  /* Set the sending method */
  if (sys_if && AmConfig::UseRawSockets)
    _send = sendraw;
  else if (sys_if && AmConfig::ForceOutboundIf)
    _send = sendmsg;
  else
    _send = sendto;

}

/* Private Methods */

/* Static Methods */
int AmRtpUdpSocket::sendto(unsigned char *buffer, size_t len, int sys_if,
			   int sd, sockaddr_storage* l_saddr,
			   sockaddr_storage* r_saddr)
{
  return ::sendto(sd, buffer, len, 0, (const struct sockaddr *)r_saddr,
		  SA_len(r_saddr));
}

int AmRtpUdpSocket::sendraw(unsigned char* buffer, size_t len, int sys_if,
			    int sd, sockaddr_storage* l_saddr,
			    sockaddr_storage* r_saddr)
{
  return raw_sender::send((char*)buffer, (unsigned int)len, sys_if,
			  l_saddr, r_saddr);
}

int AmRtpUdpSocket::sendmsg(unsigned char* buffer, size_t len, int sys_if,
			    int sd, sockaddr_storage* l_saddr,
			    sockaddr_storage* r_saddr)
{
  struct msghdr hdr;
  struct cmsghdr* cmsg;

  union {
    char cmsg4_buf[CMSG_SPACE(sizeof(struct in_pktinfo))];
    char cmsg6_buf[CMSG_SPACE(sizeof(struct in6_pktinfo))];
  } cmsg_buf;

  struct iovec msg_iov[1];
  msg_iov[0].iov_base = (void*)buffer;
  msg_iov[0].iov_len  = len;

  bzero(&hdr,sizeof(hdr));
  hdr.msg_name = (void*)r_saddr;
  hdr.msg_namelen = SA_len(r_saddr);
  hdr.msg_iov = msg_iov;
  hdr.msg_iovlen = 1;

  bzero(&cmsg_buf,sizeof(cmsg_buf));
  hdr.msg_control = &cmsg_buf;
  hdr.msg_controllen = sizeof(cmsg_buf);

  cmsg = CMSG_FIRSTHDR(&hdr);
  if(r_saddr->ss_family == AF_INET) {
    cmsg->cmsg_level = IPPROTO_IP;
    cmsg->cmsg_type = IP_PKTINFO;
    cmsg->cmsg_len = CMSG_LEN(sizeof(struct in_pktinfo));

    struct in_pktinfo* pktinfo = (struct in_pktinfo*) CMSG_DATA(cmsg);
    pktinfo->ipi_ifindex = sys_if;
  }
  else if(r_saddr->ss_family == AF_INET6) {
    cmsg->cmsg_level = IPPROTO_IPV6;
    cmsg->cmsg_type = IPV6_PKTINFO;
    cmsg->cmsg_len = CMSG_LEN(sizeof(struct in6_pktinfo));

    struct in6_pktinfo* pktinfo = (struct in6_pktinfo*) CMSG_DATA(cmsg);
    pktinfo->ipi6_ifindex = sys_if;
  }

  hdr.msg_controllen = cmsg->cmsg_len;

  return ::sendmsg(sd, &hdr, 0);
}


int AmRtpUdpSocket::send(unsigned char* buffer, size_t len, int sys_if,
			 int sd, sockaddr_storage* l_saddr,
			 sockaddr_storage* r_saddr)
{

  if (logger)
    logSent(buffer, len, r_saddr);

  if (_send(buffer, len, sys_if, sd, l_saddr, r_saddr)  < 0) {
    ERROR("while sending data: %s\n",strerror(errno));
    return -1;
  }

  return 0;
}



/* Public Methods */

int AmRtpUdpSocket::sendRtp(AmRtpPacket* rp)
{
  return send(rp->getBuffer(),(size_t)rp->getBufferSize());
}

int AmRtpUdpSocket::sendRtcp(unsigned char* buffer, int size)
{
  return send(buffer,(size_t)size);
}

int AmRtpUdpSocket::send(unsigned char* buffer, size_t len)
{
  return send(buffer, len, sys_if, l_sd, &l_saddr, &r_saddr);
}

int AmRtpUdpSocket::send(unsigned char* buffer, size_t len,
                       sockaddr_storage* addr)
{
  return send(buffer, len, sys_if, l_sd, &l_saddr, addr);
}


/* Called from the base RtpSocket Class */
void AmRtpUdpSocket::recvData(unsigned char* buffer, size_t len,
                              sockaddr_storage& addr)
{
  if (logger)
    logReceived(buffer, len, &addr);

  listener->recvData(this, buffer, len, addr);
}
