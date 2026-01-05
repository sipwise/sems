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
/** @file AmRtpReceiver.h */
#ifndef _AmRtpReceiver_h_
#define _AmRtpReceiver_h_

#include "AmThread.h"
#include "atomic_types.h"
#include "singleton.h"

#include <event2/event.h>

#include <map>
using std::greater;

class _AmRtpReceiver;

/**
 * \brief receiver for RTP for all RTP transports.
 *
 * The RtpReceiver receives RTP packets for all RTP transports
 * that are registered to it. The transport places the received packets in
 * the corresponding stream's buffer.
 */
class AmRtpReceiverThread
  : public AmThread
{
  struct event_base* ev_base;
  struct event*      ev_default;

public:    
  AmRtpReceiverThread();
  ~AmRtpReceiverThread();
    
  void run();
  void on_stop();
  const char *identify() { return "RTP receiver"; }

  struct event_base* getBase();
};

class AmRtpReceiver : public singleton<AmRtpReceiver>
{
  friend class singleton<AmRtpReceiver>;

  AmRtpReceiverThread* receivers;
  unsigned int         n_receivers;

  atomic_int next_index;

protected:    
  AmRtpReceiver();
  ~AmRtpReceiver();

public:
  void start();

  struct event_base* getBase(int sd);
};

#endif

// Local Variables:
// mode:C++
// End:
