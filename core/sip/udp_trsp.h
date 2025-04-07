/*
 * $Id: udp_trsp.h 1048 2008-07-15 18:48:07Z sayer $
 *
 * Copyright (C) 2007 Raphael Coeffic
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
#ifndef _udp_trsp_h_
#define _udp_trsp_h_

#include "transport.h"

/**
 * Maximum message length for UDP
 * not including terminating '\0'
 */
#define MAX_UDP_MSGLEN 65535

#include <sys/socket.h>

#include <string>
using std::string;

#if defined IP_RECVDSTADDR
# define DSTADDR_SOCKOPT IP_RECVDSTADDR
# define DSTADDR_DATASIZE (CMSG_SPACE(sizeof(struct in_addr)))
# define dstaddr(x) (CMSG_DATA(x))
#elif defined IP_PKTINFO
# define DSTADDR_SOCKOPT IP_PKTINFO
# define DSTADDR_DATASIZE (CMSG_SPACE(sizeof(struct in_pktinfo)))
# define dstaddr(x) (&(((struct in_pktinfo *)(CMSG_DATA(x)))->ipi_addr))
#else
# error "can't determine v4 socket option (IP_RECVDSTADDR or IP_PKTINFO)"
#endif

#if !defined IPV6_RECVPKTINFO
# define DSTADDR6_SOCKOPT IPV6_PKTINFO
# define dstaddr6(x) (&(((struct in6_pktinfo *)(CMSG_DATA(x)))->ipi6_addr))
#elif defined IPV6_PKTINFO
# define DSTADDR6_SOCKOPT IPV6_RECVPKTINFO
# define dstaddr6(x) (&(((struct in6_pktinfo *)(CMSG_DATA(x)))->ipi6_addr))
#else
# error "cant't determine v6 socket option (IPV6_RECVPKTINFO or IPV6_PKTINFO)"
#endif

class udp_trsp_socket: public trsp_socket
{
    int sendto(const sockaddr_storage* sa, const char* msg, const int msg_len);
    int sendmsg(const sockaddr_storage* sa, const char* msg, const int msg_len);

public:
    udp_trsp_socket(unsigned short if_num, unsigned int opts,
		    unsigned int sys_if_idx = 0)
	: trsp_socket(if_num,opts,sys_if_idx) {}

    ~udp_trsp_socket() {}

    /**
     * Binds the transport socket to an address
     * @return -1 if error(s) occured.
     */
    virtual int bind(const string& address, unsigned short port);

    const char* get_transport() const
    { return "udp"; }

    int set_recvbuf_size(int rcvbuf_size);

    /**
     * Sends a message.
     * @return -1 if error(s) occured.
     */
    int send(const sockaddr_storage* sa, const char* msg,
	     const int msg_len, unsigned int flags);
};

class udp_trsp: public transport
{
    char   buf[MAX_UDP_MSGLEN];
    u_char dst_addr_buf[DSTADDR_DATASIZE];

    msghdr           msg;
    sockaddr_storage from_addr;
    iovec            iov[1];

protected:
    /** @see AmThread */
    void run();
    /** @see AmThread */
    void on_stop();

public:
    /** @see transport */
    udp_trsp(udp_trsp_socket* sock);
    ~udp_trsp();
};

#endif

/** EMACS **
 * Local variables:
 * mode: c++
 * c-basic-offset: 4
 * End:
 */
