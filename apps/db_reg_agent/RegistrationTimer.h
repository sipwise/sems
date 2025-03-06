/*
 * Copyright (C) 2011 Stefan Sayer
 *
 * This file is part of SEMS, a free SIP media server.
 *
 * SEMS is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * For a license to use the sems software under conditions
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

#ifndef _RegistrationTimer_h_
#define _RegistrationTimer_h_

#include <list>
#include <vector>

#include <sys/time.h>

#include "log.h"
#include "AmThread.h"
#include "../../core/sip/wheeltimer.h"

#define TIMER_RESOLUTION (100 * 1000) // 100 ms

/**
  Additionally to normal timer operation (setting and removing timer,
  fire the timer when it is expired), this RegistrationTimer timers 
  class needs to support insert_timer_leastloaded() which should insert 
  the timer in some least loaded interval between from_time and to_time
  in order to flatten out re-register spikes (due to restart etc).

  The timer object is owned by the caller, and MUST be valid until it is
  fired or removed.
 */

class RegistrationTimer
: public _wheeltimer
{
public:
  RegistrationTimer() : _wheeltimer(TIMER_RESOLUTION) {}
};

#endif
