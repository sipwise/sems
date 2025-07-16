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
#include "SdNotify.h"

#include <unistd.h>
#include "errno.h"
#include <string>
using std::string;

#include <system_error>
#include <exception>

AmThread::~AmThread()
{
  stop();
  join();
}

void AmThread::_start()
{
  _pid = static_cast<unsigned long>(_td.native_handle());
  DBG("Thread %lu is starting.\n", _pid);

  if (!_triggers_ready)
    ready();

  run();

  DBG("Thread %lu is ending.\n", _pid);

  _state = state::stopped;
}

void AmThread::start()
{
  state expected = state::idle;
  if(!_state.compare_exchange_strong(expected, state::running)) {
    DBG("Thread %lu already running.\n", _pid);
    return;
  }

  _pid = 0;

  _td = std::thread(&AmThread::_start, this);
}

void AmThread::start(SdNotifier& sd)
{
  _sd_notifier = sd;
  sd.waiter();
  start();
}

void AmThread::ready()
{
  if (!_sd_notifier)
    return;
  _sd_notifier->get().running();
}

void AmThread::stop()
{
  state expected = state::running;
  if(!_state.compare_exchange_strong(expected, state::stopping)){
    DBG("Thread %lu already stopped\n", _pid);
    return;
  }

  // gives the thread a chance to clean up
  DBG("Thread %lu calling on_stop\n", _pid);

  on_stop();
}

void AmThread::join()
{
  // don't attempt to join thread that doesn't exist
  if (_state == state::idle)
    return;

  // make sure only one other thread joins this one. all others
  // are made to wait through the mutex
  std::lock_guard<std::mutex> _l(_join_mt);
  if (!_joined) {
    if (_td.joinable())
      _td.join();
    _joined = true;
  }
}


AmThreadWatcher* AmThreadWatcher::_instance=0;
AmMutex AmThreadWatcher::_inst_mut;

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
  thread_list.push_back(t);
  q_mut.unlock();
  DBG("added thread %lu to thread watcher.\n", t->_pid);
}

void AmThreadWatcher::run()
{
  std::unique_lock<AmMutex> _l(q_mut);

  while (!stop_requested() || !thread_list.empty()) {

    _l.unlock();

    sleep(10);

    DBG("Thread watcher starting its work\n");

    try {
      _l.lock();

      auto it = thread_list.begin();
      while (it != thread_list.end()) {

	AmThread* cur_thread = *it;

	_l.unlock();
	DBG("thread %lu is to be processed in thread watcher.\n", cur_thread->_pid);
	if(cur_thread->is_stopped()){
	  DBG("thread %lu has been destroyed.\n", cur_thread->_pid);
	  cur_thread->join();
          _l.lock();
	  it = thread_list.erase(it);
          _l.unlock();
	  delete cur_thread;
	}
	else {
	  DBG("thread %lu still running.\n", cur_thread->_pid);
	  it++;
	}

	_l.lock();
      }
    } catch(std::exception& e) {
      ERROR("Exception in thread watcher: %s\n", e.what());
    } catch(...) {
      ERROR("Unhandled exception in thread watcher\n");
    }

    // restore lock if exception left it unlocked
    if (!_l.owns_lock())
      _l.lock();

    DBG("Thread watcher finished\n");
  }
}

