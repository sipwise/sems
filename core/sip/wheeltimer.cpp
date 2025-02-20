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
    // Wake up worker thread: This triggers turn_wheel() based on how many ticks have passed,
    // and in turn brings wall_clock up to date. Finally the events queue is processed, which
    // adds the timer to the wheel based on the now-updated wall_clock.
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
  uint64_t now, next_tick, diff; // microseconds

  now = gettimeofday_us();
  next_tick = now + resolution;

  while(true){

    now = gettimeofday_us();

    if(now < next_tick){

      diff = next_tick - now;

      // Sleep up to diff ms OR up to 0.5 seconds if there are no timers,
      // but wake up early if something is added to reqs_backlog
      if (num_timers)
	reqs_cond.wait_for_to(diff / 1000);
      else
	reqs_cond.wait_for_to(500); // 0.5 s
    }
    //else {
    //printf("missed one tick\n");
    //}

    now = gettimeofday_us();

    while (now >= next_tick) {
      turn_wheel();
      next_tick += resolution;
    }

    process_events();
  }
}



void _wheeltimer::update_wheel(int wheel)
{
    // do not try do update wheel 0
    if(!wheel)
	return;
    
    for(;wheel;wheel--){

	int pos = (wall_clock >> (wheel*BITS_PER_WHEEL))
	    & ((1<<BITS_PER_WHEEL)-1);
	
	auto& w = wheels[wheel][pos];
	while (!w.empty()) {
	    auto* t = w.front();
	    w.pop_front();
	    place_timer(t, wheel-1);
	}
    }
}

void _wheeltimer::turn_wheel()
{
    u_int32_t mask = ((1<<BITS_PER_WHEEL)-1); // 0x00 00 00 FF
    int i=0;
	
    //determine which wheel should be updated
    for(;i<WHEELS;i++){
	if((wall_clock & mask) ^ mask)
	    break;
	mask <<= BITS_PER_WHEEL;
    }

    //increment time
    wall_clock++;
		
    // Update existing timer entries
    update_wheel(i);
	
    //check for expired timer to process
    process_current_timers();
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

    //check for expired timer to process in case we just added some
    process_current_timers();
}

void _wheeltimer::process_current_timers()
{
    auto& w = wheels[0][wall_clock & 0xFF];

    while (!w.empty()) {
	auto* t = w.front();
	w.pop_front();

	t->list = NULL;
	t->disarm();
	num_timers--;

	t->fire();
    }
}

void _wheeltimer::place_timer(timer* t)
{
    if (t->arm())
	num_timers++;

    if(t->get_absolute_expiry() < gettimeofday_us()) {

	// we put the late ones at the beginning of next wheel turn
	add_timer_to_wheel(t,0,((1<<BITS_PER_WHEEL)-1) & wall_clock);
	
 	return;
    }

    place_timer(t,WHEELS-1);
}

void _wheeltimer::place_timer(timer* t, int wheel)
{
    unsigned int pos;
    unsigned int expiry_ticks = t->get_absolute_expiry() / resolution;
    unsigned int clock_mask = expiry_ticks ^ wall_clock;

    for(; wheel; wheel--){

	if( (clock_mask >> (wheel*BITS_PER_WHEEL))
	    & ((1<<BITS_PER_WHEEL)-1) ) {

	    break;
	}
    }

    // we went down to wheel 0
    pos = (expiry_ticks >> (wheel*BITS_PER_WHEEL)) & ((1<<BITS_PER_WHEEL)-1);
    add_timer_to_wheel(t,wheel,pos);
}

void _wheeltimer::add_timer_to_wheel(timer* t, int wheel, unsigned int pos)
{
    t->list = &wheels[wheel][pos];
    t->list->push_front(t);
    t->pos = wheels[wheel][pos].begin();
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
