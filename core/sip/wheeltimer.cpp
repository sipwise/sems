/*
 * $Id: wheeltimer.cpp 1224 2009-01-09 09:55:37Z rco $
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

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <stdint.h>
#include <unistd.h>

#include "AmThread.h"
#include "AmUtils.h"
#include "wheeltimer.h"

#include "log.h"


timer::~timer()
{
    // DBG("timer::~timer(this=%p)\n",this);
}

void _wheeltimer::insert_timer(timer* t, uint64_t relative_us)
{
    insert_timer_abs(t, (gettimeofday_us() + relative_us));
}

void _wheeltimer::insert_timer_abs(timer* t, uint64_t us)
{
    std::lock_guard<std::mutex> lock(buckets_mut);
    place_timer(t, us);
    // Wake up worker thread: The new timer might be the next one to run
    buckets_cond.notify_one();
}

void _wheeltimer::remove_timer(timer* t)
{
    if (t == NULL){
	return;
    }

    std::lock_guard<std::mutex> lock(buckets_mut);
    delete_timer(t);
    // Don't wake up worker thread: This is not necessary because in the worst case the
    // worker thread would wake up early, see that there's nothing to do, and just go to
    // sleep again
}

void _wheeltimer::run()
{
  while(true){

    // figure out whether there's anything to run, and for how long to sleep

    uint64_t now = gettimeofday_us();

    std::unique_lock<std::mutex> lock(buckets_mut);

    auto beg = buckets.begin();

    if (beg == buckets.end()) {
	// nothing to wait for - sleep the fixed maximum allowed
	buckets_cond.wait_for(lock, std::chrono::microseconds(max_sleep_time));
	continue;
    }

    auto next = beg->first;

    if (next > now) {
      uint64_t diff = next - now;
      // don't bother sleeping more than half a millisecond
      if (diff > min_sleep_time) {
	  // Sleep up to diff ms OR up to the allowed maximum if it's longer
	  // but wake up early if something is added to reqs_backlog
	  if (diff < max_sleep_time)
	    buckets_cond.wait_for(lock, std::chrono::microseconds(diff));
	  else
	    buckets_cond.wait_for(lock, std::chrono::microseconds(max_sleep_time));
	  continue;
      }
    }

    // this slot needs to run now
    process_current_timers(beg->second, lock);

    // all done, remove bucket
    buckets.erase(beg);
  }
}

void _wheeltimer::process_current_timers(timer_list& list, std::unique_lock<std::mutex>& lock)
{
    while (!list.empty()) {
	auto* t = list.front();

	t->disarm(); // removes it from list

	// safe to unlock now
	lock.unlock();

	t->fire();

	lock.lock();
    }
}

uint64_t _wheeltimer::get_timer_bucket(uint64_t exp)
{
    // scale expiry based on resolution: this is the bucket index
    // add +1 to make sure we never run before the scheduled time
    return ((exp / resolution) + 1) * resolution;
}

uint64_t _wheeltimer::get_timer_bucket(timer* t)
{
    uint64_t exp = t->get_absolute_expiry();
    uint64_t bucket = get_timer_bucket(exp);

    // if expiry is too soon or in the past, put the timer in the next bucket up
    auto now = gettimeofday_us();
    if (bucket <= now)
	bucket = get_timer_bucket(now);

    return bucket;
}

void _wheeltimer::place_timer(timer* t, uint64_t us)
{
    t->arm(us);
    uint64_t bucket = get_timer_bucket(t);
    add_timer_to_bucket(t, bucket);
}

void _wheeltimer::add_timer_to_bucket(timer* t, uint64_t bucket)
{
    t->link(buckets[bucket]);
}

void _wheeltimer::delete_timer(timer* t)
{
    t->disarm();
    delete t;
}


/** EMACS **
 * Local variables:
 * mode: c++
 * c-basic-offset: 4
 * End:
 */
