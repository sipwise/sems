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

#include "AmEventQueue.h"
#include "log.h"
#include "AmConfig.h"

#include <typeinfo>
AmEventQueueBase::AmEventQueueBase(AmEventHandler* handler, std::mutex& _m, std::condition_variable& _c)
  : handler(handler),
    wakeup_handler(NULL),
    finalized(false),
    _mut(_m),
    _cond(_c)
{
}

AmEventQueueBase::~AmEventQueueBase()
{
  while(!ev_queue.empty()){
    delete ev_queue.front();
    ev_queue.pop();
  }
}

void AmEventQueueBase::postEvent(AmEvent* event)
{
  if (!event)
    return;

  if (AmConfig::LogEvents) 
    DBG("AmEventQueue: trying to post event\n");

  std::lock_guard<std::mutex> _l(_mut);

  bool was_empty = ev_queue.empty();

  ev_queue.push(event);
  _cond.notify_all();

  if (was_empty && NULL != wakeup_handler)
    wakeup_handler->notify(this);

  if (AmConfig::LogEvents) 
    DBG("AmEventQueue: event posted\n");
}

void AmEventQueueBase::processEvents()
{
  std::unique_lock<std::mutex> _l(_mut);

  while (!ev_queue.empty()) {
	
    AmEvent* event = ev_queue.front();
    ev_queue.pop();
    _l.unlock();

    if (AmConfig::LogEvents) 
      DBG("before processing event (%s)\n",
	  typeid(*event).name());
    handler->process(event);
    if (AmConfig::LogEvents) 
      DBG("event processed (%s)\n",
	  typeid(*event).name());
    delete event;

    if (!_l.owns_lock())
      _l.lock();

  }
}

void AmEventQueueBase::waitForEvent()
{
  std::unique_lock<std::mutex> _l(_mut);
  while (shouldSleep())
    _cond.wait(_l);
}

void AmEventQueueBase::waitForEventTimed(unsigned long msec)
{
  std::unique_lock<std::mutex> _l(_mut);
  if (shouldSleep())
    _cond.wait_for(_l, std::chrono::milliseconds(msec));
}

void AmEventQueueBase::processSingleEvent()
{
  std::unique_lock<std::mutex> _l(_mut);

  if (ev_queue.empty())
    return;

  AmEvent* event = ev_queue.front();
  ev_queue.pop();
  _l.unlock();

  if (AmConfig::LogEvents)
    DBG("before processing event\n");
  handler->process(event);
  if (AmConfig::LogEvents)
    DBG("event processed\n");
  delete event;
}

bool AmEventQueueBase::eventPending() {
  std::lock_guard<std::mutex> _l(_mut);
  return !shouldSleep();
}

void AmEventQueueBase::setEventNotificationSink(AmEventNotificationSink*
					    _wakeup_handler) {
  std::unique_lock<std::mutex> _l(_mut);
  wakeup_handler = _wakeup_handler;
  if (wakeup_handler && !ev_queue.empty())
    wakeup_handler->notify(this);
}
