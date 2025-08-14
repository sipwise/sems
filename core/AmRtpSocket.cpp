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

#include "AmRtpSocket.h"
#include "AmRtpReceiver.h"
#include "AmConfig.h"
#include "sip/resolver.h"
#include "sip/ip_util.h"
#include "sip/msg_logger.h"

#include <sys/ioctl.h>
#include <stdexcept>

using std::runtime_error;

class AmRtpSocketPair;

/*
 * AmRtpSocket
 * Manages Network socket
 * Receives the traffic from network
 * Sends traffic to network
 */

//TODO: Add a Type (SOCK_DGRAM, SOCK_STREAM) parameter for choosing UDP, TCP
AmRtpSocket::AmRtpSocket(AmRtpSocketPair* listener, int interface,
                         const string& ip, unsigned int port=0)
  : logger(NULL),
    listener(listener),
    l_if(interface),
    sys_if(AmConfig::RTP_Ifs[l_if].NetIfIdx),
    l_sd(0),
    ev_base(NULL),ev_read(NULL),
    is_udp(false)
{

  /* Set local and remote sockets to Zeroes */
  memset(&r_saddr,0,sizeof(struct sockaddr_storage));
  memset(&l_saddr,0,sizeof(struct sockaddr_storage));

  /* Initialise the local socket */

  // Set IP address
  if (!am_inet_pton(ip.c_str(), &l_saddr)) {
    throw runtime_error("Invalid IP address: " + ip);
  }
  DBG("set local ip = %s\n",ip.c_str());

  // Bind port
  int retry = 10;
  for(;retry; --retry) {

    if (port) {

      // Set Local Socket
      if((l_sd = socket(l_saddr.ss_family,SOCK_DGRAM,0)) == -1) {
        ERROR("%s\n",strerror(errno));
        close(l_sd);
        throw runtime_error("while creating new socket.");
      }

      int true_opt = 1;
      if(ioctl(l_sd, FIONBIO , &true_opt) == -1){
        ERROR("%s\n",strerror(errno));
        close(l_sd);
        throw runtime_error("while setting socket non blocking.");
      }

      am_set_port(&l_saddr,port);
      if (bind(l_sd,(const struct sockaddr*)&l_saddr,SA_len(&l_saddr))) {
        DBG("bind: %s\n",strerror(errno));
        port = 0;
        close(l_sd);
        l_sd = 0;
        if (retry == 10)
          throw runtime_error("unable to bind on the specified port");
      } else {
        break;
      }
    }

    else {
      port = AmConfig::RTP_Ifs[l_if].getNextRtpPort();
      if(!port) throw runtime_error("disabled RTP interface");
    }
  }

  if (!retry){
    ERROR("could not find a free RTP port\n");
    throw runtime_error("could not find a free RTP port");
  }

  // rco: does that make sense after bind() ????
  int true_opt = 1;
  if(setsockopt(l_sd, SOL_SOCKET, SO_REUSEADDR,
		(void*)&true_opt, sizeof (true_opt)) == -1) {

    ERROR("%s\n",strerror(errno));
    close(l_sd);
    l_sd = 0;
    throw runtime_error("while setting local address reusable.");
  }

  ev_base = AmRtpReceiver::instance()->getBase(l_sd);

  addToReceiver();
}

AmRtpSocket::~AmRtpSocket()
{
  if (logger) dec_ref(logger);
  if(l_sd) {
    close(l_sd);
  }

  // event_free does remove the Socket
  // from the event loop as well.
  if(ev_read)
    event_free(ev_read);
}

void AmRtpSocket::read_cb(evutil_socket_t sd, short what, void* arg)
{
  AmRtpSocket* socket = static_cast<AmRtpSocket*>(arg);

  socket->recv();
}


bool AmRtpSocket::isAttachedToReceiver()
{
  return (ev_read && event_pending(ev_read,EV_READ,NULL));
}

void AmRtpSocket::addToReceiver()
{
  assert(ev_base && l_sd);

  if(!ev_read)
    ev_read = event_new(ev_base,l_sd,EV_READ|EV_PERSIST, read_cb, this);

  event_add(ev_read,NULL);

  DBG("added RTP Socket [%p] to RTP receiver (%s:%i)\n", this,
      getLocalAddress().c_str(), getLocalPort());
}

void AmRtpSocket::removeFromReceiver()
{
  if(ev_read) {
    event_del(ev_read);

    DBG("removed RTP Socket [%p] from RTP receiver (%s:%i)\n", this,
        getLocalAddress().c_str(), getLocalPort());
  }
}

/*
 * Receive data from network
 */
void AmRtpSocket::recv()
{
  sockaddr_storage addr;
  socklen_t addr_len = sizeof(addr);
  unsigned char buffer[4096];
  ssize_t len = recvfrom(l_sd,buffer,sizeof(buffer),0,(struct sockaddr*)&addr,
                     &addr_len);

  if (!len) {
    ERROR("Received empty packet");
    return;
  } else if (len < 0) {
    if((errno != EINTR) && (errno != EAGAIN)) {
      ERROR("recv(%d): %s",l_sd,strerror(errno));
    }
    return;
  } else if ((size_t)len > sizeof(buffer)) {
    ERROR("Received huge RTCP packet (%zd)",len);
    return;
  }

  recvData(buffer, len, addr);
}

void AmRtpSocket::setRemoteAddress(const string& addr, unsigned short port)
{

  DBG("Setting remote address to: %s:%u [%p]", addr.c_str(), port, this);

  struct sockaddr_storage ss;
  memset (&ss, 0, sizeof (ss));

  /* inet_aton only supports dot-notation IP address strings... but an RFC
   * 4566 unicast-address, as found in c=, can be an FQDN (or other!).
   */
  dns_handle dh;
  if (resolver::instance()->resolve_name(addr.c_str(),&dh,&ss,IPv4) < 0) {
    WARN("Address not valid (host: %s).\n", addr.c_str());
    throw runtime_error("invalid address" + addr);
  }

  memcpy(&r_saddr,&ss,sizeof(struct sockaddr_storage));

  if (port)
    am_set_port(&r_saddr, port);
}

void AmRtpSocket::setLogger(msg_logger *_logger)
{
  // Set logger
  if (logger) dec_ref(logger);
  logger = _logger;
  if (logger) inc_ref(logger);
}


void AmRtpSocket::logReceived(const unsigned char* buffer, size_t size, sockaddr_storage *remote)
{
  static const cstring empty;
  logger->log((const char *)buffer, size, remote, &l_saddr, empty);
  }


void AmRtpSocket::logSent(const unsigned char* buffer, size_t size, sockaddr_storage *remote)
{
  static const cstring empty;
  logger->log((const char *)buffer, size, &l_saddr, remote, empty);
}


