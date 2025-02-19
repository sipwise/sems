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

void _wheeltimer::insert_timer(timer* t)
{
    //add new timer to user request list
    std::lock_guard<AmMutex> lock(reqs_m);
    reqs_backlog.push_back(timer_req(t,true));
    // Wake up worker thread: This causes the timer to be added to the appropriate bucket
    reqs_cond.set(true);
}

void _wheeltimer::remove_timer(timer* t)
{
    if (t == NULL){
	return;
    }

    //add timer to remove to user request list
    std::lock_guard<AmMutex> lock(reqs_m);
    reqs_backlog.push_back(timer_req(t,false));
    // Wake up worker thread: This is needed because the events queue is processed after
    // expired timers are fired, and because the worker thread would otherwise continue to
    // sleep, possibly until the next timer expires, which may be the one we want to remove.
    // IOW we want to make sure events are processed before timers are fired, in case the
    // timer we want to remove now is one of the timers that would be fired next.
    reqs_cond.set(true);
}

void _wheeltimer::run()
{
  while(true){

    // make sure everything that's been added or removed is taken into account
    process_events();

    // figure out whether there's anything to run, and for how long to sleep

    uint64_t now = gettimeofday_us();

    auto beg = buckets.begin();

    if (beg == buckets.end()) {
	// nothing to wait for - sleep the fixed maximum allowed
	reqs_cond.wait_for_to(max_sleep_time / 1000);
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
	    reqs_cond.wait_for_to(diff / 1000);
	  else
	    reqs_cond.wait_for_to(max_sleep_time / 1000);
	  continue;
      }
    }

    // this slot needs to run now
    process_current_timers(beg->second);

    // all done, remove bucket
    buckets.erase(beg);
  }
}

void _wheeltimer::process_events()
{
    // Swap the lists for timer insertion/deletion requests and reset wake condition
    std::deque<timer_req> reqs_process;
    std::unique_lock<AmMutex> lock(reqs_m);
    reqs_cond.set(false);
    reqs_process.swap(reqs_backlog);
    lock.unlock();

    while(!reqs_process.empty()) {
	timer_req rq = reqs_process.front();
	reqs_process.pop_front();

	if(rq.insert) {
	    place_timer(rq.t);
	}
	else {
	    delete_timer(rq.t);
	}
    }
}

void _wheeltimer::process_current_timers(timer_list& list)
{
    while (!list.empty()) {
	auto* t = list.front();
	list.pop_front();

	t->list = NULL;
	t->disarm();

	t->fire();
    }
}

void _wheeltimer::place_timer(timer* t)
{
    t->arm();

    uint64_t exp = t->get_absolute_expiry();

    // scale expiry based on resolution: this is the bucket index
    exp = ((exp / resolution) + 1) * resolution;

    // if expiry is too soon or in the past, put the timer in the next bucket up
    auto now = gettimeofday_us();
    if (exp <= now)
	exp = ((now / resolution) + 1) * resolution;

    add_timer_to_bucket(t, exp);
}

void _wheeltimer::add_timer_to_bucket(timer* t, uint64_t bucket)
{
    auto& b = buckets[bucket];
    t->list = &b;
    b.push_front(t);
    t->pos = b.begin();
}

void _wheeltimer::delete_timer(timer* t)
{
    if (t->list) {
	t->list->erase(t->pos);
	t->list = NULL;
    }

    delete t;
}


/** EMACS **
 * Local variables:
 * mode: c++
 * c-basic-offset: 4
 * End:
 */
