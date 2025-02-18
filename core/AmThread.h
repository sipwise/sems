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

#include <pthread.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>

#include <queue>
#include <mutex>
#include <atomic>
#include <condition_variable>

using std::lock_guard;
using std::atomic_bool;

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
 * \brief C++ Wrapper class for pthread
 */
class AmThread
{
  pthread_t _td;
  AmMutex   _m_td;

  atomic_bool _stopped;

  static void* _start(void*);

protected:
  virtual void run()=0;
  virtual void on_stop()=0;

public:
  unsigned long _pid;
  AmThread();
  virtual ~AmThread() {}

  virtual void onIdle() {}

  /** Start it ! */
  void start();
  /** Stop it ! */
  void stop();
  /** @return true if this thread doesn't run. */
  bool is_stopped() { return _stopped; }
  /** Wait for this thread to finish */
  void join();
  /** kill the thread (if pthread_setcancelstate(PTHREAD_CANCEL_ENABLED) has been set) **/ 
  void cancel();

  int setRealtime();
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

  std::queue<AmThread*> thread_queue;
  AmMutex          q_mut;

  /** the daemon only runs if this is true */
  AmCondition _run_cond;
    
  AmThreadWatcher();
  void run();
  void on_stop();

public:
  static AmThreadWatcher* instance();
  void add(AmThread*);
};

template<class T>
class AmThreadLocalStorage
{
  pthread_key_t key;
  
  static void __del_tls_obj(void* obj) {
    delete static_cast<T*>(obj);
  }

public:
  AmThreadLocalStorage() {
    pthread_key_create(&key,__del_tls_obj);
  }

  ~AmThreadLocalStorage() {
    pthread_key_delete(key);
  }

  T* get() {
    return static_cast<T*>(pthread_getspecific(key));
  }

  void set(T* p) {
    pthread_setspecific(key,(void*)p);
  }
};

#endif

// Local Variables:
// mode:C++
// End:

