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

  std::lock_guard<std::mutex> _l(run_mut);
  _state = state::stopped;
}

void AmThread::start()
{
  std::lock_guard<std::mutex> _l(run_mut);

  if (_state != state::idle) {
    DBG("Thread %lu already running.\n", _pid);
    return;
  }
  _state = state::running;

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
  std::unique_lock<std::mutex> _l(run_mut);

  if (_state != state::running) {
    DBG("Thread %lu already stopped\n", _pid);
    return;
  }
  _state = state::stopping;

  _l.unlock();

  run_cond.notify_all();

  // gives the thread a chance to clean up
  DBG("Thread %lu calling on_stop\n", _pid);

  on_stop();
}

void AmThread::join()
{
  std::unique_lock<std::mutex> _l(run_mut);

  // don't attempt to join thread that doesn't exist
  if (_state == state::idle)
    return;

  // nothing to do if already done
  if (_joined == join_state::joined)
    return;

  // is somebody else doing the joining? wait until done
  while (_joined == join_state::joining)
    run_cond.wait(_l);

  if (_joined == join_state::joined)
    return;

  // we have to do the joining (state == unjoined)
  if (_td.joinable()) {
    _joined = join_state::joining;
    _l.unlock();
    _td.join();
    _l.lock();
  }
  _joined = join_state::joined;
  run_cond.notify_all();
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
  std::lock_guard<std::mutex> _l(run_mut);
  thread_list.push_back(t);
  DBG("added thread %lu to thread watcher.\n", t->_pid);
  // TODO: add mechanism a stopping thread can notify AmTreadWatcher
}

void AmThreadWatcher::run()
{
  std::unique_lock<std::mutex> _l(run_mut);

  while (!stop_requested_unlocked() || !thread_list.empty()) {

    run_cond.wait_for(_l, std::chrono::seconds(1));

    DBG("Thread watcher starting its work\n");

    try {
      auto it = thread_list.begin();
      while (it != thread_list.end()) {

	AmThread* cur_thread = *it;

	_l.unlock();

	DBG("thread %lu is to be processed in thread watcher.\n", cur_thread->_pid);

        if (stop_requested()) {
          DBG("thread watcher requesting thread %lu to stop.\n", cur_thread->_pid);
          cur_thread->stop();
        }

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
          _l.lock();
	  it++;
          _l.unlock();
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

