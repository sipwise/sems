/*
 * $Id: wheeltimer.h 1224 2009-01-09 09:55:37Z rco $
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

#ifndef _wheeltimer_h_
#define _wheeltimer_h_

#include "../AmThread.h"
#include <sys/types.h>
#include <time.h>
#include <deque>

#include "atomic_types.h"

#define BITS_PER_WHEEL 8
#define ELMTS_PER_WHEEL (1 << BITS_PER_WHEEL)

// 20 ms == 20000 us
#define TIMER_RESOLUTION 20000

// do not change
#define WHEELS 4

class base_timer
{
public:
    base_timer* next;

    base_timer():next(0) {}
    virtual ~base_timer() {}
};

class timer: public base_timer
{
public:
    base_timer*  prev;

    timer() 
	: base_timer(),
	  prev(0), expires(0), expires_rel(0)
    {}

    timer(unsigned int expires)
        : base_timer(),
	  prev(0), expires(0), expires_rel(expires)
    {}

    ~timer(); 

    virtual void fire()=0;

    // returns true if timer was not armed before
    // return false and does nothing otherwise
    bool arm_absolute(u_int32_t wall_clock)
    {
	if (expires)
	    return false;
	expires = expires_rel + wall_clock;
	return true;
    }

    void disarm()
    {
	expires = 0;
    }

    u_int32_t get_absolute_expiry()
    {
	return expires;
    }

protected:
    u_int32_t    expires; // absolute, set after arming timer

private:
    u_int32_t    expires_rel; // relative
};

#include "singleton.h"

class _wheeltimer:
    public AmThread
{
    struct timer_req {

	timer* t;
	bool   insert; // false -> remove
	
	timer_req(timer* t, bool insert)
	    : t(t), insert(insert)
	{}
    };

    //the timer wheel
    base_timer wheels[WHEELS][ELMTS_PER_WHEEL];
    unsigned int num_timers;

    // request backlog lock (insert/remove)
    AmMutex               reqs_m;
    std::deque<timer_req> reqs_backlog;
    std::deque<timer_req> reqs_process;

    u_int32_t wall_clock; // 32 bits

    void turn_wheel();
    void process_events();
    void update_wheel(int wheel);

    void place_timer(timer* t);
    void place_timer(timer* t, int wheel);

    void add_timer_to_wheel(timer* t, int wheel, unsigned int pos);
    void delete_timer(timer* t);

    void process_current_timers();

protected:
    void run();
    void on_stop(){}

    _wheeltimer()
	: wall_clock(0) {}

public:
    //clock reference
    struct _uc {
	int64_t get()
	{
	    return time(NULL);
	}
    };
    _uc unix_clock;

    // for debugging/logging only!
    u_int32_t get_wall_clock()
    {
	return wall_clock;
    }

    void insert_timer(timer* t);
    void remove_timer(timer* t);
};

typedef singleton<_wheeltimer> wheeltimer;

#endif

/** EMACS **
 * Local variables:
 * mode: c++
 * c-basic-offset: 4
 * End:
 */
