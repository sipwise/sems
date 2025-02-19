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
#include "../AmUtils.h"
#include <sys/types.h>
#include <inttypes.h>
#include <time.h>
#include <deque>
#include <list>
#include <map>

#include "log.h"

class timer;

typedef std::list<timer*> timer_list;
typedef std::map<uint64_t, timer_list> timer_buckets;

class timer
{
public:
    // for fast removal:
    timer_list::iterator pos;
    timer_list* list;

    timer() 
	: list(NULL), expires(0), expires_rel(0)
    {}

    timer(uint64_t expires)
        : list(NULL), expires(0), expires_rel(expires)
    {}

    timer(const timer &t)
        : list(NULL), expires(t.expires), expires_rel(t.expires_rel)
    {}

    virtual ~timer();

    virtual void fire()=0;

    // returns true if timer was not armed before
    // return false and does nothing otherwise
    bool arm()
    {
	if (expires)
	    return false;
	expires = expires_rel + gettimeofday_us();
	return true;
    }

    void disarm()
    {
	expires = 0;
    }

    // microseconds
    uint64_t get_absolute_expiry()
    {
	return expires;
    }

protected:
    uint64_t    expires; // absolute, microseconds, set after arming timer

private:
    uint64_t    expires_rel; // relative, microseconds
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

    timer_buckets buckets;
    uint64_t resolution; // microseconds

    // Only go to sleep if the next timer to run is at least this far in the future.
    // If the required sleep time is less long than this, don't bother going to sleep,
    // and just run the timers now. In microseconds.
    const uint64_t min_sleep_time = 500; // half a millisecond

    // Don't sleep longer than this, even if the next timer to run is further in the
    // future (or if no timers exist). Needed not to miss the shutdown flag being set.
    const uint64_t max_sleep_time = 500000; // half a second

    // request backlog lock (insert/remove)
    AmMutex               reqs_m;
    AmCondition     reqs_cond; // to wake up worker thread when a request is added
    std::deque<timer_req> reqs_backlog;

    void process_events();

    void place_timer(timer* t);

    void add_timer_to_bucket(timer* t, uint64_t);
    void delete_timer(timer* t);

    void process_current_timers(timer_list&);

protected:
    void run();
    void on_stop(){}

    _wheeltimer()
	: resolution(20000) // 20 ms == 20000 us
    {}

    _wheeltimer(uint64_t _resolution)
	: resolution(_resolution)
    {}

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
    uint64_t get_wall_clock()
    {
	return gettimeofday_us();
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
