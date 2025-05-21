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
/** @file AmThread.h */
#ifndef _AmThread_h_
#define _AmThread_h_

#include <sys/time.h>
#include <time.h>
#include <errno.h>

#include <list>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <thread>
#include <optional>

using std::lock_guard;
using std::atomic_bool;

class SdNotifier;

/**
 * \brief Wrapper class for std::mutex
 */
class AmMutex : public std::mutex
{
public:
  AmMutex() : std::mutex() {}
  AmMutex(const AmMutex &) : std::mutex() {}
  void operator=(const AmMutex &) {}
};

/**
 * \brief Wrapper class for std::condition_variable
 */
class AmCondition
{
  bool                    t = false;
  std::mutex              m;
  std::condition_variable cond;

public:
  AmCondition() = default;
  AmCondition(const bool& _t) : t(_t) {}

  /** Change the condition's value. */
  void set(const bool newval)
  {
    std::lock_guard<std::mutex> l(m);
    t = newval;
    if(t)
      cond.notify_all();
  }
    
  bool get()
  {
    std::lock_guard<std::mutex> l(m);
    return t;
  }
    
  /** Waits for the condition to be true. */
  void wait_for()
  {
    std::unique_lock<std::mutex> l(m);
    while(!t){
      cond.wait(l);
    }
  }
  
  /** Waits for the condition to be true or a timeout. */
  bool wait_for_to(unsigned long msec)
  {
    auto timeout = std::chrono::system_clock::now();
    timeout += std::chrono::milliseconds(msec);

    std::unique_lock<std::mutex> l(m);
    while(!t){
      auto retcode = cond.wait_until(l, timeout);
      if (retcode == std::cv_status::timeout)
        break;
    }

    return t;
  }
};

/**
 * \brief Wrapper class for std::thread
 */
class AmThread
{
  std::thread _td;

  enum state {
    idle, // not started yet
    running,
    stopping, // waiting to stop
    stopped, // after stop
  };

  std::atomic<state> _state;

  std::mutex _join_mt;
  bool _joined;

  void _start();

  /** Initialise to true in subclasses that call the ready() method */
  bool _triggers_ready;

protected:
  virtual void run()=0;
  virtual void on_stop() {};

  /** @return true if this thread ought to stop. */
  bool stop_requested() { return _state == stopping; }

  std::optional<std::reference_wrapper<SdNotifier>> _sd_notifier;

  /** Signal that the thread is ready to do its job */
  void ready();

public:
  unsigned long _pid;

  AmThread(bool triggers_ready = false)
    : _state(state::idle),
      _joined(false),
      _triggers_ready(triggers_ready)
  {}

  virtual ~AmThread();

  /** Start it ! */
  void start();
  void start(SdNotifier&);

  /** Stop it ! */
  void stop();

  /** Wait for this thread to finish */
  void join();

  /** @return true if this thread has finished. */
  bool is_stopped() { return _state == stopped; }
};

/**
 * \brief Container/garbage collector for threads. 
 * 
 * AmThreadWatcher waits for threads to stop
 * and delete them.
 * It gets started automatically when needed.
 * Once you added a thread to the container,
 * there is no mean to get it out.
 */
class AmThreadWatcher: public AmThread
{
  static AmThreadWatcher* _instance;
  static AmMutex          _inst_mut;

  std::list<AmThread*> thread_list;
  AmMutex          q_mut;

  AmThreadWatcher() {}
  void run();

public:
  static AmThreadWatcher* instance();
  void add(AmThread*);
};

#endif

// Local Variables:
// mode:C++
// End:

