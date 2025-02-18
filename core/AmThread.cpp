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

#include <system_error>
#include <exception>

AmThread::AmThread()
  : _stopped(true)
{
}

void AmThread::_start()
{
  _pid = static_cast<unsigned long>(_td.native_handle());
  DBG("Thread %lu is starting.\n", _pid);
  run();

  DBG("Thread %lu is ending.\n", _pid);
  _stopped = true;
    
}

void AmThread::start()
{
  _pid = 0;

  // unless placed here, a call seq like run(); join(); will not wait to join
  // b/c creating the thread can take too long
  bool expected = true;
  if (!this->_stopped.compare_exchange_strong(expected, false)) {
    ERROR("thread already running\n");
    return;
  }

  _td = std::thread(&AmThread::_start, this);
  //DBG("Thread %lu is just created.\n", (unsigned long int) _pid);
}

void AmThread::stop()
{
  lock_guard<AmMutex> _l(_m_td);

  if(is_stopped()){
    return;
  }

  // gives the thread a chance to clean up
  DBG("Thread %lu calling on_stop, give it a chance to clean up.\n", _pid);

  try { on_stop(); } catch(...) {}

  try {
    _td.detach();
  }
  catch (std::system_error &e) {
    WARN("Failed to detach thread: code %d (%s)\n", e.code().value(), e.what());
  }
  catch (std::exception &e) {
    WARN("Failed to detach thread: %s\n", e.what());
  }
  catch (...) {
    WARN("Failed to detach thread: unknown error\n");
  }

  DBG("Thread %lu finished detach.\n", _pid);

  //pthread_cancel(_td);
}

void AmThread::join()
{
  if(!is_stopped())
    _td.join();
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
  DBG("trying to add thread %lu to thread watcher.\n", t->_pid);
  q_mut.lock();
  thread_queue.push(t);
  _run_cond.set(true);
  q_mut.unlock();
  DBG("added thread %lu to thread watcher.\n", t->_pid);
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
	DBG("thread %lu is to be processed in thread watcher.\n", cur_thread->_pid);
	if(cur_thread->is_stopped()){
	  DBG("thread %lu has been destroyed.\n", cur_thread->_pid);
	  delete cur_thread;
	}
	else {
	  DBG("thread %lu still running.\n", cur_thread->_pid);
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

