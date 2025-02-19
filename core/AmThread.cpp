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

#include "AmThread.h"
#include "log.h"

#include <unistd.h>
#include "errno.h"
#include <string>
using std::string;

AmThread::AmThread()
  : _stopped(true)
{
}

void * AmThread::_start(void * _t)
{
  AmThread* _this = (AmThread*)_t;
  _this->_pid = (unsigned long) _this->_td;
  DBG("Thread %lu is starting.\n", (unsigned long) _this->_pid);
  _this->run();

  DBG("Thread %lu is ending.\n", (unsigned long) _this->_pid);
  _this->_stopped = true;
    
  return NULL;
}

void AmThread::start()
{
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr,1024*1024);// 1 MB

  int res;
  _pid = 0;

  // unless placed here, a call seq like run(); join(); will not wait to join
  // b/c creating the thread can take too long
  bool expected = true;
  if (!this->_stopped.compare_exchange_strong(expected, false)) {
    ERROR("thread already running\n");
    return;
  }

  res = pthread_create(&_td,&attr,_start,this);
  pthread_attr_destroy(&attr);
  if (res != 0) {
    ERROR("pthread create failed with code %i\n", res);
    throw string("thread could not be started");
  }	
  //DBG("Thread %lu is just created.\n", (unsigned long int) _pid);
}

void AmThread::stop()
{
  lock_guard<AmMutex> _l(_m_td);

  if(is_stopped()){
    return;
  }

  // gives the thread a chance to clean up
  DBG("Thread %lu (%lu) calling on_stop, give it a chance to clean up.\n", 
      (unsigned long int) _pid, (unsigned long int) _td);

  try { on_stop(); } catch(...) {}

  int res;
  if ((res = pthread_detach(_td)) != 0) {
    if (res == EINVAL) {
      WARN("pthread_detach failed with code EINVAL: thread already in detached state.\n");
    } else if (res == ESRCH) {
      WARN("pthread_detach failed with code ESRCH: thread could not be found.\n");
    } else {
      WARN("pthread_detach failed with code %i\n", res);
    }
  }

  DBG("Thread %lu (%lu) finished detach.\n", (unsigned long int) _pid, (unsigned long int) _td);

  //pthread_cancel(_td);
}

void AmThread::join()
{
  if(!is_stopped())
    pthread_join(_td,NULL);
}


int AmThread::setRealtime() {
  // set process realtime
  //     int policy;
  //     struct sched_param rt_param;
  //     memset (&rt_param, 0, sizeof (rt_param));
  //     rt_param.sched_priority = 80;
  //     int res = sched_setscheduler(0, SCHED_FIFO, &rt_param);
  //     if (res) {
  // 	ERROR("sched_setscheduler failed. Try to run SEMS as root or suid.\n");
  //     }

  //     policy = sched_getscheduler(0);
    
  //     std::string str_policy = "unknown";
  //     switch(policy) {
  // 	case SCHED_OTHER: str_policy = "SCHED_OTHER"; break;
  // 	case SCHED_RR: str_policy = "SCHED_RR"; break;
  // 	case SCHED_FIFO: str_policy = "SCHED_FIFO"; break;
  //     }
 
  //     DBG("Thread has now policy '%s' - priority 80 (from %d to %d).\n", str_policy.c_str(), 
  // 	sched_get_priority_min(policy), sched_get_priority_max(policy));
  //     return 0;
  return 0;
}


AmThreadWatcher* AmThreadWatcher::_instance=0;
AmMutex AmThreadWatcher::_inst_mut;

AmThreadWatcher::AmThreadWatcher()
  : _run_cond(false)
{
}

AmThreadWatcher* AmThreadWatcher::instance()
{
  _inst_mut.lock();
  if(!_instance){
    _instance = new AmThreadWatcher();
    _instance->start();
  }

  _inst_mut.unlock();
  return _instance;
}

void AmThreadWatcher::add(AmThread* t)
{
  DBG("trying to add thread %lu to thread watcher.\n", (unsigned long int) t->_pid);
  q_mut.lock();
  thread_queue.push(t);
  _run_cond.set(true);
  q_mut.unlock();
  DBG("added thread %lu to thread watcher.\n", (unsigned long int) t->_pid);
}

void AmThreadWatcher::on_stop()
{
}

void AmThreadWatcher::run()
{
  for(;;){

    _run_cond.wait_for();
    // Let some time for to threads 
    // to stop by themselves
    sleep(10);

    q_mut.lock();
    DBG("Thread watcher starting its work\n");

    try {
      std::queue<AmThread*> n_thread_queue;

      while(!thread_queue.empty()){

	AmThread* cur_thread = thread_queue.front();
	thread_queue.pop();

	q_mut.unlock();
	DBG("thread %lu is to be processed in thread watcher.\n", (unsigned long int) cur_thread->_pid);
	if(cur_thread->is_stopped()){
	  DBG("thread %lu has been destroyed.\n", (unsigned long int) cur_thread->_pid);
	  delete cur_thread;
	}
	else {
	  DBG("thread %lu still running.\n", (unsigned long int) cur_thread->_pid);
	  n_thread_queue.push(cur_thread);
	}

	q_mut.lock();
      }

      swap(thread_queue,n_thread_queue);

    }catch(...){
      /* this one is IMHO very important, as lock is called in try block! */
      ERROR("unexpected exception, state may be invalid!\n");
    }

    bool more = !thread_queue.empty();
    q_mut.unlock();

    DBG("Thread watcher finished\n");
    if(!more)
      _run_cond.set(false);
  }
}

