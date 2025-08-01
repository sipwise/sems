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
/** @file AmEventQueue.h */
#ifndef _AMEVENTQUEUE_H_
#define _AMEVENTQUEUE_H_

#include "AmThread.h"
#include "AmEvent.h"
#include "atomic_types.h"

#include <queue>

class AmEventQueueInterface
{
 public:
  virtual ~AmEventQueueInterface() {}
  virtual void postEvent(AmEvent*)=0;
};

class AmEventQueueBase;

/** a receiver for notifications about 
    the fact that events are pending */
class AmEventNotificationSink
{
 public:
  virtual ~AmEventNotificationSink() { }
  virtual void notify(AmEventQueueBase* sender) = 0;
};

/**
 * \brief Asynchronous event queue implementation
 *
 * This class implements an event queue; a queue into which
 * \ref AmEvent can safely be posted at any time from any
 * thread, which are then processed by the registered event
 *  handler.
 *
 * This is a generic base class that requires an external
 * mutex and condition variable.
 */
class AmEventQueueBase
  : public AmEventQueueInterface,
    virtual public atomic_ref_cnt
{
protected:
  AmEventHandler*           handler;
  AmEventNotificationSink*  wakeup_handler;

  std::queue<AmEvent*>      ev_queue;

  bool finalized;

  // mutex must be held
  virtual bool shouldSleep() const {
    return ev_queue.empty();
  }

private:
  std::mutex& _mut;
  std::condition_variable& _cond;

public:
  AmEventQueueBase(AmEventHandler* handler, std::mutex& _m, std::condition_variable& _c);
  virtual ~AmEventQueueBase();

  void postEvent(AmEvent*);
  void processEvents();
  void waitForEvent();
  void waitForEventTimed(unsigned long msec);
  void processSingleEvent();
  bool eventPending();

  void setEventNotificationSink(AmEventNotificationSink* _wakeup_handler);

  bool is_finalized() { return finalized; }

  // return true to continue processing
  virtual bool startup() { return true; }
  virtual bool processingCycle() { processEvents(); return true; }
  virtual void finalize() { finalized = true; }
};

/**
 * \brief Standalone asynchronous event queue
 *
 * This class can be used on its own.
 */
class AmEventQueue : public AmEventQueueBase {
  std::mutex _mut;
  std::condition_variable _cond;

public:
  AmEventQueue(AmEventHandler* handler) : AmEventQueueBase(handler, _mut, _cond) {}
};

/**
 * \brief Thread-based asynchronous event queue
 *
 * This class can be used to run a thread based on an event
 * queue.
 */
class AmEventQueueThread
  : public AmThread,
    public AmEventQueueBase
{
protected:
  // mutex must be held
  bool shouldSleep() const {
    return !stop_requested_unlocked() && AmEventQueueBase::shouldSleep();
  }

public:
  AmEventQueueThread(AmEventHandler* handler)
    : AmEventQueueBase(handler, run_mut, run_cond)
  {}

  virtual void on_destroy() {
    stop();
    join();
  }
};

#endif

